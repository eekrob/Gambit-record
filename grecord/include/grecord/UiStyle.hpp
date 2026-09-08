#pragma once
#include "grecord/UiConfig.hpp"
#include <Windows.h>
#include <imgui.h>

namespace grecord::ui {
struct Fonts { ImFont* regular{}; ImFont* bold{}; ImFont* light{}; ImFont* icons{}; };
Fonts load_fonts(HMODULE module);
void apply_style(const Theme& theme);
bool toggle(const char* label, bool* value);
bool button(const char* label, ImVec2 size = {0, 0});
ImVec4 rgba(std::uint32_t color);
}
