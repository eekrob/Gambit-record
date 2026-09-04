#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mutex>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace evidence {
class D3DDevice {
public:
  void create();
  void recreate();
  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  IMFDXGIDeviceManager* mf_manager() const { return manager_.Get(); }
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device() const { return winrt_device_; }
  std::mutex& context_mutex() { return context_mutex_; }
private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
  UINT reset_token_{};
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
  std::mutex context_mutex_;
};
}
