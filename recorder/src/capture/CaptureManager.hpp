#pragma once
#include "capture/ICaptureBackend.hpp"
#include "common/States.hpp"
#include <atomic>
#include <memory>
#include <thread>

namespace evidence {
class CaptureManager {
public:
  CaptureManager(std::wstring process, std::string preferred_backend, D3DDevice& device, FrameCallback callback);
  ~CaptureManager();
  void start(); void stop();
  CaptureState state() const { return state_.load(); }
  HWND hwnd() const { return hwnd_.load(); }
  std::string backend_name() const;
private:
  void monitor(std::stop_token token);
  HWND find_window() const;
  void connect(HWND hwnd);
  std::wstring process_; std::string preferred_; D3DDevice& device_; FrameCallback callback_;
  std::unique_ptr<ICaptureBackend> backend_;
  std::jthread monitor_thread_; std::atomic<CaptureState> state_{CaptureState::WaitingForGame}; std::atomic<HWND> hwnd_{}; std::atomic_bool backend_failed_{};
};
}

