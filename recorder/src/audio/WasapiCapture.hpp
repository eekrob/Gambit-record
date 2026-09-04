#pragma once
#include "common/Media.hpp"
#include <AudioClient.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <functional>
#include <thread>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <mftransform.h>

namespace evidence {
class WasapiCapture final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
  using OutputCallback = std::function<void(EncodedSample)>;
  WasapiCapture(); ~WasapiCapture();
  void start(DWORD process_id, std::string mode, OutputCallback output);
  void stop() noexcept;
  bool running() const { return running_; }
  StreamDescription audio_description() const;
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override;
private:
  void activate_process(DWORD process_id);
  void activate_system();
  void activate_microphone();
  void initialize_client();
  void initialize_encoder();
  void run(std::stop_token token);
  void encode_pcm(const BYTE* data, UINT32 frames, std::int64_t pts);
  void drain_encoder();
  Microsoft::WRL::ComPtr<IAudioClient> client_; Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
  Microsoft::WRL::ComPtr<IMFTransform> encoder_; Microsoft::WRL::ComPtr<IMFMediaType> aac_type_;
  HANDLE sample_event_{}; HANDLE activate_event_{}; HRESULT activate_result_{E_PENDING}; std::jthread thread_; OutputCallback output_;
  std::atomic_bool running_{}; std::uint32_t rate_{48000}, channels_{2}; std::int64_t next_pts_{}; std::vector<std::uint8_t> user_data_;
  bool loopback_{true};
};
}
