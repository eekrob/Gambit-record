#pragma once
#include "common/Media.hpp"
#include <filesystem>
#include <string>

namespace evidence {
std::string sanitize_filename(std::string value);
std::string timestamp_for_filename();
std::filesystem::path evidence_filename(const std::filesystem::path& directory, const EvidenceMetadata& metadata, std::string_view suffix);
bool is_path_within(const std::filesystem::path& child, const std::filesystem::path& parent);
std::filesystem::path executable_directory();
}

