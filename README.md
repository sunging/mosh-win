# mosh-win

`mosh-win` is a native x64 Windows client port of [Mosh](https://mosh.org/)
1.4.0, the mobile shell. It builds only the client; there is no Windows
`mosh-server`.

A release contains two Windows PE programs. All third-party libraries and the
MinGW runtime are linked statically; only Windows system DLLs (the UCRT API
sets, Kernel32, Winsock and CNG) are used at run time:

- `mosh.exe` — logs in with the Windows OpenSSH client, starts the Linux
  `mosh-server` on the remote host, then launches the local client.
- `mosh-client.exe` — speaks Mosh's encrypted UDP protocol: state
  synchronization, roaming, prediction and VT terminal rendering.

Supported targets are x64 Windows 10 22H2 and Windows 11 with a VT-capable
terminal such as Windows Terminal / ConPTY. Nothing depends on Cygwin, MSYS2,
OpenSSL or ncurses at run time.

See also:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — components, bootstrap and
  event loop, key handling, reproducible builds.
- [docs/PATCHES.md](docs/PATCHES.md) — what the upstream patch series changes
  and how to maintain it.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — build options, tests, coding
  conventions.
- [CHANGELOG.md](CHANGELOG.md)

## Prerequisites

- CMake 3.28 or newer.
- The x86_64 MinGW-w64 **UCRT** toolchain with GCC 15.1.0. Configuration
  fails unless the compiler is x64, targets UCRT and is exactly GCC 15.1.0.
  The toolchain is located, in this order, from:
  1. `-DMINGW64_ROOT=<dir>` (or `-MingwRoot <dir>` for the scripts),
  2. the `MINGW64_ROOT` environment variable,
  3. the directory containing `gcc.exe` on `PATH`.
- Git (used only to apply the upstream patches idempotently).
- The Windows OpenSSH client, `%SystemRoot%\System32\OpenSSH\ssh.exe`.
- Network access to zlib.net and GitHub for the first configuration. Every
  download is verified against a pinned SHA-256.
- A short checkout path: the dependency build trees are deeply nested and
  `mingw32-make` does not support paths beyond `MAX_PATH` (see
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)).

To pin a toolchain for plain `cmake --preset` use without environment
variables, create a `CMakeUserPresets.json` (ignored by Git):

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local-release",
      "inherits": "mingw64-release",
      "cacheVariables": { "MINGW64_ROOT": "C:/path/to/mingw64" }
    }
  ]
}
```

## Building

From a regular PowerShell prompt:

```powershell
Set-Location <path-to-mosh-win>
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

A development build may be configured while port sources are still missing:

```powershell
cmake --preset mingw64-dev
cmake --build --preset mingw64-dev
```

The release preset sets `MOSH_REQUIRE_COMPLETE_PORT=ON`, so any missing client
or platform source fails configuration. Once the build directory already holds
the downloaded dependencies, offline mode avoids the network:

```powershell
.\scripts\build.ps1 -Configuration Release -Offline
```

The `third_party/source` directory of the corresponding-source package allows a
fully offline build. From its `mosh-win` subdirectory run:

```powershell
.\scripts\build.ps1 -Configuration Release -Clean -Offline `
  -DependencySourceRoot ..\third_party\source
```

Pinned dependencies:

| Dependency | Version | SHA-256 |
| --- | --- | --- |
| Mosh | 1.4.0 (`bc73a263`) | `ae581fbddf038730af9eee4d319a483288395a0722d0c94c7efb7fdbdbb0dbac` |
| protobuf C++ | 3.21.12 / v21.12 | `4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460` |
| zlib | 1.3.1 | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |

protobuf and zlib are linked only as static libraries. The Mosh archive is used
purely as source input: its Autotools build is not used and `mosh-server` is
not built.

Terminal character widths follow Unicode 17.0.0. The generated C++ table is
checked in, so a normal build needs neither Python nor network access.
Maintainers can verify the official data hashes and regenerate it with:

```powershell
python .\scripts\generate-unicode-width.py --download
python .\scripts\generate-unicode-width.py --check
```

## Usage

Typical invocations:

```powershell
.\mosh.exe user@example.com
.\mosh.exe -p 60001 user@example.com
.\mosh.exe --ssh-option=-i --ssh-option=$env:USERPROFILE\.ssh\id_ed25519 user@example.com
```

The launcher reuses `%USERPROFILE%\.ssh\config`, the Windows ssh-agent, host
key confirmation and password prompts. The remote host needs a compatible
`mosh-server`, and the client must be able to reach the UDP port the server
picks (usually in 60000–61000). An SSH `ProxyJump` only covers the bootstrap;
it does not forward Mosh's UDP traffic. Run `mosh.exe --help` for all options.

The client can also be started directly:

```powershell
$env:MOSH_KEY = '<22-character-key>'
.\mosh-client.exe 203.0.113.10 60001
Remove-Item Env:MOSH_KEY
```

Do not set the key by hand in normal use: `mosh.exe` builds a dedicated
environment for the child process and wipes the key from its own memory after
the process is created.

The client honours the upstream environment variables `MOSH_ESCAPE_KEY`,
`MOSH_PREDICTION_DISPLAY`, `MOSH_PREDICTION_OVERWRITE`, `MOSH_TITLE_NOPREFIX`
and `MOSH_NO_TERM_INIT`. Suspend (`Ctrl-^ Ctrl-Z`) is not available on Windows.

## Testing

```powershell
.\scripts\test.ps1 -Configuration Release
```

The script runs CTest and then checks the PE import tables of both
executables. The 12 CTest tests cover the OCB/RFC vectors and tamper rejection,
the RNG and base64, protobuf/zlib state synchronization, the Unicode
framebuffer, resize and the alternate screen, console restoration, IPv4/IPv6
sockets and UTF-8 error text, and — over real loopback UDP — loss,
duplication, reordering, source-port roaming and recovery from a virtual
15-second outage. The launcher tests use a fake `ssh.exe` to check Windows
argument quoting, remote shell quoting, bootstrap parsing and failure paths.
The PE audit rejects Cygwin/MSYS, MinGW C++ runtime, OpenSSL, protobuf, zlib
and ncurses DLLs.

`tests/wsl/run-e2e.ps1` runs an interoperability test against WSL2 Ubuntu: it
starts a temporary high-port `sshd` and the Linux `mosh-server`, then goes
Windows OpenSSH → Linux server → native UDP client. The fixture installs no
Windows service, stores no credentials, and its temporary processes should be
stopped afterwards.

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\wsl\run-e2e.ps1 `
  -Distro Ubuntu -InstallOpenSshServer
```

`-InstallOpenSshServer` installs packages only if the distribution lacks
`sshd`; the fixture always runs with a temporary configuration, a high port and
a temporary key, and never enables a persistent service. Some Windows OpenSSH
9.5 builds can return an abnormal exit code even after the remote command
succeeded ([Win32-OpenSSH #1899](https://github.com/PowerShell/Win32-OpenSSH/issues/1899)).
The launcher tolerates that exit only after it has strictly parsed a valid
`MOSH CONNECT` line, and prints a warning; missing or malformed connection
information still fails.

The reproducibility check builds two clean release directories and compares
the SHA-256 of both executables (the ZIP containers themselves are not claimed
to be byte-for-byte reproducible):

```powershell
.\scripts\verify-reproducible.ps1
```

## Packaging

```powershell
.\scripts\package.ps1
```

If the release was built from external offline dependency sources, pass the
same directory to the packager:

```powershell
.\scripts\package.ps1 -SkipBuild `
  -DependencySourceRoot ..\third_party\source
```

This writes to `dist/`:

- `mosh-win-<version>-x64.zip` — both executables, README, license, third-party
  notices, static runtime and OCB license texts, and a SHA-256 manifest.
- `mosh-win-<version>-source.zip` — this project, its patches and the Mosh,
  protobuf and zlib sources that CMake actually used.
- `SHA256SUMS.txt` — SHA-256 of both ZIP files.

The version comes from `CMakeLists.txt`. The import audit can also be run on
any binary:

```powershell
.\scripts\audit-pe-imports.ps1 -Binary .\out\build\mingw64-release\bin\mosh.exe
```

## Layout

- `src/platform/` — Win32 console, Winsock, CNG, error text and UTF-8 layer.
- `src/client/` — Windows client entry point and event loop.
- `src/launcher/` — OpenSSH launcher, argument quoting and child environment.
- `patches/series` — ordered patches applied to the pinned Mosh 1.4.0 source.
- `cmake/` — toolchain, dependencies, targets and patch application.
- `scripts/` — build, test and packaging scripts sharing `MoshWin.psm1`.
- `tests/` — CTest suites, launcher fixtures and the WSL end-to-end test.

The FetchContent upstream directory is a build artifact. Do not edit
`out/build/*/_deps/mosh_upstream-src`; keep changes as patches in `patches/`
so that clean builds stay reproducible (see [docs/PATCHES.md](docs/PATCHES.md)).

## License

This project and the Mosh-derived code are distributed under
GPL-3.0-or-later. Binary distributions must also satisfy the GPL's
corresponding-source requirement, which is why the default packaging flow
produces a source bundle. protobuf is BSD-3-Clause and zlib uses the zlib
License; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
