#include "replay/SegmentStore.hpp"
#include "common/Files.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>

namespace evidence {
namespace {
constexpr std::uint32_t Magic = 0x31565345; // ESV1
#pragma pack(push, 1)
struct DiskHeader { std::uint32_t magic; std::uint8_t track; std::uint8_t flags; std::uint16_t reserved; std::int64_t pts; std::int64_t duration; std::uint32_t size; };
#pragma pack(pop)
}

SegmentStore::Segment::~Segment() {
  if (disk && !path.empty()) { std::error_code ec; std::filesystem::remove(path, ec); }
}

std::vector<EncodedSample> SegmentStore::Segment::read() const {
  if (!disk) return memory_samples;
  std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot read replay segment");
  std::vector<EncodedSample> out;
  while (true) {
    DiskHeader h{}; in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (in.eof()) break;
    if (!in || h.magic != Magic || h.size > 128u * 1024u * 1024u) throw std::runtime_error("corrupt replay segment");
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(h.size);
    in.read(reinterpret_cast<char*>(bytes->data()), h.size); if (!in) throw std::runtime_error("truncated replay segment");
    out.push_back({static_cast<TrackKind>(h.track), h.pts, h.duration, (h.flags & 1) != 0, std::move(bytes)});
  }
  return out;
}

SegmentStore::SegmentStore(std::filesystem::path directory, std::int64_t retention_hns, std::int64_t segment_hns, bool disk)
  : directory_(std::move(directory)), retention_hns_(retention_hns), segment_hns_(segment_hns), disk_(disk) {}

void SegmentStore::initialize() {
  std::scoped_lock lock(mutex_);
  std::filesystem::create_directories(directory_);
  if (!disk_) return;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".eseg" && entry.path().filename().wstring().starts_with(L"segment_")) {
      std::error_code ec; std::filesystem::remove(entry.path(), ec);
    }
  }
}

void SegmentStore::start_segment_locked(const EncodedSample& sample) {
  active_ = std::make_shared<Segment>(); active_->disk = disk_; active_->first_pts = active_->last_pts = sample.pts;
  if (disk_) {
    active_->path = directory_ / std::format("segment_{:08}.eseg", sequence_++);
    active_file_.open(active_->path, std::ios::binary | std::ios::trunc);
    if (!active_file_) { active_.reset(); throw std::runtime_error("cannot create replay cache segment"); }
  }
}

void SegmentStore::append(const EncodedSample& sample) {
  if (!sample.bytes || sample.bytes->empty()) return;
  std::scoped_lock lock(mutex_);
  if (!active_) start_segment_locked(sample);
  if (sample.track == TrackKind::Video && sample.keyframe && sample.pts - active_->first_pts >= segment_hns_) {
    seal_locked(); start_segment_locked(sample);
  }
  if (disk_) {
    const DiskHeader h{Magic, static_cast<std::uint8_t>(sample.track), static_cast<std::uint8_t>(sample.keyframe ? 1 : 0), 0, sample.pts, sample.duration, static_cast<std::uint32_t>(sample.bytes->size())};
    active_file_.write(reinterpret_cast<const char*>(&h), sizeof(h));
    active_file_.write(reinterpret_cast<const char*>(sample.bytes->data()), static_cast<std::streamsize>(sample.bytes->size()));
    if (!active_file_) throw std::runtime_error("replay cache write failed (disk full?)");
  } else active_->memory_samples.push_back(sample);
  active_->last_pts = std::max(active_->last_pts, sample.pts + sample.duration);
  evict_locked(active_->last_pts);
}

void SegmentStore::seal_locked() {
  if (!active_) return;
  if (active_file_.is_open()) { active_file_.flush(); active_file_.close(); }
  sealed_.push_back(std::move(active_));
}

void SegmentStore::evict_locked(std::int64_t newest_pts) {
  const auto cutoff = newest_pts - retention_hns_;
  while (!sealed_.empty() && sealed_.front()->last_pts < cutoff) sealed_.pop_front();
}

SegmentStore::Snapshot SegmentStore::snapshot() {
  std::scoped_lock lock(mutex_);
  seal_locked();
  Snapshot out; out.segments.assign(sealed_.begin(), sealed_.end());
  if (!out.segments.empty()) out.available_hns = std::max<std::int64_t>(0, out.segments.back()->last_pts - out.segments.front()->first_pts);
  return out;
}

void SegmentStore::clear() {
  std::scoped_lock lock(mutex_); seal_locked(); sealed_.clear();
}

std::int64_t SegmentStore::available_hns() const {
  std::scoped_lock lock(mutex_);
  const auto first = !sealed_.empty() ? sealed_.front()->first_pts : (active_ ? active_->first_pts : 0);
  const auto last = active_ ? active_->last_pts : (!sealed_.empty() ? sealed_.back()->last_pts : 0);
  return std::max<std::int64_t>(0, last - first);
}
std::size_t SegmentStore::segment_count() const { std::scoped_lock lock(mutex_); return sealed_.size() + (active_ ? 1 : 0); }

std::vector<EncodedSample> select_replay_samples(const SegmentStore::Snapshot& snapshot, std::int64_t requested_hns) {
  std::vector<EncodedSample> all;
  for (const auto& segment : snapshot.segments) { auto part = segment->read(); all.insert(all.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end())); }
  if (all.empty()) return {};
  std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b){ return a.pts < b.pts || (a.pts == b.pts && a.track == TrackKind::Video); });
  const auto end_pts = std::max_element(all.begin(), all.end(), [](const auto& a, const auto& b){ return a.pts + a.duration < b.pts + b.duration; })->pts;
  const auto desired = end_pts - requested_hns;
  auto start = all.begin();
  for (auto it = all.begin(); it != all.end(); ++it) if (it->track == TrackKind::Video && it->keyframe && it->pts <= desired) start = it;
  if (start == all.begin() && !(start->track == TrackKind::Video && start->keyframe)) {
    start = std::find_if(all.begin(), all.end(), [](const auto& s){ return s.track == TrackKind::Video && s.keyframe; });
  }
  if (start == all.end()) return {};
  const auto start_pts = start->pts;
  std::vector<EncodedSample> result;
  for (auto& s : all) if (s.pts >= start_pts) result.push_back(std::move(s));
  rebase_timestamps(result); return result;
}

void rebase_timestamps(std::vector<EncodedSample>& samples) {
  if (samples.empty()) return;
  const auto base = std::min_element(samples.begin(), samples.end(), [](const auto& a, const auto& b){ return a.pts < b.pts; })->pts;
  for (auto& sample : samples) sample.pts = std::max<std::int64_t>(0, sample.pts - base);
}
}

