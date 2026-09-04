#include "recording/Mp4Muxer.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <cstring>
#include <stdexcept>

namespace evidence {
namespace {
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr))); }
Microsoft::WRL::ComPtr<IMFMediaType> video_type(const StreamDescription& d) {
  Microsoft::WRL::ComPtr<IMFMediaType> t; check(MFCreateMediaType(&t), "MFCreateMediaType");
  check(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video major"); check(t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "video subtype");
  check(MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, d.width, d.height), "video size"); check(MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, d.fps, 1), "video fps");
  check(MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "video aspect"); check(t->SetUINT32(MF_MT_AVG_BITRATE, d.bitrate), "video bitrate");
  check(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "video progressive");
  if (!d.sequence_header.empty()) check(t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, d.sequence_header.data(), static_cast<UINT32>(d.sequence_header.size())), "sequence header");
  return t;
}
Microsoft::WRL::ComPtr<IMFMediaType> audio_type(const StreamDescription& d) {
  Microsoft::WRL::ComPtr<IMFMediaType> t; check(MFCreateMediaType(&t), "MFCreateMediaType audio");
  check(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio major"); check(t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "audio subtype");
  check(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, d.audio_channels), "audio channels"); check(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, d.audio_rate), "audio rate");
  check(t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000), "audio bitrate"); check(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "audio bits");
  check(t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29), "aac profile");
  if (!d.audio_user_data.empty()) check(t->SetBlob(MF_MT_USER_DATA, d.audio_user_data.data(), static_cast<UINT32>(d.audio_user_data.size())), "aac user data");
  return t;
}
}

Mp4Muxer::~Mp4Muxer() { try { finalize(); } catch (...) {} }
void Mp4Muxer::open(const std::filesystem::path& path, const StreamDescription& description) {
  if (writer_) throw std::logic_error("muxer already open"); std::filesystem::create_directories(path.parent_path());
  Microsoft::WRL::ComPtr<IMFAttributes> attrs; check(MFCreateAttributes(&attrs, 2), "sink attrs");
  check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "sink hardware");
  check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer_), "create MP4 sink writer");
  auto video = video_type(description); check(writer_->AddStream(video.Get(), &video_stream_), "add video stream");
  // Identical compressed input type makes the sink writer remux instead of encode again.
  check(writer_->SetInputMediaType(video_stream_, video.Get(), nullptr), "set H264 input");
  if (description.has_audio) { auto audio = audio_type(description); check(writer_->AddStream(audio.Get(), &audio_stream_), "add audio stream"); check(writer_->SetInputMediaType(audio_stream_, audio.Get(), nullptr), "set AAC input"); }
  check(writer_->BeginWriting(), "begin MP4 writing"); finalized_ = false;
}
void Mp4Muxer::write(const EncodedSample& sample) {
  if (!writer_ || finalized_) throw std::logic_error("muxer is not open");
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(static_cast<DWORD>(sample.bytes->size()), &buffer), "sample buffer");
  BYTE* dst{}; check(buffer->Lock(&dst, nullptr, nullptr), "buffer lock"); std::memcpy(dst, sample.bytes->data(), sample.bytes->size()); buffer->Unlock(); check(buffer->SetCurrentLength(static_cast<DWORD>(sample.bytes->size())), "buffer length");
  Microsoft::WRL::ComPtr<IMFSample> mf_sample; check(MFCreateSample(&mf_sample), "sample"); check(mf_sample->AddBuffer(buffer.Get()), "add buffer");
  check(mf_sample->SetSampleTime(sample.pts), "sample time"); check(mf_sample->SetSampleDuration(sample.duration), "sample duration");
  if (sample.keyframe) check(mf_sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE), "clean point");
  const DWORD stream = sample.track == TrackKind::Video ? video_stream_ : audio_stream_;
  if (stream != MF_SINK_WRITER_INVALID_STREAM_INDEX) check(writer_->WriteSample(stream, mf_sample.Get()), "write MP4 sample");
}
void Mp4Muxer::finalize() {
  if (!writer_ || finalized_) return; finalized_ = true; check(writer_->Finalize(), "finalize MP4"); writer_.Reset();
}
}
