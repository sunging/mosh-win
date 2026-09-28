# Development guide

## Toolchain

The build accepts exactly one toolchain: x86_64 MinGW-w64 targeting UCRT with
GCC 15.1.0. `CMakeLists.txt` checks the compiler ID, version, pointer size and
the `_UCRT` macro, and the toolchain file `cmake/toolchains/mingw64-ucrt.cmake`
points every tool (`gcc`, `g++`, `windres`, `ar`, `ranlib`, `strip`,
`objdump`, `mingw32-make`) at the same installation. Pinning the compiler is
part of what makes release builds reproducible.

No installation path is hard-coded. The toolchain root is taken from, in
order: `-DMINGW64_ROOT=<dir>`, the `MINGW64_ROOT` environment variable, or the
parent of the directory holding `gcc.exe` on `PATH`. The PowerShell scripts
accept `-MingwRoot` and resolve it the same way (`Get-MingwRoot` in
`scripts/MoshWin.psm1`). Machine-specific settings belong in an untracked
`CMakeUserPresets.json` (see the README), never in `CMakePresets.json`.

## Presets and options

| Preset | Build type | `MOSH_REQUIRE_COMPLETE_PORT` | Binary directory |
| --- | --- | --- | --- |
| `mingw64-dev` | Debug | `OFF` | `out/build/mingw64-dev` |
| `mingw64-release` | Release | `ON` | `out/build/mingw64-release` |

Both presets use the MinGW Makefiles generator, export
`compile_commands.json` and set `SOURCE_DATE_EPOCH`. Cache options:

| Option | Default | Meaning |
| --- | --- | --- |
| `MINGW64_ROOT` | auto | Toolchain root (see above). |
| `MOSH_REQUIRE_COMPLETE_PORT` | `OFF` | Fail configuration if any listed port source is missing instead of deferring the target. |
| `MOSH_BUILD_TESTS` | `ON` | Build the CTest suite. |
| `MOSH_APPLY_UPSTREAM_PATCHES` | `ON` | Apply `patches/series` to the fetched Mosh tree. |
| `FETCHCONTENT_SOURCE_DIR_<DEP>` | unset | Use an existing `mosh_upstream`, `protobuf` or `zlib` tree (set by `build.ps1 -DependencySourceRoot`). |
| `FETCHCONTENT_FULLY_DISCONNECTED` | `OFF` | Never download (set by `build.ps1 -Offline`). |

## Scripts

All scripts live in `scripts/`, import the shared module `MoshWin.psm1` and
throw on failure.

| Script | Purpose |
| --- | --- |
| `build.ps1` | Configure, build and run CTest for a preset (`-Clean`, `-Offline`, `-SkipTests`, `-BuildDirectory`, `-DependencySourceRoot`, `-MingwRoot`). |
| `test.ps1` | Run CTest and the PE import audit on an existing build. |
| `audit-pe-imports.ps1` | Reject non-system or forbidden DLL imports in any PE file. |
| `verify-reproducible.ps1` | Build twice in clean directories and compare the executables. |
| `package.ps1` | Build, audit and write the binary and source ZIPs to `dist/`. |
| `package-source.ps1` | Write only the corresponding-source ZIP. |
| `generate-unicode-width.py` | Regenerate or check `src/platform/unicode_width_table.h`. |

The package version is read from `CMakeLists.txt`. To cut a new Windows
release on the same upstream version, bump `MOSH_WIN_PACKAGE_REVISION` there;
`--version` output, `config.h`, CPack and the package names all follow.

## Tests

```powershell
ctest --preset mingw64-release                 # everything
ctest --preset mingw64-release -L unit         # fast unit tests
ctest --preset mingw64-release -L integration  # launcher end-to-end with fake ssh
ctest --preset mingw64-release -R core.utf8    # one test
```

Labels in use: `core`, `unit`, `launcher`, `integration`, `negative`,
`regression`, `network`, `resilience`, `crypto`, `upstream`.

| Test | What it covers |
| --- | --- |
| `core.utf8_test` | Strict UTF-8 decoding and encoding, streaming, character widths. |
| `core.socket_test` | Winsock event set with IPv4/IPv6, UTF-8 system error text. |
| `core.console_test` | Rejecting non-console handles, input-pump delivery and cancellation, VT mode and code-page restoration (when run in a console). |
| `core.crypto_test` | CNG AES known answers, RNG, base64 key parsing and erasure. |
| `core.state_sync_test` | protobuf serialization, zlib compression and user-stream diffs. |
| `core.terminal_test` | Framebuffer, Unicode rendering, resize, alternate screen. |
| `core.network_resilience_test` | Loss, duplication, reordering, roaming and a 15 s outage over loopback UDP with a virtual clock. |
| `core.ocb_upstream` | Upstream Mosh OCB vector, iterative and tamper-rejection suite against the CNG backend. |
| `launcher.unit` | Option parsing, quoting, bootstrap parsing, process helpers. |
| `launcher.fake-ssh-e2e` | Full launcher flow; key only in the child environment. |
| `launcher.post-connect-ssh-failure` | Tolerating an OpenSSH failure after `MOSH CONNECT`. |
| `launcher.malformed-bootstrap` | Rejecting malformed bootstrap output. |

`tests/wsl/run-e2e.ps1` is a manual interoperability test against a real
Linux `mosh-server` in WSL2; it is not part of CTest.

### Adding a core test

1. Create `tests/core/<name>_test.cc`. Include `test_support.h` and use
   `using mosh::test::expect;` for assertions; failures print the test name
   and exit with status 1.
2. Register it in `tests/CMakeLists.txt` with the smallest dependency set it
   needs, for example
   `_mosh_add_core_test(<name>_test 30 mosh-win32-platform)`. The helper adds
   the include directories and `MOSH_TEST_NAME` and labels the test
   `core;unit`.

Tests must not need network access beyond loopback, a real console, or
administrator rights.

## Coding conventions

- C++17, `-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow`. New code must build
  without warnings; the remaining warnings come from upstream Mosh and
  protobuf headers.
- Match the style of the module you edit:
  - `src/launcher/` follows Google style: `CamelCase` functions, `snake_case_`
    members, `//` comments, namespace `mosh::launcher`.
  - `src/platform/` and `src/client/` use `snake_case` functions and members
    with a trailing `_`, `/* */` comments, namespace `mosh::win32`.
  - Upstream files are only changed through `patches/` and keep upstream
    style.
- Own every Win32 resource with RAII (`UniqueHandle`, `SocketHandle`,
  `ConsoleSession`, ...), and check every Win32 return value.
- Treat anything derived from the session key as secret: prefer moves over
  copies and wipe buffers with `SecureWipe` (launcher) or
  `mosh::win32::secure_erase` (client) when done.
- All user-visible text written to the console must be UTF-8. Use
  `mosh::win32::system_error_text` for system messages.
- Documentation and comments are written in English.
- Do not commit local paths, user names or machine-specific settings.

## Commits

Keep commits focused and explain *why* in the message body. Before
committing, run at least:

```powershell
.\scripts\build.ps1 -Configuration Release -Offline
.\scripts\test.ps1
```

For build-system changes, also run `.\scripts\verify-reproducible.ps1`.
