#pragma once
#include "common/Media.hpp"
#include <filesystem>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace evidence {
class Mp4Muxer {
public:
  Mp4Muxer() = default; ~Mp4Muxer();
  Mp4Muxer(const Mp4Muxer&) = delete; Mp4Muxer& operator=(const Mp4Muxer&) = delete;
  void open(const std::filesystem::path& path, const StreamDescription& description);
  void write(const EncodedSample& sample);
  void finalize();
private:
  Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
  DWORD video_stream_{static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX)};
  DWORD audio_stream_{static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX)};
  bool finalized_{true};
};
}
