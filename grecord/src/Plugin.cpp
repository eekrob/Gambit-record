// Gambit Record - ASI integration for GTA:SA / SA-MP.
// Portions of the version/address strategy are derived from GAdmin at
// c31749c02f3d76c1ab0f8ebf562c8dae0dc91152 (GPL-3.0-only).

#include "grecord/BitReader.hpp"
#include "grecord/IpcClient.hpp"
#include "grecord/Logic.hpp"
#include "grecord/UiStyle.hpp"
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
#include <deque>
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
std::atomic_bool g_worker_online{};
std::atomic_bool g_capture_ready{};
std::atomic_bool g_start_pending{};
std::atomic_bool g_start_requesting{};
std::atomic<unsigned> g_upload_percent{};
std::atomic_bool g_uploading{};
std::mutex g_state_mutex;
std::mutex g_ipc_mutex;
std::mutex g_server_command_mutex;
std::deque<std::string> g_server_commands;
std::atomic<std::uintptr_t> g_command_sender{};
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
std::atomic_bool g_dismiss_prompt{};
enum class Page { recording, uploads, settings, about };
Page g_page{Page::recording};
ImFont* g_font_regular{};
ImFont* g_font_bold{};
bool g_cursor_owned{};
grecord::ui::Settings g_ui;
grecord::ui::Theme g_theme, g_external_theme;
grecord::ui::ThemeState g_theme_state{grecord::ui::ThemeState::missing};
grecord::ui::Fonts g_fonts;
std::mutex g_theme_mutex;
bool g_hud_dragging{};
bool g_sidebar_expanded{};
float g_sidebar_width{};
float g_window_alpha{};
float g_page_alpha{1.f};
Page g_next_page{Page::recording};
double g_page_transition{-1};

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

void save_ui() {
  std::string error;
  if (!grecord::ui::save_settings(game_directory() / "grecord" / "ui.json", g_ui, error))
    notice("Не удалось сохранить настройки интерфейса: " + error);
}

void copy_link(const std::string& value) {
  const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!count || !OpenClipboard(g_window)) { notice("Не удалось открыть буфер обмена"); return; }
  auto memory = GlobalAlloc(GMEM_MOVEABLE, (count + 1) * sizeof(wchar_t));
  auto* text = memory ? static_cast<wchar_t*>(GlobalLock(memory)) : nullptr;
  bool copied = false;
  if (text) {
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text, count);
    text[count] = 0; GlobalUnlock(memory);
    copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  }
  if (!copied && memory) GlobalFree(memory);
  CloseClipboard(); notice(copied ? "Ссылка скопирована" : "Не удалось скопировать ссылку");
}

void open_recordings(const json& status) {
  const auto value = status.value("recording_directory", "");
  const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!count) { notice("Папка записей пока недоступна: дождитесь подключения recorder"); return; }
  std::wstring path(count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), path.data(), count);
  std::error_code error;
  if (!std::filesystem::path(path).is_absolute() || !std::filesystem::is_directory(path, error)) {
    notice("Папка записей пока не создана или недоступна"); return;
  }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  notice(result > 32 ? "Папка записей открыта" : "Не удалось открыть папку записей");
}
void queue_upload_announcement(const json& state) {
  auto target=state.value("youtube_last_upload_target_name","");
  const auto target_id=state.value("youtube_last_upload_target_id",-1);
  if(target.empty()&&target_id>=0)target="ID "+std::to_string(target_id);
  SYSTEMTIME now{};GetLocalTime(&now);
  auto command=grecord::Logic::upload_announcement(target,now.wDay,now.wMonth,now.wYear,now.wHour,now.wMinute);
  std::scoped_lock lock(g_server_command_mutex);g_server_commands.push_back(grecord::utf8_to_cp1251(command));
}
void dispatch_server_command() {
  const auto sender=g_command_sender.load();if(!sender||!g_send_command)return;
  std::string command;
  {std::scoped_lock lock(g_server_command_mutex);if(g_server_commands.empty())return;command=std::move(g_server_commands.front());g_server_commands.pop_front();}
  g_send_command(reinterpret_cast<void*>(sender),command.c_str());
}
std::string friendly_error(const std::string& error) {
  if (error == "WORKER_OFFLINE") return "модуль записи запускается";
  if (error == "CAPTURE_NOT_READY") return "захват игры ещё запускается";
  if (error == "YOUTUBE_DISABLED") return "загрузка на YouTube отключена";
  if (error == "YOUTUBE_NOT_CONFIGURED") return "эта сборка не настроена для YouTube";
  return error.empty() ? "неизвестная ошибка" : error;
}
void start_recording_now() {
  if (g_start_requesting.exchange(true)) return;
  auto response = request_worker({{"command", "record_start"}, {"metadata", metadata()}});
  g_start_requesting = false;
  if (response.value("success", false)) { g_start_pending = false; g_record_started = std::chrono::system_clock::now(); g_recording = true; notice("Запись начата"); }
  else {
    const auto error=response.value("error", "unknown");
    if(error=="WORKER_OFFLINE"){g_worker_online=false;g_start_pending=true;}
    else if(error=="CAPTURE_NOT_READY")g_start_pending=true;
    else g_start_pending=false;
    notice("Не удалось начать запись: " + friendly_error(error));
  }
}
void start_recording() {
  if (!g_worker_online || !g_capture_ready) { g_start_pending = true; notice("Запись начнётся автоматически, когда захват игры будет готов"); return; }
  start_recording_now();
}
void stop_recording(bool upload) {
  auto response = request_worker({{"command", "record_stop"}, {"upload", upload}, {"metadata", metadata()}}, 5000);
  if (response.value("success", false)) {
    g_recording = false;
    if (upload && response.value("upload_queued", false)) notice("Запись завершена, загрузка поставлена в очередь");
    else if (upload) notice("Запись сохранена локально: " + friendly_error(response.value("upload_error", "YOUTUBE_NOT_CONFIGURED")));
    else notice("Запись сохранена локально");
  }
  else notice("Не удалось завершить запись: " + friendly_error(response.value("error", "unknown")));
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
  g_command_sender=reinterpret_cast<std::uintptr_t>(self);
  const std::string command = raw ? grecord::cp1251_to_utf8(raw) : std::string{};
  const auto first_space = command.find(' '); const auto word = command.substr(0, first_space);
  const bool local = word == "/grecord" || word == "/estart" || word == "/estop" || word == "/estatus" || word == "/esettings";
  grecord::Action action;
  { std::scoped_lock lock(g_state_mutex); action = g_logic.on_command(command, g_recording.load()); }
  apply_action(action);
  if (word == "/esettings") { g_page = Page::settings; g_page_transition = -1; g_page_alpha = 1; }
  if (word == "/estatus") notice(g_recording ? "REC: включена" : "REC: выключена");
  if (!local) g_send_command(self, raw);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  bool prompt_open; { std::scoped_lock lock(g_state_mutex); prompt_open = g_prompt != Prompt::none; }
  const bool interactive = g_menu_open || prompt_open;
  if (interactive) {
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
      std::scoped_lock lock(g_state_mutex);
      if (g_prompt != Prompt::none) g_dismiss_prompt = true;
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
  json status; std::string target, message, url; bool prompt_open;
  {
    std::scoped_lock lock(g_state_mutex); status = g_status; target = g_logic.target_name();
    prompt_open = g_prompt != Prompt::none; url = g_last_url;
    if (std::chrono::steady_clock::now() < g_notice_until) message = g_notice;
  }
  const bool editable = g_menu_open && !prompt_open;
  if (g_hud_dragging && !editable) { g_hud_dragging = false; save_ui(); }
  if (!g_recording && !g_uploading && message.empty() && url.empty() && !g_menu_open) return;
  const auto display = ImGui::GetIO().DisplaySize;
  static ImVec2 size{250, 45};
  static ImVec2 drag_offset{};
  auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (!editable) flags |= ImGuiWindowFlags_NoInputs;
  const auto position = grecord::ui::hud_position(g_ui, {size.x, size.y}, {display.x, display.y});
  ImGui::SetNextWindowPos({position.x, position.y}, ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints({0, 0}, {std::max(1.f, display.x), std::max(1.f, display.y)});
  ImGui::SetNextWindowBgAlpha(g_ui.opacity);
  ImGui::PushFont(g_fonts.regular, 18.f * g_ui.scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f * g_ui.scale, 8.f * g_ui.scale});
  if (ImGui::Begin("##grecord-hud", nullptr, flags)) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(1.f, std::min(500.f * g_ui.scale, display.x - 32.f * g_ui.scale)));
    if (g_recording) {
      const auto seconds = status.value("recording_seconds", 0ll);
      ImGui::TextColored(grecord::ui::rgba(g_theme.red), "REC %02lld:%02lld", seconds / 60, seconds % 60);
      if (!target.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(("· " + target).c_str()); }
    }
    if (g_uploading) ImGui::TextColored(grecord::ui::rgba(g_theme.yellow), "UPLOAD %u%%", g_upload_percent.load());
    if (!message.empty()) ImGui::TextUnformatted(message.c_str());
    if (!url.empty()) ImGui::TextColored(grecord::ui::rgba(g_theme.green), "%s", url.c_str());
    if (g_menu_open && !g_recording && !g_uploading) ImGui::TextUnformatted("Gambit Record · ожидание записи");
    if (editable) ImGui::TextDisabled("Потяните мышью, чтобы переместить");
    ImGui::PopTextWrapPos();
    size = ImGui::GetWindowSize();
    auto current = grecord::ui::hud_position(g_ui, {size.x, size.y}, {display.x, display.y});
    if (editable && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const auto mouse = ImGui::GetMousePos();
      drag_offset = {mouse.x - current.x, mouse.y - current.y}; g_hud_dragging = true;
    }
    if (g_hud_dragging) {
      if (editable && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const auto mouse = ImGui::GetMousePos();
        current = grecord::ui::clamp_position({mouse.x - drag_offset.x, mouse.y - drag_offset.y}, {size.x, size.y}, {display.x, display.y});
        g_ui.custom_position = true;
        g_ui.anchor = grecord::ui::hud_anchor(current, {size.x, size.y}, {display.x, display.y});
      } else { g_hud_dragging = false; save_ui(); }
    }
    ImGui::SetWindowPos({current.x, current.y});
  }
  ImGui::End(); ImGui::PopStyleVar(); ImGui::PopFont();
}

bool action_button(const char* label, const ImVec4& color, ImVec2 size = {0, 0}) {
  auto brighten = [](ImVec4 value, float amount) {
    value.x = std::min(1.f, value.x + amount); value.y = std::min(1.f, value.y + amount); value.z = std::min(1.f, value.z + amount); return value;
  };
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(color, .08f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(color, .15f));
  const float luminance = color.x * .2126f + color.y * .7152f + color.z * .0722f;
  ImGui::PushStyleColor(ImGuiCol_Text, luminance > .5f ? ImVec4{0, 0, 0, 1} : ImVec4{1, 1, 1, 1});
  const bool pressed = grecord::ui::button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void next_button(float width) {
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  if (right - ImGui::GetItemRectMax().x >= width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
}

void card_begin(const char* id, const char* title, float height) {
  (void)height;
  ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AlwaysUseWindowPadding,
    ImGuiWindowFlags_NoBackground | ((!g_menu_open || g_sidebar_expanded) ? ImGuiWindowFlags_NoInputs : 0));
  if (g_font_bold) ImGui::PushFont(g_font_bold, 24.f);
  ImGui::TextUnformatted(title);
  if (g_font_bold) ImGui::PopFont();
  ImGui::Spacing();
}
void card_end() { ImGui::EndChild(); }

bool page_button(const char* label, const char* icon, Page page, float icon_width, float height) {
  const bool selected = g_page == page;
  const auto start = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(label, {g_sidebar_width, height});
  auto* storage = ImGui::GetStateStorage(); const auto id = ImGui::GetID(label);
  float blend = storage->GetFloat(id, selected ? 1.f : 0.f);
  blend += ((selected ? 1.f : ImGui::IsItemHovered() ? .5f : 0.f) - blend) * std::min(1.f, ImGui::GetIO().DeltaTime / .2f);
  storage->SetFloat(id, blend);
  const auto base = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
  const auto active = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive);
  const ImVec4 color{base.x + (active.x - base.x) * blend, base.y + (active.y - base.y) * blend,
    base.z + (active.z - base.z) * blend, base.w + (active.w - base.w) * blend};
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(start, {start.x + g_sidebar_width, start.y + height}, ImGui::GetColorU32(color));
  ImGui::PushFont(g_fonts.icons, 24.f);
  auto text_size = ImGui::CalcTextSize(icon);
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {start.x + (icon_width - text_size.x) / 2, start.y + (height - text_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_Text), icon);
  ImGui::PopFont();
  ImGui::PushFont(g_fonts.bold, 18.f); text_size = ImGui::CalcTextSize(label);
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {start.x + icon_width + 5, start.y + (height - text_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_Text), label);
  ImGui::PopFont();
  if (!g_sidebar_expanded && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
  if (pressed && !selected) { g_next_page = page; g_page_transition = ImGui::GetTime(); }
  return pressed;
}

void page_title(const char* title, const char* description) {
  if (g_font_bold) ImGui::PushFont(g_font_bold, 24.f);
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
  const auto display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowSize({std::min(prompt == Prompt::finish ? 620.f : 430.f, display.x - 16), 0}, ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints({0, 0}, {display.x - 16, display.y - 16});
  ImGui::SetNextWindowPos({display.x * .5f, display.y * .5f}, ImGuiCond_Always, {.5f, .5f});
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
    if (g_dismiss_prompt.exchange(false)) {
      { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; }
      ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return;
    }
    ImGui::PushTextWrapPos(0);
    if (g_font_bold) ImGui::PushFont(g_font_bold, 24.f);
    ImGui::TextColored(grecord::ui::rgba(g_theme.text[0]), "GAMBIT RECORD");
    if (g_font_bold) ImGui::PopFont();
    ImGui::Separator(); ImGui::Spacing();
    if (prompt == Prompt::start) {
      ImGui::TextUnformatted("Вы начали слежку без записи.");
      ImGui::TextDisabled("Запись поможет сохранить доказательства до выдачи наказания."); ImGui::Spacing();
      if (action_button("Начать запись", grecord::ui::rgba(g_theme.green), {180, 36})) { start_recording(); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      next_button(150); if (grecord::ui::button("Не сейчас", {150, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else if (prompt == Prompt::finish) {
      ImGui::TextUnformatted("Наказание подтверждено сервером.");
      ImGui::TextDisabled("Можно сразу поставить ролик в очередь YouTube или оставить файл локально."); ImGui::Spacing();
      if (action_button("Завершить и загрузить", grecord::ui::rgba(g_theme.green), {205, 36})) { stop_recording(true); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      next_button(190); if (grecord::ui::button("Завершить локально", {190, 36})) { stop_recording(false); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      next_button(135); if (grecord::ui::button("Продолжить", {135, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else {
      ImGui::TextWrapped("Наказание подтверждено, но доказательство не записано.");
      ImGui::Spacing(); if (grecord::ui::button("Понятно", {140, 36})) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    }
    ImGui::PopTextWrapPos(); ImGui::EndPopup();
  }
}

void render_window() {
  const float delta = ImGui::GetIO().DeltaTime;
  g_window_alpha = std::clamp(g_window_alpha + (g_menu_open ? delta : -delta) / .5f, 0.f, 1.f);
  if (g_window_alpha <= 0) return;
  json status; std::string last_url;
  { std::scoped_lock lock(g_state_mutex); status = g_status; last_url = g_last_url; }
  if (g_page_transition >= 0) {
    const auto elapsed = static_cast<float>(ImGui::GetTime() - g_page_transition);
    if (elapsed < .15f) g_page_alpha = 1.f - elapsed / .15f;
    else { g_page = g_next_page; g_page_alpha = std::min(1.f, (elapsed - .15f) / .3f); }
    if (elapsed >= .45f) g_page_transition = -1;
  }
  const float frame = ImGui::GetFrameHeight();
  const auto display = ImGui::GetIO().DisplaySize;
  const ImVec2 window_size{std::min(29.f * frame, std::max(1.f, display.x - 16)), std::min(18.f * frame, std::max(1.f, display.y - 16))};
  const float icon_width = 29.f * frame * .0629f;
  const float expanded_width = 29.f * frame * .2859f;
  if (!g_sidebar_width) g_sidebar_width = icon_width;
  const float target_width = g_sidebar_expanded ? expanded_width : icon_width;
  const float width_step = (expanded_width - icon_width) * delta / .5f;
  g_sidebar_width += std::clamp(target_width - g_sidebar_width, -width_step, width_step);
  ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
  ImGui::SetNextWindowPos({display.x * .5f, display.y * .5f}, ImGuiCond_FirstUseEver, {.5f, .5f});
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_window_alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!g_menu_open) flags |= ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("Gambit Record##main", nullptr, flags)) {
    const auto pos = ImGui::GetWindowPos();
    const auto clamped = grecord::ui::clamp_position({pos.x, pos.y}, {window_size.x, window_size.y}, {display.x, display.y});
    ImGui::SetWindowPos({clamped.x, clamped.y});
    ImGui::SetCursorPos({icon_width, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_window_alpha * g_page_alpha * (g_sidebar_expanded ? 100.f / 255.f : 1.f));
    ImGui::BeginChild("##content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
      ImGuiWindowFlags_NoBackground | ((g_sidebar_expanded || !g_menu_open) ? ImGuiWindowFlags_NoInputs : 0));
    ImGui::PushTextWrapPos(0);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 42.f);
    if (grecord::ui::button("X", {30, 28})) g_menu_open = false;
    ImGui::SetCursorPosY(12.f);

    if (g_page == Page::recording) {
      page_title("Запись", "Управление доказательством и важными моментами");
      card_begin("##record-state", "ТЕКУЩЕЕ СОСТОЯНИЕ", 112.f);
      const bool ready=g_worker_online.load()&&g_capture_ready.load();
      ImGui::TextColored(g_recording ? grecord::ui::rgba(g_theme.red) : ready ? grecord::ui::rgba(g_theme.green) : grecord::ui::rgba(g_theme.yellow),
                         "%s", g_recording ? "ИДЁТ ЗАПИСЬ" : ready ? "ГОТОВ К ЗАПИСИ" : "ПОДГОТОВКА ЗАХВАТА");
      std::string target; { std::scoped_lock lock(g_state_mutex); target = g_logic.target_name(); }
      ImGui::TextDisabled("Наблюдение: %s", target.empty() ? "не выбрано" : target.c_str());
      card_end();
      card_begin("##record-actions", "ДЕЙСТВИЯ", 150.f);
      if (!g_recording) {
        if (action_button("Начать запись", grecord::ui::rgba(g_theme.green), {180, 38})) start_recording();
      } else {
        if (action_button("Завершить и загрузить", grecord::ui::rgba(g_theme.green), {205, 38})) stop_recording(true);
        next_button(180); if (grecord::ui::button("Сохранить локально", {180, 38})) stop_recording(false);
      }
      if (grecord::ui::button("Добавить важную метку", {205, 36})) request_worker({{"command", "marker"}, {"label", "important"}});
      if (grecord::ui::button("Открыть папку записей", {205, 36})) open_recordings(status);
      card_end();
    } else if (g_page == Page::uploads) {
      page_title("Загрузки", "Очередь публикации на общем YouTube-канале");
      card_begin("##upload-channel", "КАНАЛ", 92.f);
      ImGui::Text("%s", status.value("youtube_channel", "Gambit Record").c_str());
      ImGui::TextDisabled("Приватность всех публикаций: доступ по ссылке");
      card_end();
      card_begin("##upload-state", "СОСТОЯНИЕ ЗАГРУЗКИ", 142.f);
      const auto phase=status.value("youtube_upload_phase",""); const auto pending=status.value("youtube_upload_pending",0u);
      ImGui::Text("%s", g_uploading ? phase=="preparing" ? "Подготовка файла" : "Загрузка выполняется" : pending ? "Ожидание повторной попытки" : "Очередь свободна");
      if (g_uploading) ImGui::ProgressBar(g_upload_percent / 100.f, {-1, 22}, std::format("{}%", g_upload_percent.load()).c_str());
      if (pending) ImGui::TextDisabled("В очереди: %u",pending);
      const auto upload_error=status.value("youtube_last_upload_error","");
      if (!upload_error.empty()) ImGui::TextWrapped("Последняя ошибка: %s",upload_error.c_str());
      if (!last_url.empty()) ImGui::TextWrapped("Последняя ссылка: %s", last_url.c_str());
      ImGui::BeginDisabled(last_url.empty());
      if (grecord::ui::button("Копировать ссылку")) copy_link(last_url);
      ImGui::EndDisabled();
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
      dirty |= grecord::ui::toggle("Автоматически ставить запись в очередь", &youtube);
      card_end();
      card_begin("##audio-settings", "ИСТОЧНИК ЗВУКА", 95.f);
      dirty |= ImGui::RadioButton("Без звука", &audio_source, 0); next_button(120);
      dirty |= ImGui::RadioButton("Звук игры", &audio_source, 1); next_button(120);
      dirty |= ImGui::RadioButton("Микрофон", &audio_source, 2);
      card_end();
      card_begin("##archive-settings", "ЛОКАЛЬНЫЙ АРХИВ", 90.f);
      ImGui::SetNextItemWidth(-1); dirty |= ImGui::SliderInt("##archive-size", &archive, 1, 100, "%d ГБ");
      card_end();
      if (dirty) {
        if (action_button("Сохранить настройки", grecord::ui::rgba(g_theme.green), {190, 36})) {
          const auto response = request_worker({{"command", "settings_set"}, {"settings", {{"youtube_enabled", youtube}, {"audio_enabled", audio_source == 1}, {"microphone_enabled", audio_source == 2}, {"archive_limit_gb", archive}}}});
          if (response.value("success", false)) {
            { std::scoped_lock lock(g_state_mutex); g_status["youtube_enabled"] = youtube; g_status["audio_enabled"] = audio_source == 1; g_status["microphone_enabled"] = audio_source == 2; g_status["archive_limit_gb"] = archive; }
            dirty = false; notice("Настройки сохранены");
          } else notice("Не удалось сохранить настройки: " + response.value("error", "worker offline"));
        }
        ImGui::TextColored(grecord::ui::rgba(g_theme.yellow), "Есть несохранённые изменения");
      } else {
        ImGui::TextDisabled("Изменений нет");
      }
      card_begin("##interface-settings", "ИНТЕРФЕЙС И СТАТУС-БАР", 0);
      if (grecord::ui::toggle("Использовать тему GAdmin", &g_ui.follow_gadmin)) save_ui();
      grecord::ui::ThemeState theme_state;
      { std::scoped_lock lock(g_theme_mutex); theme_state = g_theme_state; }
      ImGui::TextDisabled("%s", !g_ui.follow_gadmin ? "Встроенная тема GAdmin" :
        theme_state == grecord::ui::ThemeState::loaded ? "Тема GAdmin подключена" :
        theme_state == grecord::ui::ThemeState::missing ? "Конфигурация GAdmin не найдена — встроенная тема" :
        "Не удалось прочитать тему — сохранены последние корректные цвета");
      if (g_ui.follow_gadmin) ImGui::TextDisabled("Изменения появятся после сохранения темы GAdmin (до 5 минут).");
      ImGui::TextUnformatted("Масштаб статус-бара");
      int scale = static_cast<int>(g_ui.scale * 100 + .5f);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::SliderInt("##hud-scale", &scale, 75, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp)) g_ui.scale = scale / 100.f;
      if (ImGui::IsItemDeactivatedAfterEdit()) save_ui();
      ImGui::TextUnformatted("Прозрачность фона (100% — непрозрачный)");
      int opacity = static_cast<int>(g_ui.opacity * 100 + .5f);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::SliderInt("##hud-opacity", &opacity, 20, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) g_ui.opacity = opacity / 100.f;
      if (ImGui::IsItemDeactivatedAfterEdit()) save_ui();
      if (grecord::ui::button("Вернуть вниз по центру")) { g_ui.custom_position = false; g_hud_dragging = false; save_ui(); }
      ImGui::TextDisabled("При открытом /grecord потяните статус-бар левой кнопкой мыши.");
      card_end();
    } else {
      page_title("О программе", "Информация о сборке и используемых компонентах");
      card_begin("##about", "GAMBIT RECORD 0.1.5", 190.f);
      ImGui::TextWrapped("Нативный ASI-плагин записи доказательств для Gambit-RP.");
      ImGui::Spacing();
      ImGui::TextWrapped("Интерфейс, управление курсором и схема SA-MP-событий используют подходы GAdmin (GPLv3), commit c31749c0.");
      ImGui::Spacing(); ImGui::TextWrapped("Распространяется по GNU GPL v3 БЕЗ КАКИХ-ЛИБО ГАРАНТИЙ.");
      ImGui::Spacing(); ImGui::TextDisabled("Команда: /grecord");
      card_end();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild(); ImGui::PopStyleVar(2);
    ImGui::SetCursorPos({0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
    ImGui::BeginChild("##sidebar", {g_sidebar_width, window_size.y}, ImGuiChildFlags_None,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | (!g_menu_open ? ImGuiWindowFlags_NoInputs : 0));
    const auto origin = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##expand-menu", {g_sidebar_width, icon_width})) g_sidebar_expanded = !g_sidebar_expanded;
    auto* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) draw->AddRectFilled(origin, {origin.x + g_sidebar_width, origin.y + icon_width}, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 8);
    ImGui::PushFont(g_fonts.icons, 24.f);
    const auto icon_size = ImGui::CalcTextSize("\uE9E0");
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {origin.x + (icon_width - icon_size.x) / 2, origin.y + (icon_width - icon_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_Text), "\uE9E0");
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.bold, 24.f);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {origin.x + icon_width + 5, origin.y + 6}, ImGui::GetColorU32(ImGuiCol_Text), "Gambit Record");
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.light, 18.f);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {origin.x + icon_width + 5, origin.y + 32}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "v0.1.5");
    ImGui::PopFont();
    ImGui::SetCursorPos({0, icon_width + 5});
    page_button("Запись", "\uE956", Page::recording, icon_width, window_size.y * .066f);
    page_button("Загрузки", "\uE996", Page::uploads, icon_width, window_size.y * .066f);
    page_button("Настройки", "\uEA55", Page::settings, icon_width, window_size.y * .066f);
    page_button("О программе", "\uE9F7", Page::about, icon_width, window_size.y * .066f);
    ImGui::EndChild(); ImGui::PopStyleVar(2);
  }
  ImGui::End(); ImGui::PopStyleVar(2);
}

HRESULT WINAPI present_hook(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND override_window, const RGNDATA* dirty) {
  dispatch_server_command();
  static bool imgui_ready{};
  if (!imgui_ready) {
    D3DDEVICE_CREATION_PARAMETERS params{}; device->GetCreationParameters(&params); g_window = params.hFocusWindow;
    ImGui_ImplWin32_EnableDpiAwareness(); ImGui::CreateContext(); grecord::ui::apply_style(g_theme);
    auto& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;
    g_fonts = grecord::ui::load_fonts(g_module);
    g_font_regular = g_fonts.regular; g_font_bold = g_fonts.bold;
    std::string ui_error; g_ui = grecord::ui::load_settings(game_directory() / "grecord" / "ui.json", ui_error);
    if (!ui_error.empty()) notice("Настройки интерфейса не прочитаны; применены стандартные");
    ImGui_ImplWin32_Init(g_window); ImGui_ImplDX9_Init(device);
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    imgui_ready = true;
  }
  grecord::ui::Theme next_theme;
  { std::scoped_lock lock(g_theme_mutex); if (g_ui.follow_gadmin) next_theme = g_external_theme; }
  if (!(next_theme == g_theme)) { g_theme = next_theme; grecord::ui::apply_style(g_theme); }
  ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
  bool interactive{}; { std::scoped_lock lock(g_state_mutex); interactive = g_menu_open.load() || g_prompt != Prompt::none; }
  update_cursor(interactive); ImGui::GetIO().MouseDrawCursor = false;
  ImGui::NewFrame();
  render_window(); render_hud(); render_prompt();
  ImGui::EndFrame(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  return g_present(device, source, destination, override_window, dirty);
}
HRESULT WINAPI reset_hook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
  ImGui_ImplDX9_InvalidateDeviceObjects(); const auto result = g_reset(device, params); ImGui_ImplDX9_CreateDeviceObjects(); return result;
}

bool launch_worker() {
  const auto worker = game_directory() / "GambitRecord.exe";
  if (!std::filesystem::exists(worker)) { notice("GambitRecord.exe не найден в папке игры"); return false; }
  std::wstring command=L"\""+worker.wstring()+L"\" --parent-pid "+std::to_wstring(GetCurrentProcessId())+L" --pipe \""+pipe_name()+L"\"";
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{}; const auto directory=game_directory().wstring();
  if(!CreateProcessW(worker.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process))return false;
  CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
}

void status_loop() {
  unsigned offline_polls{};
  std::string announced_upload_url;
  while (g_running) {
    auto state = request_worker({{"command", "status"}}, 500);
    if (state.value("success", false)) {
      offline_polls=0;g_worker_online=true;g_capture_ready=state.value("capture",false);
      { std::scoped_lock lock(g_state_mutex); g_status = state; g_recording = state.value("recording", false);
        g_uploading = state.value("youtube_upload_in_progress", false); g_upload_percent = state.value("youtube_upload_percent", 0u);
        if (state.value("youtube_last_upload_success", false)) g_last_url = state.value("youtube_last_upload_url", ""); }
      const auto uploaded_url=state.value("youtube_last_upload_success",false)?state.value("youtube_last_upload_url",""):"";
      if(!uploaded_url.empty()&&uploaded_url!=announced_upload_url){queue_upload_announcement(state);announced_upload_url=uploaded_url;}
      if(g_start_pending&&g_capture_ready&&!g_recording)start_recording_now();
    } else {
      g_worker_online=false;g_capture_ready=false;
      if(++offline_polls>=4){offline_polls=0;launch_worker();}
    }
    Sleep(500);
  }
}

void theme_loop() {
  grecord::ui::ThemeReader reader;
  const auto path = game_directory() / "gadmin" / "configuration" / "main.mpk";
  while (g_running) {
    reader.poll(path);
    { std::scoped_lock lock(g_theme_mutex); g_external_theme = reader.theme(); g_theme_state = reader.state(); }
    Sleep(1000);
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
  std::thread(theme_loop).detach();
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
