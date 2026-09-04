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
#include <filesystem>
#include <format>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>

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
    case grecord::Action::open_settings: g_menu_open = !g_menu_open.load(); break;
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
  if (word == "/estatus") notice(g_recording ? "REC: включена" : "REC: выключена");
  if (!local) g_send_command(self, raw);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  bool prompt_open; { std::scoped_lock lock(g_state_mutex); prompt_open = g_prompt != Prompt::none; }
  if (g_menu_open || prompt_open) {
    ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
    if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
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
      const auto seconds = g_status.value("recording_seconds", 0ll);
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

void render_prompt() {
  Prompt prompt; { std::scoped_lock lock(g_state_mutex); prompt = g_prompt; }
  if (prompt == Prompt::none) return;
  const char* title = prompt == Prompt::start ? "Gambit Record##start" : prompt == Prompt::finish ? "Gambit Record##finish" : "Gambit Record##missing";
  ImGui::OpenPopup(title);
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (prompt == Prompt::start) {
      ImGui::TextUnformatted("Вы начали слежку без записи.");
      if (ImGui::Button("Начать запись")) { start_recording(); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Не сейчас")) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else if (prompt == Prompt::finish) {
      ImGui::TextUnformatted("Наказание подтверждено сервером.");
      if (ImGui::Button("Завершить и загрузить")) { stop_recording(true); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Завершить локально")) { stop_recording(false); std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Продолжить")) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    } else {
      ImGui::TextWrapped("Наказание подтверждено, но доказательство не записано.");
      if (ImGui::Button("Понятно")) { std::scoped_lock lock(g_state_mutex); g_prompt = Prompt::none; ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
  }
}

void render_window() {
  if (!g_menu_open) return;
  ImGui::SetNextWindowSize({610, 410}, ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Gambit Record", &open)) {
    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Запись")) {
        ImGui::Text("Статус: %s", g_recording ? "идёт запись" : "ожидание");
        if (!g_recording && ImGui::Button("Начать запись")) start_recording();
        if (g_recording && ImGui::Button("Завершить локально")) stop_recording(false);
        ImGui::SameLine(); if (g_recording && ImGui::Button("Завершить и загрузить")) stop_recording(true);
        if (ImGui::Button("Добавить метку")) request_worker({{"command", "marker"}, {"label", "important"}});
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Загрузки")) {
        ImGui::Text("Общий канал: %s", g_status.value("youtube_channel", "Gambit Record").c_str());
        ImGui::Text("Статус: %s", g_uploading ? "загрузка" : "ожидание");
        if (g_uploading) ImGui::ProgressBar(g_upload_percent / 100.f, {-1, 0});
        if (!g_last_url.empty()) ImGui::TextWrapped("Ссылка: %s", g_last_url.c_str());
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Настройки")) {
        bool youtube = g_status.value("youtube_enabled", false); int audio_source = g_status.value("microphone_enabled", false) ? 2 : g_status.value("audio_enabled", true) ? 1 : 0;
        int archive = g_status.value("archive_limit_gb", 20);
        bool changed = ImGui::Checkbox("Загружать на YouTube", &youtube);
        changed |= ImGui::RadioButton("Без звука", &audio_source, 0); ImGui::SameLine();
        changed |= ImGui::RadioButton("Звук игры", &audio_source, 1); ImGui::SameLine();
        changed |= ImGui::RadioButton("Микрофон", &audio_source, 2);
        changed |= ImGui::SliderInt("Локальный архив, ГБ", &archive, 1, 100);
        if (changed) request_worker({{"command", "settings_set"}, {"settings", {{"youtube_enabled", youtube}, {"audio_enabled", audio_source == 1}, {"microphone_enabled", audio_source == 2}, {"archive_limit_gb", archive}}}});
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("О программе")) {
        ImGui::TextUnformatted("Gambit Record 1.0.0");
        ImGui::TextWrapped("Запись доказательств для Gambit-RP. Интерфейс и SA-MP integration используют подходы GAdmin (GPLv3).");
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End(); if (!open) g_menu_open = false;
}

HRESULT WINAPI present_hook(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND override_window, const RGNDATA* dirty) {
  static bool imgui_ready{};
  if (!imgui_ready) {
    D3DDEVICE_CREATION_PARAMETERS params{}; device->GetCreationParameters(&params); g_window = params.hFocusWindow;
    ImGui::CreateContext(); ImGui::StyleColorsDark(); ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 17.f, nullptr, ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
    ImGui_ImplWin32_Init(g_window); ImGui_ImplDX9_Init(device);
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    imgui_ready = true;
  }
  ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
  { std::scoped_lock lock(g_state_mutex); ImGui::GetIO().MouseDrawCursor = g_menu_open.load() || g_prompt != Prompt::none; }
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
  if (MH_CreateHook(reinterpret_cast<void*>(g_samp + send_command_offsets[vi()]), send_command_hook,
                    reinterpret_cast<void**>(&g_send_command)) != MH_OK ||
      MH_CreateHook(reinterpret_cast<void*>(g_samp + incoming_rpc_offsets[vi()]), incoming_rpc_hook,
                    reinterpret_cast<void**>(&g_incoming_rpc)) != MH_OK) return 0;
  MH_EnableHook(reinterpret_cast<void*>(g_samp + send_command_offsets[vi()]));
  MH_EnableHook(reinterpret_cast<void*>(g_samp + incoming_rpc_offsets[vi()]));
  IDirect3DDevice9* device{};
  while (g_running && !(device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28))) Sleep(100);
  if (!g_running) return 0;
  void** table = *reinterpret_cast<void***>(device);
  MH_CreateHook(table[17], present_hook, reinterpret_cast<void**>(&g_present));
  MH_CreateHook(table[16], reset_hook, reinterpret_cast<void**>(&g_reset));
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
    if (g_initialized) MH_DisableHook(MH_ALL_HOOKS);
  }
  return TRUE;
}
