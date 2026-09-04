#pragma once
#include "common/Media.hpp"
#include "common/States.hpp"
#include "recording/Mp4Muxer.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>

namespace evidence {
class RecordingSession {
public:
  explicit RecordingSession(std::filesystem::path directory) : directory_(std::move(directory)) {}
  bool start(EvidenceMetadata metadata, StreamDescription description);
  std::filesystem::path stop();
  void on_sample(const EncodedSample& sample);
  RecordingState state() const { return state_.load(); }
  std::int64_t duration_hns() const { return duration_hns_.load(); }
  std::filesystem::path current_path() const;
  bool needs_keyframe() const { return state_.load() == RecordingState::WaitingForKeyframe; }
private:
  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::unique_ptr<Mp4Muxer> muxer_;
  StreamDescription pending_description_;
  EvidenceMetadata pending_metadata_;
  std::filesystem::path path_;
  std::int64_t base_pts_{};
  std::atomic<std::int64_t> duration_hns_{};
  std::atomic<RecordingState> state_{RecordingState::Stopped};
};
}
