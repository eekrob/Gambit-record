// Gambit Record - ASI integration for GTA:SA / SA-MP.
// Portions of the version/address strategy are derived from GAdmin at
// c31749c02f3d76c1ab0f8f562c8dae0dc91152 (GPL-3.0-only).

#include "grecord/BitReader.hpp"
#include "grecord/IpcClient.hpp"
#include "grecord/Logic.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <d3d9.h>
#include <MinHook.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using json = nlohmann::json;
enum class SampVersion : int { unknown = -1, r1 = 0, r3 = 1, r5 = 2, dl = 3 };
constexpr std::array<std::uintptr_t, 4> send_command_offsets{0x65C60, 0x69190, 0x69900, 0x69340};
constexpr std::array<std::uintptr_t, 4> incoming_rpc_offsets{0x372F0, 0x3A6A0, 0x3ADE0, 0x3A8A0};
constexpr std::array<std::uintptr_t, 4> net_game_offsets{0x21A0F8, 0x26E8DC, 0x26EB94, 0x2ACA24};
constexpr std::array<std::uintptr_t, 4> host_offsets{0x20, 0x30, 0x30, 0x30};
constexpr std::array<std::uintptr_t, 4> get_pool_offsets{0x1160, 0x1160, 0x1170, 0x1170};
constexpr std::array<std::uintptr_t, 4> get_nickname_offsets{0x13CE0, 0x16F00, 0x175C0, 0x170D0};
constexpr std::array<std::uintptr_t, 4> own_name_offsets{0xA, 0x2F22, 0xA, 0x6};

HMODULE g_module{};
std::uintptr_t g_samp{};
SampVersion g_version{SampVersion::unknown};
HWND g_window{};
WNDPROC g_original_wndproc{};
std::atomic_bool g_running{true};
std::atomic_bool g_initialized{};
std::atomic_bool g_menu_open{};
std::atomic_bool g_reload_settings{true};
std::atomic_bool g_recording{};
std::atomic<unsigned> g_upload_percent{};
std::atomic_bool g_uploading{};
std::mutex g_state_mutex;
std::mutex g_ipc_mutex;
grecord::Logic g_logic;
std::unique_ptr<grecord::IpcClient> g_ipc;
json g_status;
std::string g_admin;
std::string g_notice;
std::string g_last_url;
std::chrono::steady_clock::time_point g_notice_until;
std::chrono::system_clock::time_point g_record_started;
enum class Prompt { none, start, finish, missing };
Prompt g_prompt{Prompt::none};
enum class Page { recording, uploads, settings, about };
Page g_page{Page::recording};
ImFont* g_font_regular{};
ImFont* g_font_bold{};
bool g_cursor_owned{};

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SendCommandFn = void(__thiscall*)(void*, const char*);
struct PlayerId { std::uint32_t address; std::uint16_t port; };
using IncomingRpcFn = bool(__thiscall*)(void*, const char*, int, PlayerId);
PresentFn g_present{}; ResetFn g_reset{}; SendCommandFn g_send_command{}; IncomingRpcFn g_incoming_rpc{};

SampVersion detect_version(std::uintptr_t base) {
  if (!base) return SampVersion::unknown;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  switch (nt->OptionalHeader.AddressOfEntryPoint) {
    case 0x31DF13: return SampVersion::r1;
    case 0xCC4D0: return SampVersion::r3;
    case 0xCBC90: return SampVersion::r5;
    case 0xFDB60: return SampVersion::dl;
    default: return SampVersion::unknown;
  }
}
int vi() { return static_cast<int>(g_version); }

void write_memory(std::uintptr_t address, std::span<const std::uint8_t> bytes) {
  DWORD old_protect{};
  if (!VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), PAGE_EXECUTE_READWRITE, &old_protect)) return;
  std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());
  DWORD ignored{}; VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), old_protect, &ignored);
}
void write_memory(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes) {
  write_memory(address, std::span<const std::uint8_t>(bytes.begin(), bytes.size()));
}

// GTA:SA keeps recentering and hiding the cursor even when Win32 mouse messages are consumed.
// These are the same US 1.0 input patches used by GAdmin's cursor implementation.
void set_game_cursor(bool enabled) {
  if (enabled) {
    write_memory(0x53F417, {0x90, 0x90, 0x90, 0x90, 0x90});
    write_memory(0x53F41F, {0x33, 0xC0, 0x0F, 0x84});
    write_memory(0x6194A0, {0xC3});
  } else {
    write_memory(0x53F417, {0xE8, 0xB4, 0x7A, 0x20, 0x00});
    write_memory(0x53F41F, {0x85, 0xC0, 0x0F, 0x8C});
    write_memory(0x6194A0, {0xE9});
  }
  write_memory(0xB73424, {0, 0, 0, 0, 0, 0, 0, 0});
  write_memory(0xB7342C, {0, 0, 0, 0, 0, 0, 0, 0});
  reinterpret_cast<void(__cdecl*)()>(0x541BD0)();
  reinterpret_cast<void(__cdecl*)()>(0x541DD0)();
  ClipCursor(nullptr);
  SetCursor(enabled ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
}

void update_cursor(bool interactive) {
  if (interactive) {
    set_game_cursor(true); // GTA/SA-MP may overwrite the mode, so re-assert it every frame.
    if (!g_cursor_owned && g_window) {
      RECT area{}; GetClientRect(g_window, &area);
      POINT center{(area.right - area.left) / 2, (area.bottom - area.top) / 2};
      ClientToScreen(g_window, &center); SetCursorPos(center.x, center.y);
    }
  } else if (g_cursor_owned) {
    set_game_cursor(false);
  }
  g_cursor_owned = interactive;
}

void apply_gadmin_style() {
  auto& style = ImGui::GetStyle();
  style.AntiAliasedLines = true; style.AntiAliasedFill = true;
  style.IndentSpacing = 0.f; style.ScrollbarSize = 10.f; style.GrabMinSize = 10.f;
  style.WindowBorderSize = 1.f; style.ChildBorderSize = 1.f; style.PopupBorderSize = 1.f;
  style.FrameBorderSize = 1.f; style.TabBorderSize = 1.f;
  style.WindowRounding = 8.f; style.ChildRounding = 8.f; style.PopupRounding = 8.f;
  style.FrameRounding = 8.f; style.ScrollbarRounding = 5.f; style.GrabRounding = 5.f; style.TabRounding = 5.f;
  style.WindowPadding = {8.f, 8.f}; style.FramePadding = {8.f, 6.f};
  style.ItemSpacing = {7.f, 7.f}; style.ItemInnerSpacing = {5.f, 5.f};
  style.ButtonTextAlign = {.5f, .5f}; style.SelectableTextAlign = {.5f, .5f};

  const ImVec4 surface0{0x1e / 255.f, 0x1e / 255.f, 0x2e / 255.f, 1.f};
  const ImVec4 surface1{0x18 / 255.f, 0x18 / 255.f, 0x25 / 255.f, 1.f};
  const ImVec4 text0{0xcd / 255.f, 0xd6 / 255.f, 0xf4 / 255.f, 1.f};
  const ImVec4 text1{0xba / 255.f, 0xc2 / 255.f, 0xde / 255.f, 1.f};
  const ImVec4 overlay0{0x31 / 255.f, 0x32 / 255.f, 0x44 / 255.f, 1.f};
  const ImVec4 overlay1{0x45 / 255.f, 0x47 / 255.f, 0x5a / 255.f, 1.f};
  const ImVec4 overlay2{0x58 / 255.f, 0x5b / 255.f, 0x70 / 255.f, 1.f};
  auto* colors = style.Colors;
  colors[ImGuiCol_Text] = text0; colors[ImGuiCol_TextDisabled] = text1;
  colors[ImGuiCol_WindowBg] = surface0; colors[ImGuiCol_ChildBg] = surface1; colors[ImGuiCol_PopupBg] = surface0;
  colors[ImGuiCol_Border] = overlay2; colors[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
  colors[ImGuiCol_FrameBg] = overlay0; colors[ImGuiCol_FrameBgHovered] = overlay1; colors[ImGuiCol_FrameBgActive] = overlay2;
  colors[ImGuiCol_Button] = overlay0; colors[ImGuiCol_ButtonHovered] = overlay1; colors[ImGuiCol_ButtonActive] = overlay2;
  colors[ImGuiCol_Header] = overlay0; colors[ImGuiCol_HeaderHovered] = overlay1; colors[ImGuiCol_HeaderActive] = overlay2;
  colors[ImGuiCol_Separator] = overlay0; colors[ImGuiCol_SeparatorHovered] = overlay1; colors[ImGuiCol_SeparatorActive] = overlay2;
  colors[ImGuiCol_ScrollbarBg] = surface1; colors[ImGuiCol_ScrollbarGrab] = overlay0;
  colors[ImGuiCol_ScrollbarGrabHovered] = overlay1; colors[ImGuiCol_ScrollbarGrabActive] = overlay2;
  colors[ImGuiCol_CheckMark] = {0xa6 / 255.f, 0xe3 / 255.f, 0xa1 / 255.f, 1.f};
  colors[ImGuiCol_SliderGrab] = overlay2; colors[ImGuiCol_SliderGrabActive] = text1;
  colors[ImGuiCol_ResizeGrip] = colors[ImGuiCol_ResizeGripHovered] = colors[ImGuiCol_ResizeGripActive] = {0, 0, 0, 0};
  colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, .62f};
}

std::filesystem::path game_directory() {
  std::wstring path(32768, L'\0'); const auto n = GetModuleFileNameW(g_module, path.data(), path.size());
  path.resize(n); return std::filesystem::path(path).parent_path();
}
std::wstring pipe_name() { return LR"(\\.\pipe\GambitRecord-)" + std::to_wstring(GetCurrentProcessId()); }

std::string local_stamp() {
  SYSTEMTIME value{}; GetLocalTime(&value);
  return std::format("{:04}-{:02}-{:02} {:02}-{:02}-{:02}", value.wYear, value.wMonth, value.wDay,
                     value.wHour, value.wMinute, value.wSecond);
}
std::string elapsed_period() {
  if (g_record_started.time_since_epoch().count() == 0) return {};
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - g_record_started).count();
  return std::to_string(std::max<std::int64_t>(0, seconds)) + " seconds";
}

json request_worker(json request, unsigned timeout = 1500) {
  std::scoped_lock lock(g_ipc_mutex);
  return g_ipc ? g_ipc->request(std::move(request), timeout) : json{{"success", false}, {"error", "WORKER_OFFLINE"}};
}

json metadata() {
  std::scoped_lock lock(g_state_mutex);
  return {{"admin", g_admin}, {"target_id", g_logic.target_id()}, {"target_name", g_logic.target_name()},
          {"server", "Gambit Role Play"}, {"timestamp", local_stamp()},
          {"punishment_command", g_logic.confirmed_command()}, {"punishment_reason", g_logic.confirmed_reason()},
          {"recording_period", elapsed_period()}};
}

void notice(std::string text, std::chrono::seconds duration = std::chrono::seconds(8)) {
  std::scoped_lock lock(g_state_mutex); g_notice = std::move(text); g_notice_until = std::chrono::steady_clock::now() + duration;
}
void start_recording() {
  auto response = request_worker({{"command", "record_start"}, {"metadata", metadata()}});
  if (response.value("success", false)) { g_record_started = std::chrono::system_clock::now(); g_recording = true; notice("Запись начата"); }
  else notice("Не удалось начать запись: " + response.value("error", "unknown"));
}
void stop_recording(bool upload) {
  auto response = request_worker({{"command", "record_stop"}, {"upload", upload}, {"metadata", metadata()}}, 5000);
  if (response.value("success", false)) { g_recording = false; notice(upload ? "Запись завершена, загрузка поставлена в очередь" : "Запись сохранена локально"); }
  else notice("Не удалось завершить запись: " + response.value("error", "unknown"));
}

std::uintptr_t net_game() {
  if (vi() < 0) return 0;
  return *reinterpret_cast<std::uintptr_t*>(g_samp + net_game_offsets[vi()]);
}
std::uintptr_t player_pool() {
  auto net = net_game(); if (!net) return 0;
  using Fn = std::uintptr_t(__thiscall*)(std::uintptr_t);
  return reinterpret_cast<Fn>(g_samp + get_pool_offsets[vi()])(net);
}
std::string own_name() {
  const auto pool = player_pool(); if (!pool) return {};
  return grecord::cp1251_to_utf8(reinterpret_cast<const char*>(pool + own_name_offsets[vi()]));
}
std::string player_name(std::uint16_t id) {
  const auto pool = player_pool(); if (!pool) return {};
  using Fn = const char*(__thiscall*)(std::uintptr_t, std::uint16_t);
  const auto value = reinterpret_cast<Fn>(g_samp + get_nickname_offsets[vi()])(pool, id);
  return value ? grecord::cp1251_to_utf8(value) : std::string{};
}
std::string server_host() {
  const auto net = net_game(); if (!net) return {};
  return std::string(reinterpret_cast<const char*>(net + host_offsets[vi()]));
}
bool on_gambit_server() {
  const auto host = server_host(); if (host.empty()) return false;
  std::string lowered = host; std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
  return lowered.find("gambit") != std::string::npos || host == "85.234.65.36";
}

void apply_action(grecord::Action action) {
  switch (action) {
    case grecord::Action::open_settings: {
      const bool opening = !g_menu_open.load(); g_menu_open = opening;
      if (opening) g_reload_settings = true;
      break;
    }
    case grecord::Action::start_recording: start_recording(); break;
    case grecord::Action::stop_local: stop_recording(false); break;
    case grecord::Action::show_start_prompt: { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::start; break; }
    case grecord::Action::show_finish_prompt: { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::finish; break; }
    case grecord::Action::show_missing_evidence: { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::missing; break; }
    default: break;
  }
}

void process_rpc(std::uint8_t id, const std::vector<std::uint8_t>& bytes) {
  grecord::BitReader reader(bytes.data(), bytes.size());
  if (id == 11) {
    std::uint16_t player{}; std::uint8_t length{};
    if (reader.read(player) && reader.read(length)) g_logic.on_player_name(player, grecord::cp1251_to_utf8(reader.read_string(length)));
  } else if (id == 93) {
    std::int32_t color{}, length{};
    if (reader.read(color) && reader.read(length) && length >= 0 && length <= 4096) {
      auto message = grecord::cp1251_to_utf8(reader.read_string(static_cast<std::size_t>(length)));
      std::scoped_lock lock(g_state_mutex); const auto action = g_logic.on_server_message(message, g_recording.load());
      if (action == grecord::Action::show_finish_prompt) g_prompt = Prompt::finish;
      else if (action == grecord::Action::show_missing_evidence) g_prompt = Prompt::missing;
    }
  } else if (id == 126) {
    std::uint16_t player{}; std::uint8_t camera{};
    if (reader.read(player) && reader.read(camera)) {
      auto nickname = player_name(player); std::scoped_lock lock(g_state_mutex);
      if (!nickname.empty()) g_logic.on_player_name(player, nickname);
      if (g_logic.on_spectating_player(player, g_recording.load()) == grecord::Action::show_start_prompt) g_prompt = Prompt::start;
    }
  } else if (id == 78) {
    std::scoped_lock lock(g_state_mutex); g_logic.clear_spectating();
  }
}

bool __fastcall incoming_rpc_hook(void* self, void*, const char* data, int length, PlayerId player) {
  grecord::RpcPayload rpc;
  if (grecord::extract_rpc_payload(data, length, rpc)) process_rpc(rpc.id, rpc.bytes);
  return g_incoming_rpc(self, data, length, player);
}

void __fastcall send_command_hook(void* self, void*, const char* raw) {
  const std::string command = raw ? grecord::cp1251_to_utf8(raw) : std::string{};
  const auto first_space = command.find(' '); const auto word = command.substr(0, first_space);
  const bool local = word == "/grecord" || word == "/estart" || word == "/estop" || word == "/estatus" || word == "/esettings";
  grecord::Action action;
  { std::scoped_lock lock(g_state_mutex); action = g_logic.on_command(command, g_recording.load()); }
  apply_action(action);
  if (word == "/esettings") g_page = Page::settings;
  if (word == "/estatus") notice(g_recording ? "REC: включена" : "REC: выключена");
  if (!local) g_send_command(self, raw);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  bool prompt_open; { std::scoped_lock lock(g_state_mutex); prompt_open = g_prompt != Prompt::none; }
  const bool interactive = g_menu_open || prompt_open;
  if (interactive) {
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
      std::scoped_lock lock(g_state_mutex);
      if (g_prompt != Prompt::none) g_prompt = Prompt::none;
      else g_menu_open = false;
      return 1;
    }
    const auto handled = ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
    if (handled || message == WM_INPUT ||
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN ||
        message == WM_SYSKEYUP || message == WM_CHAR) return 1;
  }
  return CallWindowProcW(g_original_wndproc, hwnd, message, wparam, lparam);
}

void render_hud() {
  const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  ImGui::SetNextWindowBgAlpha(0.76f); ImGui::SetNextWindowPos({18, 18}, ImGuiCond_Always);
  if (ImGui::Begin("##grecord-hud", nullptr, flags)) {
    if (g_recording) {
      std::int64_t seconds{};
      { std::scoped_lock lock(g_state_mutex); seconds = g_status.value("recording_seconds", 0ll); }
      std::string target; { std::scoped_lock lock(g_state_mutex); target = g_logic.target_name(); }
      ImGui::TextColored({1.f, .2f, .2f, 1.f}, "REC %02lld:%02lld", seconds / 60, seconds % 60);
      if (!target.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(("· " + target).c_str()); }
    }
    if (g_uploading) ImGui::TextColored({.35f, .72f, 1.f, 1.f}, "UPLOAD %u%%", g_upload_percent.load());
    std::scoped_lock lock(g_state_mutex);
    if (!g_notice.empty() && std::chrono::steady_clock::now() < g_notice_until) ImGui::TextWrapped("%s", g_notice.c_str());
    if (!g_last_url.empty()) ImGui::TextColored({.35f, 1.f, .55f, 1.f}, "%s", g_last_url.c_str());
  }
  ImGui::End();
}

bool action_button(const char* label, const ImVec4& color, ImVec2 size = {0, 0}) {
  auto brighten = [](ImVec4 value, float amount) {
    value.x = std::min(1.f, value.x + amount); value.y = std::min(1.f, value.y + amount); value.z = std::min(1.f, value.z + amount); return value;
  };
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(color, .08f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(color, .15f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return pressed;
}

void card_begin(const char* id, const char* title, float height) {
  ImGui::BeginChild(id, {0, height}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  if (g_font_bold) ImGui::PushFont(g_font_bold);
  ImGui::TextUnformatted(title);
  if (g_font_bold) ImGui::PopFont();
  ImGui::Spacing();
}
void card_end() { ImGui::EndChild(); }

bool page_button(const char* label, Page page) {
  const bool selected = g_page == page;
  if (selected) {
    ImGui::PushStyleColor(ImGuiCol_Header, {0x45 / 255.f, 0x47 / 255.f, 0x5a / 255.f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0x58 / 255.f, 0x5b / 255.f, 0x70 / 255.f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0x58 / 255.f, 0x5b / 255.f, 0x70 / 255.f, 1.f});
  }
  const bool pressed = ImGui::Selectable(label, selected, 0, {ImGui::GetContentRegionAvail().x, 42.f});
  if (selected) ImGui::PopStyleColor(3);
  if (pressed) g_page = page;
  return pressed;
}

void page_title(const char* title, const char* description) {
  if (g_font_bold) ImGui::PushFont(g_font_bold);
  ImGui::TextUnformatted(title);
  if (g_font_bold) ImGui::PopFont();
  ImGui::TextDisabled("%s", description);
  ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
}

void render_prompt() {
  Prompt prompt; { std::scoped_lock lock(g_state_mutex); prompt = g_prompt; }
  if (prompt == Prompt::none) return;
  const char* title = prompt == Prompt::start ? "Gambit Record##start" : prompt == Prompt::finish ? "Gambit Record##finish" : "Gambit Record##missing";
  ImGui::OpenPopup(title);
  ImGui::SetNextWindowSize({prompt == Prompt::finish ? 620.f : 430.f, 0}, ImGuiCond_Always);
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::TextColored({0xcb / 255.f, 0xa6 / 255.f, 0xf7 / 255.f, 1.f}, "GAMBIT RECORD");
    if (g_font_bold) ImGui::PopFont();
    ImGui::Separator(); ImGui::Spacing();
    if (prompt == Prompt::start) {
      ImGui::TextUnformatted("Вы начали слежку без записи.");
      ImGui::TextDisabled("Запись поможет сохранить доказательства до выдачи наказания."); ImGui::Spacing();
      if (action_button("Начать запись", {0x40 / 255.f, 0xa0 / 255.f, 0x67 / 255.f, 1.f}, {180, 36})) { start_recording(); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Не сейчас", {150, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else if (prompt == Prompt::finish) {
      ImGui::TextUnformatted("Наказание подтверждено сервером.");
      ImGui::TextDisabled("Можно сразу поставить ролик в очередь YouTube или оставить файл локально."); ImGui::Spacing();
      if (action_button("Завершить и загрузить", {0x40 / 255.f, 0xa0 / 255.f, 0x67 / 255.f, 1.f}, {205, 36})) { stop_recording(true); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Завершить локально", {190, 36})) { stop_recording(false); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Продолжить", {135, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else {
      ImGui::TextWrapped("Наказание подтверждено, но доказательство не записано.");
      ImGui::Spacing(); if (ImGui::Button("Понятно", {140, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
  }
}

void render_window() {
  if (!g_menu_open) return;
  json status; std::string last_url;
  { std::scoped_lock lock(g_state_mutex); status = g_status; last_url = g_last_url; }
  ImGui::SetNextWindowSize({760, 470}, ImGuiCond_Always);
  const auto display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({display.x * .5f, display.y * .5f}, ImGuiCond_FirstUseEver, {.5f, .5f});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
  if (ImGui::Begin("Gambit Record##main", nullptr, flags)) {
    ImGui::BeginChild("##sidebar", {185, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.f);
    ImGui::TextColored({0xcb / 255.f, 0xa6 / 255.f, 0xf7 / 255.f, 1.f}, "GAMBIT RECORD");
    if (g_font_bold) ImGui::PopFont();
    ImGui::TextDisabled("grecord  v1.0");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    page_button("Запись", Page::recording);
    page_button("Загрузки", Page::uploads);
    page_button("Настройки", Page::settings);
    page_button("О программе", Page::about);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 45.f);
    ImGui::TextDisabled("Gambit Role Play");
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 42.f);
    if (ImGui::Button("X", {30, 28})) g_menu_open = false;
    ImGui::SetCursorPosY(12.f);

    if (g_page == Page::recording) {
      page_title("Запись", "Управление доказательством и важными моментами");
      card_begin("##record-state", "ТЕКУЩЕЕ СОСТОЯНИЕ", 112.f);
      ImGui::TextColored(g_recording ? ImVec4{0xf3 / 255.f, 0x8b / 255.f, 0xa8 / 255.f, 1.f} : ImVec4{0xa6 / 255.f, 0xe3 / 255.f, 0xa1 / 255.f, 1.f},
                         "%s", g_recording ? "● ИДЁТ ЗАПИСЬ" : "● ГОТОВ К ЗАПИСИ");
      std::string target; { std::scoped_lock lock(g_state_mutex); target = g_logic.target_name(); }
      ImGui::TextDisabled("Наблюдение: %s", target.empty() ? "не выбрано" : target.c_str());
      card_end();
      card_begin("##record-actions", "ДЕЙСТВИЯ", 150.f);
      if (!g_recording) {
        if (action_button("Начать запись", {0x40 / 255.f, 0xa0 / 255.f, 0x67 / 255.f, 1.f}, {180, 38})) start_recording();
      } else {
        if (action_button("Завершить и загрузить", {0x40 / 255.f, 0xa0 / 255.f, 0x67 / 255.f, 1.f}, {205, 38})) stop_recording(true);
        ImGui::SameLine(); if (ImGui::Button("Сохранить локально", {180, 38})) stop_recording(false);
      }
      if (ImGui::Button("Добавить важную метку", {205, 36})) request_worker({{"command", "marker"}, {"label", "important"}});
      card_end();
    } else if (g_page == Page::uploads) {
      page_title("Загрузки", "Очередь публикации на общем YouTube-канале");
      card_begin("##upload-channel", "КАНАЛ", 92.f);
      ImGui::Text("%s", status.value("youtube_channel", "Gambit Record").c_str());
      ImGui::TextDisabled("Приватность всех публикаций: доступ по ссылке");
      card_end();
      card_begin("##upload-state", "СОСТОЯНИЕ ЗАГРУЗКИ", 142.f);
      ImGui::Text("%s", g_uploading ? "Загрузка выполняется" : "Очередь свободна");
      if (g_uploading) ImGui::ProgressBar(g_upload_percent / 100.f, {-1, 22}, std::format("{}%", g_upload_percent.load()).c_str());
      if (!last_url.empty()) ImGui::TextWrapped("Последняя ссылка: %s", last_url.c_str());
      card_end();
    } else if (g_page == Page::settings) {
      page_title("Настройки", "YouTube, звук и локальный архив");
      static bool youtube{}; static int audio_source{}; static int archive{}; static bool dirty{};
      if (g_reload_settings.exchange(false)) {
        youtube = status.value("youtube_enabled", true);
        audio_source = status.value("microphone_enabled", false) ? 2 : status.value("audio_enabled", true) ? 1 : 0;
        archive = status.value("archive_limit_gb", 20); dirty = false;
      }
      card_begin("##youtube-settings", "YOUTUBE", 88.f);
      dirty |= ImGui::Checkbox("Автоматически ставить запись в очередь", &youtube);
      card_end();
      card_begin("##audio-settings", "ИСТОЧНИК ЗВУКА", 95.f);
      dirty |= ImGui::RadioButton("Без звука", &audio_source, 0); ImGui::SameLine();
      dirty |= ImGui::RadioButton("Звук игры", &audio_source, 1); ImGui::SameLine();
      dirty |= ImGui::RadioButton("Микрофон", &audio_source, 2);
      card_end();
      card_begin("##archive-settings", "ЛОКАЛЬНЫЙ АРХИВ", 90.f);
      ImGui::SetNextItemWidth(-1); dirty |= ImGui::SliderInt("##archive-size", &archive, 1, 100, "%d ГБ");
      card_end();
      if (dirty) {
        if (action_button("Сохранить настройки", {0x40 / 255.f, 0xa0 / 255.f, 0x67 / 255.f, 1.f}, {190, 36})) {
          const auto response = request_worker({{"command", "settings_set"}, {"settings", {{"youtube_enabled", youtube}, {"audio_enabled", audio_source == 1}, {"microphone_enabled", audio_source == 2}, {"archive_limit_gb", archive}}}});
          if (response.value("success", false)) {
            { std::scoped_lock lock(g_state_mutex); g_status["youtube_enabled"] = youtube; g_status["audio_enabled"] = audio_source == 1; g_status["microphone_enabled"] = audio_source == 2; g_status["archive_limit_gb"] = archive; }
            dirty = false; notice("Настройки сохранены");
          } else notice("Не удалось сохранить настройки: " + response.value("error", "worker offline"));
        }
        ImGui::SameLine(); ImGui::TextColored({0xf9 / 255.f, 0xe2 / 255.f, 0xaf / 255.f, 1.f}, "Есть несохранённые изменения");
      } else {
        ImGui::TextDisabled("Изменений нет");
      }
    } else {
      page_title("О программе", "Информация о сборке и используемых компонентах");
      card_begin("##about", "GAMBIT RECORD 1.0.0", 190.f);
      ImGui::TextWrapped("Нативный ASI-плагин записи доказательств для Gambit-RP.");
      ImGui::Spacing();
      ImGui::TextWrapped("Интерфейс, управление курсором и схема SA-MP-событий используют подходы GAdmin (GPLv3), commit c31749c0.");
      ImGui::Spacing(); ImGui::TextWrapped("Распространяется по GNU GPL v3 БЕЗ КАКИХ-ЛИБО ГАРАНТИЙ.");
      ImGui::Spacing(); ImGui::TextDisabled("Команда: /grecord");
      card_end();
    }
    ImGui::EndChild();
  }
  ImGui::End(); ImGui::PopStyleVar();
}

HRESULT WINAPI present_hook(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND override_window, const RGNDATA* dirty) {
  static bool imgui_ready{};
  if (!imgui_ready) {
    D3DDEVICE_CREATION_PARAMETERS params{}; device->GetCreationParameters(&params); g_window = params.hFocusWindow;
    ImGui_ImplWin32_EnableDpiAwareness(); ImGui::CreateContext(); apply_gadmin_style();
    auto& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    g_font_regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 17.f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    g_font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arialbd.ttf", 17.f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    if (g_font_regular) io.FontDefault = g_font_regular;
    ImGui_ImplWin32_Init(g_window); ImGui_ImplDX9_Init(device);
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    imgui_ready = true;
  }
  ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
  bool interactive{}; { std::scoped_lock lock(g_state_mutex); interactive = g_menu_open.load() || g_prompt != Prompt::none; }
  update_cursor(interactive); ImGui::GetIO().MouseDrawCursor = false;
  ImGui::NewFrame();
  render_hud(); render_window(); render_prompt();
  ImGui::EndFrame(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  return g_present(device, source, destination, override_window, dirty);
}
HRESULT WINAPI reset_hook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
  ImGui_ImplDX9_InvalidateDeviceObjects(); const auto result = g_reset(device, params); ImGui_ImplDX9_CreateDeviceObjects(); return result;
}

bool launch_worker() {
  const auto worker = game_directory() / "GambitRecord.exe";
  if (!std::filesystem::exists(worker)) { notice("GambitRecord.exe не найден в папке игры"); return false; }
  const std::wstring args = L"--parent-pid " + std::to_wstring(GetCurrentProcessId()) + L" --pipe \"" + pipe_name() + L"\"";
  SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_NOCLOSEPROCESS; info.lpVerb = L"open";
  info.lpFile = worker.c_str(); info.lpParameters = args.c_str(); info.lpDirectory = game_directory().c_str(); info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) return false; if (info.hProcess) CloseHandle(info.hProcess); return true;
}

void status_loop() {
  while (g_running) {
    auto state = request_worker({{"command", "status"}}, 500);
    if (state.value("success", false)) {
      std::scoped_lock lock(g_state_mutex); g_status = state; g_recording = state.value("recording", false);
      g_uploading = state.value("youtube_upload_in_progress", false); g_upload_percent = state.value("youtube_upload_percent", 0u);
      if (state.value("youtube_last_upload_success", false)) g_last_url = state.value("youtube_last_upload_url", "");
    }
    Sleep(500);
  }
}

DWORD WINAPI initialize(void*) {
  while (g_running && !(g_samp = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"samp.dll")))) Sleep(100);
  if (!g_running) return 0;
  g_version = detect_version(g_samp);
  if (g_version == SampVersion::unknown) { notice("Неподдерживаемая версия SA-MP/open.mp"); return 0; }
  while (g_running && !net_game()) Sleep(100);
  if (!g_running) return 0;
  for (int i = 0; g_running && server_host().empty() && i < 1200; ++i) Sleep(100);
  if (!on_gambit_server()) { notice("Gambit Record работает только на Gambit-RP"); return 0; }
  g_admin = own_name(); g_logic.set_admin(g_admin);
  g_ipc = std::make_unique<grecord::IpcClient>(pipe_name()); launch_worker();
  if (MH_Initialize() != MH_OK) return 0;
  if (MH_CreateHook(reinterpret_cast<void*>(g_samp + send_command_offsets[vi()]), reinterpret_cast<void*>(send_command_hook),
                    reinterpret_cast<void**>(&g_send_command)) != MH_OK ||
      MH_CreateHook(reinterpret_cast<void*>(g_samp + incoming_rpc_offsets[vi()]), reinterpret_cast<void*>(incoming_rpc_hook),
                    reinterpret_cast<void**>(&g_incoming_rpc)) != MH_OK) return 0;
  MH_EnableHook(reinterpret_cast<void*>(g_samp + send_command_offsets[vi()]));
  MH_EnableHook(reinterpret_cast<void*>(g_samp + incoming_rpc_offsets[vi()]));
  IDirect3DDevice9* device{};
  while (g_running && !(device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28))) Sleep(100);
  if (!g_running) return 0;
  void** table = *reinterpret_cast<void***>(device);
  MH_CreateHook(table[17], reinterpret_cast<void*>(present_hook), reinterpret_cast<void**>(&g_present));
  MH_CreateHook(table[16], reinterpret_cast<void*>(reset_hook), reinterpret_cast<void**>(&g_reset));
  MH_EnableHook(table[17]); MH_EnableHook(table[16]);
  g_initialized = true;
  std::thread(status_loop).detach();
  return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module; DisableThreadLibraryCalls(module);
    if (HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr)) CloseHandle(thread);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_running = false;
    if (g_cursor_owned) set_game_cursor(false);
    if (g_initialized) MH_DisableHook(MH_ALL_HOOKS);
  }
  return TRUE;
}
