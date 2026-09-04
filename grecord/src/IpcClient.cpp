#include "grecord/IpcClient.hpp"
#include <Windows.h>
#include <array>
#include <stdexcept>

namespace grecord {
nlohmann::json IpcClient::request(nlohmann::json message, unsigned timeout_ms) const {
  if (!WaitNamedPipeW(pipe_name_.c_str(), timeout_ms))
    return {{"success", false}, {"error", "WORKER_OFFLINE"}};
  HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return {{"success", false}, {"error", "WORKER_OFFLINE"}};
  message["protocol"] = 1;
  const std::string wire = message.dump() + "\n";
  DWORD written{};
  if (!WriteFile(pipe, wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr) || written != wire.size()) {
    CloseHandle(pipe); return {{"success", false}, {"error", "IPC_WRITE_FAILED"}};
  }
  std::string response; std::array<char, 2048> buffer{};
  while (response.size() < 64 * 1024) {
    DWORD read{};
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || !read) break;
    response.append(buffer.data(), read); if (response.find('\n') != std::string::npos) break;
  }
  CloseHandle(pipe);
  try { if (auto nl = response.find('\n'); nl != std::string::npos) response.resize(nl); return nlohmann::json::parse(response); }
  catch (...) { return {{"success", false}, {"error", "INVALID_WORKER_RESPONSE"}}; }
}
} // namespace grecord
