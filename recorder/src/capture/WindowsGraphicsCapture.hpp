#pragma once
#include "capture/ICaptureBackend.hpp"
#include <mutex>
#include <winrt/Windows.Graphics.Capture.h>

namespace evidence {
class WindowsGraphicsCaptureBackend final : public ICaptureBackend {
public:
  bool supported() const override;
  void start(HWND hwnd, D3DDevice& device, FrameCallback frame, CaptureErrorCallback error) override;
  void stop() noexcept override;
  std::string name() const override { return "Windows Graphics Capture"; }
private:
  void on_frame(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&);
  D3DDevice* device_{}; FrameCallback callback_; CaptureErrorCallback error_callback_;
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::event_token frame_token_{}; winrt::event_token closed_token_{};
  std::mutex mutex_; bool running_{}; std::int64_t fallback_pts_{};
};
}

