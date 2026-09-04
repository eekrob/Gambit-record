#pragma once

#include <Windows.h>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace evidence {

// Local-only, newline-delimited JSON IPC. The pipe DACL permits only the
// current logon user and SYSTEM; PIPE_REJECT_REMOTE_CLIENTS rejects SMB use.
class IpcServer {
public:
  using Handler = std::function<nlohmann::json(const nlohmann::json&)>;
  IpcServer(std::wstring pipe_name, Handler handler);
  ~IpcServer();
  void start();
  void stop();
  const std::wstring& pipe_name() const noexcept { return pipe_name_; }

private:
  void run(std::stop_token token);
  void handle_client(HANDLE pipe);
  std::wstring pipe_name_;
  Handler handler_;
  std::jthread thread_;
  std::atomic<HANDLE> active_pipe_{INVALID_HANDLE_VALUE};
};

} // namespace evidence
