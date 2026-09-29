// SPDX-License-Identifier: GPL-3.0-or-later

#include "launcher_core.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>
#include <utility>

namespace mosh::launcher {
namespace {

// Works for narrow and wide strings, views and literals alike.
template <typename Text, typename Prefix>
bool StartsWith(const Text& value, const Prefix& prefix) {
  using View = std::basic_string_view<typename Text::value_type>;
  const View text(value);
  const View head(prefix);
  return text.size() >= head.size() && text.substr(0, head.size()) == head;
}

std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return value;
}

// NUL would truncate the Win32 command line; CR/LF would let an argument
// smuggle extra lines into the remote shell or the bootstrap protocol.
void RejectControlCharacters(const std::wstring& value,
                             const char* description) {
  if (std::find_if(value.begin(), value.end(), [](wchar_t c) {
        return c == L'\0' || c == L'\r' || c == L'\n';
      }) != value.end()) {
    throw LauncherError(std::string(description) +
                        " contains a forbidden control character");
  }
}

// Option values must also be non-empty.
void RejectEmbeddedControl(const std::wstring& value,
                           const char* option_name) {
  if (value.empty()) {
    throw LauncherError(std::string(option_name) + " may not be empty");
  }
  RejectControlCharacters(value, option_name);
}

std::wstring RequireNext(const std::vector<std::wstring>& arguments,
                         std::size_t* index,
                         const char* option_name) {
  if (*index + 1 >= arguments.size()) {
    throw LauncherError(std::string(option_name) + " requires a value");
  }
  ++*index;
  return arguments[*index];
}

std::optional<std::wstring> LongOptionValue(
    const std::wstring& argument,
    std::wstring_view name,
    const std::vector<std::wstring>& arguments,
    std::size_t* index,
    const char* printable_name) {
  if (argument == name) {
    return RequireNext(arguments, index, printable_name);
  }
  std::wstring prefix(name);
  prefix.push_back(L'=');
  if (StartsWith(argument, prefix)) {
    return argument.substr(prefix.size());
  }
  return std::nullopt;
}

// Strict unsigned decimal parser for narrow or wide text: no sign, no
// whitespace, no leading "+", and overflow-checked against |maximum|.
template <typename Text>
std::uint32_t ParseDecimal(const Text& value,
                           std::uint32_t maximum,
                           const char* description) {
  if (value.empty()) {
    throw LauncherError(std::string(description) + " is empty");
  }
  std::uint32_t result = 0;
  for (const auto c : value) {
    if (c < '0' || c > '9') {
      throw LauncherError(std::string(description) + " is not numeric");
    }
    const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
    if (result > (maximum - digit) / 10) {
      throw LauncherError(std::string(description) + " is out of range");
    }
    result = result * 10 + digit;
  }
  return result;
}

void ValidatePortRequest(const std::wstring& request) {
  const auto colon = request.find(L':');
  if (colon == std::wstring::npos) {
    if (ParseDecimal(request, 65535, "UDP port") == 0) {
      throw LauncherError("UDP port may not be zero");
    }
    return;
  }
  if (request.find(L':', colon + 1) != std::wstring::npos) {
    throw LauncherError("UDP port range contains more than one colon");
  }
  const std::uint32_t low =
      ParseDecimal(std::wstring_view(request).substr(0, colon), 65535,
                   "UDP range start");
  const std::uint32_t high =
      ParseDecimal(std::wstring_view(request).substr(colon + 1), 65535,
                   "UDP range end");
  if (low == 0) {
    throw LauncherError("a UDP port range may not start at zero");
  }
  if (high == 0 || low > high) {
    throw LauncherError("invalid UDP port range");
  }
}

bool IsPrediction(const std::wstring& value) {
  return value == L"adaptive" || value == L"always" ||
         value == L"never" || value == L"experimental";
}

bool IsBase64KeyChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '/' || c == '+';
}

std::vector<std::string_view> SplitAsciiWords(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i == line.size()) {
      break;
    }
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
      ++i;
    }
    words.emplace_back(line.substr(start, i - start));
  }
  return words;
}

void ValidateAddressToken(std::string_view address, const char* description) {
  if (address.empty() || address.size() > 255) {
    throw LauncherError(std::string("invalid ") + description);
  }
  for (unsigned char c : address) {
    if (c <= 0x20 || c == 0x7f) {
      throw LauncherError(std::string("invalid ") + description);
    }
  }
}

void ValidateNumericAddressToken(std::string_view address,
                                 const char* description) {
  ValidateAddressToken(address, description);
  std::string value(address);
  IN_ADDR ipv4{};
  if (InetPtonA(AF_INET, value.c_str(), &ipv4) == 1) {
    return;
  }

  const std::size_t scope = value.find('%');
  if (scope != std::string::npos) {
    if (value.find('%', scope + 1) != std::string::npos ||
        scope + 1 == value.size()) {
      throw LauncherError(std::string("invalid ") + description);
    }
    ParseDecimal(std::string_view(value).substr(scope + 1),
                 std::numeric_limits<std::uint32_t>::max(),
                 "IPv6 scope ID");
    value.resize(scope);
  }
  IN6_ADDR ipv6{};
  if (InetPtonA(AF_INET6, value.c_str(), &ipv6) != 1) {
    throw LauncherError(std::string("invalid ") + description);
  }
}

std::vector<std::wstring> CommonSshOptions(const Options& options) {
  std::vector<std::wstring> result;
  if (options.family == AddressFamily::ipv4) {
    result.emplace_back(L"-4");
  } else if (options.family == AddressFamily::ipv6) {
    result.emplace_back(L"-6");
  }
  result.insert(result.end(), options.ssh_options.begin(),
                options.ssh_options.end());
  return result;
}

std::string JoinQuoted(const std::vector<std::string>& tokens) {
  std::string result;
  for (const auto& token : tokens) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += QuotePosixToken(token);
  }
  return result;
}

}  // namespace

Options ParseOptions(const std::vector<std::wstring>& arguments) {
  Options result;
  bool end_of_options = false;

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::wstring& argument = arguments[i];
    if (!result.destination.empty()) {
      result.remote_command.push_back(argument);
      continue;
    }
    if (!end_of_options && argument == L"--") {
      end_of_options = true;
      continue;
    }
    if (end_of_options || argument.empty() || argument.front() != L'-') {
      result.destination = argument;
      continue;
    }
    if (argument == L"-h" || argument == L"--help") {
      result.show_help = true;
      continue;
    }
    if (argument == L"--version") {
      result.show_version = true;
      continue;
    }
    if (argument == L"-4") {
      result.family = AddressFamily::ipv4;
      continue;
    }
    if (argument == L"-6") {
      result.family = AddressFamily::ipv6;
      continue;
    }
    if (argument == L"-a") {
      result.prediction = L"always";
      continue;
    }
    if (argument == L"-n") {
      result.prediction = L"never";
      continue;
    }
    if (argument == L"-o" || argument == L"--predict-overwrite") {
      result.prediction_overwrite = true;
      continue;
    }
    if (argument == L"--no-ssh-pty") {
      result.ssh_pty = false;
      continue;
    }
    if (argument == L"--no-init") {
      result.initialize_terminal = false;
      continue;
    }

    if (auto value = LongOptionValue(argument, L"--client", arguments, &i,
                                     "--client")) {
      RejectEmbeddedControl(*value, "--client");
      result.client_path = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--ssh-path", arguments,
                                     &i, "--ssh-path")) {
      RejectEmbeddedControl(*value, "--ssh-path");
      result.ssh_path = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--ssh-option", arguments,
                                     &i, "--ssh-option")) {
      RejectEmbeddedControl(*value, "--ssh-option");
      result.ssh_options.push_back(std::move(*value));
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--port", arguments, &i,
                                     "--port")) {
      ValidatePortRequest(*value);
      result.port_request = std::move(*value);
      continue;
    }
    if (argument == L"-p" || StartsWith(argument, L"-p")) {
      std::wstring value =
          argument == L"-p" ? RequireNext(arguments, &i, "-p")
                            : argument.substr(2);
      ValidatePortRequest(value);
      result.port_request = std::move(value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--predict", arguments, &i,
                                     "--predict")) {
      *value = Lower(std::move(*value));
      if (!IsPrediction(*value)) {
        throw LauncherError(
            "--predict must be adaptive, always, never, or experimental");
      }
      result.prediction = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--server", arguments, &i,
                                     "--server")) {
      RejectEmbeddedControl(*value, "--server");
      result.server = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--bind-server", arguments,
                                     &i, "--bind-server")) {
      RejectEmbeddedControl(*value, "--bind-server");
      const std::wstring lowered = Lower(*value);
      if (lowered == L"ssh" || lowered == L"any") {
        *value = lowered;
      }
      result.bind_server = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--server-address", arguments,
                                     &i, "--server-address")) {
      RejectEmbeddedControl(*value, "--server-address");
      result.server_address = std::move(*value);
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--remote-ip", arguments, &i,
                                     "--remote-ip")) {
      *value = Lower(std::move(*value));
      if (*value == L"local") {
        result.remote_ip = RemoteIpMode::local;
      } else if (*value == L"remote") {
        result.remote_ip = RemoteIpMode::remote;
      } else {
        throw LauncherError("--remote-ip must be local or remote");
      }
      continue;
    }
    if (auto value = LongOptionValue(argument, L"--family", arguments, &i,
                                     "--family")) {
      *value = Lower(std::move(*value));
      if (*value == L"inet") {
        result.family = AddressFamily::ipv4;
      } else if (*value == L"inet6") {
        result.family = AddressFamily::ipv6;
      } else if (*value == L"prefer-inet") {
        result.family = AddressFamily::prefer_ipv4;
      } else if (*value == L"prefer-inet6") {
        result.family = AddressFamily::prefer_ipv6;
      } else {
        throw LauncherError(
            "--family must be inet, inet6, prefer-inet, or prefer-inet6");
      }
      continue;
    }
    throw LauncherError("unknown option: " + WideToUtf8(argument));
  }

  if (!result.show_help && !result.show_version) {
    if (result.destination.empty()) {
      throw LauncherError("missing [user@]host destination");
    }
    RejectEmbeddedControl(result.destination, "destination");
    if (result.destination.front() == L'-') {
      throw LauncherError("destination may not begin with '-'");
    }
    for (const auto& token : result.remote_command) {
      RejectControlCharacters(token, "remote command argument");
    }
  }
  return result;
}

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
  if (argument.empty()) {
    return L"\"\"";
  }
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }

  std::wstring result;
  result.push_back(L'\"');
  std::size_t backslashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'\"');
    } else {
      result.append(backslashes, L'\\');
      result.push_back(c);
    }
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring BuildWindowsCommandLine(
    const std::vector<std::wstring>& arguments) {
  std::wstring result;
  for (const auto& argument : arguments) {
    if (!result.empty()) {
      result.push_back(L' ');
    }
    result += QuoteWindowsArgument(argument);
  }
  return result;
}

std::string QuotePosixToken(std::string_view token) {
  std::string result;
  result.push_back('\'');
  for (char c : token) {
    if (c == '\'') {
      result += "'\\''";
    } else {
      result.push_back(c);
    }
  }
  result.push_back('\'');
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw LauncherError("text is too long for UTF-8 conversion");
  }
  const int source_size = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       text.data(), source_size, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) {
    throw LauncherError("invalid UTF-16 command-line text");
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          source_size, result.data(), size, nullptr, nullptr) !=
      size) {
    throw LauncherError("failed to convert command-line text to UTF-8");
  }
  return result;
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw LauncherError("text is too long for UTF-16 conversion");
  }
  const int source_size = static_cast<int>(text.size());
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(), source_size, nullptr, 0);
  if (size <= 0) {
    throw LauncherError("invalid UTF-8 process output");
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          source_size, result.data(), size) != size) {
    throw LauncherError("failed to convert process output to UTF-16");
  }
  return result;
}

std::string BuildRemoteServerCommand(const Options& options) {
  std::vector<std::string> server_tokens;
  server_tokens.push_back(WideToUtf8(options.server));
  server_tokens.emplace_back("new");
  server_tokens.emplace_back("-c");
  server_tokens.emplace_back("256");

  if (options.bind_server.empty() || options.bind_server == L"ssh") {
    server_tokens.emplace_back("-s");
  } else if (options.bind_server != L"any") {
    server_tokens.emplace_back("-i");
    server_tokens.push_back(WideToUtf8(options.bind_server));
  }
  if (options.port_request) {
    server_tokens.emplace_back("-p");
    server_tokens.push_back(WideToUtf8(*options.port_request));
  }
  if (!options.remote_command.empty()) {
    server_tokens.emplace_back("--");
    for (const auto& argument : options.remote_command) {
      server_tokens.push_back(WideToUtf8(argument));
    }
  }

  if (options.remote_ip == RemoteIpMode::local) {
    return JoinQuoted(server_tokens);
  }

  const std::string probe =
      "if [ -n \"$SSH_CONNECTION\" ]; then "
      "printf \"\\nMOSH SSH_CONNECTION %s\\n\" \"$SSH_CONNECTION\"; "
      "fi; exec \"$@\"";
  std::vector<std::string> shell_tokens = {
      "sh", "-c", probe, "mosh-bootstrap"};
  shell_tokens.insert(shell_tokens.end(), server_tokens.begin(),
                      server_tokens.end());
  return JoinQuoted(shell_tokens);
}

std::vector<std::wstring> BuildSshConfigArguments(
    const Options& options,
    const std::wstring& ssh_path) {
  std::vector<std::wstring> result = {ssh_path, L"-G"};
  auto common = CommonSshOptions(options);
  result.insert(result.end(), common.begin(), common.end());
  result.emplace_back(L"--");
  result.push_back(options.destination);
  return result;
}

std::vector<std::wstring> BuildSshSessionArguments(
    const Options& options,
    const std::wstring& ssh_path) {
  std::vector<std::wstring> result = {ssh_path, L"-n"};
  if (options.ssh_pty) {
    result.emplace_back(L"-tt");
  }
  auto common = CommonSshOptions(options);
  result.insert(result.end(), common.begin(), common.end());
  result.push_back(options.destination);
  result.emplace_back(L"--");
  result.push_back(Utf8ToWide(BuildRemoteServerCommand(options)));
  return result;
}

bool BootstrapParser::ConsumeLine(std::string_view input) {
  if (!input.empty() && input.back() == '\r') {
    input.remove_suffix(1);
  }
  if (StartsWith(input, "MOSH CONNECT")) {
    const auto words = SplitAsciiWords(input);
    if (words.size() != 4 || words[0] != "MOSH" || words[1] != "CONNECT" ||
        words[3].size() != 22 ||
        !std::all_of(words[3].begin(), words[3].end(), IsBase64KeyChar)) {
      throw LauncherError("malformed MOSH CONNECT line");
    }
    const std::uint32_t port =
        ParseDecimal(words[2], 65535, "MOSH CONNECT port");
    if (port == 0) {
      throw LauncherError("MOSH CONNECT port may not be zero");
    }
    if (port_) {
      throw LauncherError("duplicate MOSH CONNECT line");
    }
    port_ = static_cast<std::uint16_t>(port);
    key_.assign(words[3]);
    return true;
  }
  if (StartsWith(input, "MOSH SSH_CONNECTION")) {
    const auto words = SplitAsciiWords(input);
    if (words.size() != 6 || words[0] != "MOSH" ||
        words[1] != "SSH_CONNECTION") {
      throw LauncherError("malformed MOSH SSH_CONNECTION line");
    }
    ValidateNumericAddressToken(words[2], "SSH client address");
    ValidateNumericAddressToken(words[4], "SSH server address");
    const auto client_port =
        ParseDecimal(words[3], 65535, "SSH client port");
    const auto server_port =
        ParseDecimal(words[5], 65535, "SSH server port");
    if (client_port == 0 || server_port == 0) {
      throw LauncherError("SSH_CONNECTION ports may not be zero");
    }
    if (ssh_server_address_) {
      throw LauncherError("duplicate MOSH SSH_CONNECTION line");
    }
    ssh_server_address_ = std::string(words[4]);
    return true;
  }
  if (StartsWith(input, "MOSH IP")) {
    const auto words = SplitAsciiWords(input);
    if (words.size() != 3 || words[0] != "MOSH" || words[1] != "IP") {
      throw LauncherError("malformed MOSH IP line");
    }
    ValidateNumericAddressToken(words[2], "MOSH IP address");
    if (announced_address_) {
      throw LauncherError("duplicate MOSH IP line");
    }
    announced_address_ = std::string(words[2]);
    return true;
  }
  return false;
}

BootstrapParser::~BootstrapParser() { SecureWipe(key_); }

BootstrapResult::~BootstrapResult() { SecureWipe(key); }

BootstrapResult::BootstrapResult(BootstrapResult&& other) noexcept
    : port(other.port),
      key(std::move(other.key)),
      ssh_server_address(std::move(other.ssh_server_address)),
      announced_address(std::move(other.announced_address)) {
  other.port = 0;
  SecureWipe(other.key);
}

BootstrapResult& BootstrapResult::operator=(BootstrapResult&& other) noexcept {
  if (this != &other) {
    SecureWipe(key);
    port = other.port;
    key = std::move(other.key);
    ssh_server_address = std::move(other.ssh_server_address);
    announced_address = std::move(other.announced_address);
    other.port = 0;
    SecureWipe(other.key);
  }
  return *this;
}

BootstrapResult BootstrapParser::Finish() {
  if (!port_ || key_.empty()) {
    throw LauncherError(
        "did not receive a valid MOSH CONNECT line; is mosh-server installed?");
  }
  BootstrapResult result;
  result.port = *port_;
  result.key = std::move(key_);
  result.ssh_server_address = ssh_server_address_;
  result.announced_address = announced_address_;
  return result;
}

std::optional<std::wstring> ParseSshConfigHostname(std::string_view input) {
  if (!input.empty() && input.back() == '\r') {
    input.remove_suffix(1);
  }
  const auto words = SplitAsciiWords(input);
  if (words.size() != 2) {
    return std::nullopt;
  }
  std::string name(words[0]);
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (name != "hostname") {
    return std::nullopt;
  }
  ValidateAddressToken(words[1], "OpenSSH HostName");
  return Utf8ToWide(words[1]);
}

void SecureWipe(std::string& value) noexcept {
  if (!value.empty()) {
    SecureZeroMemory(value.data(), value.size());
    value.clear();
  }
}

void SecureWipe(std::wstring& value) noexcept {
  if (!value.empty()) {
    SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
  }
}

const char* UsageText() {
  return
      "Usage: mosh [options] [--] [user@]host [remote-command...]\n"
      "\n"
      "Windows-native Mosh client bootstrapper.\n"
      "\n"
      "  -4, -6                     select IPv4 or IPv6\n"
      "      --family MODE          inet, inet6, prefer-inet, prefer-inet6\n"
      "  -p, --port PORT[:PORT]     request server UDP port or range\n"
      "  -a                         always show local prediction\n"
      "  -n                         never show local prediction\n"
      "      --predict MODE         adaptive, always, never, experimental\n"
      "  -o, --predict-overwrite    overwrite instead of insert prediction\n"
      "      --client PATH          use this mosh-client executable\n"
      "      --ssh-path PATH        use this OpenSSH client\n"
      "      --ssh-option ARG       append one OpenSSH argument (repeatable)\n"
      "      --server PATH          remote mosh-server command\n"
      "      --bind-server MODE     ssh, any, or an address\n"
      "      --server-address HOST  override the UDP server address\n"
      "      --remote-ip MODE       local or remote address discovery\n"
      "      --no-ssh-pty           do not request an SSH pseudo-terminal\n"
      "      --no-init              do not initialize terminal state\n"
      "  -h, --help                 show this help\n"
      "      --version              show version\n";
}

}  // namespace mosh::launcher
