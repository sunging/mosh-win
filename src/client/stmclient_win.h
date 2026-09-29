/*
 * Native Windows frontend for the Mosh 1.4 state synchronization client.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "platform/win32_console.h"
#include "platform/win32_socket.h"

#include "completeterminal.h"
#include "networktransport.h"
#include "terminaldisplay.h"
#include "terminaloverlay.h"
#include "user.h"

#include <memory>
#include <string>

/*
 * Windows counterpart of upstream STMClient.  It owns the raw VT console
 * session, the console input thread, the Ctrl/close signal event and the
 * Mosh transport, and multiplexes them in a single-threaded event loop.
 * The session key is erased as soon as the transport has been created.
 */
class STMClientWin final {
public:
  STMClientWin(const char *ip, const char *port, const char *key,
               const char *prediction_mode, unsigned int verbose,
               const char *prediction_overwrite);
  ~STMClientWin();

  STMClientWin(const STMClientWin &) = delete;
  STMClientWin &operator=(const STMClientWin &) = delete;

  /* Opens the alternate screen and creates the UDP transport. */
  void init();
  /* Restores the terminal and reports an unclean exit; idempotent. */
  void shutdown() noexcept;
  /* Runs until the session ends; returns true for a clean shutdown. */
  bool main_loop();

private:
  using NetworkType =
      Network::Transport<Network::UserStream, Terminal::Complete>;

  void initialize_escape_key();
  void initialize_network();
  void process_network_input();
  bool process_user_input(const std::string &bytes);
  void process_resize(mosh::win32::ConsoleSize size);
  void output_new_frame();
  void request_shutdown(const std::u32string &message);
  [[nodiscard]] bool still_connecting() const;

  /* Event-loop steps, in the order main_loop() runs them.  The bool
     handlers return false when the loop must stop immediately. */
  [[nodiscard]] DWORD next_wait_ms();
  void drain_network();
  [[nodiscard]] bool handle_console_input();
  [[nodiscard]] bool handle_console_signal();
  [[nodiscard]] bool shutdown_finished();
  void update_connecting_notification();
  void publish_send_error();

  std::string ip_;
  std::string port_;
  std::string key_;
  int escape_key_{0x1e};
  int escape_pass_key_{'^'};
  int escape_pass_key2_{'^'};
  bool escape_requires_lf_{false};
  std::u32string escape_key_help_{U"?"};

  mosh::win32::ConsoleSession console_;
  mosh::win32::ConsoleInputPump input_;
  mosh::win32::ConsoleSignal signal_;
  mosh::win32::SocketEventSet socket_events_;
  mosh::win32::ConsoleSize window_size_{};

  Terminal::Framebuffer local_framebuffer_{1, 1};
  Terminal::Framebuffer new_state_{1, 1};
  Overlay::OverlayManager overlays_;
  std::unique_ptr<NetworkType> network_;
  Terminal::Display display_{false};

  std::u32string connecting_notification_;
  bool initialized_{false};
  bool display_open_{false};
  bool repaint_requested_{false};
  bool lf_entered_{false};
  bool quit_sequence_started_{false};
  bool clean_shutdown_{false};
  unsigned int verbose_{0};
};
