#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Platform-independent parts of the mosh.exe launcher: command-line parsing,
// Windows and POSIX quoting, construction of the OpenSSH command lines, and
// parsing of the server's bootstrap output.  Win32 process and Winsock code
// lives in process_windows.h.
namespace mosh::launcher {

// -4/-6/--family: which address family ssh and the UDP client may use.
enum class AddressFamily {
  any,
  ipv4,
  ipv6,
  prefer_ipv4,
  prefer_ipv6,
};

// --remote-ip: resolve the server address locally from `ssh -G`, or trust
// the address reported by the remote side.
enum class RemoteIpMode {
  local,
  remote,
};

// Parsed mosh.exe command line.  Defaults match upstream mosh.
struct Options {
  std::optional<std::wstring> client_path;
  std::optional<std::wstring> ssh_path;
  std::vector<std::wstring> ssh_options;
  AddressFamily family = AddressFamily::any;
  std::optional<std::wstring> port_request;
  std::wstring prediction = L"adaptive";
  bool prediction_overwrite = false;
  std::wstring server = L"mosh-server";
  std::wstring bind_server = L"ssh";
  bool ssh_pty = true;
  bool initialize_terminal = true;
  std::optional<std::wstring> server_address;
  RemoteIpMode remote_ip = RemoteIpMode::local;
  bool show_help = false;
  bool show_version = false;
  std::wstring destination;
  std::vector<std::wstring> remote_command;
};

class LauncherError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Parses argv (without the program name).  Throws LauncherError for invalid
// options or values containing NUL, CR or LF.
Options ParseOptions(const std::vector<std::wstring>& arguments);

// Quotes one argument so CommandLineToArgvW/the MSVC CRT recovers it exactly.
std::wstring QuoteWindowsArgument(std::wstring_view argument);
std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments);
// Single-quotes one token for a POSIX shell.
std::string QuotePosixToken(std::string_view token);

// Strict conversions; invalid input throws LauncherError.
std::string WideToUtf8(std::wstring_view text);
std::wstring Utf8ToWide(std::string_view text);

// The quoted `mosh-server new ...` command run by ssh on the remote host.
std::string BuildRemoteServerCommand(const Options& options);
// argv for `ssh -G`, used to learn the effective HostName.
std::vector<std::wstring> BuildSshConfigArguments(
    const Options& options,
    const std::wstring& ssh_path);
// argv for the ssh session that starts mosh-server.
std::vector<std::wstring> BuildSshSessionArguments(
    const Options& options,
    const std::wstring& ssh_path);

// Connection details announced by mosh-server.  |key| is the session key
// and is wiped on destruction and when moved from.
struct BootstrapResult {
  BootstrapResult() = default;
  ~BootstrapResult();
  BootstrapResult(const BootstrapResult&) = delete;
  BootstrapResult& operator=(const BootstrapResult&) = delete;
  BootstrapResult(BootstrapResult&& other) noexcept;
  BootstrapResult& operator=(BootstrapResult&& other) noexcept;

  std::uint16_t port = 0;
  std::string key;
  std::optional<std::string> ssh_server_address;
  std::optional<std::string> announced_address;
};

// Consumes ssh output line by line and strictly validates the
// "MOSH CONNECT", "MOSH IP" and "MOSH SSH_CONNECTION" control lines.
class BootstrapParser {
 public:
  ~BootstrapParser();

  // Returns true for a launcher control line, which must not be echoed.
  bool ConsumeLine(std::string_view line);
  BootstrapResult Finish();

 private:
  std::optional<std::uint16_t> port_;
  std::string key_;
  std::optional<std::string> ssh_server_address_;
  std::optional<std::string> announced_address_;
};

// Returns the value of a "hostname <value>" line from `ssh -G` output.
std::optional<std::wstring> ParseSshConfigHostname(std::string_view line);

// Zero the characters of a secret (the session key) and clear the string.
// This cannot reach copies the allocator made earlier, so secrets should be
// moved rather than copied.
void SecureWipe(std::string& value) noexcept;
void SecureWipe(std::wstring& value) noexcept;

const char* UsageText();

}  // namespace mosh::launcher
