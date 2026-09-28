# Architecture

This document describes how the Windows port is put together: which code comes
from upstream Mosh, what the Windows layer replaces, how a session is
bootstrapped, and how the client multiplexes console and network input.

## Components

| Component | Output | Sources |
| --- | --- | --- |
| Launcher | `mosh.exe` | `src/launcher/` |
| Client | `mosh-client.exe` | `src/client/`, upstream `src/frontend/terminaloverlay.cc` |
| Mosh core | `mosh-core` (static) | upstream crypto, terminal, network fragments and state sync, plus `src/platform/*` replacements |
| Win32 platform | `mosh-win32-platform` (static) | `src/platform/win32_{console,socket,crypto,error}`, `utf8` |
| Protocol messages | `mosh-protos` (static) | upstream `.proto` files compiled with the pinned `protoc` |

The launcher deliberately does not link the platform or core libraries: it only
needs Winsock name resolution and process creation, and it never handles
terminal data.

### Upstream code versus Windows replacements

`cmake/MoshTargets.cmake` lists every source explicitly. Upstream files are
compiled unchanged except for the patch series (see [PATCHES.md](PATCHES.md)).
The following upstream pieces are replaced rather than patched:

| Upstream | Windows implementation | Why |
| --- | --- | --- |
| `network/network.cc` | `src/platform/mosh_network_win32.{h,cc}` | Winsock `SOCKET` handles, non-blocking UDP, Winsock error codes |
| `util/timestamp.cc` | `src/platform/win32_timestamp.cc` | `GetTickCount64()` monotonic clock |
| `util/locale_utils.cc` | `src/platform/win32_locale.cc` | The console is always UTF-8; UCRT `.UTF-8` locale |
| `util/swrite.cc` | `src/platform/win32_swrite.cc` | UCRT `_write` |
| `terminal/terminaldisplayinit.cc` | `src/platform/win32_terminaldisplayinit.cc` | Fixed VT profile instead of terminfo/ncurses |
| `frontend/stmclient.cc`, `mosh-client.cc` | `src/client/` | Win32 console and event loop |
| OpenSSL/Nettle AES | `src/platform/win32_crypto.{h,cc}` | CNG (`bcrypt.dll`) AES-128-ECB and system RNG |
| `wcwidth`/`mbrtowc` | `src/platform/utf8.{h,cc}`, `unicode_width_table.h` | 16-bit `wchar_t` cannot hold non-BMP characters |
| `scripts/mosh.pl` | `src/launcher/` | Native launcher using Windows OpenSSH |

## Session bootstrap (`mosh.exe`)

`src/launcher/main.cc` drives the steps below; parsing and quoting live in
`launcher_core.cc` and all Win32 process/Winsock work in `process_windows.cc`.

1. **Parse options** (`ParseOptions`). Values containing NUL, CR or LF are
   rejected so that they cannot truncate a command line or inject lines into
   the remote shell or the bootstrap protocol.
2. **Locate programs.** OpenSSH defaults to
   `%SystemRoot%\System32\OpenSSH\ssh.exe`; `mosh-client.exe` defaults to the
   launcher's own directory. `--ssh-path`/`--client` override them.
3. **Resolve the server address locally** (default `--remote-ip=local`): run
   `ssh -G <destination>` to obtain the effective `HostName` from the user's
   SSH configuration and resolve it with `GetAddrInfoW`.
4. **Start the server.** Run
   `ssh -n -tt <destination> -- mosh-server new -c 256 -s [-p PORT] [-- command]`,
   with every remote token POSIX-quoted. With `--remote-ip=remote` the command
   is wrapped in a small `sh -c` probe that also prints
   `MOSH SSH_CONNECTION ...`.
5. **Parse the bootstrap output** line by line (`BootstrapParser`). Lines
   starting with `MOSH CONNECT`, `MOSH IP` and `MOSH SSH_CONNECTION` are
   validated strictly (port range, a 22-character base64 key, numeric
   addresses only) and never echoed; any other output is passed to stderr.
   Output lines are limited to 1 MiB.
6. **Select the UDP address:** `--server-address`, else the locally resolved
   `HostName`, else the address announced by `MOSH IP`, else the server side of
   `SSH_CONNECTION`. Addresses from the remote side are never looked up in DNS.
7. **Start the client** with the address and port as arguments and a
   dedicated environment block containing `MOSH_KEY`,
   `MOSH_PREDICTION_DISPLAY`, and, when requested,
   `MOSH_PREDICTION_OVERWRITE` / `MOSH_NO_TERM_INIT`. The launcher waits for
   the client and returns its exit code.

A non-zero OpenSSH exit status is fatal unless a valid `MOSH CONNECT` line was
already parsed; that case is tolerated with a warning to work around
[Win32-OpenSSH #1899](https://github.com/PowerShell/Win32-OpenSSH/issues/1899).

## Client event loop (`mosh-client.exe`)

`STMClientWin` (`src/client/stmclient_win.{h,cc}`) is the Windows counterpart of
upstream `STMClient`. Everything runs on one thread except the console reader.

```
 console ReadFile ──► ConsoleInputPump thread ──► buffer + manual-reset event ─┐
 Ctrl-C / close ────► ConsoleSignal handler ─────► manual-reset event ─────────┤
 UDP sockets (≤10) ─► WSAEventSelect(FD_READ|FD_CLOSE) ──► WSAEVENTs ──────────┤
                                                                              ▼
                     SocketEventSet::wait = WaitForMultipleObjectsEx + select()
                                                                              │
                                             STMClientWin::main_loop steps ◄──┘
```

- **ConsoleSession** switches both console code pages to `CP_UTF8`, enables
  raw VT input (`ENABLE_VIRTUAL_TERMINAL_INPUT`, no echo/line/processed input)
  and VT output, and restores every setting on destruction, including after
  partial initialization failures.
- **ConsoleInputPump** exists because console handles cannot be waited on
  together with sockets for *data*. A dedicated thread performs blocking
  `ReadFile` calls and hands bytes to the main loop through a mutex-protected
  buffer (capped at 1 MiB) and an event. Shutdown cancels the pending read
  with `CancelSynchronousIo`, retrying until the thread has exited.
- **ConsoleSignal** turns Ctrl-C, Ctrl-Break, close, logoff and shutdown
  notifications into a waitable event; the loop then starts a graceful Mosh
  shutdown instead of dying.
- **SocketEventSet** keeps one WSAEVENT per UDP socket in Mosh's port-hopping
  set. After a wait it drains every signalled socket, and it takes a
  `select()` snapshot so that a datagram arriving just as a console event wins
  the wait is not delayed until the next timer.

Each iteration of `main_loop` renders a frame, waits (at most 100 ms, because
`ReadFile` does not report console resizes, and at most 250 ms while still
connecting), then in order: drains the network, handles console input
(including the `Ctrl-^ .` escape sequence), handles console signals, polls the
window size, checks for shutdown completion, updates the "connecting"
notification, ticks the transport and publishes send errors. Network and
Winsock errors are shown in the overlay and retried; a fatal integrity error
ends the session.

## Secret handling

The 128-bit session key is handled as a base64 string and is erased on a
best-effort basis wherever this project controls its memory:

- **Launcher:** the key from `MOSH CONNECT` lives in `BootstrapParser` and
  `BootstrapResult`, whose destructors and move operations zero it
  (`SecureWipe`). It is converted to UTF-16 only to build the child's
  environment block; the block and the override values are wiped immediately
  after `CreateProcessW`, whether or not it succeeded. The key never appears
  on a command line.
- **Client:** `MOSH_KEY` is copied and removed from the environment at startup,
  then erased once `Network::Transport` has been constructed. Patch 0004 makes
  upstream `Base64Key` erase its buffers, and CNG key objects are zeroed
  before being freed.

Memory not owned by this code (for example earlier copies made by the C runtime
environment functions, or the child's own environment block) cannot be
scrubbed.

## Reproducible builds

Release executables are bit-for-bit reproducible across build directories and
checkout locations. `scripts/verify-reproducible.ps1` checks this by building
twice in clean directories. The ingredients are:

- a pinned compiler (GCC 15.1.0, x64, UCRT — enforced at configure time) and
  pinned, hash-verified dependency archives;
- `-ffile-prefix-map`/`-fdebug-prefix-map` for the checkout (`.`), the build
  tree (`build`), the upstream tree (`third_party/mosh`) and each dependency
  (`mosh_prefix_map_flags` in `cmake/ProjectOptions.cmake`), so no absolute
  path reaches `__FILE__` strings or debug data;
- `-Wl,--no-insert-timestamp` and `SOURCE_DATE_EPOCH` (the Mosh 1.4.0 release
  date) in the presets;
- LF line endings in every checkout (`.gitattributes`), so the source bundle
  does not depend on `core.autocrlf`.

The guarantee covers the shipped `mosh.exe` and `mosh-client.exe`. The
build-time `protoc.exe` is linked by protobuf's own build without
`--no-insert-timestamp`, so it differs between builds; it is not shipped.
