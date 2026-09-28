#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mosh::launcher {

enum class AddressFamily {
  any,
  ipv4,
  ipv6,
  prefer_ipv4,
  prefer_ipv6,
};

enum class RemoteIpMode {
  local,
  remote,
};

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

Options ParseOptions(const std::vector<std::wstring>& arguments);

std::wstring QuoteWindowsArgument(std::wstring_view argument);
std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments);
std::string QuotePosixToken(std::string_view token);

std::string WideToUtf8(std::wstring_view text);
std::wstring Utf8ToWide(std::string_view text);

std::string BuildRemoteServerCommand(const Options& options);
std::vector<std::wstring> BuildSshConfigArguments(
    const Options& options,
    const std::wstring& ssh_path);
std::vector<std::wstring> BuildSshSessionArguments(
    const Options& options,
    const std::wstring& ssh_path);

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

std::optional<std::wstring> ParseSshConfigHostname(std::string_view line);

// Zero the characters of a secret (the session key) and clear the string.
// This cannot reach copies the allocator made earlier, so secrets should be
// moved rather than copied.
void SecureWipe(std::string& value) noexcept;
void SecureWipe(std::wstring& value) noexcept;

const char* UsageText();

}  // namespace mosh::launcher
