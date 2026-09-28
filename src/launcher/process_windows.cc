#include "process_windows.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <cwchar>
#include <exception>
#include <map>
#include <memory>
#include <utility>

namespace mosh::launcher {
namespace {

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE value) : value_(value) {}
  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  HANDLE get() const noexcept { return value_; }
  explicit operator bool() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  HANDLE release() noexcept {
    HANDLE result = value_;
    value_ = nullptr;
    return result;
  }
  void reset(HANDLE replacement = nullptr) noexcept {
    if (*this) {
      CloseHandle(value_);
    }
    value_ = replacement;
  }

 private:
  HANDLE value_ = nullptr;
};

class EnvironmentStrings {
 public:
  EnvironmentStrings() : value_(GetEnvironmentStringsW()) {
    if (!value_) {
      throw LauncherError("GetEnvironmentStringsW failed");
    }
  }
  ~EnvironmentStrings() {
    if (value_) {
      FreeEnvironmentStringsW(value_);
    }
  }
  EnvironmentStrings(const EnvironmentStrings&) = delete;
  EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;
  LPWCH get() const noexcept { return value_; }

 private:
  LPWCH value_ = nullptr;
};

class WinsockSession {
 public:
  WinsockSession() {
    WSADATA data{};
    const int status = WSAStartup(MAKEWORD(2, 2), &data);
    if (status != 0) {
      throw LauncherError("WSAStartup failed with error " +
                          std::to_string(status));
    }
    active_ = true;
  }
  ~WinsockSession() {
    if (active_) {
      WSACleanup();
    }
  }
  WinsockSession(const WinsockSession&) = delete;
  WinsockSession& operator=(const WinsockSession&) = delete;

 private:
  bool active_ = false;
};

std::string WindowsError(const char* operation, DWORD error) {
  LPWSTR allocated = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&allocated), 0, nullptr);
  std::string message(operation);
  message += " failed (Windows error ";
  message += std::to_string(error);
  message += ')';
  if (size && allocated) {
    std::wstring text(allocated, allocated + size);
    LocalFree(allocated);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' ||
            text.back() == L' ')) {
      text.pop_back();
    }
    if (!text.empty()) {
      message += ": ";
      try {
        message += WideToUtf8(text);
      } catch (...) {
        message += "(localized error text could not be converted)";
      }
    }
  }
  return message;
}

bool IsRegularFile(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FullPath(const std::wstring& path) {
  DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    throw LauncherError(WindowsError("GetFullPathNameW", GetLastError()));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(needed));
  const DWORD written =
      GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
  if (written == 0 || written >= needed) {
    throw LauncherError(WindowsError("GetFullPathNameW", GetLastError()));
  }
  return std::wstring(buffer.data(), written);
}

std::wstring SearchExecutable(const std::wstring& name) {
  DWORD needed = SearchPathW(nullptr, name.c_str(), nullptr, 0, nullptr, nullptr);
  if (needed == 0) {
    return {};
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1);
  const DWORD written = SearchPathW(nullptr, name.c_str(), nullptr,
                                    static_cast<DWORD>(buffer.size()),
                                    buffer.data(), nullptr);
  if (written == 0 || written >= buffer.size()) {
    return {};
  }
  return std::wstring(buffer.data(), written);
}

std::vector<wchar_t> MutableCommandLine(
    const std::vector<std::wstring>& arguments) {
  const std::wstring command_line = BuildWindowsCommandLine(arguments);
  std::vector<wchar_t> result(command_line.begin(), command_line.end());
  result.push_back(L'\0');
  return result;
}

struct CaseInsensitiveLess {
  bool operator()(const std::wstring& lhs, const std::wstring& rhs) const {
    return _wcsicmp(lhs.c_str(), rhs.c_str()) < 0;
  }
};

std::pair<std::wstring, std::wstring> SplitEnvironmentEntry(
    const std::wstring& entry) {
  const std::size_t search_from = !entry.empty() && entry.front() == L'=' ? 1 : 0;
  const std::size_t separator = entry.find(L'=', search_from);
  if (separator == std::wstring::npos) {
    return {entry, {}};
  }
  return {entry.substr(0, separator), entry.substr(separator + 1)};
}

std::vector<wchar_t> BuildEnvironmentBlock(
    const EnvironmentOverrides& overrides) {
  std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
  EnvironmentStrings environment;
  for (const wchar_t* cursor = environment.get(); *cursor != L'\0';) {
    std::wstring entry(cursor);
    cursor += entry.size() + 1;
    auto [name, value] = SplitEnvironmentEntry(entry);
    variables[std::move(name)] = std::move(value);
  }

  for (const auto& [name, value] : overrides) {
    if (name.empty() || name.find(L'=') != std::wstring::npos ||
        name.find(L'\0') != std::wstring::npos) {
      throw LauncherError("invalid environment variable name");
    }
    if (value) {
      if (value->find(L'\0') != std::wstring::npos) {
        throw LauncherError("environment variable contains NUL");
      }
      variables[name] = *value;
    } else {
      variables.erase(name);
    }
  }

  std::vector<wchar_t> block;
  for (const auto& [name, value] : variables) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (block.size() == 1) {
    block.push_back(L'\0');
  }
  for (auto& [name, value] : variables) {
    if (!value.empty()) {
      SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    }
  }
  return block;
}

void Wipe(EnvironmentOverrides* overrides) noexcept {
  if (!overrides) {
    return;
  }
  for (auto& [name, value] : *overrides) {
    if (value && !value->empty()) {
      SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
      value->clear();
    }
  }
  overrides->clear();
}

void Wipe(std::vector<wchar_t>* buffer) noexcept {
  if (buffer && !buffer->empty()) {
    SecureZeroMemory(buffer->data(), buffer->size() * sizeof(wchar_t));
    buffer->clear();
  }
}

std::uint32_t WaitAndGetExitCode(HANDLE process) {
  const DWORD wait = WaitForSingleObject(process, INFINITE);
  if (wait != WAIT_OBJECT_0) {
    throw LauncherError(WindowsError("WaitForSingleObject", GetLastError()));
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process, &exit_code)) {
    throw LauncherError(WindowsError("GetExitCodeProcess", GetLastError()));
  }
  return exit_code;
}

struct ChildProcess {
  UniqueHandle process;
  UniqueHandle thread;
};

// Starts |executable|.  With |replace_environment| the child receives the
// parent environment merged with |environment|; otherwise it inherits the
// parent environment unchanged.  The merged block and every override value
// may hold the session key, so both are wiped whether or not CreateProcessW
// succeeds.
ChildProcess LaunchProcess(const std::wstring& executable,
                           const std::vector<std::wstring>& arguments,
                           STARTUPINFOW* startup,
                           bool inherit_handles,
                           EnvironmentOverrides* environment,
                           bool replace_environment) {
  if (arguments.empty()) {
    throw LauncherError("process argument vector may not be empty");
  }
  auto command_line = MutableCommandLine(arguments);
  std::vector<wchar_t> environment_block;
  PROCESS_INFORMATION process{};
  try {
    if (replace_environment) {
      environment_block = BuildEnvironmentBlock(*environment);
    }
    const DWORD flags = replace_environment ? CREATE_UNICODE_ENVIRONMENT : 0;
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                        nullptr, inherit_handles ? TRUE : FALSE, flags,
                        replace_environment ? environment_block.data()
                                            : nullptr,
                        nullptr, startup, &process)) {
      throw LauncherError(WindowsError("CreateProcessW", GetLastError()));
    }
  } catch (...) {
    Wipe(&environment_block);
    Wipe(environment);
    throw;
  }
  Wipe(&environment_block);
  Wipe(environment);
  return {UniqueHandle(process.hProcess), UniqueHandle(process.hThread)};
}

// Resolves a user-supplied executable: bare names are searched like the
// shell would, then the result must name an existing file.
std::wstring ResolveExplicitExecutable(const std::wstring& path,
                                       const char* option_name) {
  std::wstring candidate = path;
  if (candidate.find(L'\\') == std::wstring::npos &&
      candidate.find(L'/') == std::wstring::npos) {
    const std::wstring searched = SearchExecutable(candidate);
    if (!searched.empty()) {
      candidate = searched;
    }
  }
  candidate = FullPath(candidate);
  if (!IsRegularFile(candidate)) {
    throw LauncherError(std::string(option_name) +
                        " does not name an existing file: " +
                        WideToUtf8(candidate));
  }
  return candidate;
}

}  // namespace

std::wstring FindOpenSsh(
    const std::optional<std::wstring>& explicit_path) {
  if (explicit_path) {
    return ResolveExplicitExecutable(*explicit_path, "--ssh-path");
  }

  const UINT needed = GetSystemDirectoryW(nullptr, 0);
  if (needed == 0) {
    throw LauncherError(WindowsError("GetSystemDirectoryW", GetLastError()));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1);
  const UINT written =
      GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (written == 0 || written >= buffer.size()) {
    throw LauncherError(WindowsError("GetSystemDirectoryW", GetLastError()));
  }
  std::wstring path(buffer.data(), written);
  path += L"\\OpenSSH\\ssh.exe";
  if (!IsRegularFile(path)) {
    throw LauncherError(
        "Windows OpenSSH was not found in System32; install the OpenSSH "
        "Client optional feature or pass --ssh-path");
  }
  return path;
}

std::wstring FindSiblingMoshClient() {
  std::vector<wchar_t> buffer(260);
  for (;;) {
    const DWORD written = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      throw LauncherError(WindowsError("GetModuleFileNameW", GetLastError()));
    }
    if (written < buffer.size() - 1) {
      buffer.resize(written);
      break;
    }
    if (buffer.size() > 32768) {
      throw LauncherError("launcher executable path is too long");
    }
    buffer.resize(buffer.size() * 2);
  }
  std::wstring path(buffer.begin(), buffer.end());
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    throw LauncherError("could not determine launcher directory");
  }
  path.resize(slash + 1);
  path += L"mosh-client.exe";
  if (!IsRegularFile(path)) {
    throw LauncherError("sibling mosh-client.exe was not found: " +
                        WideToUtf8(path));
  }
  return path;
}

std::wstring FindMoshClient(
    const std::optional<std::wstring>& explicit_path) {
  if (!explicit_path) {
    return FindSiblingMoshClient();
  }
  return ResolveExplicitExecutable(*explicit_path, "--client");
}

std::uint32_t RunProcessLines(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    const OutputLineHandler& line_handler,
    EnvironmentOverrides environment) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE raw_read = nullptr;
  HANDLE raw_write = nullptr;
  if (!CreatePipe(&raw_read, &raw_write, &security, 0)) {
    throw LauncherError(WindowsError("CreatePipe", GetLastError()));
  }
  UniqueHandle read_pipe(raw_read);
  UniqueHandle write_pipe(raw_write);
  if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) {
    throw LauncherError(
        WindowsError("SetHandleInformation", GetLastError()));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_pipe.get();
  startup.hStdError = write_pipe.get();

  const bool replace_environment = !environment.empty();
  const ChildProcess child =
      LaunchProcess(executable, arguments, &startup, true, &environment,
                    replace_environment);
  const HANDLE process = child.process.get();
  write_pipe.reset();

  std::exception_ptr callback_error;
  std::string pending;
  std::array<char, 4096> buffer{};
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(read_pipe.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      const DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        break;
      }
      TerminateProcess(process, 255);
      WaitForSingleObject(process, INFINITE);
      SecureZeroMemory(buffer.data(), buffer.size());
      SecureWipe(pending);
      throw LauncherError(WindowsError("ReadFile", error));
    }
    if (read == 0) {
      break;
    }
    pending.append(buffer.data(), read);
    SecureZeroMemory(buffer.data(), buffer.size());
    if (pending.size() > 1024 * 1024) {
      TerminateProcess(process, 255);
      WaitForSingleObject(process, INFINITE);
      SecureWipe(pending);
      throw LauncherError("child process produced a line longer than 1 MiB");
    }

    std::size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, newline);
      SecureZeroMemory(pending.data(), newline + 1);
      pending.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      try {
        line_handler(line);
      } catch (...) {
        SecureWipe(line);
        callback_error = std::current_exception();
        break;
      }
      SecureWipe(line);
    }
    if (callback_error) {
      TerminateProcess(process, 255);
      break;
    }
  }

  if (!callback_error && !pending.empty()) {
    if (pending.back() == '\r') {
      pending.pop_back();
    }
    try {
      line_handler(pending);
    } catch (...) {
      callback_error = std::current_exception();
      TerminateProcess(process, 255);
    }
  }
  SecureWipe(pending);
  read_pipe.reset();
  const std::uint32_t exit_code = WaitAndGetExitCode(process);
  if (callback_error) {
    std::rethrow_exception(callback_error);
  }
  return exit_code;
}

std::uint32_t RunInteractiveProcess(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    EnvironmentOverrides environment) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  const ChildProcess child = LaunchProcess(executable, arguments, &startup,
                                           false, &environment, true);
  return WaitAndGetExitCode(child.process.get());
}

std::wstring QueryConfiguredHostname(const Options& options,
                                     const std::wstring& ssh_path) {
  std::optional<std::wstring> hostname;
  const auto arguments = BuildSshConfigArguments(options, ssh_path);
  const std::uint32_t exit_code = RunProcessLines(
      ssh_path, arguments,
      [&](const std::string& line) {
        auto candidate = ParseSshConfigHostname(line);
        if (!candidate) {
          return;
        }
        if (hostname && _wcsicmp(hostname->c_str(), candidate->c_str()) != 0) {
          throw LauncherError("ssh -G returned more than one HostName");
        }
        hostname = std::move(candidate);
      });
  if (exit_code != 0) {
    throw LauncherError("ssh -G failed with exit code " +
                        std::to_string(exit_code));
  }
  if (!hostname) {
    throw LauncherError("ssh -G did not report a HostName");
  }
  return *hostname;
}

std::wstring ResolveNumericAddress(const std::wstring& input,
                                   AddressFamily family,
                                   bool numeric_only) {
  WinsockSession winsock;
  std::wstring host = input;
  if (host.size() >= 2 && host.front() == L'[' && host.back() == L']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty()) {
    throw LauncherError("server address is empty");
  }

  ADDRINFOW hints{};
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  hints.ai_family = family == AddressFamily::ipv4
                        ? AF_INET
                        : family == AddressFamily::ipv6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_flags = numeric_only ? AI_NUMERICHOST : 0;
  PADDRINFOW raw_addresses = nullptr;
  const int status = GetAddrInfoW(host.c_str(), nullptr, &hints, &raw_addresses);
  if (status != 0) {
    throw LauncherError("could not resolve " + WideToUtf8(host) +
                        " (Winsock error " + std::to_string(status) + ')');
  }
  std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(
      raw_addresses, FreeAddrInfoW);

  const ADDRINFOW* selected = nullptr;
  const int preferred_family =
      family == AddressFamily::prefer_ipv6 ? AF_INET6 : AF_INET;
  if (family == AddressFamily::any || family == AddressFamily::prefer_ipv4 ||
      family == AddressFamily::prefer_ipv6) {
    for (const ADDRINFOW* address = addresses.get(); address;
         address = address->ai_next) {
      if (address->ai_family == preferred_family) {
        selected = address;
        break;
      }
    }
  }
  if (!selected) {
    for (const ADDRINFOW* address = addresses.get(); address;
         address = address->ai_next) {
      if (address->ai_family == AF_INET || address->ai_family == AF_INET6) {
        selected = address;
        break;
      }
    }
  }
  if (!selected) {
    throw LauncherError("address resolution returned no IPv4 or IPv6 address");
  }

  std::array<wchar_t, NI_MAXHOST> numeric{};
  const int name_status = GetNameInfoW(
      selected->ai_addr, static_cast<socklen_t>(selected->ai_addrlen),
      numeric.data(), static_cast<DWORD>(numeric.size()), nullptr, 0,
      NI_NUMERICHOST);
  if (name_status != 0) {
    throw LauncherError("GetNameInfoW failed with error " +
                        std::to_string(name_status));
  }
  return numeric.data();
}

}  // namespace mosh::launcher
