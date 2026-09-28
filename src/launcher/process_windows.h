#pragma once

#include "launcher_core.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mosh::launcher {

using EnvironmentOverride =
    std::pair<std::wstring, std::optional<std::wstring>>;
using EnvironmentOverrides = std::vector<EnvironmentOverride>;
using OutputLineHandler = std::function<void(const std::string&)>;

std::wstring FindOpenSsh(
    const std::optional<std::wstring>& explicit_path = std::nullopt);
std::wstring FindMoshClient(
    const std::optional<std::wstring>& explicit_path = std::nullopt);
std::wstring FindSiblingMoshClient();

std::uint32_t RunProcessLines(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    const OutputLineHandler& line_handler,
    EnvironmentOverrides environment = {});

std::uint32_t RunInteractiveProcess(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    EnvironmentOverrides environment);

std::wstring QueryConfiguredHostname(const Options& options,
                                     const std::wstring& ssh_path);

// Returns a canonical numeric address. If numeric_only is true, DNS is never
// consulted (used for untrusted SSH_CONNECTION output).
std::wstring ResolveNumericAddress(const std::wstring& host,
                                   AddressFamily family,
                                   bool numeric_only);

}  // namespace mosh::launcher
