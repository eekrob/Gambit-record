#include "capture/DesktopDuplication.hpp"
#include <dxgi1_2.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace evidence {
void DesktopDuplicationCaptureBackend::start(HWND hwnd, D3DDevice& device, FrameCallback frame, CaptureErrorCallback error) {
  stop(); hwnd_ = hwnd; device_ = &device; callback_ = std::move(frame); error_callback_ = std::move(error); thread_ = std::jthread([this](std::stop_token t){ run(t); });
}
void DesktopDuplicationCaptureBackend::stop() noexcept { if (thread_.joinable()) { thread_.request_stop(); thread_.join(); } callback_ = {}; error_callback_ = {}; device_ = nullptr; hwnd_ = nullptr; }
void DesktopDuplicationCaptureBackend::run(std::stop_token token) {
  try {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device; if (FAILED(device_->device()->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) throw std::runtime_error("DXGI device unavailable");
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter; dxgi_device->GetAdapter(&adapter);
    HMONITOR wanted = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST); Microsoft::WRL::ComPtr<IDXGIOutput1> output1; DXGI_OUTPUT_DESC output_desc{};
    for (UINT i = 0;; ++i) { Microsoft::WRL::ComPtr<IDXGIOutput> output; if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break; output->GetDesc(&output_desc); if (output_desc.Monitor == wanted) { output.As(&output1); break; } }
    if (!output1) throw std::runtime_error("window monitor not found on capture adapter");
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication; HRESULT hr = output1->DuplicateOutput(device_->device(), &duplication); if (FAILED(hr)) throw std::runtime_error("DuplicateOutput failed");
    LARGE_INTEGER freq{}, start{}; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    while (!token.stop_requested()) {
      DXGI_OUTDUPL_FRAME_INFO info{}; Microsoft::WRL::ComPtr<IDXGIResource> resource; hr = duplication->AcquireNextFrame(100, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue; if (hr == DXGI_ERROR_ACCESS_LOST) throw std::runtime_error("desktop duplication access lost"); if (FAILED(hr)) throw std::runtime_error("AcquireNextFrame failed");
      Microsoft::WRL::ComPtr<ID3D11Texture2D> source; resource.As(&source); RECT wr{}; GetWindowRect(hwnd_, &wr);
      RECT bounds = output_desc.DesktopCoordinates; const LONG left = std::clamp(wr.left, bounds.left, bounds.right), top = std::clamp(wr.top, bounds.top, bounds.bottom); const LONG right = std::clamp(wr.right, bounds.left, bounds.right), bottom = std::clamp(wr.bottom, bounds.top, bounds.bottom);
      if (right > left && bottom > top) {
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc); desc.Width = right - left; desc.Height = bottom - top; desc.MipLevels = 1; desc.ArraySize = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> cropped; if (SUCCEEDED(device_->device()->CreateTexture2D(&desc, nullptr, &cropped))) {
          D3D11_BOX box{static_cast<UINT>(left - bounds.left), static_cast<UINT>(top - bounds.top), 0, static_cast<UINT>(right - bounds.left), static_cast<UINT>(bottom - bounds.top), 1};
          { std::scoped_lock lock(device_->context_mutex()); device_->context()->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, source.Get(), 0, &box); }
          LARGE_INTEGER now{}; QueryPerformanceCounter(&now); const auto pts = (now.QuadPart - start.QuadPart) * HnsPerSecond / freq.QuadPart;
          if (callback_) callback_({std::move(cropped), desc.Width, desc.Height, pts, 0});
        }
      }
      duplication->ReleaseFrame();
    }
  } catch (const std::exception& e) { if (!token.stop_requested() && error_callback_) error_callback_(e.what()); }
}
}
