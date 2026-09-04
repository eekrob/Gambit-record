#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace grecord {

class IpcClient {
public:
  explicit IpcClient(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}
  nlohmann::json request(nlohmann::json message, unsigned timeout_ms = 1500) const;
  const std::wstring& pipe_name() const noexcept { return pipe_name_; }

private:
  std::wstring pipe_name_;
};

} // namespace grecord
