#include "launcher_core.h"
#include "process_windows.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line,
                 expression);
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)

template <typename Function>
void CheckThrows(Function&& function, const char* expression,
                 const char* file, int line) {
  try {
    function();
    std::fprintf(stderr, "%s:%d: expected LauncherError: %s\n", file, line,
                 expression);
    ++failures;
  } catch (const mosh::launcher::LauncherError&) {
  }
}

#define CHECK_THROWS(expression) \
  CheckThrows([&] { expression; }, #expression, __FILE__, __LINE__)

void TestOptions() {
  using namespace mosh::launcher;
  const Options options = ParseOptions(
      {L"--client=C:\\Program Files\\mosh-client.exe",
       L"--ssh-path", L"C:\\Windows\\ssh.exe", L"--ssh-option=-Jjump",
       L"--ssh-option", L"-oServerAliveInterval=10", L"--family=prefer-inet6",
       L"-p", L"60000:60010", L"--predict=experimental", L"-o",
       L"--server", L"/opt/mosh server", L"--bind-server=any",
       L"--remote-ip=remote", L"--no-ssh-pty", L"--no-init",
       L"user@example.test", L"printf", L"a b"});
  CHECK(options.client_path == L"C:\\Program Files\\mosh-client.exe");
  CHECK(options.ssh_options.size() == 2);
  CHECK(options.family == AddressFamily::prefer_ipv6);
  CHECK(options.port_request == L"60000:60010");
  CHECK(options.prediction == L"experimental");
  CHECK(options.prediction_overwrite);
  CHECK(options.bind_server == L"any");
  CHECK(options.remote_ip == RemoteIpMode::remote);
  CHECK(!options.ssh_pty);
  CHECK(!options.initialize_terminal);
  CHECK(options.destination == L"user@example.test");
  CHECK(options.remote_command.size() == 2);

  CHECK_THROWS(ParseOptions({L"-p", L"0", L"host"}));
  CHECK_THROWS(ParseOptions({L"-p", L"0:2", L"host"}));
  CHECK_THROWS(ParseOptions({L"-p", L"60002:60001", L"host"}));
  CHECK_THROWS(ParseOptions({L"--remote-ip=proxy", L"host"}));
  CHECK_THROWS(ParseOptions({L"--family=all", L"host"}));
  CHECK_THROWS(ParseOptions({L"--unknown", L"host"}));
  CHECK_THROWS(ParseOptions({}));

  const Options end_options = ParseOptions({L"--", L"host", L"--remote"});
  CHECK(end_options.destination == L"host");
  CHECK(end_options.remote_command == std::vector<std::wstring>{L"--remote"});

  const Options ipv4 = ParseOptions({L"-4", L"host"});
  const Options ipv6 = ParseOptions({L"-6", L"host"});
  CHECK(ipv4.family == AddressFamily::ipv4);
  CHECK(ipv6.family == AddressFamily::ipv6);
  const auto ipv4_ssh = BuildSshSessionArguments(ipv4, L"ssh.exe");
  const auto ipv6_ssh = BuildSshSessionArguments(ipv6, L"ssh.exe");
  CHECK(std::find(ipv4_ssh.begin(), ipv4_ssh.end(), L"-4") !=
        ipv4_ssh.end());
  CHECK(std::find(ipv6_ssh.begin(), ipv6_ssh.end(), L"-6") !=
        ipv6_ssh.end());
}

void TestQuoting() {
  using namespace mosh::launcher;
  CHECK(QuoteWindowsArgument(L"") == L"\"\"");
  CHECK(QuoteWindowsArgument(L"plain") == L"plain");
  CHECK(QuoteWindowsArgument(L"a b") == L"\"a b\"");
  CHECK(QuoteWindowsArgument(L"a\\\"") == L"\"a\\\\\\\"\"");
  CHECK(QuoteWindowsArgument(L"ends \\") == L"\"ends \\\\\"");
  CHECK(QuotePosixToken("") == "''");
  CHECK(QuotePosixToken("plain") == "'plain'");
  CHECK(QuotePosixToken("a'b") == "'a'\\''b'");
}

void TestCommandConstruction() {
  using namespace mosh::launcher;
  Options options;
  options.destination = L"me@alias";
  options.family = AddressFamily::prefer_ipv6;
  options.ssh_options = {L"-Jjump", L"-oServerAliveInterval=5"};
  options.server = L"/opt/mosh server";
  options.port_request = L"60000:60010";
  options.bind_server = L"203.0.113.4";
  options.remote_ip = RemoteIpMode::remote;
  options.remote_command = {L"printf", L"a'b", L"$(touch /tmp/no)"};

  const auto config = BuildSshConfigArguments(options, L"ssh.exe");
  CHECK(std::find(config.begin(), config.end(), L"-6") == config.end());
  CHECK(std::find(config.begin(), config.end(), L"-4") == config.end());
  CHECK(config[1] == L"-G");
  CHECK(config.back() == L"me@alias");

  const auto session = BuildSshSessionArguments(options, L"ssh.exe");
  CHECK(session[1] == L"-n");
  CHECK(session[2] == L"-tt");
  const std::string remote = WideToUtf8(session.back());
  CHECK(remote.find("'sh' '-c'") == 0);
  CHECK(remote.find("'$('") == std::string::npos);
  CHECK(remote.find("'$(touch /tmp/no)'") != std::string::npos);
  CHECK(remote.find("'a'\\''b'") != std::string::npos);
  CHECK(remote.find("'-i' '203.0.113.4'") != std::string::npos);
}

void TestBootstrapParser() {
  using namespace mosh::launcher;
  BootstrapParser parser;
  CHECK(!parser.ConsumeLine("welcome"));
  CHECK(parser.ConsumeLine(
      "MOSH SSH_CONNECTION 198.51.100.1 54321 203.0.113.7 22\r"));
  CHECK(parser.ConsumeLine(
      "MOSH CONNECT 60001 QUJDREVGR0hJSktMTU5PUA\r"));
  BootstrapResult result = parser.Finish();
  CHECK(result.port == 60001);
  CHECK(result.key == "QUJDREVGR0hJSktMTU5PUA");
  CHECK(result.ssh_server_address == "203.0.113.7");
  SecureWipe(result.key);

  CHECK_THROWS([] {
    BootstrapParser invalid;
    invalid.ConsumeLine("MOSH CONNECT x bad");
  }());
  CHECK_THROWS([] {
    BootstrapParser invalid;
    invalid.ConsumeLine(
        "MOSH SSH_CONNECTION one 1 two 2 extra");
  }());
  CHECK_THROWS([] {
    BootstrapParser invalid;
    invalid.ConsumeLine(
        "MOSH SSH_CONNECTION host.example 123 server.example 22");
  }());
  CHECK_THROWS([] {
    BootstrapParser duplicate;
    duplicate.ConsumeLine(
        "MOSH CONNECT 60001 QUJDREVGR0hJSktMTU5PUA");
    duplicate.ConsumeLine(
        "MOSH CONNECT 60002 QUJDREVGR0hJSktMTU5PUA");
  }());
  CHECK_THROWS([] {
    BootstrapParser empty;
    (void)empty.Finish();
  }());

  CHECK(ParseSshConfigHostname("hostname example.test\r") ==
        L"example.test");
  CHECK(!ParseSshConfigHostname("port 22"));
}

void TestProcessBoundaries(const std::wstring& fake_ssh) {
  using namespace mosh::launcher;
  std::vector<std::string> lines;
  const std::vector<std::wstring> echo_arguments = {
      fake_ssh, L"--echo-args", L"plain", L"a b", L"quote\"inside",
      L"trailing slash \\", L"中文"};
  const auto echo_exit = RunProcessLines(
      fake_ssh, echo_arguments,
      [&](const std::string& line) { lines.push_back(line); });
  CHECK(echo_exit == 0);
  CHECK(lines.size() == echo_arguments.size());
  for (std::size_t i = 0; i < echo_arguments.size() && i < lines.size(); ++i) {
    const std::string expected =
        "ARG" + std::to_string(i) + ':' + WideToUtf8(echo_arguments[i]);
    CHECK(lines[i] == expected);
  }

  Options options;
  options.destination = L"fake";
  CHECK(QueryConfiguredHostname(options, fake_ssh) == L"203.0.113.19");
  CHECK(ResolveNumericAddress(L"127.0.0.1", AddressFamily::ipv4, true) ==
        L"127.0.0.1");
  CHECK_THROWS(ResolveNumericAddress(L"localhost", AddressFamily::any, true));

  BootstrapParser parser;
  int banners = 0;
  const auto ssh_exit = RunProcessLines(
      fake_ssh, {fake_ssh, L"fake-host", L"remote command"},
      [&](const std::string& line) {
        if (!parser.ConsumeLine(line)) {
          ++banners;
        }
      });
  CHECK(ssh_exit == 0);
  CHECK(banners == 1);
  BootstrapResult bootstrap = parser.Finish();
  CHECK(bootstrap.port == 60001);
  CHECK(bootstrap.ssh_server_address == "203.0.113.7");
  SecureWipe(bootstrap.key);

  lines.clear();
  EnvironmentOverrides environment;
  environment.emplace_back(L"MOSH_KEY",
                           std::optional<std::wstring>(
                               L"QUJDREVGR0hJSktMTU5PUA"));
  const auto safe_exit = RunProcessLines(
      fake_ssh, {fake_ssh, L"--check-client-env"},
      [&](const std::string& line) { lines.push_back(line); },
      std::move(environment));
  CHECK(safe_exit == 0);
  CHECK(lines == std::vector<std::string>{"SAFE"});

  bool rejected_malformed = false;
  try {
    BootstrapParser malformed;
    EnvironmentOverrides malformed_environment;
    malformed_environment.emplace_back(
        L"MOSH_FAKE_SSH_MODE", std::optional<std::wstring>(L"malformed"));
    (void)RunProcessLines(
        fake_ssh, {fake_ssh, L"fake-host", L"remote command"},
        [&](const std::string& line) { (void)malformed.ConsumeLine(line); },
        std::move(malformed_environment));
  } catch (const LauncherError&) {
    rejected_malformed = true;
  }
  CHECK(rejected_malformed);

  CHECK(!FindOpenSsh(fake_ssh).empty());
  CHECK(!FindOpenSsh().empty());
  CHECK(!FindMoshClient(fake_ssh).empty());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  TestOptions();
  TestQuoting();
  TestCommandConstruction();
  TestBootstrapParser();
  if (argc >= 2) {
    TestProcessBoundaries(argv[1]);
  } else {
    std::fputs("launcher tests: fake_ssh.exe argument not supplied; "
               "process tests skipped\n",
               stderr);
  }

  if (failures != 0) {
    std::fprintf(stderr, "launcher tests: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("launcher tests: all checks passed");
  return 0;
}
