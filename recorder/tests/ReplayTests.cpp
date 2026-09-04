#include "Test.hpp"
#include "replay/SegmentStore.hpp"
#include <Windows.h>
#include <filesystem>

using namespace evidence;
namespace {
EncodedSample sample(std::int64_t second, bool key = false) { return {TrackKind::Video, second * HnsPerSecond, HnsPerSecond, key, std::make_shared<std::vector<std::uint8_t>>(100, static_cast<std::uint8_t>(second))}; }
std::filesystem::path temp_dir(std::string_view name) { auto p = std::filesystem::temp_directory_path() / ("evidence_test_" + std::string(name) + "_" + std::to_string(GetCurrentProcessId())); std::filesystem::create_directories(p); return p; }
}

TEST_CASE("ring buffer evicts data older than 600 seconds") {
  auto path = temp_dir("eviction"); SegmentStore store(path, 600 * HnsPerSecond, 10 * HnsPerSecond, false); store.initialize();
  for (int i = 0; i <= 700; ++i) store.append(sample(i, i % 10 == 0));
  auto snapshot = store.snapshot(); REQUIRE(snapshot.available_hns <= 611 * HnsPerSecond); auto selected = select_replay_samples(snapshot, 600 * HnsPerSecond); REQUIRE(!selected.empty()); REQUIRE(selected.back().pts <= 601 * HnsPerSecond); store.clear(); std::filesystem::remove_all(path);
}

TEST_CASE("replay begins on the nearest keyframe before desired start") {
  auto path = temp_dir("keyframe"); SegmentStore store(path, 100 * HnsPerSecond, 10 * HnsPerSecond, false); store.initialize();
  for (int i = 0; i <= 35; ++i) store.append(sample(i, i == 0 || i == 10 || i == 20 || i == 30));
  auto selected = select_replay_samples(store.snapshot(), 12 * HnsPerSecond); REQUIRE(!selected.empty()); REQUIRE(selected.front().keyframe); REQUIRE(selected.front().pts == 0); REQUIRE(selected.back().pts >= 15 * HnsPerSecond); store.clear(); std::filesystem::remove_all(path);
}

TEST_CASE("timestamp rebasing preserves intervals") {
  std::vector<EncodedSample> samples{sample(40, true), sample(41), {TrackKind::Audio, 40 * HnsPerSecond + 1000, 1000, true, std::make_shared<std::vector<std::uint8_t>>(10)}};
  rebase_timestamps(samples); REQUIRE(samples[0].pts == 0); REQUIRE(samples[1].pts == HnsPerSecond); REQUIRE(samples[2].pts == 1000);
}

TEST_CASE("disk segment stays alive while snapshot references it") {
  auto path = temp_dir("refs"); std::filesystem::path segment;
  { SegmentStore store(path, 30 * HnsPerSecond, HnsPerSecond, true); store.initialize(); store.append(sample(0, true)); store.append(sample(2, true)); auto snapshot = store.snapshot(); REQUIRE(!snapshot.segments.empty()); segment = snapshot.segments.front()->path; REQUIRE(std::filesystem::exists(segment)); store.clear(); REQUIRE(std::filesystem::exists(segment)); snapshot.segments.clear(); REQUIRE(!std::filesystem::exists(segment)); }
  std::filesystem::remove_all(path);
}
