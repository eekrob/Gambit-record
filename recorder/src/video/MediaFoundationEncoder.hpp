#pragma once
#include "common/Media.hpp"
#include "video/D3DDevice.hpp"
#include "video/VideoProcessor.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <wrl/client.h>
#include <mftransform.h>

namespace evidence {
class MediaFoundationEncoder {
public:
  using OutputCallback = std::function<void(EncodedSample)>;
  MediaFoundationEncoder(D3DDevice& device, std::uint32_t fps, std::uint32_t bitrate, std::uint32_t keyframe_seconds, bool prefer_hardware, OutputCallback output);
  ~MediaFoundationEncoder();
  void encode(const VideoFrame& frame);
  void request_keyframe();
  void stop();
  StreamDescription description() const;
  std::string encoder_name() const;
  bool configured() const;
private:
  void configure(std::uint32_t width, std::uint32_t height);
  void create_transform();
  Microsoft::WRL::ComPtr<IMFSample> make_input(ID3D11Texture2D* nv12, std::int64_t pts, std::int64_t duration);
  void drain_output();
  void drain_events();
  void teardown();
  D3DDevice& device_; VideoProcessor converter_; const std::uint32_t fps_, bitrate_, keyframe_seconds_; const bool prefer_hardware_; OutputCallback output_;
  mutable std::mutex mutex_; Microsoft::WRL::ComPtr<IMFTransform> transform_; Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_; Microsoft::WRL::ComPtr<IMFMediaType> output_type_;
  StreamDescription description_; std::string encoder_name_{"unavailable"}; bool async_{}; bool gpu_input_{}; bool configured_{}; bool force_keyframe_{}; std::int64_t last_pts_{};
  bool hardware_{}; bool need_input_{}; bool force_software_{};
};
}
