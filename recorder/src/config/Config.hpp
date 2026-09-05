#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace evidence {
struct Config {
  struct Capture { std::wstring process{L"gta_sa.exe"}; std::string backend{"windows_graphics_capture"}; } capture;
  struct Video { std::uint32_t fps{60}; std::uint32_t bitrate{16'000'000}; std::string codec{"h264"}; bool prefer_hardware_encoder{true}; std::uint32_t keyframe_interval_seconds{2}; std::size_t queue_frames{6}; } video;
  struct Audio { bool enabled{true}; std::string mode{"process"}; bool microphone{false}; } audio;
  struct Broker {
    bool enabled{true};
    std::string endpoint{"https://gambit-record-broker.whatdroyidclo.workers.dev"};
    std::string channel_title{"Gambit Record"};
    std::uint32_t retry_seconds{30};
  } broker;
  struct Recording { std::filesystem::path directory{"grecord/records"}; std::uint32_t archive_limit_gb{20}; } recording;
  struct Replay { bool enabled{true}; bool auto_start{true}; std::uint32_t seconds{600}; std::string storage{"disk"}; std::filesystem::path cache_directory{"grecord/cache"}; std::uint32_t segment_seconds{10}; } replay;
  struct Logging { std::filesystem::path directory{"grecord/logs"}; std::string level{"info"}; } logging;

  static Config load_or_create(const std::filesystem::path& path);
  void save(const std::filesystem::path& path) const;
  void validate(const std::filesystem::path& base) const;
  void resolve_paths(const std::filesystem::path& base);
};
}
