/*
 * Native Winsock implementation of Mosh 1.4's network.h interface.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#ifndef _WIN32
#error "mosh_network_win32.h is only available on Windows"
#endif

#include "platform/win32_socket.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <string>
#include <vector>

#include "crypto.h"

using namespace Crypto;

namespace Network {

using SocketHandle = mosh::win32::socket_handle;

static const unsigned int MOSH_PROTOCOL_VERSION = 2;

uint64_t timestamp(void);
uint16_t timestamp16(void);
uint16_t timestamp_diff(uint16_t newer, uint16_t older);

class NetworkException : public std::exception {
public:
  std::string function;
  int the_errno;

  NetworkException(std::string operation = "<none>", int error = 0);
  const char *what() const noexcept override { return message_.c_str(); }
  ~NetworkException() noexcept override = default;

private:
  std::string message_;
};

enum Direction { TO_SERVER = 0, TO_CLIENT = 1 };

class Packet {
public:
  const uint64_t seq;
  Direction direction;
  uint16_t timestamp;
  uint16_t timestamp_reply;
  std::string payload;

  Packet(Direction packet_direction, uint16_t packet_timestamp,
         uint16_t packet_timestamp_reply, const std::string &packet_payload)
      : seq(Crypto::unique()), direction(packet_direction),
        timestamp(packet_timestamp), timestamp_reply(packet_timestamp_reply),
        payload(packet_payload) {}

  explicit Packet(const Message &message);
  Message toMessage(void);
};

union Addr {
  struct sockaddr sa;
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  struct sockaddr_storage ss;
};

class Connection {
private:
  inline static constexpr int IPV4_HEADER_LEN = 20 + 8;
  inline static constexpr int IPV6_HEADER_LEN = 40 + 16 + 8;
  inline static constexpr int DEFAULT_SEND_MTU = 500;
  inline static constexpr int DEFAULT_IPV4_MTU = 1280;
  inline static constexpr int DEFAULT_IPV6_MTU = 1280;
  inline static constexpr uint64_t MIN_RTO = 50;
  inline static constexpr uint64_t MAX_RTO = 1000;
  inline static constexpr int PORT_RANGE_LOW = 60001;
  inline static constexpr int PORT_RANGE_HIGH = 60999;
  inline static constexpr unsigned int SERVER_ASSOCIATION_TIMEOUT = 40000;
  inline static constexpr unsigned int PORT_HOP_INTERVAL = 10000;
  inline static constexpr unsigned int MAX_PORTS_OPEN = 10;
  inline static constexpr unsigned int MAX_OLD_SOCKET_AGE = 60000;

  bool try_bind(const char *address, int port_low, int port_high);

  class Socket {
  public:
    explicit Socket(int family);
    ~Socket() = default;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&) noexcept = default;
    Socket &operator=(Socket &&) noexcept = default;

    [[nodiscard]] SocketHandle fd() const noexcept { return socket_.get(); }

  private:
    mosh::win32::SocketHandle socket_;
  };

  std::deque<Socket> socks;
  bool has_remote_addr;
  Addr remote_addr;
  int remote_addr_len;
  bool server;
  int MTU;
  Base64Key key;
  Session session;

  void setup(void);
  Direction direction;
  uint16_t saved_timestamp;
  uint64_t saved_timestamp_received_at;
  uint64_t expected_receiver_seq;
  uint64_t last_heard;
  uint64_t last_port_choice;
  uint64_t last_roundtrip_success;
  bool RTT_hit;
  double SRTT;
  double RTTVAR;
  std::string send_error;

  Packet new_packet(const std::string &payload);
  void hop_port(void);
  [[nodiscard]] SocketHandle sock(void) const {
    assert(!socks.empty());
    return socks.back().fd();
  }
  void prune_sockets(void);
  std::string recv_one(SocketHandle socket);
  void set_MTU(int family);

public:
  inline static constexpr int ADDED_BYTES = 8 + 4;

  Connection(const char *desired_ip, const char *desired_port);
  Connection(const char *key_string, const char *ip, const char *port);

  void send(const std::string &payload);
  std::string recv(void);
  [[nodiscard]] std::vector<SocketHandle> fds(void) const;
  [[nodiscard]] int get_MTU(void) const { return MTU; }
  [[nodiscard]] std::string port(void) const;
  [[nodiscard]] std::string get_key(void) const { return key.printable_key(); }
  [[nodiscard]] bool get_has_remote_addr(void) const {
    return has_remote_addr;
  }
  [[nodiscard]] uint64_t timeout(void) const;
  [[nodiscard]] double get_SRTT(void) const { return SRTT; }
  [[nodiscard]] const Addr &get_remote_addr(void) const { return remote_addr; }
  [[nodiscard]] int get_remote_addr_len(void) const { return remote_addr_len; }
  [[nodiscard]] std::string &get_send_error(void) { return send_error; }
  void set_last_roundtrip_success(uint64_t success) {
    last_roundtrip_success = success;
  }

  static bool parse_portrange(const char *desired_port,
                              int &desired_port_low,
                              int &desired_port_high);
};

} // namespace Network
