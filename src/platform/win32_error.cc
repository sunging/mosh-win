/* GPL-3.0-or-later */
#include "win32_error.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits>

namespace mosh::win32 {

std::string system_error_text(std::uint32_t error) {
  wchar_t *message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(error), 0,
      reinterpret_cast<wchar_t *>(&message), 0, nullptr);
  if (length == 0 || message == nullptr) {
    return {};
  }

  std::wstring wide(message, length);
  LocalFree(message);
  while (!wide.empty() &&
         (wide.back() == L'\r' || wide.back() == L'\n' ||
          wide.back() == L' ' || wide.back() == L'.')) {
    wide.pop_back();
  }
  if (wide.empty() ||
      wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }

  const int wide_length = static_cast<int>(wide.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length,
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, result.data(),
                          size, nullptr, nullptr) != size) {
    return {};
  }
  return result;
}

} // namespace mosh::win32
