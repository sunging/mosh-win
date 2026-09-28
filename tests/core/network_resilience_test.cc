/* GPL-3.0-or-later */
#include "platform/mosh_network_win32.h"
#include "test_support.h"

#include "fatal_assert.h"
#include "networktransport-impl.h"
#include "user.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

std::uint64_t test_clock_ms = 100000;

using mosh::test::expect;

void make_nonblocking(SOCKET socket) {
  u_long enabled = 1;
  expect(ioctlsocket(socket, FIONBIO, &enabled) == 0,
         "failed to make fixture socket nonblocking");
}

sockaddr_in loopback_destination(const std::string &port) {
  const unsigned long parsed = std::strtoul(port.c_str(), nullptr, 10);
  expect(parsed > 0 && parsed <= std::numeric_limits<std::uint16_t>::max(),
         "invalid ephemeral server port");

  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(static_cast<std::uint16_t>(parsed));
  return destination;
}

std::uint16_t local_port(SOCKET socket) {
  sockaddr_storage local{};
  int length = sizeof(local);
  expect(getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length) ==
             0,
         "getsockname failed");
  expect(local.ss_family == AF_INET, "fixture socket was not IPv4");
  return ntohs(reinterpret_cast<const sockaddr_in *>(&local)->sin_port);
}

std::uint16_t remote_port(const Network::Connection &connection) {
  const Network::Addr &remote = connection.get_remote_addr();
  expect(remote.sa.sa_family == AF_INET, "remote address was not IPv4");
  return ntohs(remote.sin.sin_port);
}

void send_raw(SOCKET sender, const sockaddr_in &destination,
              const std::string &datagram) {
  const int sent = sendto(sender, datagram.data(),
                          static_cast<int>(datagram.size()), 0,
                          reinterpret_cast<const sockaddr *>(&destination),
                          sizeof(destination));
  expect(sent == static_cast<int>(datagram.size()), "fixture sendto failed");
}

std::string receive_connection(Network::Connection &connection) {
  const ULONGLONG deadline = GetTickCount64() + 2000;
  do {
    try {
      return connection.recv();
    } catch (const Network::NetworkException &error) {
      if (error.the_errno != WSAEWOULDBLOCK) {
        throw;
      }
    }
    Sleep(1);
  } while (GetTickCount64() < deadline);
  expect(false, "timed out receiving a loopback datagram");
  return {};
}

std::string receive_raw(SOCKET socket) {
  char buffer[Crypto::Session::RECEIVE_MTU]{};
  const ULONGLONG deadline = GetTickCount64() + 2000;
  do {
    sockaddr_storage source{};
    int source_length = sizeof(source);
    const int received =
        recvfrom(socket, buffer, sizeof(buffer), 0,
                 reinterpret_cast<sockaddr *>(&source), &source_length);
    if (received >= 0) {
      return std::string(buffer, static_cast<std::size_t>(received));
    }
    expect(WSAGetLastError() == WSAEWOULDBLOCK,
           "fixture recvfrom failed unexpectedly");
    Sleep(1);
  } while (GetTickCount64() < deadline);
  expect(false, "timed out receiving a raw loopback datagram");
  return {};
}

template <class TransportType>
void receive_transport(TransportType &transport) {
  const ULONGLONG deadline = GetTickCount64() + 2000;
  do {
    try {
      transport.recv();
      return;
    } catch (const Network::NetworkException &error) {
      if (error.the_errno != WSAEWOULDBLOCK) {
        throw;
      }
    }
    Sleep(1);
  } while (GetTickCount64() < deadline);
  expect(false, "timed out delivering a datagram to Transport");
}

std::string encrypt_packet(Crypto::Session &session,
                           Network::Packet &packet) {
  return session.encrypt(packet.toMessage());
}

void loss_reorder_duplicate_and_roaming_test() {
  Network::Connection server("127.0.0.1", "0");
  const sockaddr_in destination = loopback_destination(server.port());

  mosh::win32::SocketHandle sender_a =
      mosh::win32::SocketHandle::udp(AF_INET);
  mosh::win32::SocketHandle sender_b =
      mosh::win32::SocketHandle::udp(AF_INET);
  make_nonblocking(sender_a.get());
  make_nonblocking(sender_b.get());

  Crypto::Base64Key key(server.get_key());
  Crypto::Session encryptor(key);

  Network::Packet dropped(Network::TO_SERVER, Network::timestamp16(),
                          std::uint16_t(-1), "dropped");
  Network::Packet older(Network::TO_SERVER, Network::timestamp16(),
                        std::uint16_t(-1), "older");
  Network::Packet newest(Network::TO_SERVER, Network::timestamp16(),
                         std::uint16_t(-1), "newest");
  const std::string dropped_datagram = encrypt_packet(encryptor, dropped);
  const std::string older_datagram = encrypt_packet(encryptor, older);
  const std::string newest_datagram = encrypt_packet(encryptor, newest);
  (void)dropped_datagram; // Deliberately never put this packet on the wire.

  send_raw(sender_a.get(), destination, newest_datagram);
  expect(receive_connection(server) == "newest",
         "a packet after a loss was not accepted");
  const std::uint16_t sender_a_port = local_port(sender_a.get());
  expect(remote_port(server) == sender_a_port,
         "server did not learn the first source port");

  send_raw(sender_b.get(), destination, older_datagram);
  expect(receive_connection(server) == "older",
         "out-of-order payload was not returned to the transport layer");
  expect(remote_port(server) == sender_a_port,
         "out-of-order packet incorrectly changed the roaming target");

  send_raw(sender_b.get(), destination, newest_datagram);
  expect(receive_connection(server) == "newest",
         "duplicate payload was not returned to the transport layer");
  expect(remote_port(server) == sender_a_port,
         "duplicate packet incorrectly changed the roaming target");

  Network::Packet fresh(Network::TO_SERVER, Network::timestamp16(),
                        std::uint16_t(-1), "fresh-from-new-port");
  const std::string fresh_datagram = encrypt_packet(encryptor, fresh);
  send_raw(sender_b.get(), destination, fresh_datagram);
  expect(receive_connection(server) == "fresh-from-new-port",
         "fresh packet from a new source port was not accepted");
  const std::uint16_t sender_b_port = local_port(sender_b.get());
  expect(sender_b_port != sender_a_port,
         "fixture did not allocate distinct source ports");
  expect(remote_port(server) == sender_b_port,
         "fresh packet did not move the server roaming target");

  server.send("reply-to-new-port");
  Crypto::Session decryptor(key);
  Network::Packet reply(decryptor.decrypt(receive_raw(sender_b.get())));
  expect(reply.direction == Network::TO_CLIENT,
         "server reply had the wrong direction");
  expect(reply.payload == "reply-to-new-port",
         "server reply did not reach the new source port");
}

void transport_convergence_test() {
  Network::UserStream server_local;
  Network::UserStream server_remote;
  using UserTransport =
      Network::Transport<Network::UserStream, Network::UserStream>;
  UserTransport server(server_local, server_remote, "127.0.0.1", "0");

  Network::UserStream client_local;
  Network::UserStream client_remote;
  const std::string key = server.get_key();
  const std::string port = server.port();
  UserTransport client(client_local, client_remote, key.c_str(), "127.0.0.1",
                       port.c_str());
  const sockaddr_in destination = loopback_destination(port);
  mosh::win32::SocketHandle injector =
      mosh::win32::SocketHandle::udp(AF_INET);
  make_nonblocking(injector.get());

  // Capture two successive transport states before the server sees either.
  // Each state is a single encrypted UDP datagram for this small fixture.
  client.get_current_state().push_back(Parser::UserByte('A'));
  client.tick();
  const std::string state_a = receive_raw(server.fds().front());

  client.get_current_state().push_back(Parser::UserByte('B'));
  client.tick(); // Arms the minimum/frame-rate send timer.
  test_clock_ms += client.send_interval();
  client.tick();
  const std::string state_ab = receive_raw(server.fds().front());

  // The sender has not received an ACK yet, so both updates are based on the
  // initial state. Deliver AB first: the receiver must accept the newest
  // self-contained diff, then ignore the older A state and its duplicate.
  send_raw(injector.get(), destination, state_ab);
  receive_transport(server);
  expect(server.get_remote_state_num() == 2,
         "transport did not accept the newest out-of-order state");
  expect(server.get_latest_remote_state().state == client.get_current_state(),
         "out-of-order transport state did not reconstruct AB");

  send_raw(injector.get(), destination, state_a);
  receive_transport(server);
  expect(server.get_remote_state_num() == 2,
         "older transport state regressed the receiver");
  send_raw(injector.get(), destination, state_a);
  receive_transport(server);
  expect(server.get_remote_state_num() == 2,
         "duplicate transport state was not idempotent");

  // Capture and discard the next state to model actual loss. At the periodic
  // ACK deadline, TransportSender emits a new diff based on a state the peer
  // is known to have; delivering that retry must converge without dallying in
  // wall-clock time.
  client.get_current_state().push_back(Parser::UserByte('C'));
  client.tick();
  test_clock_ms += client.send_interval();
  client.tick();
  const std::string dropped_state_abc = receive_raw(server.fds().front());
  (void)dropped_state_abc;

  test_clock_ms += 3000; // Upstream TransportSender::ACK_INTERVAL.
  client.tick();
  const std::string retry_state_abc = receive_raw(server.fds().front());
  send_raw(injector.get(), destination, retry_state_abc);
  receive_transport(server);
  expect(server.get_remote_state_num() == 3,
         "transport did not advance across a lost state");
  expect(server.get_latest_remote_state().state == client.get_current_state(),
         "transport state did not converge after loss/reorder/duplicate");
}

void simulated_blackout_port_hop_test() {
  test_clock_ms = 200000;
  Network::Connection server("127.0.0.1", "0");
  const std::string key = server.get_key();
  const std::string port = server.port();
  Network::Connection client(key.c_str(), "127.0.0.1", port.c_str());

  expect(client.fds().size() == 1, "client did not start with one UDP socket");
  client.send("before-blackout");
  expect(receive_connection(server) == "before-blackout",
         "initial client packet was not received");
  const std::uint16_t old_client_port = remote_port(server);
  server.send("initial-reply");
  expect(receive_connection(client) == "initial-reply",
         "initial server reply was not received");
  client.set_last_roundtrip_success(test_clock_ms);

  // Advance a virtual 15 seconds without sleeping or exchanging packets. The
  // next send uses the established path once and then opens a new source port,
  // matching Mosh's recovery behavior after its 10-second hop threshold.
  test_clock_ms += 15000;
  client.send("probe-old-port");
  expect(client.fds().size() == 2,
         "15-second blackout did not retain old socket and open a new one");
  expect(receive_connection(server) == "probe-old-port",
         "probe on the established path was not received");
  expect(remote_port(server) == old_client_port,
         "first recovery probe unexpectedly changed source port");

  client.send("probe-new-port");
  expect(receive_connection(server) == "probe-new-port",
         "probe on the hopped source port was not received");
  const std::uint16_t new_client_port = remote_port(server);
  expect(new_client_port != old_client_port,
         "client port hop did not select a new source port");

  server.send("recovered");
  expect(receive_connection(client) == "recovered",
         "client did not receive a reply on its hopped socket");
}

} // namespace

// The test compiles mosh_network_win32.cc directly and supplies this virtual
// clock instead of win32_timestamp.cc. No production-only test hook is needed.
std::uint64_t frozen_timestamp(void) { return test_clock_ms; }
void freeze_timestamp(void) {}

int main() {
  mosh::win32::WinsockRuntime winsock;
  loss_reorder_duplicate_and_roaming_test();
  transport_convergence_test();
  simulated_blackout_port_hop_test();
  return 0;
}
