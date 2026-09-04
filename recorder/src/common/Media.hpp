#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>

namespace evidence {
using Microsoft::WRL::ComPtr;
constexpr std::int64_t HnsPerSecond = 10'000'000;

enum class TrackKind : std::uint8_t { Video = 1, Audio = 2 };

struct VideoFrame {
  ComPtr<ID3D11Texture2D> texture;
  std::uint32_t width{};
  std::uint32_t height{};
  std::int64_t pts{};
  std::int64_t duration{};
};

struct EncodedSample {
  TrackKind track{TrackKind::Video};
  std::int64_t pts{};
  std::int64_t duration{};
  bool keyframe{};
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
};

struct StreamDescription {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t fps{};
  std::uint32_t bitrate{};
  std::vector<std::uint8_t> sequence_header;
  bool has_audio{};
  std::uint32_t audio_rate{48000};
  std::uint32_t audio_channels{2};
  std::vector<std::uint8_t> audio_user_data;
};

struct EvidenceMetadata {
  std::string admin;
  int target_id{-1};
  std::string target_name;
  std::string server;
  std::string timestamp;
  std::string punishment_command;
  std::string punishment_reason;
  std::string recording_period;
};
}
