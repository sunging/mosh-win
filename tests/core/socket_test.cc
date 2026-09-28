#include "platform/win32_socket.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "socket_test: " << message << '\n';
    std::exit(1);
  }
}

void ipv4_event_test() {
  using namespace mosh::win32;
  SocketHandle receiver = SocketHandle::udp(AF_INET);
  SocketHandle sender = SocketHandle::udp(AF_INET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  expect(bind(receiver.get(), reinterpret_cast<sockaddr *>(&address),
              sizeof(address)) == 0,
         "IPv4 bind failed");
  int length = sizeof(address);
  expect(getsockname(receiver.get(), reinterpret_cast<sockaddr *>(&address),
                     &length) == 0,
         "IPv4 getsockname failed");

  HANDLE input = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  expect(input != nullptr && signal != nullptr, "CreateEventW failed");
  SocketEventSet events;
  events.update({receiver.get()});

  const char payload[] = "event-loop";
  expect(sendto(sender.get(), payload, sizeof(payload), 0,
                reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
             sizeof(payload),
         "IPv4 sendto failed");
  SetEvent(input);
  SetEvent(signal);
  const WaitResult ready = events.wait(input, signal, 2000);
  expect(ready.input_ready && ready.signal_ready && ready.network_ready,
         "simultaneous console/socket events were lost");
  expect(std::find(ready.readable_sockets.begin(),
                   ready.readable_sockets.end(), receiver.get()) !=
             ready.readable_sockets.end(),
         "readable socket missing from result");

  char received[32]{};
  sockaddr_storage source{};
  int source_length = sizeof(source);
  int received_count = SOCKET_ERROR;
  const ULONGLONG deadline = GetTickCount64() + 2000;
  do {
    received_count = recvfrom(receiver.get(), received, sizeof(received), 0,
                              reinterpret_cast<sockaddr *>(&source),
                              &source_length);
    if (received_count != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK) {
      break;
    }
    Sleep(1);
  } while (GetTickCount64() < deadline);
  if (received_count == SOCKET_ERROR) {
    const int error = WSAGetLastError();
    std::cerr << "socket_test: recvfrom WSA error " << error << '\n';
  }
  expect(received_count == sizeof(payload), "IPv4 recvfrom failed");
  expect(std::memcmp(received, payload, sizeof(payload)) == 0,
         "IPv4 datagram changed");
  events.clear();
  CloseHandle(signal);
  CloseHandle(input);

  const SOCKET native = sender.get();
  SocketHandle moved(std::move(sender));
  expect(!sender && moved.get() == native, "SocketHandle move lost ownership");
}

void ipv6_smoke_test() {
  using namespace mosh::win32;
  try {
    SocketHandle socket = SocketHandle::udp(AF_INET6);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    address.sin6_port = 0;
    if (bind(socket.get(), reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) == SOCKET_ERROR) {
      const int error = WSAGetLastError();
      expect(error == WSAEADDRNOTAVAIL || error == WSAEAFNOSUPPORT,
             "unexpected IPv6 bind error");
    }
  } catch (const SocketError &error) {
    expect(error.error() == WSAEAFNOSUPPORT,
           "unexpected IPv6 socket creation error");
  }
}

} // namespace

int main() {
  mosh::win32::WinsockRuntime winsock;
  ipv4_event_test();
  ipv6_smoke_test();
  return 0;
}
