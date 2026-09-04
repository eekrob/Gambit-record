#include "capture/WindowsGraphicsCapture.hpp"
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <format>

namespace evidence {
using namespace winrt::Windows::Graphics;
bool WindowsGraphicsCaptureBackend::supported() const {
  try { return Capture::GraphicsCaptureSession::IsSupported(); } catch (...) { return false; }
}
void WindowsGraphicsCaptureBackend::start(HWND hwnd, D3DDevice& device, FrameCallback frame, CaptureErrorCallback error) {
  stop(); device_ = &device; callback_ = std::move(frame); error_callback_ = std::move(error);
  auto factory = winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item_)));
  auto size = item_.Size();
  pool_ = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(device.winrt_device(), winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
  session_ = pool_.CreateCaptureSession(item_);
  if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) session_.IsCursorCaptureEnabled(false);
  frame_token_ = pool_.FrameArrived({this, &WindowsGraphicsCaptureBackend::on_frame});
  closed_token_ = item_.Closed([this](auto&&, auto&&){ if (error_callback_) error_callback_("capture item closed"); });
  { std::scoped_lock lock(mutex_); running_ = true; }
  session_.StartCapture();
}
void WindowsGraphicsCaptureBackend::on_frame(const Capture::Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) {
  std::scoped_lock lock(mutex_); if (!running_) return;
  try {
    auto frame = sender.TryGetNextFrame(); if (!frame) return;
    const auto size = frame.ContentSize(); if (size.Width <= 0 || size.Height <= 0) return;
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source; winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc); desc.Width = size.Width; desc.Height = size.Height; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; desc.MiscFlags = 0; desc.CPUAccessFlags = 0; desc.Usage = D3D11_USAGE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> owned; winrt::check_hresult(device_->device()->CreateTexture2D(&desc, nullptr, &owned));
    D3D11_BOX content_box{0, 0, 0, static_cast<UINT>(size.Width), static_cast<UINT>(size.Height), 1};
    { std::scoped_lock d3d_lock(device_->context_mutex()); device_->context()->CopySubresourceRegion(owned.Get(), 0, 0, 0, 0, source.Get(), 0, &content_box); }
    const auto relative = frame.SystemRelativeTime();
    const auto pts = relative.count() != 0 ? relative.count() : fallback_pts_++ * (HnsPerSecond / 60);
    if (callback_) callback_({std::move(owned), static_cast<std::uint32_t>(size.Width), static_cast<std::uint32_t>(size.Height), pts, 0});
    if (size.Width != item_.Size().Width || size.Height != item_.Size().Height) pool_.Recreate(device_->winrt_device(), winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
  } catch (const winrt::hresult_error& e) { if (error_callback_) error_callback_(winrt::to_string(e.message())); }
}
void WindowsGraphicsCaptureBackend::stop() noexcept {
  std::scoped_lock lock(mutex_); running_ = false;
  try { if (pool_) pool_.FrameArrived(frame_token_); if (item_) item_.Closed(closed_token_); if (session_) session_.Close(); if (pool_) pool_.Close(); } catch (...) {}
  session_ = nullptr; pool_ = nullptr; item_ = nullptr; callback_ = {}; error_callback_ = {}; device_ = nullptr;
}
}
