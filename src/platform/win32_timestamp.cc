/* Native implementation of Mosh util/timestamp.cc. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "timestamp.h"

#include <cstdint>

namespace {
uint64_t cached_milliseconds = uint64_t(-1);
}

uint64_t frozen_timestamp(void) {
  if (cached_milliseconds == uint64_t(-1)) {
    freeze_timestamp();
  }
  return cached_milliseconds;
}

void freeze_timestamp(void) { cached_milliseconds = GetTickCount64(); }
