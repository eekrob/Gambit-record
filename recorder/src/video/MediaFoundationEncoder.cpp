#include "video/MediaFoundationEncoder.hpp"
#include "logging/Logger.hpp"
#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace evidence {
namespace {
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr))); }
std::string utf8(const wchar_t* value) { if (!value) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr); std::string out(std::max(0, n - 1), '\0'); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), n - 1, nullptr, nullptr); return out; }
struct Activation { Microsoft::WRL::ComPtr<IMFActivate> value; std::wstring name; int rank{}; };
std::vector<Activation> enumerate(UINT32 flags) {
  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12}, output{MFMediaType_Video, MFVideoFormat_H264}; IMFActivate** raw{}; UINT32 count{};
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output, &raw, &count); if (FAILED(hr)) return {};
  std::vector<Activation> values; for (UINT32 i = 0; i < count; ++i) { WCHAR* name{}; UINT32 len{}; raw[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len); std::wstring n = name ? name : L"H.264 encoder"; CoTaskMemFree(name); int rank = 50; if (n.find(L"NVIDIA") != std::wstring::npos) rank = 0; else if (n.find(L"AMD") != std::wstring::npos || n.find(L"Advanced Micro") != std::wstring::npos) rank = 10; else if (n.find(L"Intel") != std::wstring::npos) rank = 20; values.push_back({raw[i], n, rank}); raw[i]->Release(); }
  CoTaskMemFree(raw); std::stable_sort(values.begin(), values.end(), [](auto& a, auto& b){ return a.rank < b.rank; }); return values;
}
bool contains_idr(const std::vector<std::uint8_t>& bytes) {
  for (std::size_t i = 0; i + 4 < bytes.size(); ++i) {
    if (bytes[i] == 0 && bytes[i + 1] == 0 && ((bytes[i + 2] == 1 && (bytes[i + 3] & 0x1f) == 5) || (bytes[i + 2] == 0 && bytes[i + 3] == 1 && (bytes[i + 4] & 0x1f) == 5))) return true;
  }
  for (std::size_t i = 0; i + 5 <= bytes.size();) { const std::uint32_t n = (std::uint32_t(bytes[i]) << 24) | (std::uint32_t(bytes[i + 1]) << 16) | (std::uint32_t(bytes[i + 2]) << 8) | bytes[i + 3]; if (!n || i + 4 + n > bytes.size()) break; if ((bytes[i + 4] & 0x1f) == 5) return true; i += 4 + n; }
  return false;
}
}

MediaFoundationEncoder::MediaFoundationEncoder(D3DDevice& device, std::uint32_t fps, std::uint32_t bitrate, std::uint32_t keyframe_seconds, bool prefer_hardware, OutputCallback output)
  : device_(device), converter_(device), fps_(fps), bitrate_(bitrate), keyframe_seconds_(keyframe_seconds), prefer_hardware_(prefer_hardware), output_(std::move(output)) {}
MediaFoundationEncoder::~MediaFoundationEncoder() { try { stop(); } catch (...) {} }
void MediaFoundationEncoder::create_transform() {
  auto candidates = prefer_hardware_ && !force_software_ ? enumerate(MFT_ENUM_FLAG_HARDWARE) : std::vector<Activation>{};
  hardware_ = !candidates.empty();
  if (candidates.empty()) candidates = enumerate(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT);
  if (candidates.empty()) throw std::runtime_error("no Media Foundation H.264 encoder is installed");
  HRESULT last = E_FAIL;
  for (auto& c : candidates) { Microsoft::WRL::ComPtr<IMFTransform> t; last = c.value->ActivateObject(IID_PPV_ARGS(&t)); if (SUCCEEDED(last)) { transform_ = std::move(t); encoder_name_ = utf8(c.name.c_str()); break; } }
  check(last, "activate H264 encoder");
  Microsoft::WRL::ComPtr<IMFAttributes> attrs; if (SUCCEEDED(transform_->GetAttributes(&attrs))) { UINT32 value{}; async_ = SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &value)) && value; if (async_) { attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE); transform_.As(&events_); } }
  gpu_input_ = hardware_ && SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(device_.mf_manager())));
}
void MediaFoundationEncoder::configure(std::uint32_t width, std::uint32_t height) {
  teardown(); create_transform(); width &= ~1u; height &= ~1u;
  check(MFCreateMediaType(&output_type_), "create encoder output type"); output_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); output_type_->SetUINT32(MF_MT_AVG_BITRATE, bitrate_); output_type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); output_type_->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High); MFSetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(output_type_.Get(), MF_MT_FRAME_RATE, fps_, 1); MFSetAttributeRatio(output_type_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  check(transform_->SetOutputType(0, output_type_.Get(), 0), "set H264 output type");
  Microsoft::WRL::ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "create encoder input type"); input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps_, 1); MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); check(transform_->SetInputType(0, input.Get(), 0), "set NV12 input type");
  Microsoft::WRL::ComPtr<ICodecAPI> codec; if (SUCCEEDED(transform_.As(&codec))) { VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = keyframe_seconds_ * fps_; codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v); v.ulVal = eAVEncCommonRateControlMode_CBR; codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v); VariantClear(&v); }
  check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "encoder begin streaming"); check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "encoder start stream");
  UINT32 blob{}; output_type_->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob); description_ = {width, height, fps_, bitrate_}; if (blob) { description_.sequence_header.resize(blob); output_type_->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, description_.sequence_header.data(), blob, nullptr); }
  configured_ = true; last_pts_ = 0; need_input_ = !async_; log_info("ENCODER_SELECTED", "encoder=\"" + encoder_name_ + "\"");
}
Microsoft::WRL::ComPtr<IMFSample> MediaFoundationEncoder::make_input(ID3D11Texture2D* nv12, std::int64_t pts, std::int64_t duration) {
  Microsoft::WRL::ComPtr<IMFSample> sample; check(MFCreateSample(&sample), "create input sample"); Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (gpu_input_) check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer), "create DXGI sample buffer");
  else {
    D3D11_TEXTURE2D_DESC desc{}; nv12->GetDesc(&desc); auto staging_desc = desc; staging_desc.Usage = D3D11_USAGE_STAGING; staging_desc.BindFlags = 0; staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; check(device_.device()->CreateTexture2D(&staging_desc, nullptr, &staging), "create staging NV12"); D3D11_MAPPED_SUBRESOURCE mapped{};
    { std::scoped_lock d3d_lock(device_.context_mutex()); device_.context()->CopyResource(staging.Get(), nv12); check(device_.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map NV12");
      const DWORD size = desc.Width * desc.Height * 3 / 2; check(MFCreateMemoryBuffer(size, &buffer), "create CPU input buffer"); BYTE* dst{}; buffer->Lock(&dst, nullptr, nullptr); const BYTE* src = static_cast<const BYTE*>(mapped.pData); for (UINT y = 0; y < desc.Height; ++y) std::memcpy(dst + y * desc.Width, src + y * mapped.RowPitch, desc.Width); const auto* uv = src + mapped.RowPitch * desc.Height; auto* duv = dst + desc.Width * desc.Height; for (UINT y = 0; y < desc.Height / 2; ++y) std::memcpy(duv + y * desc.Width, uv + y * mapped.RowPitch, desc.Width); buffer->Unlock(); buffer->SetCurrentLength(size); device_.context()->Unmap(staging.Get(), 0); }
  }
  sample->AddBuffer(buffer.Get()); sample->SetSampleTime(pts); sample->SetSampleDuration(duration); return sample;
}
void MediaFoundationEncoder::encode(const VideoFrame& frame) {
  std::scoped_lock lock(mutex_);
  try {
  if (!configured_ || (frame.width & ~1u) != description_.width || (frame.height & ~1u) != description_.height) configure(frame.width, frame.height);
  const auto duration = HnsPerSecond / fps_; auto nv12 = converter_.convert_bgra_to_nv12(frame.texture.Get(), frame.width, frame.height);
  if (force_keyframe_) { Microsoft::WRL::ComPtr<ICodecAPI> codec; if (SUCCEEDED(transform_.As(&codec))) { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v); VariantClear(&v); } force_keyframe_ = false; }
  drain_events(); drain_output();
  while (async_ && !need_input_) { Microsoft::WRL::ComPtr<IMFMediaEvent> event; check(events_->GetEvent(0, &event), "wait for encoder input"); MediaEventType type{}; event->GetType(&type); if (type == METransformNeedInput) need_input_ = true; else if (type == METransformHaveOutput) drain_output(); }
  auto sample = make_input(nv12.Get(), frame.pts, duration); HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0); if (hr == MF_E_NOTACCEPTING) { drain_events(); drain_output(); hr = transform_->ProcessInput(0, sample.Get(), 0); } check(hr, "encoder ProcessInput"); need_input_ = false; drain_events(); drain_output(); last_pts_ = frame.pts;
  } catch (...) { if (hardware_) { log_warn("HARDWARE_ENCODER_FAILED", "falling back to software H.264"); force_software_ = true; teardown(); } throw; }
}
void MediaFoundationEncoder::drain_events() {
  if (!events_) return; for (;;) { Microsoft::WRL::ComPtr<IMFMediaEvent> event; HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event); if (hr == MF_E_NO_EVENTS_AVAILABLE) break; if (FAILED(hr)) break; MediaEventType type{}; event->GetType(&type); if (type == METransformHaveOutput) drain_output(); else if (type == METransformNeedInput) need_input_ = true; }
}
void MediaFoundationEncoder::drain_output() {
  if (!transform_) return; MFT_OUTPUT_STREAM_INFO info{}; if (FAILED(transform_->GetOutputStreamInfo(0, &info))) return;
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER out{}; Microsoft::WRL::ComPtr<IMFSample> provided;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) { MFCreateSample(&provided); Microsoft::WRL::ComPtr<IMFMediaBuffer> b; MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 2 * 1024 * 1024), &b); provided->AddBuffer(b.Get()); out.pSample = provided.Get(); }
    DWORD status{}; HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status); if (out.pEvents) out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break; if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { Microsoft::WRL::ComPtr<IMFMediaType> changed; if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &changed))) { transform_->SetOutputType(0, changed.Get(), 0); output_type_ = changed; } continue; } if (FAILED(hr)) { if (async_) break; check(hr, "encoder ProcessOutput"); }
    IMFSample* raw = out.pSample ? out.pSample : provided.Get(); if (!raw) continue; LONGLONG pts{}, duration{}; raw->GetSampleTime(&pts); raw->GetSampleDuration(&duration); UINT32 clean{}; raw->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous; if (SUCCEEDED(raw->ConvertToContiguousBuffer(&contiguous))) { BYTE* data{}; DWORD len{}; contiguous->Lock(&data, nullptr, &len); auto bytes = std::make_shared<std::vector<std::uint8_t>>(data, data + len); contiguous->Unlock(); if (description_.sequence_header.empty()) { UINT32 blob{}; if (SUCCEEDED(output_type_->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob)) && blob) { description_.sequence_header.resize(blob); output_type_->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, description_.sequence_header.data(), blob, nullptr); } } if (output_) output_({TrackKind::Video, pts, duration, clean != 0 || contains_idr(*bytes), std::move(bytes)}); }
    if (out.pSample && out.pSample != provided.Get()) out.pSample->Release();
  }
}
void MediaFoundationEncoder::request_keyframe() { std::scoped_lock lock(mutex_); force_keyframe_ = true; }
void MediaFoundationEncoder::stop() { std::scoped_lock lock(mutex_); if (!transform_) return; transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); drain_events(); drain_output(); transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); teardown(); }
void MediaFoundationEncoder::teardown() { transform_.Reset(); events_.Reset(); output_type_.Reset(); converter_.reset(); configured_ = false; gpu_input_ = false; async_ = false; hardware_ = false; need_input_ = false; }
StreamDescription MediaFoundationEncoder::description() const { std::scoped_lock lock(mutex_); return description_; }
std::string MediaFoundationEncoder::encoder_name() const { std::scoped_lock lock(mutex_); return encoder_name_; }
bool MediaFoundationEncoder::configured() const { std::scoped_lock lock(mutex_); return configured_; }
}
