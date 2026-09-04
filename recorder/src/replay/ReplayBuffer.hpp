#pragma once
#include "common/Media.hpp"
#include "common/States.hpp"
#include "replay/SegmentStore.hpp"
#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>

namespace evidence {
struct SaveResult { bool success{}; std::filesystem::path path; std::int64_t duration_hns{}; std::string error; };

class ReplayBuffer {
public:
  ReplayBuffer(std::filesystem::path cache, std::filesystem::path output, std::uint32_t seconds, std::uint32_t segment_seconds, bool disk);
  void initialize();
  bool start();
  bool stop();
  void reset();
  void on_sample(const EncodedSample& sample);
  std::future<SaveResult> save(EvidenceMetadata metadata, StreamDescription description);
  ReplayState state() const { return state_.load(); }
  std::int64_t available_hns() const { return store_.available_hns(); }
  std::uint32_t retention_seconds() const { return seconds_; }
private:
  SegmentStore store_;
  std::filesystem::path output_;
  std::uint32_t seconds_;
  std::atomic<ReplayState> state_{ReplayState::Stopped};
  std::mutex save_mutex_;
  bool save_active_{};
};
}
