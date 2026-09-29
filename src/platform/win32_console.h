/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifndef _WIN32
#error "win32_console.h is only available on Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mosh::win32 {

class ConsoleError : public std::runtime_error {
public:
  ConsoleError(const char *operation, DWORD error);
  [[nodiscard]] DWORD error() const noexcept { return error_; }

private:
  DWORD error_;
};

struct ConsoleSize {
  std::uint16_t columns;
  std::uint16_t rows;

  friend bool operator==(ConsoleSize left, ConsoleSize right) noexcept {
    return left.columns == right.columns && left.rows == right.rows;
  }
  friend bool operator!=(ConsoleSize left, ConsoleSize right) noexcept {
    return !(left == right);
  }
};

/* Owns raw VT console mode and restores every modified process setting. */
class ConsoleSession final {
public:
  ConsoleSession();
  ConsoleSession(HANDLE input, HANDLE output);
  ~ConsoleSession();

  ConsoleSession(const ConsoleSession &) = delete;
  ConsoleSession &operator=(const ConsoleSession &) = delete;

  [[nodiscard]] HANDLE input_handle() const noexcept { return input_; }
  [[nodiscard]] HANDLE output_handle() const noexcept { return output_; }
  [[nodiscard]] ConsoleSize size() const;
  void write(std::string_view bytes) const;
  void restore() noexcept;

private:
  void initialize();

  HANDLE input_{INVALID_HANDLE_VALUE};
  HANDLE output_{INVALID_HANDLE_VALUE};
  DWORD saved_input_mode_{0};
  DWORD saved_output_mode_{0};
  UINT saved_input_codepage_{0};
  UINT saved_output_codepage_{0};
  bool active_{false};
};

/*
 * ReadFile on a console cannot be waited alongside WSAEVENT handles. This
 * pump dedicates one blocking thread to VT input and exposes a manual-reset
 * event to the main loop. No input translation is performed: Windows emits
 * the same VT byte stream that Mosh sends to a POSIX server.
 */
class ConsoleInputPump final {
public:
  explicit ConsoleInputPump(HANDLE input);
  ~ConsoleInputPump();

  ConsoleInputPump(const ConsoleInputPump &) = delete;
  ConsoleInputPump &operator=(const ConsoleInputPump &) = delete;

  [[nodiscard]] HANDLE event_handle() const noexcept { return ready_event_; }
  [[nodiscard]] std::string take();
  [[nodiscard]] bool eof() const noexcept { return eof_.load(); }
  [[nodiscard]] DWORD error() const noexcept { return error_.load(); }
  void stop() noexcept;

private:
  static DWORD WINAPI thread_entry(void *context) noexcept;
  void run() noexcept;

  HANDLE input_;
  HANDLE ready_event_{nullptr};
  HANDLE thread_{nullptr};
  mutable std::mutex mutex_;
  std::string buffer_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> eof_{false};
  std::atomic<DWORD> error_{ERROR_SUCCESS};
};

/* Converts console close/Ctrl events into a normal waitable event. */
class ConsoleSignal final {
public:
  ConsoleSignal();
  ~ConsoleSignal();

  ConsoleSignal(const ConsoleSignal &) = delete;
  ConsoleSignal &operator=(const ConsoleSignal &) = delete;

  [[nodiscard]] HANDLE event_handle() const noexcept { return event_; }
  void acknowledge() noexcept { ResetEvent(event_); }

private:
  static BOOL WINAPI handler(DWORD control_type) noexcept;
  HANDLE event_{nullptr};
};

} // namespace mosh::win32
