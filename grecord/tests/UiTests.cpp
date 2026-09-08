#include "grecord/UiConfig.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace grecord::ui;
using nlohmann::json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool close(float a, float b) { return std::abs(a - b) < .001f; }
json fixture() {
  return {{"internal", {{"theme", {
    {"interface_colors", {{"surface", {"FF2E1E1E", "FF251818"}}, {"text", {"FFF4D6CD", "FFDEC2BA"}}, {"overlay", {"FF443231", "FF5A4745", "FF705B58"}}}},
    {"accent_colors", {{"red", "FFA88BF3"}, {"green", "FFA1E3A6"}, {"yellow", "FFAFE2F9"}}}
  }}}}};
}
void write_theme(const std::filesystem::path& path, const json& j) {
  const auto bytes = json::to_msgpack(j);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / (L"grecord-ui-тест-" + std::to_wstring(GetCurrentProcessId()));
  try {
    auto bytes = json::to_msgpack(fixture());
    auto theme = parse_theme(bytes);
    check(theme && *theme == Theme{}, "Default theme does not match GAdmin");
    check((theme->red & 255) == 0xf3 && ((theme->red >> 16) & 255) == 0xa8, "ABGR channels swapped");
    auto j = fixture(); j["internal"]["theme"]["accent_colors"]["red"] = "80112233";
    theme = parse_theme(json::to_msgpack(j));
    check(theme && theme->red == 0x80112233, "Alpha was discarded");
    for (const auto& bad : {json("garbage!"), json("FFA88B"), json(123), json(nullptr)}) {
      j["internal"]["theme"]["accent_colors"]["red"] = bad;
      check(!parse_theme(json::to_msgpack(j)), "Invalid color accepted");
    }
    bytes.resize(bytes.size() / 2); check(!parse_theme(bytes), "Truncated MessagePack accepted");
    j = fixture(); j["internal"]["theme"]["interface_colors"]["surface"] = {"FF000000"};
    check(!parse_theme(json::to_msgpack(j)), "Short palette accepted");
    check(!parse_theme(json::to_msgpack(json::object())), "Missing theme accepted");

    Settings settings; std::string error;
    const auto path = root / L"настройки" / "ui.json";
    check(load_settings(path, error).follow_gadmin && error.empty(), "Missing config defaults");
    settings.custom_position = true; settings.anchor = {.25f, .6f}; settings.scale = 1.5f; settings.opacity = .4f; settings.follow_gadmin = false;
    check(save_settings(path, settings, error), "Save failed");
    auto loaded = load_settings(path, error);
    check(error.empty() && loaded.custom_position && !loaded.follow_gadmin && close(loaded.scale, 1.5f) && close(loaded.opacity, .4f) && close(loaded.anchor.x, .25f), "Settings round trip failed");
    settings.scale = 99; settings.opacity = -1;
    check(save_settings(path, settings, error), "Atomic replacement failed");
    loaded = load_settings(path, error);
    check(loaded.scale == 2 && loaded.opacity == .2f, "Invalid settings not bounded");
    check(!std::filesystem::exists(path.wstring() + L".tmp"), "Temporary file left after save");
    std::ofstream(path) << "{partial";
    loaded = load_settings(path, error);
    check(!error.empty() && loaded.scale == 1 && !loaded.custom_position, "Corrupt settings fallback failed");

    settings = {};
    auto pos = hud_position(settings, {200, 40}, {1920, 1080});
    check(close(pos.x, 860) && close(pos.y, 1022), "Default bottom center incorrect");
    auto grown = hud_position(settings, {400, 90}, {1920, 1080});
    check(close(grown.x + 200, pos.x + 100) && close(grown.y + 90, pos.y + 40), "Content changes move anchor");
    settings.custom_position = true; settings.anchor = hud_anchor({200, 300}, {200, 40}, {1000, 800});
    pos = hud_position(settings, {200, 40}, {2000, 1600});
    check(close(pos.x, 500) && close(pos.y, 640), "Resolution scaling incorrect");
    pos = clamp_position({-200, 900}, {200, 40}, {800, 600});
    check(pos.x == 0 && pos.y == 560, "Edge clamp incorrect");
    pos = hud_position(settings, {1000, 1000}, {320, 240});
    check(pos.x == 0 && pos.y == 0, "Oversized HUD not clamped");
    pos = clamp_position({std::numeric_limits<float>::quiet_NaN(), 5}, {10, 10}, {0, 0});
    check(pos.x == 0 && pos.y == 0, "Nonfinite/zero display handling");

    const auto source = root / "main.mpk"; ThemeReader reader;
    reader.poll(source); check(reader.state() == ThemeState::missing, "Missing theme state");
    j = fixture(); j["internal"]["theme"]["accent_colors"]["red"] = "FF010203";
    write_theme(source, j); reader.poll(source);
    check(reader.state() == ThemeState::loaded && reader.theme().red == 0xff010203, "Theme not loaded");
    const auto stamp = std::filesystem::last_write_time(source);
    std::ofstream(source) << "partial";
    std::filesystem::last_write_time(source, stamp + std::chrono::seconds(1));
    reader.poll(source);
    check(reader.state() == ThemeState::invalid && reader.theme().red == 0xff010203, "Last good theme not preserved");
    write_theme(source, fixture());
    std::filesystem::last_write_time(source, stamp + std::chrono::seconds(1));
    reader.poll(source);
    check(reader.state() == ThemeState::loaded && reader.theme() == Theme{}, "Retry with same timestamp failed");
    std::filesystem::remove(source); reader.poll(source);
    check(reader.state() == ThemeState::missing && reader.theme() == Theme{}, "Removed theme fallback failed");
    std::filesystem::remove_all(root);
    std::cout << "UI configuration, theme recovery and HUD geometry passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; std::filesystem::remove_all(root); return 1;
  }
}
