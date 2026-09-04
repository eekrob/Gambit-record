#include "grecord/Logic.hpp"
#include <Windows.h>

namespace grecord {
std::string cp1251_to_utf8(std::string_view input) {
  if (input.empty()) return {};
  const int wide_size = MultiByteToWideChar(1251, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (!wide_size) return std::string(input);
  std::wstring wide(wide_size, L'\0');
  MultiByteToWideChar(1251, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), wide.data(), wide_size);
  const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
  std::string result(utf8_size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, result.data(), utf8_size, nullptr, nullptr);
  return result;
}
} // namespace grecord
