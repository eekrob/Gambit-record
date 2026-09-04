#include "ipc/IpcServer.hpp"
#include "logging/Logger.hpp"
#include <sddl.h>
#include <array>
#include <stdexcept>

namespace evidence {
namespace {
constexpr DWORD max_message = 64 * 1024;

struct LocalSecurity final {
  PSECURITY_DESCRIPTOR descriptor{};
  SECURITY_ATTRIBUTES attributes{};
  LocalSecurity() {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr))
      throw std::runtime_error("cannot create named-pipe security descriptor");
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
  }
  ~LocalSecurity() { if (descriptor) LocalFree(descriptor); }
};

bool write_all(HANDLE pipe, const char* data, DWORD size) {
  while (size) {
    DWORD written{};
    if (!WriteFile(pipe, data, size, &written, nullptr) || !written) return false;
    data += written;
    size -= written;
  }
  return true;
}
} // namespace

IpcServer::IpcServer(std::wstring pipe_name, Handler handler)
    : pipe_name_(std::move(pipe_name)), handler_(std::move(handler)) {
  if (!pipe_name_.starts_with(LR"(\\.\pipe\GambitRecord-)") || pipe_name_.size() > 240)
    throw std::invalid_argument("invalid Gambit Record pipe name");
}

IpcServer::~IpcServer() { stop(); }

void IpcServer::start() {
  if (!thread_.joinable()) thread_ = std::jthread([this](std::stop_token token) { run(token); });
}

void IpcServer::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  const HANDLE pipe = active_pipe_.exchange(INVALID_HANDLE_VALUE);
  if (pipe != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  HANDLE wake = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
  thread_.join();
}

void IpcServer::run(std::stop_token token) {
  LocalSecurity security;
  log_info("IPC_LISTENING", "named_pipe=GambitRecord");
  while (!token.stop_requested()) {
    HANDLE pipe = CreateNamedPipeW(
        pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, max_message, max_message, 3000, &security.attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      log_error("IPC_START_FAILED", "CreateNamedPipeW error=" + std::to_string(GetLastError()));
      return;
    }
    active_pipe_ = pipe;
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected && !token.stop_requested()) handle_client(pipe);
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    if (active_pipe_.exchange(INVALID_HANDLE_VALUE) == pipe) CloseHandle(pipe);
  }
}

void IpcServer::handle_client(HANDLE pipe) {
  std::string request;
  request.reserve(2048);
  std::array<char, 2048> buffer{};
  while (request.size() < max_message) {
    DWORD got{};
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || !got) break;
    request.append(buffer.data(), got);
    if (request.find('\n') != std::string::npos) break;
  }
  nlohmann::json response;
  try {
    if (request.size() >= max_message) throw std::runtime_error("request too large");
    if (const auto newline = request.find('\n'); newline != std::string::npos) request.resize(newline);
    auto json = nlohmann::json::parse(request);
    if (json.value("protocol", 0) != 1)
      response = {{"success", false}, {"error", "PROTOCOL_MISMATCH"}, {"protocol", 1}};
    else
      response = handler_(json);
  } catch (const std::exception& e) {
    response = {{"success", false}, {"error", "BAD_REQUEST"}, {"message", e.what()}};
  }
  const auto wire = response.dump() + "\n";
  write_all(pipe, wire.data(), static_cast<DWORD>(wire.size()));
}

} // namespace evidence
