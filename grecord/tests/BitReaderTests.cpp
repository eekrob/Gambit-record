#include "grecord/BitReader.hpp"
#include <cassert>

int main() {
  const unsigned char aligned[]{0x12, 0x34, 0xAB};
  grecord::BitReader reader(aligned, sizeof(aligned));
  std::uint8_t first{}; std::uint16_t rest{};
  assert(reader.read(first) && first == 0x12);
  assert(reader.read(rest));
#if defined(_WIN32)
  assert(rest == 0xAB34);
#endif
  return 0;
}
