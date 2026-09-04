#pragma once
#include "video/D3DDevice.hpp"
#include <cstdint>
#include <wrl/client.h>

namespace evidence {
class VideoProcessor {
public:
  explicit VideoProcessor(D3DDevice& device) : device_(device) {}
  Microsoft::WRL::ComPtr<ID3D11Texture2D> convert_bgra_to_nv12(ID3D11Texture2D* source, std::uint32_t width, std::uint32_t height);
  void reset();
private:
  void ensure(std::uint32_t width, std::uint32_t height);
  D3DDevice& device_; std::uint32_t width_{}; std::uint32_t height_{};
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_; Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_; Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
};
}

