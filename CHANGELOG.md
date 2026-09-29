# Changelog

Versions are `<upstream Mosh version>-win<revision>`.

## 1.4.0-win2

### Fixed

- Upstream patches are applied correctly when the project is itself a Git
  work tree. Previously `git apply` resolved paths against the enclosing
  repository and reported a pristine upstream tree as already patched.
- Winsock and console error messages are UTF-8. They were produced in the
  ANSI code page and appeared as mojibake on non-English systems.
- The PE import audit reports every problem instead of stopping at the first.
- The UTF-8 decoder replaces a provably invalid sequence immediately instead
  of waiting for more input; streaming and one-shot decoding give identical
  results.
- `secure_random()` splits requests larger than 4 GiB instead of throwing.
- The `install` target defaults to `Program Files` instead of
  `Program Files (x86)` and installs the licenses of the statically linked
  runtime, like the binary ZIP.
- Scripts resolve relative `-BuildDirectory` and `-DependencySourceRoot` paths
  against the PowerShell location rather than the process working directory.
- The corresponding-source package takes the project files Git sees, so
  ignored output such as an IDE's `cmake-build-*` directory no longer ends up
  in it.

### Changed

- The MinGW-w64 toolchain is located through `-DMINGW64_ROOT`, the
  `MINGW64_ROOT` environment variable or `gcc.exe` on `PATH`; no installation
  path is hard-coded any more.
- Version strings come from `CMakeLists.txt` only.
- Shared PowerShell helpers moved to `scripts/MoshWin.psm1`; shared test
  assertions moved to `tests/test_support.h`.
- Launcher, client event loop and CMake prefix-map handling were refactored
  to remove duplicated code, without behavior changes. The CMake refactor
  leaves the release executables bit-identical.
- Documentation is in English and now includes `docs/ARCHITECTURE.md`,
  `docs/PATCHES.md` and `docs/DEVELOPMENT.md`.
- GitHub Actions CI: release build and tests, PE import audit, reproducibility
  check and Unicode table verification, using a SHA-256-pinned toolchain.
- Every project source carries an `SPDX-License-Identifier:
  GPL-3.0-or-later` tag.
- The end-to-end test moved to `tests/e2e/` and runs against any Linux host
  (`-Target user@host`) as well as a temporary WSL fixture (`-Wsl`). It checks
  the host (Linux, `mosh-server`, UTF-8 locale) before starting Mosh, detects
  a missing console up front, and can run itself in a new console window
  (`-NewWindow`).

### Added

- `scripts/install.ps1` builds and installs for the current user without
  administrator rights, optionally adds the install to the user `PATH`, and
  uninstalls exactly what it installed.
- The `install.layout` test checks the installed file layout.

## 1.4.0-win1

- First native Windows client port of Mosh 1.4.0: `mosh.exe` OpenSSH launcher
  and `mosh-client.exe`, statically linked against protobuf 3.21.12 and zlib
  1.3.1, using CNG for AES and random numbers.
- Unicode 17.0.0 character widths with full non-BMP support.
- Reproducible release executables, PE import audit, and a corresponding-source
  package.
