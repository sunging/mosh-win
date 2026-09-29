/* Native implementation of Mosh util/locale_utils.cc. SPDX-License-Identifier: GPL-3.0-or-later */
#include "locale_utils.h"

#include <clocale>
#include <cstdlib>
#include <string>

const std::string LocaleVar::str(void) const {
  return name.empty() ? std::string("[no charset variables]")
                      : name + "=" + value;
}

const LocaleVar get_ctype(void) {
  if (const char *value = std::getenv("LC_ALL")) {
    return LocaleVar("LC_ALL", value);
  }
  if (const char *value = std::getenv("LC_CTYPE")) {
    return LocaleVar("LC_CTYPE", value);
  }
  if (const char *value = std::getenv("LANG")) {
    return LocaleVar("LANG", value);
  }
  return LocaleVar("", "");
}

const char *locale_charset(void) { return "UTF-8"; }

bool is_utf8_locale(void) { return true; }

void set_native_locale(void) {
  /* UCRT accepts .UTF-8 independently of the user's POSIX locale variables. */
  if (std::setlocale(LC_ALL, ".UTF-8") == nullptr) {
    std::setlocale(LC_ALL, "C");
  }
}

void clear_locale_variables(void) {
  static const char *const names[] = {
      "LANG",        "LANGUAGE",     "LC_CTYPE",   "LC_NUMERIC",
      "LC_TIME",     "LC_COLLATE",   "LC_MONETARY", "LC_MESSAGES",
      "LC_PAPER",    "LC_NAME",      "LC_ADDRESS", "LC_TELEPHONE",
      "LC_MEASUREMENT", "LC_IDENTIFICATION", "LC_ALL"};
  for (const char *name : names) {
    _putenv_s(name, "");
  }
}
