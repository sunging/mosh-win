# Upstream patch series

The build never edits the Mosh sources by hand. CMake fetches the pinned Mosh
1.4.0 archive and applies the patches listed in `patches/series`, in order.
Everything Windows-specific that *can* live outside the upstream tree does
(`src/platform/`, `src/client/`, `src/launcher/`); the patches only add the
hooks upstream code needs to call into it.

## The patches

| Patch | Files | Purpose |
| --- | --- | --- |
| `0001-win32-cng-crypto.patch` | `crypto/ocb_internal.cc`, `crypto/prng.h`, `crypto/crypto.cc` | Adds a `USE_BCRYPT_AES` backend to the OCB3 implementation that performs AES-128-ECB through `mosh::win32::Aes128Ecb` (CNG). The PRNG reads from `BCryptGenRandom` instead of `/dev/urandom`. `disable_dumping_core()` becomes a no-op, because Windows has no Unix core files. |
| `0002-win32-network-interface.patch` | `network/network.h`, `network/networktransport.h` | On `_WIN32`, `network.h` includes `platform/mosh_network_win32.h` in place of the POSIX `Connection` declaration, and `Transport::fds()` returns Winsock `SOCKET` handles instead of `int` descriptors. |
| `0003-char32-terminal.patch` | `terminal/parser.{h,cc}`, `terminal/parseraction.h`, `terminal/parserstate.cc`, `frontend/terminaloverlay.{h,cc}` | Switches the terminal parser and overlays from `wchar_t` to `char32_t`. On Windows `wchar_t` is 16 bits and cannot hold characters outside the BMP (emoji, CJK Extension B, ...). UTF-8 decoding uses `mosh::win32::utf8::Decoder` and character widths use `utf8::display_width()` instead of `mbrtowc`/`wcwidth`. |
| `0004-secure-key-erasure.patch` | `crypto/crypto.{h,cc}` | Gives `Base64Key` a destructor and scoped guards that zero the raw key, the decoded buffer and the printable key on every path, including exceptions. |

## How patches are applied

`cmake/ApplyMoshPatches.cmake` runs during configuration, right after
FetchContent has populated the upstream tree:

1. `git apply --reverse --check` — if the reverse applies cleanly, the patch
   is already present and is skipped. Reconfiguring an existing build
   directory is therefore safe.
2. `git apply --check`, then `git apply`. Any failure stops configuration
   with git's diagnostic.

Git runs with `GIT_CEILING_DIRECTORIES` set to the parent of the upstream
tree. Without it, git would find this project's own repository (the tree lives
under `out/build/*/_deps/`), resolve patch paths against the project root, and
silently skip every file, so a pristine tree would be reported as "already
applied".

When `-DependencySourceRoot` / `FETCHCONTENT_SOURCE_DIR_MOSH_UPSTREAM` points
at an external source tree, the patches are applied to that tree in place.

Set `-DMOSH_APPLY_UPSTREAM_PATCHES=OFF` to configure against an unpatched tree
(the build is not expected to succeed; this is for inspecting upstream).

## Changing a patch

Never edit `out/build/*/_deps/mosh_upstream-src` and leave it at that: the next
clean build discards the change. Instead:

```powershell
# 1. Unpack the pinned archive (see cmake/Dependencies.cmake for its URL and
#    SHA-256) outside this repository and turn it into a scratch repository.
tar -xzf mosh-1.4.0.tar.gz -C $env:TEMP
Set-Location $env:TEMP\mosh-mosh-1.4.0
git init -q; git add -A; git commit -qm upstream

# 2. Apply and commit every patch that precedes the one you are changing,
#    e.g. 0001 and 0002 when editing 0003.
git apply <repo>\patches\0001-win32-cng-crypto.patch
git apply <repo>\patches\0002-win32-network-interface.patch
git commit -qam base

# 3. Apply the patch being changed without committing, edit the sources,
#    and write the complete working-tree diff back over it.  Use --output:
#    PowerShell redirection (>) would re-encode the patch with CRLF endings.
git apply <repo>\patches\0003-char32-terminal.patch
# ... edit ...
git diff --output=<repo>\patches\0003-char32-terminal.patch

# 4. Check that the later patches still apply on top.
git apply --check <repo>\patches\0004-secure-key-erasure.patch
```

Keep each patch focused on a single concern. To add a new patch, create
`NNNN-short-name.patch` and append it to `patches/series`. Patches are stored
byte-for-byte (`*.patch -text` in `.gitattributes`) and must use LF line
endings like the upstream sources. Verify the result with a clean configure;
the log must show `Applied Mosh patch: ...` for every entry:

```powershell
.\scripts\build.ps1 -Configuration Release -Clean
```
