/* GPL-3.0-or-later */
#include "platform/mosh_network_win32.h"

#include "byteorder.h"
#include "dos_assert.h"
#include "fatal_assert.h"
#include "timestamp.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Network;
using namespace Crypto;

namespace {

const uint64_t DIRECTION_MASK = uint64_t(1) << 63;
const uint64_t SEQUENCE_MASK = uint64_t(-1) ^ DIRECTION_MASK;
constexpr int congestion_timestamp_penalty = 500;

mosh::win32::WinsockRuntime &winsock_runtime() {
  static mosh::win32::WinsockRuntime runtime;
  return runtime;
}

[[nodiscard]] int last_socket_error() noexcept { return WSAGetLastError(); }

[[nodiscard]] std::string address_error(const char *node, int error) {
  const char *description = gai_strerrorA(error);
  return std::string("Bad IP address (") + (node != nullptr ? node : "(null)") +
         "): " + (description != nullptr ? description : "getaddrinfo error");
}

class AddrInfo final {
public:
  AddrInfo(const char *node, const char *service, const addrinfo *hints) {
    (void)winsock_runtime();
    const int status = getaddrinfo(node, service, hints, &result_);
    if (status != 0) {
      throw NetworkException(address_error(node, status), 0);
    }
  }
  ~AddrInfo() {
    if (result_ != nullptr) {
      freeaddrinfo(result_);
    }
  }
  AddrInfo(const AddrInfo &) = delete;
  AddrInfo &operator=(const AddrInfo &) = delete;
  [[nodiscard]] addrinfo *get() const noexcept { return result_; }

private:
  addrinfo *result_{nullptr};
};

} // namespace

NetworkException::NetworkException(std::string operation, int error)
    : function(std::move(operation)), the_errno(error),
      message_(function +
               (error != 0
                    ? ": " + mosh::win32::socket_error_text(error) + " (" +
                          std::to_string(error) + ')'
                    : std::string())) {}

Packet::Packet(const Message &message)
    : seq(message.nonce.val() & SEQUENCE_MASK),
      direction((message.nonce.val() & DIRECTION_MASK) ? TO_CLIENT
                                                       : TO_SERVER),
      timestamp(uint16_t(-1)), timestamp_reply(uint16_t(-1)), payload() {
  dos_assert(message.text.size() >= 2 * sizeof(uint16_t));
  uint16_t encoded[2];
  std::memcpy(encoded, message.text.data(), sizeof(encoded));
  timestamp = be16toh(encoded[0]);
  timestamp_reply = be16toh(encoded[1]);
  payload.assign(message.text.begin() + sizeof(encoded), message.text.end());
}

Message Packet::toMessage(void) {
  const uint64_t direction_sequence =
      (uint64_t(direction == TO_CLIENT) << 63) | (seq & SEQUENCE_MASK);
  const uint16_t encoded[2] = {static_cast<uint16_t>(htobe16(timestamp)),
                               static_cast<uint16_t>(
                                   htobe16(timestamp_reply))};
  return Message(Nonce(direction_sequence),
                 std::string(reinterpret_cast<const char *>(encoded),
                             sizeof(encoded)) +
                     payload);
}

Packet Connection::new_packet(const std::string &payload) {
  uint16_t timestamp_reply = uint16_t(-1);
  const uint64_t now = Network::timestamp();
  if (now - saved_timestamp_received_at < 1000) {
    timestamp_reply =
        static_cast<uint16_t>(saved_timestamp + now - saved_timestamp_received_at);
    saved_timestamp = uint16_t(-1);
    saved_timestamp_received_at = 0;
  }
  return Packet(direction, timestamp16(), timestamp_reply, payload);
}

Connection::Socket::Socket(int family)
    : socket_(mosh::win32::SocketHandle::udp(family)) {
  u_long nonblocking = 1;
  if (ioctlsocket(socket_.get(), FIONBIO, &nonblocking) == SOCKET_ERROR) {
    throw NetworkException("ioctlsocket(FIONBIO)", last_socket_error());
  }

  int dscp = 0x02;
  (void)setsockopt(socket_.get(), IPPROTO_IP, IP_TOS,
                   reinterpret_cast<const char *>(&dscp), sizeof(dscp));
}

void Connection::setup(void) { last_port_choice = Network::timestamp(); }

std::vector<SocketHandle> Connection::fds(void) const {
  std::vector<SocketHandle> result;
  result.reserve(socks.size());
  for (const Socket &socket : socks) {
    result.push_back(socket.fd());
  }
  return result;
}

void Connection::set_MTU(int family) {
  if (family == AF_INET) {
    MTU = DEFAULT_IPV4_MTU - IPV4_HEADER_LEN;
  } else if (family == AF_INET6) {
    MTU = DEFAULT_IPV6_MTU - IPV6_HEADER_LEN;
  } else {
    throw NetworkException("Unknown address family", WSAEAFNOSUPPORT);
  }
}

Connection::Connection(const char *desired_ip, const char *desired_port)
    : socks(), has_remote_addr(false), remote_addr(), remote_addr_len(0),
      server(true), MTU(DEFAULT_SEND_MTU), key(), session(key),
      direction(TO_CLIENT), saved_timestamp(uint16_t(-1)),
      saved_timestamp_received_at(0), expected_receiver_seq(0),
      last_heard(uint64_t(-1)), last_port_choice(uint64_t(-1)),
      last_roundtrip_success(uint64_t(-1)), RTT_hit(false), SRTT(1000),
      RTTVAR(500), send_error() {
  setup();
  int low = -1;
  int high = -1;
  if (desired_port != nullptr && !parse_portrange(desired_port, low, high)) {
    throw NetworkException("Invalid port range", WSAEINVAL);
  }
  if (desired_ip != nullptr && try_bind(desired_ip, low, high)) {
    return;
  }
  if (try_bind(nullptr, low, high)) {
    return;
  }
  throw NetworkException("Could not bind", last_socket_error());
}

bool Connection::try_bind(const char *address, int port_low, int port_high) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
  AddrInfo addresses(address, "0", &hints);

  Addr local{};
  const int address_length = static_cast<int>(addresses.get()->ai_addrlen);
  fatal_assert(static_cast<std::size_t>(address_length) <= sizeof(local));
  std::memcpy(&local.sa, addresses.get()->ai_addr, address_length);

  const int low = port_low == -1 ? PORT_RANGE_LOW : port_low;
  const int high = port_high == -1 ? PORT_RANGE_HIGH : port_high;
  socks.emplace_back(local.sa.sa_family);
  for (int port = low; port <= high; ++port) {
    if (local.sa.sa_family == AF_INET) {
      local.sin.sin_port = htons(static_cast<uint16_t>(port));
    } else if (local.sa.sa_family == AF_INET6) {
      local.sin6.sin6_port = htons(static_cast<uint16_t>(port));
    } else {
      throw NetworkException("Unknown address family", WSAEAFNOSUPPORT);
    }
    if (::bind(sock(), &local.sa, address_length) == 0) {
      set_MTU(local.sa.sa_family);
      return true;
    }
  }
  const int error = last_socket_error();
  socks.pop_back();
  if (error == WSAEADDRNOTAVAIL && address != nullptr) {
    return false;
  }
  throw NetworkException("bind", error);
}

Connection::Connection(const char *key_string, const char *ip,
                       const char *port)
    : socks(), has_remote_addr(false), remote_addr(), remote_addr_len(0),
      server(false), MTU(DEFAULT_SEND_MTU), key(key_string), session(key),
      direction(TO_SERVER), saved_timestamp(uint16_t(-1)),
      saved_timestamp_received_at(0), expected_receiver_seq(0),
      last_heard(uint64_t(-1)), last_port_choice(uint64_t(-1)),
      last_roundtrip_success(uint64_t(-1)), RTT_hit(false), SRTT(1000),
      RTTVAR(500), send_error() {
  setup();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  AddrInfo addresses(ip, port, &hints);
  fatal_assert(addresses.get()->ai_addrlen <= sizeof(remote_addr));
  remote_addr_len = static_cast<int>(addresses.get()->ai_addrlen);
  std::memcpy(&remote_addr.sa, addresses.get()->ai_addr, remote_addr_len);
  has_remote_addr = true;
  socks.emplace_back(remote_addr.sa.sa_family);
  set_MTU(remote_addr.sa.sa_family);
}

void Connection::hop_port(void) {
  assert(!server);
  setup();
  assert(remote_addr_len != 0);
  socks.emplace_back(remote_addr.sa.sa_family);
  prune_sockets();
}

void Connection::prune_sockets(void) {
  if (socks.size() > 1 &&
      Network::timestamp() - last_port_choice > MAX_OLD_SOCKET_AGE) {
    while (socks.size() > 1) {
      socks.pop_front();
    }
  }
  while (socks.size() > MAX_PORTS_OPEN) {
    socks.pop_front();
  }
}

void Connection::send(const std::string &payload) {
  if (!has_remote_addr) {
    return;
  }
  const std::string encrypted = session.encrypt(new_packet(payload).toMessage());
  if (encrypted.size() > static_cast<std::size_t>(
                             std::numeric_limits<int>::max())) {
    throw NetworkException("sendto payload too large", WSAEMSGSIZE);
  }
  const int sent = sendto(sock(), encrypted.data(),
                          static_cast<int>(encrypted.size()), 0,
                          &remote_addr.sa, remote_addr_len);
  if (sent != static_cast<int>(encrypted.size())) {
    const int error = last_socket_error();
    send_error = "sendto: " + mosh::win32::socket_error_text(error);
    if (error == WSAEMSGSIZE) {
      MTU = DEFAULT_SEND_MTU;
    }
  }

  const uint64_t now = Network::timestamp();
  if (server) {
    if (now - last_heard > SERVER_ASSOCIATION_TIMEOUT) {
      has_remote_addr = false;
    }
  } else if (now - last_port_choice > PORT_HOP_INTERVAL &&
             now - last_roundtrip_success > PORT_HOP_INTERVAL) {
    hop_port();
  }
}

std::string Connection::recv(void) {
  assert(!socks.empty());
  for (const Socket &socket : socks) {
    try {
      std::string payload = recv_one(socket.fd());
      prune_sockets();
      return payload;
    } catch (const NetworkException &error) {
      if (error.the_errno == WSAEWOULDBLOCK) {
        continue;
      }
      throw;
    }
  }
  throw NetworkException("No packet received", WSAEWOULDBLOCK);
}

std::string Connection::recv_one(SocketHandle socket) {
  Addr packet_remote{};
  int packet_remote_length = sizeof(packet_remote);
  char payload[Session::RECEIVE_MTU];
  const int received = recvfrom(socket, payload, sizeof(payload), 0,
                                &packet_remote.sa, &packet_remote_length);
  if (received == SOCKET_ERROR) {
    throw NetworkException("recvfrom", last_socket_error());
  }

  Packet packet(session.decrypt(payload, static_cast<std::size_t>(received)));
  dos_assert(packet.direction == (server ? TO_SERVER : TO_CLIENT));
  if (packet.seq < expected_receiver_seq) {
    return packet.payload;
  }
  expected_receiver_seq = packet.seq + 1;

  if (packet.timestamp != uint16_t(-1)) {
    saved_timestamp = packet.timestamp;
    saved_timestamp_received_at = Network::timestamp();
  }
  if (packet.timestamp_reply != uint16_t(-1)) {
    const double round_trip = timestamp_diff(timestamp16(),
                                             packet.timestamp_reply);
    if (round_trip < 5000) {
      if (!RTT_hit) {
        SRTT = round_trip;
        RTTVAR = round_trip / 2;
        RTT_hit = true;
      } else {
        constexpr double alpha = 1.0 / 8.0;
        constexpr double beta = 1.0 / 4.0;
        RTTVAR = (1 - beta) * RTTVAR + beta * std::fabs(SRTT - round_trip);
        SRTT = (1 - alpha) * SRTT + alpha * round_trip;
      }
    }
  }

  has_remote_addr = true;
  last_heard = Network::timestamp();
  if (server &&
      (remote_addr_len != packet_remote_length ||
       std::memcmp(&remote_addr, &packet_remote,
                   static_cast<std::size_t>(remote_addr_len)) != 0)) {
    remote_addr = packet_remote;
    remote_addr_len = packet_remote_length;
  }
  return packet.payload;
}

std::string Connection::port(void) const {
  Addr local{};
  int length = sizeof(local);
  if (getsockname(sock(), &local.sa, &length) == SOCKET_ERROR) {
    throw NetworkException("getsockname", last_socket_error());
  }
  char service[NI_MAXSERV];
  const int status = getnameinfo(&local.sa, length, nullptr, 0, service,
                                 sizeof(service),
                                 NI_DGRAM | NI_NUMERICSERV);
  if (status != 0) {
    throw NetworkException("getnameinfo", status);
  }
  return service;
}

uint64_t Network::timestamp(void) { return frozen_timestamp(); }

uint16_t Network::timestamp16(void) {
  uint16_t result = static_cast<uint16_t>(timestamp() % 65536);
  if (result == uint16_t(-1)) {
    ++result;
  }
  return result;
}

uint16_t Network::timestamp_diff(uint16_t newer, uint16_t older) {
  int difference = newer - older;
  if (difference < 0) {
    difference += 65536;
  }
  return static_cast<uint16_t>(difference);
}

uint64_t Connection::timeout(void) const {
  uint64_t result = static_cast<uint64_t>(std::llround(std::ceil(
      SRTT + 4 * RTTVAR)));
  return std::clamp(result, MIN_RTO, MAX_RTO);
}

bool Connection::parse_portrange(const char *desired_port, int &low,
                                 int &high) {
  low = high = 0;
  if (desired_port == nullptr || *desired_port == '\0') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  long value = std::strtol(desired_port, &end, 10);
  if (errno != 0 || value < 0 || value > 65535 ||
      (*end != '\0' && *end != ':')) {
    return false;
  }
  low = static_cast<int>(value);
  if (*end == '\0') {
    high = low;
    return true;
  }
  const char *upper = end + 1;
  errno = 0;
  value = std::strtol(upper, &end, 10);
  if (errno != 0 || *end != '\0' || value < 0 || value > 65535) {
    return false;
  }
  high = static_cast<int>(value);
  return low != 0 && low <= high;
}
