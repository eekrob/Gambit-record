#include "audio/WasapiCapture.hpp"
#include "logging/Logger.hpp"
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cstring>
#include <stdexcept>

namespace evidence {
namespace {
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr))); }
WAVEFORMATEX pcm_format() { WAVEFORMATEX f{}; f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = 48000; f.wBitsPerSample = 16; f.nBlockAlign = f.nChannels * f.wBitsPerSample / 8; f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign; return f; }
}
WasapiCapture::WasapiCapture() { activate_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); sample_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
WasapiCapture::~WasapiCapture() { stop(); if (sample_event_) CloseHandle(sample_event_); if (activate_event_) CloseHandle(activate_event_); }
STDMETHODIMP WasapiCapture::ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) {
  Microsoft::WRL::ComPtr<IUnknown> unknown; HRESULT inner{}; HRESULT hr = operation->GetActivateResult(&inner, &unknown); activate_result_ = FAILED(hr) ? hr : inner; if (SUCCEEDED(activate_result_)) unknown.As(&client_); SetEvent(activate_event_); return S_OK;
}
void WasapiCapture::activate_process(DWORD process_id) {
  AUDIOCLIENT_ACTIVATION_PARAMS params{}; params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK; params.ProcessLoopbackParams.TargetProcessId = process_id; params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT activation_value{}; activation_value.vt = VT_BLOB; activation_value.blob.cbSize = sizeof(params); activation_value.blob.pBlobData = reinterpret_cast<BYTE*>(&params); Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
  activate_result_ = E_PENDING; check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation_value, this, &operation), "activate process audio");
  if (WaitForSingleObject(activate_event_, 5000) != WAIT_OBJECT_0) throw std::runtime_error("process audio activation timed out"); check(activate_result_, "process audio activation");
}
void WasapiCapture::activate_system() {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator; check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "audio enumerator"); Microsoft::WRL::ComPtr<IMMDevice> endpoint; check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint), "default audio endpoint"); check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "activate system audio");
}
void WasapiCapture::activate_microphone() {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator; check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "audio enumerator");
  Microsoft::WRL::ComPtr<IMMDevice> endpoint; check(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &endpoint), "default microphone endpoint");
  check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "activate microphone");
}
void WasapiCapture::initialize_client() {
  auto format = pcm_format(); DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr), "initialize WASAPI capture");
  check(client_->SetEventHandle(sample_event_), "set WASAPI event"); check(client_->GetService(IID_PPV_ARGS(&capture_)), "get audio capture client");
}
void WasapiCapture::initialize_encoder() {
  check(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder_)), "create AAC encoder");
  Microsoft::WRL::ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "AAC input type"); input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_); input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate_); input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels_ * 2); input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate_ * channels_ * 2); check(encoder_->SetInputType(0, input.Get(), 0), "set AAC PCM input");
  for (DWORD i = 0;; ++i) { Microsoft::WRL::ComPtr<IMFMediaType> candidate; if (encoder_->GetOutputAvailableType(0, i, &candidate) == MF_E_NO_MORE_TYPES) break; UINT32 rate{}, channels{}, bytes{}; candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels); candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes); if (rate == rate_ && channels == channels_ && bytes >= 20000) { aac_type_ = candidate; break; } }
  if (!aac_type_) throw std::runtime_error("AAC 48 kHz stereo output type unavailable"); check(encoder_->SetOutputType(0, aac_type_.Get(), 0), "set AAC output"); encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  UINT32 size{}; if (SUCCEEDED(aac_type_->GetBlobSize(MF_MT_USER_DATA, &size)) && size) { user_data_.resize(size); aac_type_->GetBlob(MF_MT_USER_DATA, user_data_.data(), size, nullptr); }
}
void WasapiCapture::start(DWORD process_id, std::string mode, OutputCallback output) {
  stop(); output_ = std::move(output);
  loopback_ = mode != "microphone";
  if (mode == "process") activate_process(process_id); else if (mode == "microphone") activate_microphone(); else activate_system();
  initialize_client(); initialize_encoder(); check(client_->Start(), "start WASAPI"); running_ = true; thread_ = std::jthread([this](std::stop_token t){ run(t); }); log_info("AUDIO_CAPTURE_STARTED", "mode=" + mode + " rate=48000 channels=2");
}
void WasapiCapture::run(std::stop_token token) {
  while (!token.stop_requested()) {
    if (WaitForSingleObject(sample_event_, 100) != WAIT_OBJECT_0) continue;
    for (;;) { UINT32 frames{}; HRESULT packet_hr = capture_->GetNextPacketSize(&frames); if (FAILED(packet_hr)) { running_ = false; log_warn("AUDIO_DEVICE_CHANGED", "WASAPI packet query failed"); return; } if (!frames) break; BYTE* data{}; DWORD flags{}; UINT64 device_pos{}, qpc{}; HRESULT buffer_hr = capture_->GetBuffer(&data, &frames, &flags, &device_pos, &qpc); if (FAILED(buffer_hr)) { running_ = false; log_warn("AUDIO_DEVICE_CHANGED", "WASAPI buffer failed"); return; } std::vector<BYTE> silence; if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { silence.assign(static_cast<std::size_t>(frames) * channels_ * 2, 0); data = silence.data(); } try { encode_pcm(data, frames, static_cast<std::int64_t>(qpc)); } catch (const std::exception& e) { log_error("AUDIO_ENCODE_FAILED", e.what()); } capture_->ReleaseBuffer(frames); }
  }
}
void WasapiCapture::encode_pcm(const BYTE* data, UINT32 frames, std::int64_t pts) {
  const DWORD length = frames * channels_ * 2; Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(length, &buffer), "PCM buffer"); BYTE* dst{}; buffer->Lock(&dst, nullptr, nullptr); std::memcpy(dst, data, length); buffer->Unlock(); buffer->SetCurrentLength(length);
  Microsoft::WRL::ComPtr<IMFSample> sample; MFCreateSample(&sample); sample->AddBuffer(buffer.Get()); const auto duration = static_cast<std::int64_t>(frames) * HnsPerSecond / rate_; if (!pts) pts = next_pts_; sample->SetSampleTime(pts); sample->SetSampleDuration(duration); next_pts_ = pts + duration;
  HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0); if (hr == MF_E_NOTACCEPTING) { drain_encoder(); hr = encoder_->ProcessInput(0, sample.Get(), 0); } check(hr, "AAC ProcessInput"); drain_encoder();
}
void WasapiCapture::drain_encoder() {
  MFT_OUTPUT_STREAM_INFO info{}; encoder_->GetOutputStreamInfo(0, &info);
  for (;;) { Microsoft::WRL::ComPtr<IMFSample> sample; MFCreateSample(&sample); Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer; MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 16 * 1024), &buffer); sample->AddBuffer(buffer.Get()); MFT_OUTPUT_DATA_BUFFER out{}; out.pSample = sample.Get(); DWORD status{}; HRESULT hr = encoder_->ProcessOutput(0, 1, &out, &status); if (out.pEvents) out.pEvents->Release(); if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break; check(hr, "AAC ProcessOutput"); LONGLONG pts{}, duration{}; sample->GetSampleTime(&pts); sample->GetSampleDuration(&duration); BYTE* data{}; DWORD len{}; buffer->Lock(&data, nullptr, &len); auto bytes = std::make_shared<std::vector<std::uint8_t>>(data, data + len); buffer->Unlock(); if (output_) output_({TrackKind::Audio, pts, duration, true, std::move(bytes)}); }
}
void WasapiCapture::stop() noexcept {
  running_ = false; if (thread_.joinable()) { thread_.request_stop(); SetEvent(sample_event_); thread_.join(); }
  if (client_) client_->Stop(); if (encoder_) { encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); try { drain_encoder(); } catch (...) {} encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); }
  encoder_.Reset(); aac_type_.Reset(); capture_.Reset(); client_.Reset(); output_ = {}; next_pts_ = 0;
}
StreamDescription WasapiCapture::audio_description() const { StreamDescription d; d.has_audio = running_; d.audio_rate = rate_; d.audio_channels = channels_; d.audio_user_data = user_data_; return d; }
}
