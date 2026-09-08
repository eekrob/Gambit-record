#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace grecord::ui {
struct Point { float x{}, y{}; };
struct Settings {
  bool follow_gadmin{true};
  bool custom_position{false};
  Point anchor{.5f, 1.f}; // Normalized bottom-center of the HUD.
  float scale{1.f};
  float opacity{.76f};
};
struct Theme {
  // GAdmin serializes colors as eight hexadecimal ABGR digits.
  std::array<std::uint32_t, 2> surface{0xFF2E1E1E, 0xFF251818};
  std::array<std::uint32_t, 2> text{0xFFF4D6CD, 0xFFDEC2BA};
  std::array<std::uint32_t, 3> overlay{0xFF443231, 0xFF5A4745, 0xFF705B58};
  std::uint32_t red{0xFFA88BF3}, green{0xFFA1E3A6}, yellow{0xFFAFE2F9};
  bool operator==(const Theme&) const = default;
};
Settings load_settings(const std::filesystem::path& path, std::string& error);
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::string& error);
Point clamp_position(Point position, Point size, Point display);
Point hud_position(const Settings& settings, Point size, Point display);
Point hud_anchor(Point position, Point size, Point display);
std::optional<Theme> parse_theme(std::span<const std::uint8_t> bytes);
enum class ThemeState { missing, loaded, invalid };
class ThemeReader {
 public:
  void poll(const std::filesystem::path& path);
  const Theme& theme() const { return theme_; }
  ThemeState state() const { return state_; }
 private:
  Theme theme_;
  ThemeState state_{ThemeState::missing};
  std::optional<std::filesystem::file_time_type> modified_;
};
}
