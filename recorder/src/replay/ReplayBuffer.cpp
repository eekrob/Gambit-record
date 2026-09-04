#include "replay/ReplayBuffer.hpp"
#include "common/Files.hpp"
#include "logging/Logger.hpp"
#include "recording/Mp4Muxer.hpp"

namespace evidence {
ReplayBuffer::ReplayBuffer(std::filesystem::path cache, std::filesystem::path output, std::uint32_t seconds, std::uint32_t segment_seconds, bool disk)
  : store_(std::move(cache), static_cast<std::int64_t>(seconds) * HnsPerSecond, static_cast<std::int64_t>(segment_seconds) * HnsPerSecond, disk), output_(std::move(output)), seconds_(seconds) {}
void ReplayBuffer::initialize() { store_.initialize(); }
bool ReplayBuffer::start() {
  auto expected = ReplayState::Stopped; if (!state_.compare_exchange_strong(expected, ReplayState::Starting)) return false;
  state_ = ReplayState::Running; log_info("REPLAY_BUFFER_STARTED"); return true;
}
bool ReplayBuffer::stop() {
  auto s = state_.load(); if (s == ReplayState::Stopped) return false;
  if (s == ReplayState::Saving) return false;
  store_.clear(); state_ = ReplayState::Stopped; log_info("REPLAY_BUFFER_STOPPED"); return true;
}
void ReplayBuffer::reset() { store_.clear(); log_info("REPLAY_BUFFER_RESET"); }
void ReplayBuffer::on_sample(const EncodedSample& sample) {
  if (state_.load() != ReplayState::Running && state_.load() != ReplayState::Saving) return;
  try { store_.append(sample); } catch (const std::exception& e) { state_ = ReplayState::Error; log_error("REPLAY_WRITE_FAILED", e.what()); }
}
std::future<SaveResult> ReplayBuffer::save(EvidenceMetadata metadata, StreamDescription description) {
  std::scoped_lock lock(save_mutex_);
  if (state_.load() != ReplayState::Running || save_active_) {
    return std::async(std::launch::deferred, []{ return SaveResult{false, {}, 0, "replay is not running or save is already active"}; });
  }
  save_active_ = true; state_ = ReplayState::Saving;
  auto snapshot = store_.snapshot();
  return std::async(std::launch::async, [this, snapshot = std::move(snapshot), metadata = std::move(metadata), description = std::move(description)]() mutable {
    SaveResult result;
    try {
      auto samples = select_replay_samples(snapshot, static_cast<std::int64_t>(seconds_) * HnsPerSecond);
      if (samples.empty()) throw std::runtime_error("no keyframe is available yet");
      result.path = evidence_filename(output_, metadata, "replay");
      Mp4Muxer muxer; muxer.open(result.path, description);
      for (const auto& sample : samples) muxer.write(sample);
      muxer.finalize();
      result.duration_hns = samples.back().pts + samples.back().duration; result.success = true;
      log_info("REPLAY_SAVED", "path=\"" + result.path.string() + "\"");
    } catch (const std::exception& e) { result.error = e.what(); log_error("REPLAY_SAVE_FAILED", e.what()); }
    { std::scoped_lock done(save_mutex_); save_active_ = false; if (state_.load() != ReplayState::Error) state_ = ReplayState::Running; }
    return result;
  });
}
}
