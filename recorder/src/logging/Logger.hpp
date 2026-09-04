#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace evidence {
enum class LogLevel { Debug, Info, Warn, Error };
class Logger {
public:
  static Logger& instance();
  void initialize(const std::filesystem::path& directory, std::string_view level);
  void log(LogLevel level, std::string_view event, std::string_view detail = {});
private:
  std::mutex mutex_; std::ofstream file_; LogLevel minimum_{LogLevel::Info};
};
inline void log_info(std::string_view e, std::string_view d = {}) { Logger::instance().log(LogLevel::Info, e, d); }
inline void log_warn(std::string_view e, std::string_view d = {}) { Logger::instance().log(LogLevel::Warn, e, d); }
inline void log_error(std::string_view e, std::string_view d = {}) { Logger::instance().log(LogLevel::Error, e, d); }
}

