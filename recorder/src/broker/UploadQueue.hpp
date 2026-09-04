#pragma once

#include "broker/BrokerUploader.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace evidence {

class UploadQueue {
public:
  UploadQueue(std::filesystem::path state_file, Config::Broker settings);
  ~UploadQueue();
  void start();
  void stop();
  std::string enqueue(const std::filesystem::path& path, const EvidenceMetadata& metadata);
  nlohmann::json status() const;
  void configure(Config::Broker settings);
  void enforce_archive_limit(const std::filesystem::path& directory, std::uint64_t limit_bytes,
                             const std::filesystem::path& keep = {});

private:
  struct Job { std::string id; std::filesystem::path path; EvidenceMetadata metadata; unsigned attempts{}; std::int64_t next_attempt{}; std::string upload_id; };
  void load();
  void save_locked() const;
  void run(std::stop_token token);
  std::filesystem::path state_file_;
  Config::Broker settings_;
  mutable std::mutex mutex_;
  std::vector<Job> jobs_;
  std::optional<UploadResult> last_;
  std::string active_id_;
  std::atomic<std::uint64_t> sent_{};
  std::atomic<std::uint64_t> total_{};
  std::jthread thread_;
};

} // namespace evidence
