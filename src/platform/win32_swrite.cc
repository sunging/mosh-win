/* Native implementation of Mosh util/swrite.cc. SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/types.h>
#include "swrite.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <io.h>

int swrite(int fd, const char *text, ssize_t length) {
  ssize_t written_total = 0;
  const ssize_t requested =
      length >= 0 ? length : static_cast<ssize_t>(std::strlen(text));
  while (written_total < requested) {
    const unsigned int chunk = static_cast<unsigned int>(
        std::min<ssize_t>(requested - written_total, INT_MAX));
    const int written = _write(fd, text + written_total, chunk);
    if (written <= 0) {
      return -1;
    }
    written_total += written;
  }
  return 0;
}
