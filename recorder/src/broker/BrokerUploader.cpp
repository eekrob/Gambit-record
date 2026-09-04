#include "broker/BrokerUploader.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

#ifndef GRECORD_BROKER_KEY
#define GRECORD_BROKER_KEY ""
#endif

using json = nlohmann::json;
namespace evidence {
namespace {
struct Handle {
  HINTERNET value{};
  explicit Handle(HINTERNET h = nullptr) : value(h) {}
  ~Handle() { if (value) WinHttpCloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept { if (this != &other) { if (value) WinHttpCloseHandle(value); value = std::exchange(other.value, nullptr); } return *this; }
  operator HINTERNET() const { return value; }
};
std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!n) return {};
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
  return out;
}
std::string read_response(HINTERNET request) {
  std::string result;
  DWORD available{};
  while (WinHttpQueryDataAvailable(request, &available) && available) {
    std::string block(available, '\0'); DWORD read{};
    if (!WinHttpReadData(request, block.data(), available, &read) || !read) break;
    block.resize(read); result += block;
  }
  return result;
}
DWORD response_status(HINTERNET request) {
  DWORD code{}, size = sizeof(code);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
  return code;
}
struct Endpoint { std::wstring host; std::wstring prefix; INTERNET_PORT port{}; DWORD flags{}; };
bool parse_endpoint(const std::string& source, Endpoint& endpoint) {
  auto url = widen(source); URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
  endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
  endpoint.prefix.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  while (endpoint.prefix.size() > 1 && endpoint.prefix.back() == L'/') endpoint.prefix.pop_back();
  endpoint.port = parts.nPort; endpoint.flags = WINHTTP_FLAG_SECURE;
  return true;
}
struct Response { DWORD status{}; std::string body; };
Response request(HINTERNET connection, const Endpoint& endpoint, const wchar_t* method,
                 const std::wstring& path, const std::wstring& extra_headers,
                 const void* body, DWORD size) {
  Handle req{WinHttpOpenRequest(connection, method, (endpoint.prefix + path).c_str(), nullptr,
                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.flags)};
  if (!req) return {};
  std::wstring headers = L"X-GRecord-Key: " + widen(GRECORD_BROKER_KEY) + L"\r\n" + extra_headers;
  if (!WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1L), const_cast<void*>(body), size, size, 0)
      || !WinHttpReceiveResponse(req, nullptr)) return {};
  return {response_status(req), read_response(req)};
}
std::string json_string(const std::string& body, const char* key) {
  try { return json::parse(body).value(key, ""); } catch (...) { return {}; }
}
} // namespace

std::string BrokerUploader::video_title(const EvidenceMetadata& metadata) {
  const auto admin = metadata.admin.empty() ? "Unknown_Admin" : metadata.admin;
  auto stamp = metadata.timestamp;
  std::replace(stamp.begin(), stamp.end(), '_', ' ');
  if (stamp.size() >= 19) {
    std::replace(stamp.begin() + 11, stamp.end(), ':', '-');
    return admin + " | " + stamp.substr(0, 10) + " | " + stamp.substr(11, 8);
  }
  return admin + " | " + stamp;
}

std::string BrokerUploader::sha256(const std::filesystem::path& file) {
  BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  DWORD object_size{}, cb{};
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &cb, 0) != 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return {}; }
  std::vector<unsigned char> object(object_size); std::array<unsigned char, 32> digest{};
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return {}; }
  std::ifstream input(file, std::ios::binary); std::array<char, 1024 * 1024> buffer{};
  while (input) { input.read(buffer.data(), buffer.size()); const auto got = input.gcount(); if (got > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0); }
  const bool ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {};
  static constexpr char hex[] = "0123456789abcdef"; std::string result; result.reserve(64);
  for (const auto value : digest) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 15]); }
  return result;
}

UploadResult BrokerUploader::upload(const std::filesystem::path& file, const EvidenceMetadata& metadata,
                                    ProgressCallback progress, std::string resume_id, std::stop_token stop) const {
  UploadResult result;
  if (!settings_.enabled) { result.error = "Broker upload is disabled"; return result; }
  if (std::string_view(GRECORD_BROKER_KEY).empty()) { result.error = "This build has no broker key"; return result; }
  std::error_code ec; const auto total = std::filesystem::file_size(file, ec);
  if (ec || !total) { result.error = "recording file is missing"; return result; }
  if (total > 4ull * 1024 * 1024 * 1024) { result.error = "recording exceeds the 4 GiB broker limit"; return result; }
  Endpoint endpoint; if (!parse_endpoint(settings_.endpoint, endpoint)) { result.error = "invalid HTTPS broker endpoint"; return result; }
  Handle session{WinHttpOpen(L"GambitRecord/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) { result.error = "WinHTTP initialization failed"; return result; }
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
  Handle connection{WinHttpConnect(session, endpoint.host.c_str(), endpoint.port, 0)};
  if (!connection) { result.error = "broker unavailable"; return result; }

  const auto hash = sha256(file);
  const auto description = "Server: " + metadata.server + "\nObserved player: " + metadata.target_name +
      (metadata.target_id >= 0 ? " [" + std::to_string(metadata.target_id) + "]" : "") +
      "\nCommand: " + metadata.punishment_command + "\nReason: " + metadata.punishment_reason +
      "\nRecording period: " + metadata.recording_period + "\nGambit Record 1.0.0\nSHA-256: " + hash;
  std::uint64_t offset{};
  result.upload_id = std::move(resume_id);
  if (!result.upload_id.empty()) {
    auto resumed = request(connection, endpoint, L"GET", L"/v1/uploads/" + widen(result.upload_id), {}, nullptr, 0);
    if (resumed.status == 200) {
      const auto completed_url = json_string(resumed.body, "url");
      if (!completed_url.empty()) { result.success = true; result.url = completed_url; result.video_id = json_string(resumed.body, "videoId"); return result; }
      try { offset = std::stoull(json_string(resumed.body, "nextOffset")); } catch (...) { offset = 0; }
      if (offset > total) offset = 0;
    } else if (resumed.status == 404 || resumed.status == 410) result.upload_id.clear();
    else { result.error = "broker resume failed (HTTP " + std::to_string(resumed.status) + ")"; return result; }
  }
  if (result.upload_id.empty()) {
    const json create_body = {{"size", total}, {"contentType", "video/mp4"}, {"title", video_title(metadata)},
                              {"description", description}, {"privacyStatus", "unlisted"}, {"sha256", hash}};
    const auto create_text = create_body.dump();
    auto created = request(connection, endpoint, L"POST", L"/v1/uploads", L"Content-Type: application/json\r\n",
                           create_text.data(), static_cast<DWORD>(create_text.size()));
    if (created.status != 201) { result.error = "broker session failed (HTTP " + std::to_string(created.status) + "): " + created.body.substr(0, 240); return result; }
    result.upload_id = json_string(created.body, "id");
    if (result.upload_id.empty()) { result.error = "broker returned no upload id"; return result; }
  }

  std::ifstream input(file, std::ios::binary); constexpr std::uint64_t chunk_size = 8ull * 1024 * 1024;
  std::vector<char> buffer(static_cast<std::size_t>(chunk_size)); input.seekg(static_cast<std::streamoff>(offset));
  if (progress) progress(offset, total);
  while (offset < total) {
    if (stop.stop_requested()) { result.error = "upload cancelled during shutdown"; return result; }
    const auto count = std::min<std::uint64_t>(chunk_size, total - offset);
    input.read(buffer.data(), static_cast<std::streamsize>(count));
    if (static_cast<std::uint64_t>(input.gcount()) != count) { result.error = "recording read failed"; return result; }
    const std::wstring headers = L"Content-Type: video/mp4\r\nContent-Range: bytes " + std::to_wstring(offset) +
        L"-" + std::to_wstring(offset + count - 1) + L"/" + std::to_wstring(total) + L"\r\n";
    auto uploaded = request(connection, endpoint, L"PUT", L"/v1/uploads/" + widen(result.upload_id), headers,
                            buffer.data(), static_cast<DWORD>(count));
    if (uploaded.status == 200 || uploaded.status == 201) {
      result.video_id = json_string(uploaded.body, "videoId"); result.url = json_string(uploaded.body, "url");
      offset += count; if (progress) progress(offset, total); break;
    }
    if (uploaded.status != 308) { result.error = "broker upload failed (HTTP " + std::to_string(uploaded.status) + "): " + uploaded.body.substr(0, 240); return result; }
    auto next = json_string(uploaded.body, "nextOffset");
    try { offset = next.empty() ? offset + count : std::stoull(next); } catch (...) { offset += count; }
    input.clear(); input.seekg(static_cast<std::streamoff>(offset)); if (progress) progress(offset, total);
  }
  if (result.url.empty() && !result.video_id.empty()) result.url = "https://youtu.be/" + result.video_id;
  result.success = !result.url.empty(); if (!result.success) result.error = "broker returned no video URL";
  return result;
}

ChannelResult BrokerUploader::channel() const {
  Endpoint endpoint; if (!parse_endpoint(settings_.endpoint, endpoint)) return {false, {}, {}, "invalid HTTPS broker endpoint"};
  Handle session{WinHttpOpen(L"GambitRecord/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) return {false, {}, {}, "WinHTTP initialization failed"};
  Handle connection{WinHttpConnect(session, endpoint.host.c_str(), endpoint.port, 0)};
  if (!connection) return {false, {}, {}, "broker unavailable"};
  auto response = request(connection, endpoint, L"GET", L"/v1/channel", {}, nullptr, 0);
  if (response.status != 200) return {false, {}, {}, "broker channel request failed"};
  try { auto body = json::parse(response.body); return {true, body.value("id", ""), body.value("title", ""), {}}; }
  catch (const std::exception& e) { return {false, {}, {}, e.what()}; }
}

} // namespace evidence
