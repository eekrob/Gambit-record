#include "common/Files.hpp"
#include <Windows.h>
#include <algorithm>
#include <format>

namespace evidence {
std::string sanitize_filename(std::string value) {
  static constexpr std::string_view invalid = "<>:\"/\\|?*";
  for (char& c : value) if (static_cast<unsigned char>(c) < 32 || invalid.find(c) != std::string_view::npos) c = '_';
  while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
  if (value.empty()) value = "unknown";
  if (value.size() > 80) value.resize(80);
  return value;
}

std::string timestamp_for_filename() {
  SYSTEMTIME local{}; GetLocalTime(&local);
  return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
}

std::filesystem::path evidence_filename(const std::filesystem::path& directory, const EvidenceMetadata& metadata, std::string_view suffix) {
  std::string stem = timestamp_for_filename();
  if (!metadata.target_name.empty()) {
    stem += "_" + sanitize_filename(metadata.target_name);
    if (metadata.target_id >= 0) stem += "_ID" + std::to_string(metadata.target_id);
  }
  stem += "_"; stem += suffix; stem += ".mp4";
  return directory / stem;
}

bool is_path_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
  std::error_code ec1, ec2;
  const auto c = std::filesystem::weakly_canonical(child, ec1);
  const auto p = std::filesystem::weakly_canonical(parent, ec2);
  if (ec1 || ec2) return false;
  auto ci = c.begin(); auto pi = p.begin();
  for (; pi != p.end(); ++pi, ++ci) if (ci == c.end() || *ci != *pi) return false;
  return true;
}

std::filesystem::path executable_directory() {
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(n);
  return std::filesystem::path(path).parent_path();
}
}
