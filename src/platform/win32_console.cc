/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "win32_console.h"
#include "win32_error.h"

#include <algorithm>
#include <array>
#include <limits>

namespace mosh::win32 {
namespace {

std::atomic<HANDLE> signal_event{nullptr};
constexpr std::size_t max_input_buffer = 1024U * 1024U;

[[nodiscard]] std::string windows_error_message(const char *operation,
                                                DWORD error) {
  std::string message = std::string(operation) + " failed (Windows error " +
                        std::to_string(error) + ')';
  const std::string text = system_error_text(error);
  if (!text.empty()) {
    message += ": ";
    message += text;
  }
  return message;
}

[[noreturn]] void throw_last_error(const char *operation) {
  throw ConsoleError(operation, GetLastError());
}

void set_console_mode_checked(HANDLE handle, DWORD mode,
                              const char *operation) {
  if (SetConsoleMode(handle, mode) == FALSE) {
    throw_last_error(operation);
  }
}

} // namespace

ConsoleError::ConsoleError(const char *operation, DWORD error)
    : std::runtime_error(windows_error_message(operation, error)),
      error_(error) {}

ConsoleSession::ConsoleSession()
    : ConsoleSession(GetStdHandle(STD_INPUT_HANDLE),
                     GetStdHandle(STD_OUTPUT_HANDLE)) {}

ConsoleSession::ConsoleSession(HANDLE input, HANDLE output)
    : input_(input), output_(output) {
  initialize();
}

ConsoleSession::~ConsoleSession() { restore(); }

void ConsoleSession::initialize() {
  if (input_ == nullptr || input_ == INVALID_HANDLE_VALUE ||
      output_ == nullptr || output_ == INVALID_HANDLE_VALUE) {
    throw ConsoleError("GetStdHandle", ERROR_INVALID_HANDLE);
  }
  if (GetConsoleMode(input_, &saved_input_mode_) == FALSE) {
    throw_last_error("GetConsoleMode(stdin)");
  }
  if (GetConsoleMode(output_, &saved_output_mode_) == FALSE) {
    throw_last_error("GetConsoleMode(stdout)");
  }

  saved_input_codepage_ = GetConsoleCP();
  saved_output_codepage_ = GetConsoleOutputCP();
  if (saved_input_codepage_ == 0 || saved_output_codepage_ == 0) {
    throw_last_error("GetConsoleCP");
  }

  /* Mark active before the first mutation so exceptions restore partial work. */
  active_ = true;
  try {
    if (SetConsoleCP(CP_UTF8) == FALSE) {
      throw_last_error("SetConsoleCP(CP_UTF8)");
    }
    if (SetConsoleOutputCP(CP_UTF8) == FALSE) {
      throw_last_error("SetConsoleOutputCP(CP_UTF8)");
    }

    DWORD input_mode = saved_input_mode_;
    input_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                    ENABLE_PROCESSED_INPUT);
    input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    set_console_mode_checked(input_, input_mode,
                             "SetConsoleMode(stdin VT input)");

    DWORD output_mode = saved_output_mode_;
    output_mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    output_mode &= ~DISABLE_NEWLINE_AUTO_RETURN;
    set_console_mode_checked(output_, output_mode,
                             "SetConsoleMode(stdout VT output)");
  } catch (...) {
    restore();
    throw;
  }
}

ConsoleSize ConsoleSession::size() const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(output_, &info) == FALSE) {
    throw_last_error("GetConsoleScreenBufferInfo");
  }
  const SHORT columns = info.srWindow.Right - info.srWindow.Left + 1;
  const SHORT rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  if (columns <= 0 || rows <= 0) {
    throw ConsoleError("GetConsoleScreenBufferInfo(size)",
                       ERROR_INVALID_DATA);
  }
  return {static_cast<std::uint16_t>(columns),
          static_cast<std::uint16_t>(rows)};
}

void ConsoleSession::write(std::string_view bytes) const {
  while (!bytes.empty()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size(), std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (WriteFile(output_, bytes.data(), chunk, &written, nullptr) == FALSE) {
      throw_last_error("WriteFile(console)");
    }
    if (written == 0) {
      throw ConsoleError("WriteFile(console short write)", ERROR_WRITE_FAULT);
    }
    bytes.remove_prefix(written);
  }
}

void ConsoleSession::restore() noexcept {
  if (!active_) {
    return;
  }
  active_ = false;
  SetConsoleMode(output_, saved_output_mode_);
  SetConsoleMode(input_, saved_input_mode_);
  if (saved_output_codepage_ != 0) {
    SetConsoleOutputCP(saved_output_codepage_);
  }
  if (saved_input_codepage_ != 0) {
    SetConsoleCP(saved_input_codepage_);
  }
}

ConsoleInputPump::ConsoleInputPump(HANDLE input) : input_(input) {
  if (input_ == nullptr || input_ == INVALID_HANDLE_VALUE) {
    throw ConsoleError("ConsoleInputPump", ERROR_INVALID_HANDLE);
  }
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ready_event_ == nullptr) {
    throw_last_error("CreateEventW(console input)");
  }
  thread_ = CreateThread(nullptr, 0, &ConsoleInputPump::thread_entry, this, 0,
                         nullptr);
  if (thread_ == nullptr) {
    const DWORD error = GetLastError();
    CloseHandle(ready_event_);
    ready_event_ = nullptr;
    throw ConsoleError("CreateThread(console input)", error);
  }
}

ConsoleInputPump::~ConsoleInputPump() {
  stop();
  if (ready_event_ != nullptr) {
    CloseHandle(ready_event_);
  }
}

DWORD WINAPI ConsoleInputPump::thread_entry(void *context) noexcept {
  static_cast<ConsoleInputPump *>(context)->run();
  return 0;
}

void ConsoleInputPump::run() noexcept {
  std::array<char, 16384> bytes{};
  for (;;) {
    if (stopping_.load()) {
      eof_.store(true);
      SetEvent(ready_event_);
      return;
    }
    DWORD count = 0;
    const BOOL ok = ReadFile(input_, bytes.data(),
                             static_cast<DWORD>(bytes.size()), &count,
                             nullptr);
    if (ok == FALSE) {
      const DWORD code = GetLastError();
      if (!stopping_.load() && code != ERROR_OPERATION_ABORTED) {
        error_.store(code);
      }
      eof_.store(true);
      SetEvent(ready_event_);
      return;
    }
    if (count == 0) {
      eof_.store(true);
      SetEvent(ready_event_);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (buffer_.size() > max_input_buffer - count) {
        error_.store(ERROR_BUFFER_OVERFLOW);
        eof_.store(true);
        SetEvent(ready_event_);
        return;
      }
      buffer_.append(bytes.data(), count);
      SetEvent(ready_event_);
    }
  }
}

std::string ConsoleInputPump::take() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string result;
  result.swap(buffer_);
  ResetEvent(ready_event_);
  /* Producer cannot append while mutex_ is held, so no wakeup is lost. */
  return result;
}

void ConsoleInputPump::stop() noexcept {
  if (stopping_.exchange(true)) {
    return;
  }
  if (thread_ != nullptr) {
    /* Cancellation can race with the narrow gap between two ReadFile calls.
       Retry until the thread has observed stopping_: if the first cancel ran
       before the next ReadFile became pending, the following pass cancels it.
       This keeps the pump from ever outliving the ConsoleSession it refers to. */
    while (WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT) {
      if (CancelSynchronousIo(thread_) == FALSE) {
        CancelIoEx(input_, nullptr);
      }
      if (WaitForSingleObject(thread_, 20) == WAIT_OBJECT_0) {
        break;
      }
    }
    CloseHandle(thread_);
    thread_ = nullptr;
  }
}

ConsoleSignal::ConsoleSignal() {
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event_ == nullptr) {
    throw_last_error("CreateEventW(console signal)");
  }
  HANDLE expected = nullptr;
  if (!signal_event.compare_exchange_strong(expected, event_)) {
    CloseHandle(event_);
    event_ = nullptr;
    throw ConsoleError("ConsoleSignal(singleton)", ERROR_ALREADY_EXISTS);
  }
  if (SetConsoleCtrlHandler(&ConsoleSignal::handler, TRUE) == FALSE) {
    signal_event.store(nullptr);
    CloseHandle(event_);
    event_ = nullptr;
    throw_last_error("SetConsoleCtrlHandler");
  }
}

ConsoleSignal::~ConsoleSignal() {
  if (event_ != nullptr) {
    SetConsoleCtrlHandler(&ConsoleSignal::handler, FALSE);
    signal_event.store(nullptr);
    CloseHandle(event_);
  }
}

BOOL WINAPI ConsoleSignal::handler(DWORD control_type) noexcept {
  switch (control_type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT: {
    const HANDLE event = signal_event.load();
    if (event != nullptr) {
      SetEvent(event);
      return TRUE;
    }
    return FALSE;
  }
  default:
    return FALSE;
  }
}

} // namespace mosh::win32
