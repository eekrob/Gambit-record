#pragma once
#include "capture/ICaptureBackend.hpp"
#include <atomic>
#include <thread>

namespace evidence {
class DesktopDuplicationCaptureBackend final : public ICaptureBackend {
public:
  bool supported() const override { return true; }
  void start(HWND hwnd, D3DDevice& device, FrameCallback frame, CaptureErrorCallback error) override;
  void stop() noexcept override;
  std::string name() const override { return "Desktop Duplication (window crop)"; }
private:
  void run(std::stop_token token);
  HWND hwnd_{}; D3DDevice* device_{}; FrameCallback callback_; CaptureErrorCallback error_callback_; std::jthread thread_;
};
}

