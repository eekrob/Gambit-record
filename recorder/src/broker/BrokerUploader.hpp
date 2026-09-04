#pragma once

#include "common/Media.hpp"
#include "config/Config.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>

namespace evidence {

struct UploadResult {
  bool success{};
  std::string upload_id;
  std::string video_id;
  std::string url;
  std::string error;
};

struct ChannelResult {
  bool success{};
  std::string channel_id;
  std::string title;
  std::string error;
};

class BrokerUploader {
public:
  using ProgressCallback = std::function<void(std::uint64_t sent, std::uint64_t total)>;
  explicit BrokerUploader(Config::Broker settings) : settings_(std::move(settings)) {}
  UploadResult upload(const std::filesystem::path& file, const EvidenceMetadata& metadata,
                      ProgressCallback progress = {}, std::string resume_id = {}, std::stop_token stop = {}) const;
  ChannelResult channel() const;
  static std::string sha256(const std::filesystem::path& file);
  static std::string video_title(const EvidenceMetadata& metadata);

private:
  Config::Broker settings_;
};

} // namespace evidence
