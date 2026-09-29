// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string Utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) {
    return "<conversion-error>";
  }
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), count,
                      nullptr, nullptr);
  return result;
}

bool HasArgument(int argc, wchar_t** argv, const wchar_t* wanted) {
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == wanted) {
      return true;
    }
  }
  return false;
}

bool EnvironmentEquals(const wchar_t* name, const wchar_t* expected) {
  wchar_t value[128]{};
  const DWORD count = GetEnvironmentVariableW(
      name, value, static_cast<DWORD>(std::size(value)));
  return count > 0 && count < std::size(value) &&
         std::wstring(value) == expected;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (HasArgument(argc, argv, L"-G")) {
    std::puts("host fake-alias");
    std::puts("hostname 203.0.113.19");
    std::puts("port 22");
    return 0;
  }

  if (HasArgument(argc, argv, L"--echo-args")) {
    for (int i = 0; i < argc; ++i) {
      const std::string value = Utf8(argv[i]);
      std::printf("ARG%d:%s\n", i, value.c_str());
    }
    return 0;
  }

  const DWORD inherited_key_size =
      GetEnvironmentVariableW(L"MOSH_KEY", nullptr, 0);
  if (HasArgument(argc, argv, L"--check-client-env") ||
      (argc == 3 && inherited_key_size > 0)) {
    std::vector<wchar_t> key(256);
    const DWORD count = GetEnvironmentVariableW(
        L"MOSH_KEY", key.data(), static_cast<DWORD>(key.size()));
    bool safe = count > 0 && count < key.size();
    if (safe) {
      for (int i = 0; i < argc; ++i) {
        if (std::wstring(argv[i]).find(key.data()) != std::wstring::npos) {
          safe = false;
        }
      }
    }
    if (GetEnvironmentVariableW(L"MOSH_FAKE_EXPECT_FULL_ENV", nullptr, 0) >
        0) {
      safe = safe &&
             EnvironmentEquals(L"MOSH_PREDICTION_DISPLAY", L"always") &&
             EnvironmentEquals(L"MOSH_PREDICTION_OVERWRITE", L"yes") &&
             EnvironmentEquals(L"MOSH_NO_TERM_INIT", L"1");
    }
    SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    std::puts(safe ? "SAFE" : "UNSAFE");
    return safe ? 0 : 7;
  }

  wchar_t mode[64]{};
  GetEnvironmentVariableW(L"MOSH_FAKE_SSH_MODE", mode,
                          static_cast<DWORD>(std::size(mode)));
  std::puts("fake ssh banner");
  if (std::wstring(mode) == L"malformed") {
    std::puts("MOSH CONNECT not-a-port short");
    return 0;
  }
  std::puts(
      "MOSH SSH_CONNECTION 198.51.100.12 55123 203.0.113.7 22");
  std::puts("MOSH CONNECT 60001 QUJDREVGR0hJSktMTU5PUA");
  if (std::wstring(mode) == L"duplicate") {
    std::puts("MOSH CONNECT 60002 QUJDREVGR0hJSktMTU5PUA");
  }
  if (std::wstring(mode) == L"post-connect-failure") {
    return 255;
  }
  return 0;
}
