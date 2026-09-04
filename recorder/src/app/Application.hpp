#pragma once
#include "audio/WasapiCapture.hpp"
#include "capture/CaptureManager.hpp"
#include "common/BoundedQueue.hpp"
#include "config/Config.hpp"
#include "ipc/IpcServer.hpp"
#include "recording/RecordingSession.hpp"
#include "replay/ReplayBuffer.hpp"
#include "video/MediaFoundationEncoder.hpp"
#include "broker/BrokerUploader.hpp"
#include "broker/UploadQueue.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace evidence {
class Application {
public:
  Application(Config config, std::wstring pipe_name); ~Application();
  void initialize(); void run(); void request_stop();
private:
  nlohmann::json handle_command(const nlohmann::json& request);
  EvidenceMetadata parse_metadata(const nlohmann::json& request) const;
  void encoder_loop(std::stop_token token);
  void on_encoded(EncodedSample sample);
  StreamDescription stream_description() const;
  void collect_save_result();
  void recover_pipeline();
  Config config_; D3DDevice d3d_; BoundedQueue<VideoFrame> frames_;
  std::unique_ptr<MediaFoundationEncoder> encoder_; std::unique_ptr<CaptureManager> capture_; Microsoft::WRL::ComPtr<WasapiCapture> audio_;
  std::unique_ptr<ReplayBuffer> replay_; std::unique_ptr<RecordingSession> recording_; std::unique_ptr<IpcServer> ipc_; std::jthread encoder_thread_;
  std::unique_ptr<BrokerUploader> broker_; std::unique_ptr<UploadQueue> upload_queue_;
  std::atomic_bool stop_{}; mutable std::mutex stream_mutex_; StreamDescription stream_;
  std::mutex save_mutex_; std::future<SaveResult> save_future_; std::optional<SaveResult> last_save_; std::uint64_t save_generation_{}; std::atomic<std::uint64_t> dropped_frames_{};
  std::mutex upload_mutex_;
  std::mutex settings_mutex_;
  std::atomic_bool audio_failed_{};
  std::atomic<DWORD> audio_pid_{};
  std::string active_audio_mode_;
  bool mf_started_{};
  std::wstring pipe_name_;
};
}
