#include "video/D3DDevice.hpp"
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <d3d10.h>
#include <stdexcept>
#include <winrt/base.h>

namespace evidence {
void D3DDevice::create() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1}; D3D_FEATURE_LEVEL level{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_);
#ifdef _DEBUG
  if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) { flags &= ~D3D11_CREATE_DEVICE_DEBUG; hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_); }
#endif
  if (FAILED(hr)) throw std::runtime_error("D3D11CreateDevice failed");
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; if (FAILED(device_.As(&dxgi))) throw std::runtime_error("IDXGIDevice unavailable");
  winrt::com_ptr<::IInspectable> inspectable; winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
  winrt_device_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  if (FAILED(MFCreateDXGIDeviceManager(&reset_token_, &manager_)) || FAILED(manager_->ResetDevice(device_.Get(), reset_token_))) throw std::runtime_error("MF DXGI manager failed");
  Microsoft::WRL::ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
}
void D3DDevice::recreate() { winrt_device_ = nullptr; manager_.Reset(); context_.Reset(); device_.Reset(); create(); }
}
