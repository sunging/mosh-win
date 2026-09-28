/* Native mosh-client entry point. GPL-3.0-or-later */
#include "stmclient_win.h"

#include "crypto.h"
#include "platform/win32_crypto.h"
#include "platform/win32_socket.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

void print_version(FILE *stream) {
  std::fputs("mosh-client (mosh " MOSH_WIN_VERSION ")\n"
             "Copyright 2012 Keith Winstein and contributors\n"
             "License GPLv3+: GNU GPL version 3 or later.\n",
             stream);
}

void print_usage(FILE *stream, const char *program) {
  print_version(stream);
  std::fprintf(stream, "\nUsage: %s [-v] IP PORT\n       %s -c\n", program,
               program);
}

bool decimal_port(const char *value) {
  if (value == nullptr || *value == '\0') {
    return false;
  }
  return std::all_of(value, value + std::strlen(value), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

} // namespace

int main(int argc, char **argv) {
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  Crypto::disable_dumping_core();

  unsigned int verbose = 0;
  int index = 1;
  while (index < argc) {
    if (std::strcmp(argv[index], "--help") == 0) {
      print_usage(stdout, argv[0]);
      return 0;
    }
    if (std::strcmp(argv[index], "--version") == 0) {
      print_version(stdout);
      return 0;
    }
    if (std::strcmp(argv[index], "-c") == 0) {
      std::puts("256");
      return 0;
    }
    if (std::strcmp(argv[index], "-v") == 0) {
      ++verbose;
      ++index;
      continue;
    }
    if (std::strcmp(argv[index], "-#") == 0 && index + 1 < argc) {
      index += 2;
      continue;
    }
    if (std::strcmp(argv[index], "--") == 0) {
      ++index;
    }
    break;
  }

  if (argc - index != 2 || !decimal_port(argv[index + 1])) {
    print_usage(stderr, argv[0]);
    return 2;
  }
  const char *environment_key = std::getenv("MOSH_KEY");
  if (environment_key == nullptr) {
    std::fputs("MOSH_KEY environment variable not found.\n", stderr);
    return 1;
  }
  std::string key(environment_key);
  _putenv_s("MOSH_KEY", "");

  bool success = false;
  try {
    mosh::win32::WinsockRuntime winsock;
    STMClientWin client(argv[index], argv[index + 1], key.c_str(),
                        std::getenv("MOSH_PREDICTION_DISPLAY"), verbose,
                        std::getenv("MOSH_PREDICTION_OVERWRITE"));
    mosh::win32::secure_erase(key.data(), key.size());
    client.init();
    success = client.main_loop();
    client.shutdown();
  } catch (const std::exception &error) {
    mosh::win32::secure_erase(key.data(), key.size());
    std::fprintf(stderr, "mosh-client: %s\n", error.what());
  }

  std::puts("[mosh is exiting.]");
  return success ? 0 : 1;
}
