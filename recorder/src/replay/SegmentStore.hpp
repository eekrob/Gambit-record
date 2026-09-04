#pragma once
#include "common/Media.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace evidence {
class SegmentStore {
public:
  struct Segment {
    std::filesystem::path path;
    std::vector<EncodedSample> memory_samples;
    std::int64_t first_pts{};
    std::int64_t last_pts{};
    bool disk{};
    ~Segment();
    std::vector<EncodedSample> read() const;
  };
  struct Snapshot {
    std::vector<std::shared_ptr<const Segment>> segments;
    std::int64_t available_hns{};
  };

  SegmentStore(std::filesystem::path directory, std::int64_t retention_hns, std::int64_t segment_hns, bool disk);
  void initialize();
  void append(const EncodedSample& sample);
  Snapshot snapshot();
  void clear();
  std::int64_t available_hns() const;
  std::size_t segment_count() const;
private:
  void start_segment_locked(const EncodedSample& sample);
  void seal_locked();
  void evict_locked(std::int64_t newest_pts);
  std::filesystem::path directory_;
  std::int64_t retention_hns_;
  std::int64_t segment_hns_;
  bool disk_;
  mutable std::mutex mutex_;
  std::deque<std::shared_ptr<Segment>> sealed_;
  std::shared_ptr<Segment> active_;
  std::ofstream active_file_;
  std::uint64_t sequence_{};
};

std::vector<EncodedSample> select_replay_samples(const SegmentStore::Snapshot& snapshot, std::int64_t requested_hns);
void rebase_timestamps(std::vector<EncodedSample>& samples);
}

