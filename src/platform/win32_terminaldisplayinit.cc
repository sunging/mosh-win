/* Native VT profile for Terminal::Display. SPDX-License-Identifier: GPL-3.0-or-later */
#include "terminaldisplay.h"

#include <cstdlib>

Terminal::Display::Display(bool /* use_environment */)
    : has_ech(false), has_bce(false), has_title(true), smcup(nullptr),
      rmcup(nullptr) {
  if (std::getenv("MOSH_NO_TERM_INIT") == nullptr) {
    smcup = "\x1b[?1049h";
    rmcup = "\x1b[?1049l";
  }
}
