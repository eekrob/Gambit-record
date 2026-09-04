#include "grecord/BitReader.hpp"
#include <algorithm>
#include <cstring>

namespace grecord {

bool BitReader::skip(std::size_t count) {
  if (count > remaining()) return false;
  offset_ += count; return true;
}

bool BitReader::read_bits(void* raw_output, std::size_t count, bool align_right) {
  if (!count || count > remaining()) return false;
  auto* output = static_cast<std::uint8_t*>(raw_output);
  std::memset(output, 0, (count + 7) / 8);
  std::size_t output_offset{};
  const auto initial_mod = offset_ & 7;
  auto left = count;
  while (left) {
    output[output_offset] |= static_cast<std::uint8_t>(data_[offset_ >> 3] << initial_mod);
    if (initial_mod && left > 8 - initial_mod)
      output[output_offset] |= static_cast<std::uint8_t>(data_[(offset_ >> 3) + 1] >> (8 - initial_mod));
    if (left < 8) {
      if (align_right) output[output_offset] >>= 8 - left;
      offset_ += left; left = 0;
    } else {
      offset_ += 8; left -= 8;
    }
    ++output_offset;
  }
  return true;
}

bool BitReader::read_bool(bool& value) {
  if (!remaining()) return false;
  value = (data_[offset_ >> 3] & (0x80 >> (offset_ & 7))) != 0;
  ++offset_; return true;
}

bool BitReader::read_compressed_u32(std::uint32_t& value) {
  auto* output = reinterpret_cast<std::uint8_t*>(&value);
  value = 0;
  int current = 3;
  while (current > 0) {
    bool matched{}; if (!read_bool(matched)) return false;
    if (matched) output[current--] = 0;
    else return read_bits(output, static_cast<std::size_t>(current + 1) * 8);
  }
  bool half{}; if (!read_bool(half)) return false;
  return half ? read_bits(output, 4) : read_bits(output, 8);
}

std::string BitReader::read_string(std::size_t length) {
  if (length > remaining() / 8) return {};
  std::string result(length, '\0');
  if (length && !read_bits(result.data(), length * 8)) return {};
  return result;
}

bool extract_rpc_payload(const char* packet, int length, RpcPayload& result) {
  if (!packet || length <= 0) return false;
  BitReader stream(packet, static_cast<std::size_t>(length));
  std::uint8_t packet_id{}; if (!stream.read(packet_id)) return false;
  // ID_TIMESTAMP in RakNet wraps packet id after an 8-byte timestamp.
  if (packet_id == 40) { if (!stream.skip(8 * (sizeof(std::uint32_t) + 1))) return false; }
  if (!stream.read(result.id)) return false;
  std::uint32_t bits{}; if (!stream.read_compressed_u32(bits) || bits > stream.remaining()) return false;
  result.bytes.assign((bits + 7) / 8, 0); result.bit_count = bits;
  return bits == 0 || stream.read_bits(result.bytes.data(), bits, false);
}

} // namespace grecord
