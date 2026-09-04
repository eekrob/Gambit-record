#include "logging/Logger.hpp"
#include <Windows.h>
#include <format>
#include <iostream>

namespace evidence {
Logger& Logger::instance() { static Logger logger; return logger; }
void Logger::initialize(const std::filesystem::path& directory, std::string_view level) {
  std::scoped_lock lock(mutex_);
  std::filesystem::create_directories(directory);
  if (level == "debug") minimum_ = LogLevel::Debug; else if (level == "warn") minimum_ = LogLevel::Warn; else if (level == "error") minimum_ = LogLevel::Error;
  file_.open(directory / "grecord.log", std::ios::app);
  if (!file_) throw std::runtime_error("cannot open grecord.log");
}
void Logger::log(LogLevel level, std::string_view event, std::string_view detail) {
  if (level < minimum_) return;
  static constexpr std::string_view names[]{"DEBUG", "INFO", "WARN", "ERROR"};
  SYSTEMTIME local{}; GetLocalTime(&local);
  const auto line = std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] {} {}{}{}", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, names[static_cast<int>(level)], event, detail.empty() ? "" : " ", detail);
  std::scoped_lock lock(mutex_); std::clog << line << '\n'; if (file_) { file_ << line << '\n'; file_.flush(); }
}
}
