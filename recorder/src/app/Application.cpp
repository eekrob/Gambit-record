#include "app/Application.hpp"
#include "common/Files.hpp"
#include "logging/Logger.hpp"
#include <mfapi.h>
#include <chrono>
#include <fstream>

namespace evidence {
Application::Application(Config config, std::wstring pipe_name)
    : config_(std::move(config)), frames_(config_.video.queue_frames), pipe_name_(std::move(pipe_name)) {}
Application::~Application() {
  request_stop(); if (ipc_) ipc_->stop(); if (capture_) capture_->stop(); frames_.close();
  if (encoder_thread_.joinable()) { encoder_thread_.request_stop(); encoder_thread_.join(); }
  if (audio_) audio_->stop(); if (encoder_) encoder_->stop();
  if (recording_ && recording_->state() != RecordingState::Stopped) recording_->stop();
  { std::scoped_lock lock(save_mutex_); if (save_future_.valid()) last_save_ = save_future_.get(); }
  if (upload_queue_) upload_queue_->stop();
  if (mf_started_) MFShutdown();
}
void Application::initialize() {
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) throw std::runtime_error("Media Foundation startup failed"); mf_started_ = true; d3d_.create();
  replay_ = std::make_unique<ReplayBuffer>(config_.replay.cache_directory, config_.replay.cache_directory.parent_path() / "replays", config_.replay.seconds, config_.replay.segment_seconds, config_.replay.storage == "disk"); replay_->initialize(); if (config_.replay.enabled && config_.replay.auto_start) replay_->start();
  recording_ = std::make_unique<RecordingSession>(config_.recording.directory); audio_ = Microsoft::WRL::Make<WasapiCapture>(); broker_ = std::make_unique<BrokerUploader>(config_.broker);
  upload_queue_ = std::make_unique<UploadQueue>(config_.recording.directory.parent_path() / "upload-queue.json", config_.broker); upload_queue_->start();
  encoder_ = std::make_unique<MediaFoundationEncoder>(d3d_, config_.video.fps, config_.video.bitrate, config_.video.keyframe_interval_seconds, config_.video.prefer_hardware_encoder, [this](EncodedSample s){ on_encoded(std::move(s)); });
  capture_ = std::make_unique<CaptureManager>(config_.capture.process, config_.capture.backend, d3d_, [this](VideoFrame frame){ if (!frames_.push_drop_oldest(std::move(frame))) { const auto count = ++dropped_frames_; if (count == 1 || count % 60 == 0) log_warn("VIDEO_FRAMES_DROPPED", "count=" + std::to_string(count)); } });
  ipc_ = std::make_unique<IpcServer>(pipe_name_, [this](const auto& request){ return handle_command(request); });
  encoder_thread_ = std::jthread([this](std::stop_token t){ encoder_loop(t); }); capture_->start(); ipc_->start(); log_info("RECORDER_STARTED");
}
void Application::encoder_loop(std::stop_token token) {
  unsigned consecutive_failures = 0;
  std::int64_t last_capture_pts = 0;
  while (!token.stop_requested()) {
    auto frame = frames_.pop(token); if (!frame) continue;
    try {
      const auto minimum_interval = HnsPerSecond / config_.video.fps * 3 / 4;
      if (last_capture_pts && frame->pts > last_capture_pts && frame->pts - last_capture_pts < minimum_interval) { ++dropped_frames_; continue; }
      if (frame->pts <= last_capture_pts) frame->pts = last_capture_pts + HnsPerSecond / config_.video.fps;
      last_capture_pts = frame->pts;
      const auto before = stream_description();
      if (before.width && (before.width != (frame->width & ~1u) || before.height != (frame->height & ~1u))) {
        log_warn("CAPTURE_SIZE_CHANGED", std::to_string(before.width) + "x" + std::to_string(before.height) + " -> " + std::to_string(frame->width) + "x" + std::to_string(frame->height));
        if (recording_->state() != RecordingState::Stopped) recording_->stop();
        replay_->reset();
        encoder_->stop();
        last_capture_pts = 0;
      }
      encoder_->encode(*frame);
      { std::scoped_lock lock(stream_mutex_); stream_ = encoder_->description(); if (audio_ && audio_->running()) { auto ad = audio_->audio_description(); stream_.has_audio = ad.has_audio; stream_.audio_rate = ad.audio_rate; stream_.audio_channels = ad.audio_channels; stream_.audio_user_data = std::move(ad.audio_user_data); } }
      if (recording_->needs_keyframe()) encoder_->request_keyframe();
      DWORD pid{}; GetWindowThreadProcessId(capture_->hwnd(), &pid);
      if (audio_->running() && pid && audio_pid_ != pid) { audio_->stop(); audio_failed_ = false; }
      bool audio_enabled{}, microphone_enabled{}; std::string audio_mode;
      { std::scoped_lock lock(settings_mutex_); audio_enabled = config_.audio.enabled; microphone_enabled = config_.audio.microphone; audio_mode = microphone_enabled ? "microphone" : config_.audio.mode; }
      audio_enabled = audio_enabled || microphone_enabled;
      if (audio_->running() && active_audio_mode_ != audio_mode) { audio_->stop(); audio_failed_ = false; }
      if (!audio_enabled || audio_mode == "disabled") { if (audio_->running()) audio_->stop(); }
      if (audio_enabled && audio_mode != "disabled" && !audio_->running() && !audio_failed_) {
        if (pid) { try { audio_->start(pid, audio_mode, [this](EncodedSample s){ on_encoded(std::move(s)); }); audio_pid_ = pid; active_audio_mode_ = audio_mode; auto ad = audio_->audio_description(); std::scoped_lock lock(stream_mutex_); stream_.has_audio = ad.has_audio; stream_.audio_rate = ad.audio_rate; stream_.audio_channels = ad.audio_channels; stream_.audio_user_data = std::move(ad.audio_user_data); } catch (const std::exception& e) { audio_failed_ = true; log_error("AUDIO_CAPTURE_DISABLED", e.what()); } }
      }
      consecutive_failures = 0;
    } catch (const std::exception& e) { log_error("VIDEO_PIPELINE_ERROR", e.what()); if (++consecutive_failures >= 3) { try { recover_pipeline(); consecutive_failures = 0; last_capture_pts = 0; } catch (const std::exception& recovery) { log_error("PIPELINE_RECOVERY_FAILED", recovery.what()); } } }
  }
}
void Application::recover_pipeline() {
  log_warn("PIPELINE_RECOVERY_STARTED"); capture_->stop(); audio_->stop(); encoder_->stop(); frames_.clear();
  if (recording_->state() != RecordingState::Stopped) recording_->stop();
  replay_->reset();
  d3d_.recreate();
  audio_failed_ = false; audio_pid_ = 0; active_audio_mode_.clear();
  capture_->start();
  { std::scoped_lock lock(stream_mutex_); stream_ = {}; } log_info("PIPELINE_RECOVERY_COMPLETED");
}
void Application::on_encoded(EncodedSample sample) { replay_->on_sample(sample); recording_->on_sample(sample); }
StreamDescription Application::stream_description() const { std::scoped_lock lock(stream_mutex_); return stream_; }
EvidenceMetadata Application::parse_metadata(const nlohmann::json& request) const {
  EvidenceMetadata m; auto it = request.find("metadata"); if (it == request.end() || !it->is_object()) return m;
  m.admin = it->value("admin", ""); m.target_id = it->value("target_id", -1); m.target_name = it->value("target_name", ""); m.server = it->value("server", ""); m.timestamp = it->value("timestamp", "");
  m.punishment_command = it->value("punishment_command", ""); m.punishment_reason = it->value("punishment_reason", ""); m.recording_period = it->value("recording_period", ""); return m;
}
void Application::collect_save_result() {
  std::scoped_lock lock(save_mutex_); if (save_future_.valid() && save_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) last_save_ = save_future_.get();
}
nlohmann::json Application::handle_command(const nlohmann::json& request) {
  collect_save_result(); const auto command = request.value("command", "");
  if (command == "record_start") { auto desc = stream_description(); if (!desc.width) return {{"success", false}, {"error", "CAPTURE_NOT_READY"}, {"message", "Game capture is not ready"}}; const bool ok = recording_->start(parse_metadata(request), desc); if (ok) encoder_->request_keyframe(); return {{"success", ok}, {"message", ok ? "Recording start requested" : "Recording already active"}}; }
  if (command == "record_stop") {
    if (recording_->state() == RecordingState::Stopped) return {{"success", false}, {"error", "NOT_RECORDING"}, {"message", "Recording is not active"}};
    auto path = recording_->stop(); nlohmann::json response{{"success", !path.empty()}, {"message", path.empty() ? "Recording stop failed" : "Recording stopped"}, {"path", path.string()}};
    const bool upload_requested = request.value("upload", true);
    bool broker_enabled{}; { std::scoped_lock lock(settings_mutex_); broker_enabled = config_.broker.enabled; }
    if (!path.empty() && upload_requested && broker_enabled && broker_) {
      const auto id = upload_queue_->enqueue(path, parse_metadata(request));
      response["upload_queued"] = true; response["upload_id"] = id; log_info("BROKER_UPLOAD_QUEUED", "path=\"" + path.string() + "\"");
    } else response["upload_queued"] = false;
    if (!path.empty()) upload_queue_->enforce_archive_limit(
        config_.recording.directory,
        static_cast<std::uint64_t>(config_.recording.archive_limit_gb) * 1024ull * 1024ull * 1024ull,
        path);
    return response;
  }
  if (command == "settings") {
    std::scoped_lock lock(settings_mutex_);
    return {{"success", true}, {"youtube_enabled", config_.broker.enabled}, {"youtube_configured", !config_.broker.endpoint.empty()}, {"youtube_channel", config_.broker.channel_title}, {"audio_enabled", config_.audio.enabled}, {"microphone_enabled", config_.audio.microphone}, {"microphone_supported", true}, {"archive_limit_gb", config_.recording.archive_limit_gb}};
  }
  if (command == "youtube_channel") {
    Config::Broker settings; { std::scoped_lock lock(settings_mutex_); settings = config_.broker; }
    const auto channel = BrokerUploader(std::move(settings)).channel();
    return {{"success", channel.success}, {"channel_id", channel.channel_id}, {"channel_title", channel.title}, {"error", channel.error}, {"message", channel.success ? "YouTube channel found" : channel.error}};
  }
  if (command == "settings_set") {
    const auto values = request.find("settings"); if (values == request.end() || !values->is_object()) return {{"success", false}, {"error", "INVALID_SETTINGS"}, {"message", "settings object is required"}};
    std::scoped_lock lock(settings_mutex_);
    if (values->contains("youtube_enabled")) config_.broker.enabled = values->value("youtube_enabled", config_.broker.enabled);
    if (values->contains("audio_enabled")) config_.audio.enabled = values->value("audio_enabled", config_.audio.enabled);
    if (values->contains("microphone_enabled")) config_.audio.microphone = values->value("microphone_enabled", config_.audio.microphone);
    if (config_.audio.microphone) config_.audio.enabled = false;
    else if (config_.audio.enabled) config_.audio.microphone = false;
    if (values->contains("archive_limit_gb")) config_.recording.archive_limit_gb = values->value("archive_limit_gb", config_.recording.archive_limit_gb);
    try { config_.save(executable_directory() / "grecord" / "config.json"); } catch (const std::exception& e) { return {{"success", false}, {"error", "CONFIG_WRITE_FAILED"}, {"message", e.what()}}; }
    { std::scoped_lock upload_lock(upload_mutex_); broker_ = std::make_unique<BrokerUploader>(config_.broker); upload_queue_->configure(config_.broker); }
    return {{"success", true}, {"message", "Settings saved"}, {"youtube_enabled", config_.broker.enabled}, {"youtube_configured", !config_.broker.endpoint.empty()}, {"audio_enabled", config_.audio.enabled}, {"microphone_enabled", config_.audio.microphone}, {"microphone_supported", true}};
  }
  if (command == "replay_start") { const bool ok = replay_->start(); return {{"success", ok}, {"message", ok ? "Replay buffer started" : "Replay buffer already active"}}; }
  if (command == "marker") {
    if (recording_->state() == RecordingState::Stopped) return {{"success", false}, {"error", "NOT_RECORDING"}};
    const auto file = config_.recording.directory.parent_path() / "markers.jsonl";
    std::ofstream out(file, std::ios::app);
    if (!out) return {{"success", false}, {"error", "MARKER_WRITE_FAILED"}};
    out << nlohmann::json{{"recording_seconds", recording_->duration_hns() / HnsPerSecond}, {"label", request.value("label", "important")}}.dump() << '\n';
    return {{"success", true}};
  }
  if (command == "replay_stop") { const bool ok = replay_->stop(); return {{"success", ok}, {"message", ok ? "Replay buffer stopped" : "Replay buffer is stopped or saving"}}; }
  if (command == "replay_save") {
    if (replay_->state() != ReplayState::Running) return {{"success", false}, {"error", "REPLAY_NOT_RUNNING"}, {"message", "Replay buffer is not running"}}; auto desc = stream_description(); if (!desc.width) return {{"success", false}, {"error", "CAPTURE_NOT_READY"}, {"message", "No encoded video is available"}};
    std::scoped_lock lock(save_mutex_); if (save_future_.valid()) return {{"success", false}, {"error", "SAVE_IN_PROGRESS"}, {"message", "Replay save is already in progress"}}; last_save_.reset(); save_future_ = replay_->save(parse_metadata(request), desc); ++save_generation_; return {{"success", true}, {"saving", true}, {"save_id", save_generation_}, {"message", "Replay save started"}};
  }
  if (command == "status") {
    nlohmann::json j{{"success", true}, {"capture", capture_->state() == CaptureState::Running}, {"capture_state", std::string(to_string(capture_->state()))}, {"game_found", capture_->hwnd() != nullptr}, {"recording", recording_->state() == RecordingState::Recording || recording_->state() == RecordingState::WaitingForKeyframe}, {"recording_state", std::string(to_string(recording_->state()))}, {"recording_seconds", recording_->duration_hns() / HnsPerSecond}, {"replay_buffer", replay_->state() != ReplayState::Stopped && replay_->state() != ReplayState::Error}, {"replay_state", std::string(to_string(replay_->state()))}, {"replay_seconds", config_.replay.seconds}, {"replay_available_seconds", replay_->available_hns() / HnsPerSecond}, {"encoder", encoder_->encoder_name()}, {"fps", config_.video.fps}, {"dropped_frames", dropped_frames_.load()}, {"save_id", save_generation_}};
    auto d = stream_description(); j["width"] = d.width; j["height"] = d.height; j["audio"] = audio_->running();
    { std::scoped_lock lock(settings_mutex_); j["youtube_enabled"] = config_.broker.enabled; j["youtube_configured"] = !config_.broker.endpoint.empty(); j["youtube_channel"] = config_.broker.channel_title; j["audio_enabled"] = config_.audio.enabled; j["microphone_enabled"] = config_.audio.microphone; j["microphone_supported"] = true; }
    { auto upload = upload_queue_->status(); j["youtube_upload_in_progress"] = upload.value("in_progress", false); j["youtube_upload_sent_bytes"] = upload.value("sent_bytes", 0ull); j["youtube_upload_total_bytes"] = upload.value("total_bytes", 0ull); j["youtube_upload_percent"] = upload.value("percent", 0u); j["youtube_upload_pending"] = upload.value("pending", 0u); j["youtube_last_upload_success"] = upload.value("last_success", false); j["youtube_last_upload_url"] = upload.value("last_url", ""); j["youtube_last_upload_error"] = upload.value("last_error", ""); }
    { std::scoped_lock lock(save_mutex_); j["save_in_progress"] = save_future_.valid(); if (last_save_) { j["last_save_success"] = last_save_->success; j["last_save_path"] = last_save_->path.string(); j["last_save_duration_seconds"] = last_save_->duration_hns / static_cast<double>(HnsPerSecond); j["last_save_error"] = last_save_->error; } } return j;
  }
  return {{"success", false}, {"error", "UNKNOWN_COMMAND"}, {"message", "Unknown command"}};
}
void Application::run() { using namespace std::chrono_literals; while (!stop_) { collect_save_result(); std::this_thread::sleep_for(100ms); } }
void Application::request_stop() { stop_ = true; }
}
