/* GPL-3.0-or-later */
#include "stmclient_win.h"

#include "fatal_assert.h"
#include "networktransport-impl.h"
#include "parseraction.h"
#include "platform/win32_crypto.h"
#include "timestamp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

std::u32string ascii_widen(const std::string &text) {
  return std::u32string(text.begin(), text.end());
}

} // namespace

STMClientWin::STMClientWin(const char *ip, const char *port, const char *key,
                           const char *prediction_mode,
                           unsigned int verbose,
                           const char *prediction_overwrite)
    : ip_(ip != nullptr ? ip : ""), port_(port != nullptr ? port : ""),
      key_(key != nullptr ? key : ""), console_(),
      input_(console_.input_handle()), signal_(), verbose_(verbose) {
  if (prediction_mode != nullptr) {
    auto &engine = overlays_.get_prediction_engine();
    if (std::strcmp(prediction_mode, "always") == 0) {
      engine.set_display_preference(Overlay::PredictionEngine::Always);
    } else if (std::strcmp(prediction_mode, "never") == 0) {
      engine.set_display_preference(Overlay::PredictionEngine::Never);
    } else if (std::strcmp(prediction_mode, "adaptive") == 0) {
      engine.set_display_preference(Overlay::PredictionEngine::Adaptive);
    } else if (std::strcmp(prediction_mode, "experimental") == 0) {
      engine.set_display_preference(Overlay::PredictionEngine::Experimental);
    } else {
      throw std::invalid_argument(std::string("Unknown prediction mode: ") +
                                  prediction_mode);
    }
  }
  if (prediction_overwrite != nullptr &&
      std::strcmp(prediction_overwrite, "yes") == 0) {
    overlays_.get_prediction_engine().set_predict_overwrite(true);
  }
}

STMClientWin::~STMClientWin() {
  shutdown();
  mosh::win32::secure_erase(key_.data(), key_.size());
}

void STMClientWin::initialize_escape_key() {
  const char *configured = std::getenv("MOSH_ESCAPE_KEY");
  if (configured != nullptr) {
    if (configured[0] == '\0') {
      escape_key_ = -1;
    } else if (configured[1] == '\0') {
      escape_key_ = static_cast<unsigned char>(configured[0]);
    }
  }

  if (escape_key_ == 0x03 || escape_key_ == 0x04 || escape_key_ == 0x0a ||
      escape_key_ == 0x0c || escape_key_ == 0x0d || escape_key_ >= 128) {
    escape_key_ = 0x1e;
  }
  if (escape_key_ > 0 && escape_key_ < 32) {
    escape_pass_key_ = escape_key_ + '@';
  } else if (escape_key_ > 0) {
    escape_pass_key_ = escape_key_;
    escape_requires_lf_ = true;
  }
  escape_pass_key2_ =
      escape_pass_key_ >= 'A' && escape_pass_key_ <= 'Z'
          ? escape_pass_key_ + ('a' - 'A')
          : escape_pass_key_;

  if (escape_key_ > 0) {
    const char32_t pass = static_cast<char32_t>(escape_pass_key_);
    const char32_t key = escape_key_ < 32
                            ? static_cast<char32_t>(escape_key_ + '@')
                            : static_cast<char32_t>(escape_key_);
    escape_key_help_ = U"Commands: Ctrl-Z is unavailable on Windows, \".";
    escape_key_help_ += U" quits, \"";
    escape_key_help_.push_back(pass);
    escape_key_help_ += U"\" sends literal ";
    if (escape_key_ < 32) {
      escape_key_help_ += U"Ctrl-";
    }
    escape_key_help_.push_back(key);
    overlays_.get_notification_engine().set_escape_key_string(
        std::string(1, static_cast<char>(escape_pass_key_)));
  }
}

void STMClientWin::init() {
  if (initialized_) {
    return;
  }
  initialize_escape_key();
  console_.write(display_.open());
  display_open_ = true;
  if (std::getenv("MOSH_TITLE_NOPREFIX") == nullptr) {
    overlays_.set_title_prefix(U"[mosh] ");
  }
  connecting_notification_ =
      U"Nothing received from server on UDP port " + ascii_widen(port_) + U".";
  initialize_network();
  initialized_ = true;
}

void STMClientWin::initialize_network() {
  window_size_ = console_.size();
  local_framebuffer_ =
      Terminal::Framebuffer(window_size_.columns, window_size_.rows);
  new_state_ = Terminal::Framebuffer(1, 1);
  console_.write(
      display_.new_frame(false, local_framebuffer_, local_framebuffer_));

  Network::UserStream blank;
  Terminal::Complete terminal(window_size_.columns, window_size_.rows);
  try {
    network_ = std::make_unique<NetworkType>(blank, terminal, key_.c_str(),
                                             ip_.c_str(), port_.c_str());
  } catch (...) {
    mosh::win32::secure_erase(key_.data(), key_.size());
    key_.clear();
    throw;
  }
  mosh::win32::secure_erase(key_.data(), key_.size());
  key_.clear();
  network_->set_send_delay(1);
  network_->get_current_state().push_back(
      Parser::Resize(window_size_.columns, window_size_.rows));
  network_->set_verbose(verbose_);
  socket_events_.update(network_->fds());
}

void STMClientWin::shutdown() noexcept {
  socket_events_.clear();
  const bool was_initialized = initialized_;
  if (was_initialized) {
    initialized_ = false;
    try {
      overlays_.get_notification_engine().set_notification_string(U"");
      overlays_.get_notification_engine().server_heard(timestamp());
      overlays_.set_title_prefix(U"");
      output_new_frame();
    } catch (...) {
      /* Continue with terminal restoration. */
    }
  }
  if (display_open_) {
    display_open_ = false;
    try {
      console_.write(display_.close());
    } catch (...) {
      /* ConsoleSession still restores modes/code pages in its destructor. */
    }
  }
  if (!was_initialized) {
    return;
  }
  if (still_connecting()) {
    std::fprintf(stderr,
                 "\nmosh did not make a successful connection to %s:%s.\n"
                 "Verify that UDP port %s is reachable.\n",
                 ip_.c_str(), port_.c_str(), port_.c_str());
  } else if (network_ && !clean_shutdown_) {
    std::fputs("\nmosh did not shut down cleanly; the remote mosh-server "
               "may still be running.\n",
               stderr);
  }
}

bool STMClientWin::still_connecting() const {
  return network_ && network_->get_remote_state_num() == 0;
}

void STMClientWin::output_new_frame() {
  if (!network_) {
    return;
  }
  new_state_ = network_->get_latest_remote_state().state.get_fb();
  overlays_.apply(new_state_);
  console_.write(display_.new_frame(!repaint_requested_, local_framebuffer_,
                                    new_state_));
  repaint_requested_ = false;
  local_framebuffer_ = new_state_;
}

void STMClientWin::process_network_input() {
  network_->recv();
  auto &notifications = overlays_.get_notification_engine();
  notifications.server_heard(network_->get_latest_remote_state().timestamp);
  notifications.server_acked(network_->get_sent_state_acked_timestamp());
  auto &prediction = overlays_.get_prediction_engine();
  prediction.set_local_frame_acked(network_->get_sent_state_acked());
  prediction.set_send_interval(network_->send_interval());
  prediction.set_local_frame_late_acked(
      network_->get_latest_remote_state().state.get_echo_ack());
}

bool STMClientWin::process_user_input(const std::string &bytes) {
  if (bytes.empty()) {
    return !input_.eof();
  }
  if (network_->shutdown_in_progress()) {
    return true;
  }

  auto &prediction = overlays_.get_prediction_engine();
  prediction.set_local_frame_sent(network_->get_sent_state_last());
  const bool paste = bytes.size() > 100;
  if (paste) {
    prediction.reset();
  }

  for (const unsigned char byte : bytes) {
    if (!paste) {
      prediction.new_user_byte(static_cast<char>(byte), local_framebuffer_);
    }

    if (quit_sequence_started_) {
      if (byte == '.') {
        if (network_->has_remote_addr() &&
            !network_->shutdown_in_progress()) {
          request_shutdown(U"Exiting on user request...");
          return true;
        }
        return false;
      }
      if (byte == 0x1a) {
        overlays_.get_notification_engine().set_notification_string(
            U"Suspend is not available in the Windows client.", true);
      } else if (byte == escape_pass_key_ || byte == escape_pass_key2_) {
        network_->get_current_state().push_back(Parser::UserByte(escape_key_));
      } else {
        network_->get_current_state().push_back(Parser::UserByte(escape_key_));
        network_->get_current_state().push_back(Parser::UserByte(byte));
      }
      quit_sequence_started_ = false;
      if (overlays_.get_notification_engine().get_notification_string() ==
          escape_key_help_) {
        overlays_.get_notification_engine().set_notification_string(U"");
      }
      continue;
    }

    quit_sequence_started_ =
        escape_key_ > 0 && byte == escape_key_ &&
        (lf_entered_ || !escape_requires_lf_);
    if (quit_sequence_started_) {
      lf_entered_ = false;
      overlays_.get_notification_engine().set_notification_string(
          escape_key_help_, true, false);
      continue;
    }
    lf_entered_ = byte == '\n' || byte == '\r';
    if (byte == 0x0c) {
      repaint_requested_ = true;
    }
    network_->get_current_state().push_back(Parser::UserByte(byte));
  }
  return true;
}

void STMClientWin::process_resize(mosh::win32::ConsoleSize size) {
  if (size == window_size_) {
    return;
  }
  window_size_ = size;
  if (!network_->shutdown_in_progress()) {
    network_->get_current_state().push_back(
        Parser::Resize(size.columns, size.rows));
  }
  overlays_.get_prediction_engine().reset();
  repaint_requested_ = true;
}

void STMClientWin::request_shutdown(const std::u32string &message) {
  if (!network_->has_remote_addr()) {
    return;
  }
  if (!network_->shutdown_in_progress()) {
    overlays_.get_notification_engine().set_notification_string(message, true);
    network_->start_shutdown();
  }
}

bool STMClientWin::main_loop() {
  if (!initialized_ || !network_) {
    throw std::logic_error("STMClientWin::init must be called first");
  }

  for (;;) {
    try {
      freeze_timestamp();
      output_new_frame();
      int wait_time = std::min(network_->wait_time(), overlays_.wait_time());
      if (still_connecting()) {
        wait_time = std::min(wait_time, 250);
      }
      /* Poll viewport size at 10 Hz; console ReadFile does not return resize. */
      wait_time = wait_time < 0 ? 100 : std::clamp(wait_time, 0, 100);

      socket_events_.update(network_->fds());
      const auto events = socket_events_.wait(
          input_.event_handle(), signal_.event_handle(),
          static_cast<DWORD>(wait_time));
      freeze_timestamp();

      if (events.network_ready) {
        /* WSAEnumNetworkEvents resets every signaled FD_READ event. Drain the
           Transport until Connection has tried every port-hop socket and all
           return WSAEWOULDBLOCK, so no unread socket can lose its wakeup. */
        for (;;) {
          try {
            process_network_input();
          } catch (const Network::NetworkException &error) {
            if (error.the_errno == WSAEWOULDBLOCK) {
              break;
            }
            throw;
          }
        }
      }
      if (events.input_ready) {
        const std::string bytes = input_.take();
        if (input_.error() != ERROR_SUCCESS) {
          throw mosh::win32::ConsoleError("ReadFile(console input)",
                                          input_.error());
        }
        if (!process_user_input(bytes)) {
          if (!network_->has_remote_addr()) {
            break;
          }
          request_shutdown(U"Exiting...");
        }
      }
      if (events.signal_ready) {
        signal_.acknowledge();
        if (!network_->has_remote_addr()) {
          break;
        }
        request_shutdown(U"Console signal received, shutting down...");
      }

      process_resize(console_.size());

      if (network_->shutdown_in_progress() &&
          network_->shutdown_acknowledged()) {
        clean_shutdown_ = true;
        break;
      }
      if (network_->shutdown_in_progress() &&
          network_->shutdown_ack_timed_out()) {
        break;
      }
      if (network_->counterparty_shutdown_ack_sent()) {
        clean_shutdown_ = true;
        break;
      }

      if (still_connecting() && !network_->shutdown_in_progress() &&
          timestamp() - network_->get_latest_remote_state().timestamp > 250) {
        if (timestamp() - network_->get_latest_remote_state().timestamp >
            15000) {
          request_shutdown(U"Timed out waiting for server...");
        } else {
          overlays_.get_notification_engine().set_notification_string(
              connecting_notification_);
        }
      } else if (network_->get_remote_state_num() != 0 &&
                 overlays_.get_notification_engine()
                         .get_notification_string() ==
                     connecting_notification_) {
        overlays_.get_notification_engine().set_notification_string(U"");
      }

      network_->tick();
      std::string &send_error = network_->get_send_error();
      if (!send_error.empty()) {
        overlays_.get_notification_engine().set_network_error(send_error);
        send_error.clear();
      } else {
        overlays_.get_notification_engine().clear_network_error();
      }
    } catch (const Network::NetworkException &error) {
      if (error.the_errno != WSAEWOULDBLOCK &&
          !network_->shutdown_in_progress()) {
        overlays_.get_notification_engine().set_network_error(error.what());
      }
      Sleep(200);
      freeze_timestamp();
    } catch (const mosh::win32::SocketError &error) {
      if (!network_->shutdown_in_progress()) {
        overlays_.get_notification_engine().set_network_error(error.what());
      }
      Sleep(200);
      freeze_timestamp();
    } catch (const Crypto::CryptoException &error) {
      if (error.fatal) {
        throw;
      }
      overlays_.get_notification_engine().set_notification_string(
          U"Packet failed its integrity check.");
    }
  }
  return clean_shutdown_;
}
