#include "capture/CaptureManager.hpp"
#include "capture/DesktopDuplication.hpp"
#include "capture/WindowsGraphicsCapture.hpp"
#include "logging/Logger.hpp"
#include <TlHelp32.h>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>

namespace evidence {
CaptureManager::CaptureManager(std::wstring process, std::string preferred_backend, D3DDevice& device, FrameCallback callback)
  : process_(std::move(process)), preferred_(std::move(preferred_backend)), device_(device), callback_(std::move(callback)) {}
CaptureManager::~CaptureManager() { stop(); }
void CaptureManager::start() { if (!monitor_thread_.joinable()) monitor_thread_ = std::jthread([this](std::stop_token t){ monitor(t); }); }
void CaptureManager::stop() { if (monitor_thread_.joinable()) { monitor_thread_.request_stop(); monitor_thread_.join(); } if (backend_) backend_->stop(); backend_.reset(); hwnd_ = nullptr; state_ = CaptureState::WaitingForGame; }
std::string CaptureManager::backend_name() const { return backend_ ? backend_->name() : "none"; }

HWND CaptureManager::find_window() const {
  DWORD wanted_pid{}; HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  PROCESSENTRY32W pe{sizeof(pe)}; if (Process32FirstW(snapshot, &pe)) do { if (_wcsicmp(pe.szExeFile, process_.c_str()) == 0) { wanted_pid = pe.th32ProcessID; break; } } while (Process32NextW(snapshot, &pe)); CloseHandle(snapshot); if (!wanted_pid) return nullptr;
  struct Context { DWORD pid; HWND found; } ctx{wanted_pid, nullptr};
  EnumWindows([](HWND hwnd, LPARAM p) -> BOOL { auto* c = reinterpret_cast<Context*>(p); DWORD pid{}; GetWindowThreadProcessId(hwnd, &pid); if (pid == c->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) { c->found = hwnd; return FALSE; } return TRUE; }, reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}
void CaptureManager::connect(HWND hwnd) {
  state_ = hwnd_.load() ? CaptureState::Restarting : CaptureState::Starting; if (backend_) backend_->stop();
  auto wgc = std::make_unique<WindowsGraphicsCaptureBackend>();
  if (preferred_ == "windows_graphics_capture" && wgc->supported()) backend_ = std::move(wgc); else backend_ = std::make_unique<DesktopDuplicationCaptureBackend>();
  backend_failed_ = false;
  try {
    backend_->start(hwnd, device_, callback_, [this](std::string message){ log_warn("CAPTURE_INTERRUPTED", message); backend_failed_ = true; });
    hwnd_ = hwnd; state_ = CaptureState::Running; log_info("GAME_CAPTURE_CONNECTED", std::format("hwnd=0x{:x} backend=\"{}\"", reinterpret_cast<std::uintptr_t>(hwnd), backend_->name()));
  } catch (const std::exception& e) {
    if (backend_->name() == "Windows Graphics Capture") {
      log_warn("WGC_START_FAILED", e.what()); backend_ = std::make_unique<DesktopDuplicationCaptureBackend>();
      backend_->start(hwnd, device_, callback_, [this](std::string message){ log_warn("CAPTURE_INTERRUPTED", message); backend_failed_ = true; }); hwnd_ = hwnd; state_ = CaptureState::Running;
    } else { state_ = CaptureState::Error; throw; }
  }
}
void CaptureManager::monitor(std::stop_token token) {
  using namespace std::chrono_literals;
  while (!token.stop_requested()) {
    try {
      HWND found = find_window();
      if (!found) { if (backend_) { backend_->stop(); backend_.reset(); } hwnd_ = nullptr; state_ = CaptureState::WaitingForGame; }
      else if (found != hwnd_.load() || backend_failed_.exchange(false) || !IsWindow(found)) connect(found);
    } catch (const std::exception& e) { state_ = CaptureState::Error; log_error("CAPTURE_CONNECT_FAILED", e.what()); }
    for (int i = 0; i < 10 && !token.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
  }
}
}
