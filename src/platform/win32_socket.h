/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifndef _WIN32
#error "win32_socket.h is only available on Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mosh::win32 {

using socket_handle = SOCKET;
inline constexpr socket_handle invalid_socket = INVALID_SOCKET;

class SocketError : public std::runtime_error {
public:
  SocketError(const char *operation, int error);
  [[nodiscard]] int error() const noexcept { return error_; }

private:
  int error_;
};

[[nodiscard]] std::string socket_error_text(int error);

/* Process-scoped Winsock 2.2 lifetime. Multiple instances are safe. */
class WinsockRuntime final {
public:
  WinsockRuntime();
  ~WinsockRuntime();

  WinsockRuntime(const WinsockRuntime &) = delete;
  WinsockRuntime &operator=(const WinsockRuntime &) = delete;
};

class SocketHandle final {
public:
  SocketHandle() noexcept = default;
  explicit SocketHandle(socket_handle socket) noexcept : socket_(socket) {}
  ~SocketHandle();

  SocketHandle(const SocketHandle &) = delete;
  SocketHandle &operator=(const SocketHandle &) = delete;
  SocketHandle(SocketHandle &&other) noexcept;
  SocketHandle &operator=(SocketHandle &&other) noexcept;

  [[nodiscard]] static SocketHandle udp(int family);
  [[nodiscard]] socket_handle get() const noexcept { return socket_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return socket_ != invalid_socket;
  }
  [[nodiscard]] socket_handle release() noexcept;
  void reset(socket_handle replacement = invalid_socket) noexcept;

private:
  socket_handle socket_{invalid_socket};
};

struct WaitResult {
  bool input_ready{false};
  bool signal_ready{false};
  bool network_ready{false};
  bool timed_out{false};
  std::vector<socket_handle> readable_sockets;
};

/*
 * Persistent WSAEVENT associations for the client's rotating UDP sockets.
 * At most 10 sockets are used by upstream Mosh, keeping the wait set below
 * MAXIMUM_WAIT_OBJECTS even with console and shutdown handles.
 */
class SocketEventSet final {
public:
  SocketEventSet() = default;
  ~SocketEventSet();

  SocketEventSet(const SocketEventSet &) = delete;
  SocketEventSet &operator=(const SocketEventSet &) = delete;

  void update(const std::vector<socket_handle> &sockets);
  [[nodiscard]] WaitResult wait(HANDLE input_event, HANDLE signal_event,
                                DWORD timeout_ms);
  void clear() noexcept;

private:
  struct Entry {
    socket_handle socket;
    WSAEVENT event;
  };
  std::vector<Entry> entries_;
};

} // namespace mosh::win32
