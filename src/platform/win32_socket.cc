/* GPL-3.0-or-later */
#include "win32_socket.h"
#include "win32_error.h"

#include <algorithm>
#include <mutex>

namespace mosh::win32 {
namespace {

std::mutex winsock_mutex;
unsigned winsock_references = 0;

[[nodiscard]] std::string error_message(const char *operation, int error) {
  return std::string(operation) + ": " + socket_error_text(error) + " (" +
         std::to_string(error) + ')';
}

[[noreturn]] void throw_socket_error(const char *operation) {
  throw SocketError(operation, WSAGetLastError());
}

} // namespace

SocketError::SocketError(const char *operation, int error)
    : std::runtime_error(error_message(operation, error)), error_(error) {}

std::string socket_error_text(int error) {
  std::string text = system_error_text(static_cast<std::uint32_t>(error));
  return text.empty() ? std::string("Winsock error") : text;
}

WinsockRuntime::WinsockRuntime() {
  std::lock_guard<std::mutex> lock(winsock_mutex);
  if (winsock_references == 0) {
    WSADATA data{};
    const int status = WSAStartup(MAKEWORD(2, 2), &data);
    if (status != 0) {
      throw SocketError("WSAStartup", status);
    }
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
      WSACleanup();
      throw SocketError("WSAStartup(version)", WSAVERNOTSUPPORTED);
    }
  }
  ++winsock_references;
}

WinsockRuntime::~WinsockRuntime() {
  std::lock_guard<std::mutex> lock(winsock_mutex);
  if (winsock_references != 0 && --winsock_references == 0) {
    WSACleanup();
  }
}

SocketHandle::~SocketHandle() { reset(); }

SocketHandle::SocketHandle(SocketHandle &&other) noexcept
    : socket_(other.release()) {}

SocketHandle &SocketHandle::operator=(SocketHandle &&other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

SocketHandle SocketHandle::udp(int family) {
  const socket_handle value =
      WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                 WSA_FLAG_OVERLAPPED);
  if (value == invalid_socket) {
    throw_socket_error("WSASocketW(SOCK_DGRAM)");
  }
  return SocketHandle(value);
}

socket_handle SocketHandle::release() noexcept {
  const socket_handle value = socket_;
  socket_ = invalid_socket;
  return value;
}

void SocketHandle::reset(socket_handle replacement) noexcept {
  if (socket_ != invalid_socket) {
    closesocket(socket_);
  }
  socket_ = replacement;
}

SocketEventSet::~SocketEventSet() { clear(); }

void SocketEventSet::update(const std::vector<socket_handle> &sockets) {
  if (sockets.size() + 2 > MAXIMUM_WAIT_OBJECTS) {
    throw std::length_error("too many Mosh UDP sockets for Win32 wait set");
  }
  for (const socket_handle socket : sockets) {
    if (socket == invalid_socket) {
      throw std::invalid_argument("invalid socket in wait set");
    }
  }

  /* Remove associations no longer used by the upstream port-hop deque. */
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (std::find(sockets.begin(), sockets.end(), it->socket) ==
        sockets.end()) {
      WSAEventSelect(it->socket, nullptr, 0);
      WSACloseEvent(it->event);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }

  for (const socket_handle socket : sockets) {
    const auto existing =
        std::find_if(entries_.begin(), entries_.end(),
                     [socket](const Entry &entry) {
                       return entry.socket == socket;
                     });
    if (existing != entries_.end()) {
      continue;
    }

    const WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
      throw_socket_error("WSACreateEvent");
    }
    if (WSAEventSelect(socket, event, FD_READ | FD_CLOSE) == SOCKET_ERROR) {
      const int error = WSAGetLastError();
      WSACloseEvent(event);
      throw SocketError("WSAEventSelect", error);
    }
    entries_.push_back({socket, event});
  }
}

WaitResult SocketEventSet::wait(HANDLE input_event, HANDLE signal_event,
                                DWORD timeout_ms) {
  if (input_event == nullptr || input_event == INVALID_HANDLE_VALUE ||
      signal_event == nullptr || signal_event == INVALID_HANDLE_VALUE) {
    throw std::invalid_argument("invalid console event in wait set");
  }

  std::vector<HANDLE> handles;
  handles.reserve(entries_.size() + 2);
  handles.push_back(input_event);
  handles.push_back(signal_event);
  for (const Entry &entry : entries_) {
    handles.push_back(entry.event);
  }

  const DWORD status = WaitForMultipleObjectsEx(
      static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout_ms,
      TRUE);
  WaitResult result;
  if (status == WAIT_TIMEOUT) {
    result.timed_out = true;
    return result;
  }
  if (status == WAIT_IO_COMPLETION) {
    /* Alertable APC completion is not one of Mosh's logical events. */
    return result;
  }
  if (status == WAIT_FAILED) {
    throw SocketError("WaitForMultipleObjectsEx",
                      static_cast<int>(GetLastError()));
  }

  const DWORD index = status - WAIT_OBJECT_0;
  if (index >= handles.size()) {
    throw SocketError("WaitForMultipleObjectsEx(result)", WSAEINVAL);
  }
  /* Multiple manual-reset events may become ready together. */
  result.input_ready =
      WaitForSingleObject(input_event, 0) == WAIT_OBJECT_0;
  result.signal_ready =
      WaitForSingleObject(signal_event, 0) == WAIT_OBJECT_0;

  /* Drain every signaled network event, not just the first returned handle. */
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (WaitForSingleObject(entries_[i].event, 0) != WAIT_OBJECT_0) {
      continue;
    }
    WSANETWORKEVENTS events{};
    if (WSAEnumNetworkEvents(entries_[i].socket, entries_[i].event,
                             &events) == SOCKET_ERROR) {
      throw_socket_error("WSAEnumNetworkEvents");
    }
    if ((events.lNetworkEvents & FD_READ) != 0) {
      if (events.iErrorCode[FD_READ_BIT] != 0) {
        throw SocketError("FD_READ", events.iErrorCode[FD_READ_BIT]);
      }
      result.network_ready = true;
      result.readable_sockets.push_back(entries_[i].socket);
    }
    if ((events.lNetworkEvents & FD_CLOSE) != 0 &&
        events.iErrorCode[FD_CLOSE_BIT] != 0) {
      throw SocketError("FD_CLOSE", events.iErrorCode[FD_CLOSE_BIT]);
    }
  }

  /* A console event can win the wait just before Winsock signals its event.
     Supplement the event drain with a zero-time readiness snapshot so that
     simultaneous keyboard/signal traffic cannot delay an already queued
     datagram until the next transport timer. */
  if (!entries_.empty()) {
    fd_set readable;
    FD_ZERO(&readable);
    for (const Entry &entry : entries_) {
      FD_SET(entry.socket, &readable);
    }
    /* A one-millisecond grace period is used only when another handle won
       the wait. On Windows the loopback datagram can become visible just
       after sendto returns but before the WSAEVENT delivery is scheduled. */
    timeval grace{};
    grace.tv_usec = (result.input_ready || result.signal_ready) ? 1000 : 0;
    const int selected = select(0, &readable, nullptr, nullptr, &grace);
    if (selected == SOCKET_ERROR) {
      throw_socket_error("select(network readiness snapshot)");
    }
    if (selected == 0) {
      return result;
    }
    for (const Entry &entry : entries_) {
      if (!FD_ISSET(entry.socket, &readable)) {
        continue;
      }
      result.network_ready = true;
      if (std::find(result.readable_sockets.begin(),
                    result.readable_sockets.end(), entry.socket) ==
          result.readable_sockets.end()) {
        result.readable_sockets.push_back(entry.socket);
      }
    }
  }
  return result;
}

void SocketEventSet::clear() noexcept {
  for (const Entry &entry : entries_) {
    WSAEventSelect(entry.socket, nullptr, 0);
    WSACloseEvent(entry.event);
  }
  entries_.clear();
}

} // namespace mosh::win32
