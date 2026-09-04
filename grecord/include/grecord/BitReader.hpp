#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace grecord {

class BitReader {
public:
  BitReader(const void* data, std::size_t bytes) : data_(static_cast<const std::uint8_t*>(data)), bits_(bytes * 8) {}
  bool skip(std::size_t count);
  bool read_bits(void* output, std::size_t count, bool align_right = true);
  bool read_bool(bool& value);
  bool read_compressed_u32(std::uint32_t& value);
  template<class T> bool read(T& value) { return read_bits(&value, sizeof(T) * 8, true); }
  std::string read_string(std::size_t length);
  std::size_t offset() const noexcept { return offset_; }
  std::size_t remaining() const noexcept { return bits_ > offset_ ? bits_ - offset_ : 0; }

private:
  const std::uint8_t* data_{};
  std::size_t bits_{};
  std::size_t offset_{};
};

struct RpcPayload { std::uint8_t id{}; std::vector<std::uint8_t> bytes; std::size_t bit_count{}; };
bool extract_rpc_payload(const char* packet, int length, RpcPayload& result);

} // namespace grecord
