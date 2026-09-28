/* GPL-3.0-or-later */
#pragma once

#ifndef _WIN32
#error "win32_error.h is only available on Windows"
#endif

#include <cstdint>
#include <string>

namespace mosh::win32 {

/*
 * Returns the system message for a Win32 or Winsock error code as UTF-8,
 * without trailing whitespace or a final period.  The console runs in
 * CP_UTF8, so the ANSI FormatMessageA text (for example GBK on a Chinese
 * system) would be shown as mojibake.  Returns an empty string when the
 * system has no text for the code.
 */
[[nodiscard]] std::string system_error_text(std::uint32_t error);

} // namespace mosh::win32
