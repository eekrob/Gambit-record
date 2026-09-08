#include "grecord/UiConfig.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <vector>

namespace grecord::ui {
using nlohmann::json;
namespace {
float bounded(float value, float minimum, float maximum, float fallback) {
  return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
Settings validated(Settings value) {
  value.scale = bounded(value.scale, .75f, 2.f, 1.f);
  value.opacity = bounded(value.opacity, .2f, 1.f, .76f);
  value.anchor.x = bounded(value.anchor.x, 0.f, 1.f, .5f);
  value.anchor.y = bounded(value.anchor.y, 0.f, 1.f, 1.f);
  return value;
}
std::uint32_t color(const json& value) {
  const auto& str = value.get_ref<const std::string&>();
  std::uint32_t result{};
  const auto parsed = std::from_chars(str.data(), str.data() + str.size(), result, 16);
  if (str.size() != 8 || parsed.ec != std::errc{} || parsed.ptr != str.data() + str.size())
    throw std::runtime_error("Invalid ABGR color");
  return result;
}
template<std::size_t N> void colors(const json& value, std::array<std::uint32_t, N>& target) {
  if (!value.is_array() || value.size() != N) throw std::runtime_error("Invalid palette");
  for (std::size_t i = 0; i < N; ++i) target[i] = color(value.at(i));
}
}
Settings load_settings(const std::filesystem::path& path, std::string& error) {
  error.clear();
  try {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream file(path, std::ios::binary);
    const auto j = json::parse(file);
    if (j.value("version", 1) != 1) throw std::runtime_error("Unsupported UI settings version");
    Settings result;
    result.follow_gadmin = j.value("follow_gadmin", true);
    result.custom_position = j.value("custom_position", false);
    result.scale = j.value("scale", 1.f);
    result.opacity = j.value("opacity", .76f);
    result.anchor = {j.value("anchor_x", .5f), j.value("anchor_y", 1.f)};
    return validated(result);
  } catch (const std::exception& e) { error = e.what(); return {}; }
}
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::string& error) {
  error.clear();
  auto temp = path; temp += L".tmp";
  try {
    const auto value = validated(settings);
    const json j{{"version", 1}, {"follow_gadmin", value.follow_gadmin},
      {"custom_position", value.custom_position}, {"anchor_x", value.anchor.x},
      {"anchor_y", value.anchor.y}, {"scale", value.scale}, {"opacity", value.opacity}};
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file << j.dump(2) << '\n'; file.flush();
    if (!file) throw std::runtime_error("Cannot write UI settings");
    file.close();
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Cannot replace UI settings: " + std::to_string(GetLastError()));
    return true;
  } catch (const std::exception& e) {
    error = e.what(); std::error_code ec; std::filesystem::remove(temp, ec); return false;
  }
}
Point clamp_position(Point position, Point size, Point display) {
  return {bounded(position.x, 0, std::max(0.f, display.x - size.x), 0),
          bounded(position.y, 0, std::max(0.f, display.y - size.y), 0)};
}
Point hud_position(const Settings& settings, Point size, Point display) {
  const auto value = validated(settings);
  Point anchor = value.custom_position ? Point{value.anchor.x * display.x, value.anchor.y * display.y}
                                       : Point{display.x * .5f, display.y - 18.f};
  return clamp_position({anchor.x - size.x * .5f, anchor.y - size.y}, size, display);
}
Point hud_anchor(Point position, Point size, Point display) {
  position = clamp_position(position, size, display);
  return {bounded((position.x + size.x * .5f) / std::max(1.f, display.x), 0, 1, .5f),
          bounded((position.y + size.y) / std::max(1.f, display.y), 0, 1, 1)};
}
std::optional<Theme> parse_theme(std::span<const std::uint8_t> bytes) {
  try {
    const auto root = json::from_msgpack(bytes.begin(), bytes.end());
    const auto& theme = root.at("internal").at("theme");
    const auto& base = theme.at("interface_colors");
    const auto& accent = theme.at("accent_colors");
    Theme result;
    colors(base.at("surface"), result.surface); colors(base.at("text"), result.text);
    colors(base.at("overlay"), result.overlay);
    result.red = color(accent.at("red")); result.green = color(accent.at("green"));
    result.yellow = color(accent.at("yellow"));
    return result;
  } catch (...) { return std::nullopt; }
}
void ThemeReader::poll(const std::filesystem::path& path) {
  try {
    if (!std::filesystem::exists(path)) {
      state_ = ThemeState::missing; modified_.reset(); theme_ = Theme{}; return;
    }
    const auto stamp = std::filesystem::last_write_time(path);
    if (state_ == ThemeState::loaded && modified_ == stamp) return;
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 16 * 1024 * 1024) throw std::runtime_error("Invalid configuration size");
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Incomplete read");
    const auto parsed = parse_theme(bytes);
    if (!parsed || std::filesystem::last_write_time(path) != stamp) throw std::runtime_error("Invalid theme");
    theme_ = *parsed; state_ = ThemeState::loaded; modified_ = stamp;
  } catch (...) { state_ = ThemeState::invalid; } // Retry even if the timestamp did not change.
}
}
