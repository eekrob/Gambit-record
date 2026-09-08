// Adapted from GAdmin c31749c02f3d76c1ab0f8ebf562c8dae0dc91152.
// Copyright (C) 2023-2026 The Contributors. SPDX-License-Identifier: GPL-3.0-only
#include "grecord/UiStyle.hpp"
#include <algorithm>
#include <cmath>

namespace grecord::ui {
ImVec4 rgba(std::uint32_t color) { return ImGui::ColorConvertU32ToFloat4(color); }
Fonts load_fonts(HMODULE module) {
  Fonts result;
  ImFont** fonts[]{&result.regular, &result.bold, &result.light, &result.icons};
  static const ImWchar icon_ranges[]{0xe000, 0xf8ff, 0};
  for (int i = 0; i < 4; ++i) {
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(101 + i), RT_RCDATA);
    if (!resource) continue;
    auto* data = LockResource(LoadResource(module, resource));
    const auto size = SizeofResource(module, resource);
    if (!data || !size) continue;
    ImFontConfig config; config.FontDataOwnedByAtlas = false;
    *fonts[i] = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), 18.f, &config,
      i == 3 ? icon_ranges : ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
  }
  if (result.regular) ImGui::GetIO().FontDefault = result.regular;
  return result;
}
void apply_style(const Theme& theme) {
  auto& style = ImGui::GetStyle();
  style.AntiAliasedLines = style.AntiAliasedFill = true;
  style.IndentSpacing = 0; style.ScrollbarSize = style.GrabMinSize = 10;
  style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = style.FrameBorderSize = style.TabBorderSize = 1;
  style.WindowRounding = style.ChildRounding = style.PopupRounding = style.FrameRounding = 8;
  style.ScrollbarRounding = style.GrabRounding = style.TabRounding = 5;
  style.WindowPadding = {8, 8}; style.FramePadding = style.ItemSpacing = {5, 5};
  style.ItemInnerSpacing = style.TouchExtraPadding = {0, 0};
  style.ButtonTextAlign = style.SelectableTextAlign = {.5f, .5f};
  auto* colors = style.Colors;
  for (auto id : {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_TableRowBg, ImGuiCol_TableRowBgAlt}) colors[id] = rgba(theme.surface[0]);
  for (auto id : {ImGuiCol_ChildBg, ImGuiCol_ScrollbarBg, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive, ImGuiCol_TableHeaderBg}) colors[id] = rgba(theme.surface[1]);
  for (auto id : {ImGuiCol_FrameBg, ImGuiCol_ScrollbarGrab, ImGuiCol_Button, ImGuiCol_Header, ImGuiCol_Separator,
                  ImGuiCol_SeparatorHovered, ImGuiCol_Tab}) colors[id] = rgba(theme.overlay[0]);
  for (auto id : {ImGuiCol_FrameBgHovered, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ButtonHovered, ImGuiCol_HeaderHovered,
                  ImGuiCol_SeparatorActive, ImGuiCol_TabHovered}) colors[id] = rgba(theme.overlay[1]);
  for (auto id : {ImGuiCol_Border, ImGuiCol_FrameBgActive, ImGuiCol_ScrollbarGrabActive, ImGuiCol_ButtonActive,
                  ImGuiCol_HeaderActive, ImGuiCol_TextSelectedBg, ImGuiCol_TabSelected, ImGuiCol_TableBorderLight,
                  ImGuiCol_TableBorderStrong}) colors[id] = rgba(theme.overlay[2]);
  for (auto id : {ImGuiCol_BorderShadow, ImGuiCol_ResizeGrip, ImGuiCol_ResizeGripActive, ImGuiCol_ResizeGripHovered}) colors[id] = {0, 0, 0, 0};
  colors[ImGuiCol_Text] = rgba(theme.text[0]); colors[ImGuiCol_TextDisabled] = rgba(theme.text[1]);
  colors[ImGuiCol_CheckMark] = rgba(theme.green);
  colors[ImGuiCol_PlotHistogram] = rgba(theme.green); colors[ImGuiCol_PlotHistogramHovered] = rgba(theme.yellow);
  colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgActive] = rgba(theme.surface[1]);
  colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, .5f};
}
bool toggle(const char* label, bool* value) {
  // GAdmin's outlined switch and animated circular thumb.
  const auto pos = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight();
  const ImVec2 size{height * 1.35f, height / 1.4f};
  ImGui::BeginGroup();
  const bool pressed = ImGui::InvisibleButton(label, size);
  if (pressed) *value = !*value;
  const auto id = ImGui::GetID(label);
  auto* storage = ImGui::GetStateStorage();
  float t = storage->GetFloat(id, *value ? 1.f : 0.f);
  const float step = std::min(1.f, ImGui::GetIO().DeltaTime / .15f);
  t += ((*value ? 1.f : 0.f) - t) * step;
  storage->SetFloat(id, t);
  const auto off = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
  const auto on = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive);
  const ImVec4 color{off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, off.w + (on.w - off.w) * t};
  const auto packed = ImGui::GetColorU32(color);
  const float padding = ImGui::GetStyle().FramePadding.x;
  const float radius = (size.y - padding) / 3.f;
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRect(pos, {pos.x + size.x, pos.y + size.y}, packed, ImGui::GetStyle().GrabRounding * 2);
  draw->AddCircleFilled({pos.x + padding + radius + (size.x - 2 * (padding + radius)) * t, pos.y + size.y / 2}, radius, packed, 32);
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size.y - ImGui::GetTextLineHeight()) * .5f);
  ImGui::TextUnformatted(label);
  ImGui::EndGroup();
  return pressed;
}
bool button(const char* label, ImVec2 size) {
  const auto text_size = ImGui::CalcTextSize(label, nullptr, true);
  const auto padding = ImGui::GetStyle().FramePadding;
  const float available = std::max(1.f, ImGui::GetContentRegionAvail().x);
  size.x = std::min(available, size.x == 0 ? text_size.x + 2 * padding.x : size.x < 0 ? available : size.x);
  if (size.y <= 0) size.y = text_size.y + 2 * padding.y;
  const auto start = ImGui::GetCursorScreenPos();
  const ImVec2 end{start.x + size.x, start.y + size.y};
  const bool pressed = ImGui::InvisibleButton(label, size);
  auto* storage = ImGui::GetStateStorage();
  const auto id = ImGui::GetID(label);
  float hover = storage->GetFloat(id, 0);
  hover += ((ImGui::IsItemHovered() ? 1.f : 0.f) - hover) * std::min(1.f, ImGui::GetIO().DeltaTime / .2f);
  storage->SetFloat(id, hover);
  if (pressed) {
    storage->SetFloat(id + 1, static_cast<float>(ImGui::GetTime()));
    const auto mouse = ImGui::GetMousePos();
    storage->SetFloat(id + 2, mouse.x - start.x); storage->SetFloat(id + 3, mouse.y - start.y);
  }
  auto* draw = ImGui::GetWindowDrawList();
  const float rounding = ImGui::GetStyle().FrameRounding;
  draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_Button), rounding);
  draw->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_ButtonHovered, hover), rounding, 1.f, ImDrawFlags_None);
  const float elapsed = static_cast<float>(ImGui::GetTime()) - storage->GetFloat(id + 1, -10);
  if (elapsed >= 0 && elapsed < .7f) {
    const float alpha = elapsed < .4f ? elapsed / .4f : 1.f - (elapsed - .4f) / .3f;
    draw->PushClipRect({start.x + 1, start.y + 1}, {end.x - 1, end.y - 1}, true);
    draw->AddCircleFilled({start.x + storage->GetFloat(id + 2), start.y + storage->GetFloat(id + 3)},
      std::max(size.x, size.y) * elapsed / .7f, ImGui::GetColorU32(ImGuiCol_ButtonActive, alpha), 64);
    draw->PopClipRect();
  }
  const char* text_end = label;
  while (*text_end && !(text_end[0] == '#' && text_end[1] == '#')) ++text_end;
  draw->PushClipRect(start, end, true);
  draw->AddText({start.x + (size.x - text_size.x) * .5f, start.y + (size.y - text_size.y) * .5f},
    ImGui::GetColorU32(ImGuiCol_Text), label, text_end);
  draw->PopClipRect();
  return pressed;
}
}
