#include "recording/RecordingSession.hpp"
#include "common/Files.hpp"
#include "logging/Logger.hpp"
#include <algorithm>

namespace evidence {
bool RecordingSession::start(EvidenceMetadata metadata, StreamDescription description) {
  auto expected = RecordingState::Stopped;
  if (!state_.compare_exchange_strong(expected, RecordingState::WaitingForKeyframe)) return false;
  std::scoped_lock lock(mutex_); pending_metadata_ = std::move(metadata); pending_description_ = std::move(description); path_.clear(); duration_hns_ = 0;
  log_info("RECORD_START_REQUESTED"); return true;
}
void RecordingSession::on_sample(const EncodedSample& sample) {
  std::scoped_lock lock(mutex_);
  try {
    if (state_ == RecordingState::WaitingForKeyframe) {
      if (sample.track != TrackKind::Video || !sample.keyframe) return;
      path_ = evidence_filename(directory_, pending_metadata_, "record"); base_pts_ = sample.pts; muxer_ = std::make_unique<Mp4Muxer>(); muxer_->open(path_, pending_description_); state_ = RecordingState::Recording;
      log_info("RECORD_STARTED", "path=\"" + path_.string() + "\"");
    }
    if (state_ == RecordingState::Recording && sample.pts >= base_pts_) { auto rebased = sample; rebased.pts -= base_pts_; muxer_->write(rebased); duration_hns_ = std::max(duration_hns_.load(), rebased.pts + rebased.duration); }
  } catch (const std::exception& e) { state_ = RecordingState::Error; muxer_.reset(); log_error("RECORD_WRITE_FAILED", e.what()); }
}
std::filesystem::path RecordingSession::stop() {
  std::scoped_lock lock(mutex_);
  const auto old = state_.load(); if (old == RecordingState::Stopped) return {};
  state_ = RecordingState::Stopping;
  try { if (muxer_) muxer_->finalize(); } catch (const std::exception& e) { state_ = RecordingState::Error; log_error("RECORD_FINALIZE_FAILED", e.what()); return {}; }
  muxer_.reset(); state_ = RecordingState::Stopped; log_info("RECORD_STOPPED", "path=\"" + path_.string() + "\""); return path_;
}
std::filesystem::path RecordingSession::current_path() const { std::scoped_lock lock(mutex_); return path_; }
}
