#include "config/Config.hpp"
#include "common/Files.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;
namespace evidence {
namespace {
std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n); return out;
}
json default_json() {
  return {
    {"capture", {{"process", "gta_sa.exe"}, {"backend", "windows_graphics_capture"}}},
    {"video", {{"fps", 60}, {"bitrate", 16000000}, {"codec", "h264"}, {"prefer_hardware_encoder", true}, {"keyframe_interval_seconds", 2}, {"queue_frames", 6}}},
    {"audio", {{"enabled", true}, {"mode", "process"}, {"microphone", false}}},
    {"broker", {{"enabled", false}, {"endpoint", "https://upload.gambit-record.invalid"}, {"channel_title", "Gambit Record"}, {"retry_seconds", 30}}},
    {"recording", {{"directory", "grecord/records"}, {"archive_limit_gb", 20}}},
    {"replay", {{"enabled", true}, {"auto_start", true}, {"seconds", 600}, {"storage", "disk"}, {"cache_directory", "grecord/cache"}, {"segment_seconds", 10}}},
    {"logging", {{"directory", "grecord/logs"}, {"level", "info"}}}
  };
}
template<class T> T value_at(const json& j, std::string_view group, std::string_view key, T fallback) {
  auto g = j.find(group); if (g == j.end() || !g->is_object()) return fallback;
  auto v = g->find(key); if (v == g->end()) return fallback;
  return v->get<T>();
}
}

Config Config::load_or_create(const std::filesystem::path& path) {
  json j;
  if (!std::filesystem::exists(path)) {
    j = default_json();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path); out << j.dump(2) << '\n';
  } else {
    std::ifstream in(path);
    try { in >> j; } catch (const std::exception& e) { throw std::runtime_error(std::string("Invalid config JSON: ") + e.what()); }
  }
  Config c;
  c.capture.process = widen(value_at(j, "capture", "process", std::string("gta_sa.exe")));
  c.capture.backend = value_at(j, "capture", "backend", c.capture.backend);
  c.video.fps = value_at(j, "video", "fps", c.video.fps);
  c.video.bitrate = value_at(j, "video", "bitrate", c.video.bitrate);
  c.video.codec = value_at(j, "video", "codec", c.video.codec);
  c.video.prefer_hardware_encoder = value_at(j, "video", "prefer_hardware_encoder", c.video.prefer_hardware_encoder);
  c.video.keyframe_interval_seconds = value_at(j, "video", "keyframe_interval_seconds", c.video.keyframe_interval_seconds);
  c.video.queue_frames = value_at(j, "video", "queue_frames", c.video.queue_frames);
  c.audio.enabled = value_at(j, "audio", "enabled", c.audio.enabled);
  c.audio.mode = value_at(j, "audio", "mode", c.audio.mode);
  c.audio.microphone = value_at(j, "audio", "microphone", c.audio.microphone);
  c.broker.enabled = value_at(j, "broker", "enabled", c.broker.enabled);
  c.broker.endpoint = value_at(j, "broker", "endpoint", c.broker.endpoint);
  c.broker.channel_title = value_at(j, "broker", "channel_title", c.broker.channel_title);
  c.broker.retry_seconds = value_at(j, "broker", "retry_seconds", c.broker.retry_seconds);
  c.recording.directory = value_at(j, "recording", "directory", c.recording.directory.string());
  c.recording.archive_limit_gb = value_at(j, "recording", "archive_limit_gb", c.recording.archive_limit_gb);
  c.replay.enabled = value_at(j, "replay", "enabled", c.replay.enabled);
  c.replay.auto_start = value_at(j, "replay", "auto_start", c.replay.auto_start);
  c.replay.seconds = value_at(j, "replay", "seconds", c.replay.seconds);
  c.replay.storage = value_at(j, "replay", "storage", c.replay.storage);
  c.replay.cache_directory = value_at(j, "replay", "cache_directory", c.replay.cache_directory.string());
  c.replay.segment_seconds = value_at(j, "replay", "segment_seconds", c.replay.segment_seconds);
  c.logging.directory = value_at(j, "logging", "directory", c.logging.directory.string());
  c.logging.level = value_at(j, "logging", "level", c.logging.level);
  return c;
}

void Config::save(const std::filesystem::path& path) const {
  json j = {
    {"capture", {{"process", "gta_sa.exe"}, {"backend", capture.backend}}},
    {"video", {{"fps", video.fps}, {"bitrate", video.bitrate}, {"codec", video.codec}, {"prefer_hardware_encoder", video.prefer_hardware_encoder}, {"keyframe_interval_seconds", video.keyframe_interval_seconds}, {"queue_frames", video.queue_frames}}},
    {"audio", {{"enabled", audio.enabled}, {"mode", audio.mode}, {"microphone", audio.microphone}}},
    {"broker", {{"enabled", broker.enabled}, {"endpoint", broker.endpoint}, {"channel_title", broker.channel_title}, {"retry_seconds", broker.retry_seconds}}},
    {"recording", {{"directory", recording.directory.string()}, {"archive_limit_gb", recording.archive_limit_gb}}},
    {"replay", {{"enabled", replay.enabled}, {"auto_start", replay.auto_start}, {"seconds", replay.seconds}, {"storage", replay.storage}, {"cache_directory", replay.cache_directory.string()}, {"segment_seconds", replay.segment_seconds}}},
    {"logging", {{"directory", logging.directory.string()}, {"level", logging.level}}}
  };
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("Cannot write config: " + path.string());
  out << j.dump(2) << '\n';
}

void Config::resolve_paths(const std::filesystem::path& base) {
  if (recording.directory.is_relative()) recording.directory = base / recording.directory;
  if (replay.cache_directory.is_relative()) replay.cache_directory = base / replay.cache_directory;
  if (logging.directory.is_relative()) logging.directory = base / logging.directory;
}

void Config::validate(const std::filesystem::path& base) const {
  if (video.codec != "h264" || video.fps < 10 || video.fps > 240 || video.bitrate < 500'000 || video.bitrate > 200'000'000) throw std::runtime_error("invalid video configuration");
  if (video.queue_frames < 2 || video.queue_frames > 120) throw std::runtime_error("video.queue_frames out of range");
  if (replay.seconds < 10 || replay.seconds > 3600 || replay.segment_seconds < 1 || replay.segment_seconds > 60) throw std::runtime_error("invalid replay configuration");
  if (replay.storage != "disk" && replay.storage != "memory") throw std::runtime_error("replay.storage must be disk or memory");
  if (audio.mode != "process" && audio.mode != "system" && audio.mode != "disabled") throw std::runtime_error("audio.mode must be process, system, or disabled");
  if (capture.backend != "windows_graphics_capture" && capture.backend != "desktop_duplication") throw std::runtime_error("capture.backend is invalid");
  const auto allowed = std::filesystem::weakly_canonical(base / "grecord");
  if (!is_path_within(replay.cache_directory, allowed)) throw std::runtime_error("cache_directory must remain inside the executable grecord directory");
  if (!is_path_within(recording.directory, allowed)) throw std::runtime_error("recording.directory must remain inside the executable grecord directory");
  if (recording.archive_limit_gb < 1 || recording.archive_limit_gb > 1024) throw std::runtime_error("recording.archive_limit_gb out of range");
  if (broker.retry_seconds < 5 || broker.retry_seconds > 3600) throw std::runtime_error("broker.retry_seconds out of range");
  if (broker.enabled && !broker.endpoint.starts_with("https://")) throw std::runtime_error("broker.endpoint must use HTTPS");
}
}
