#include "launcher_core.h"
#include "process_windows.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mosh::launcher::AddressFamily;
using mosh::launcher::BootstrapParser;
using mosh::launcher::BootstrapResult;
using mosh::launcher::EnvironmentOverrides;
using mosh::launcher::LauncherError;
using mosh::launcher::Options;
using mosh::launcher::RemoteIpMode;

void WriteLineToStderr(const std::string& line) {
  if (!line.empty()) {
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

std::wstring SelectServerAddress(const Options& options,
                                 const BootstrapResult& bootstrap,
                                 const std::optional<std::wstring>& local) {
  if (options.server_address) {
    return mosh::launcher::ResolveNumericAddress(
        *options.server_address, options.family, false);
  }
  if (local) {
    return *local;
  }
  if (bootstrap.announced_address) {
    return mosh::launcher::ResolveNumericAddress(
        mosh::launcher::Utf8ToWide(*bootstrap.announced_address),
        options.family, true);
  }
  if (bootstrap.ssh_server_address) {
    return mosh::launcher::ResolveNumericAddress(
        mosh::launcher::Utf8ToWide(*bootstrap.ssh_server_address),
        options.family, true);
  }
  throw LauncherError(
      "SSH did not report its server address; use --server-address or "
      "--remote-ip=local");
}

int Run(const std::vector<std::wstring>& arguments) {
  Options options = mosh::launcher::ParseOptions(arguments);
  if (options.show_help) {
    std::fputs(mosh::launcher::UsageText(), stdout);
    return 0;
  }
  if (options.show_version) {
    std::fputs("mosh 1.4.0-win1\n", stdout);
    return 0;
  }

  const std::wstring ssh_path =
      mosh::launcher::FindOpenSsh(options.ssh_path);
  const std::wstring client_path =
      mosh::launcher::FindMoshClient(options.client_path);

  std::optional<std::wstring> local_address;
  if (!options.server_address && options.remote_ip == RemoteIpMode::local) {
    const std::wstring configured_hostname =
        mosh::launcher::QueryConfiguredHostname(options, ssh_path);
    local_address = mosh::launcher::ResolveNumericAddress(
        configured_hostname, options.family, false);
  }

  BootstrapParser parser;
  const auto ssh_arguments =
      mosh::launcher::BuildSshSessionArguments(options, ssh_path);
  const std::uint32_t ssh_exit = mosh::launcher::RunProcessLines(
      ssh_path, ssh_arguments, [&](const std::string& line) {
        if (!parser.ConsumeLine(line)) {
          WriteLineToStderr(line);
        }
      });
  BootstrapResult bootstrap;
  try {
    bootstrap = parser.Finish();
  } catch (const LauncherError& bootstrap_error) {
    if (ssh_exit != 0) {
      throw LauncherError("OpenSSH failed with exit code " +
                          std::to_string(ssh_exit) + ": " +
                          bootstrap_error.what());
    }
    throw;
  }
  if (ssh_exit != 0) {
    WriteLineToStderr(
        "mosh: warning: OpenSSH returned exit code " +
        std::to_string(ssh_exit) +
        " after a valid MOSH CONNECT response; continuing");
  }
  const std::wstring address =
      SelectServerAddress(options, bootstrap, local_address);
  const std::wstring port = std::to_wstring(bootstrap.port);

  std::wstring key = mosh::launcher::Utf8ToWide(bootstrap.key);
  mosh::launcher::SecureWipe(bootstrap.key);
  EnvironmentOverrides environment;
  environment.emplace_back(L"MOSH_KEY", std::move(key));
  mosh::launcher::SecureWipe(key);
  environment.emplace_back(L"MOSH_PREDICTION_DISPLAY", options.prediction);
  environment.emplace_back(
      L"MOSH_PREDICTION_OVERWRITE",
      options.prediction_overwrite
          ? std::optional<std::wstring>(L"yes")
          : std::nullopt);
  environment.emplace_back(
      L"MOSH_NO_TERM_INIT",
      options.initialize_terminal ? std::nullopt
                                  : std::optional<std::wstring>(L"1"));

  const std::vector<std::wstring> client_arguments = {
      client_path, address, port};
  return static_cast<int>(mosh::launcher::RunInteractiveProcess(
      client_path, client_arguments, std::move(environment)));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    std::vector<std::wstring> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
      arguments.emplace_back(argv[i]);
    }
    return Run(arguments);
  } catch (const LauncherError& error) {
    std::fprintf(stderr, "mosh: %s\n", error.what());
    return 2;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "mosh: unexpected error: %s\n", error.what());
    return 2;
  }
}
