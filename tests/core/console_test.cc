#include "platform/win32_console.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "console_test: " << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main() {
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  expect(CreatePipe(&read_pipe, &write_pipe, nullptr, 0) != FALSE,
         "CreatePipe failed");

  bool rejected_non_console = false;
  try {
    mosh::win32::ConsoleSession invalid(read_pipe, write_pipe);
  } catch (const mosh::win32::ConsoleError &) {
    rejected_non_console = true;
  }
  expect(rejected_non_console, "non-console handles were accepted");

  {
    mosh::win32::ConsoleInputPump pump(read_pipe);
    const char bytes[] = "vt-input";
    DWORD written = 0;
    expect(WriteFile(write_pipe, bytes, sizeof(bytes) - 1, &written, nullptr) !=
               FALSE &&
               written == sizeof(bytes) - 1,
           "pipe WriteFile failed");
    expect(WaitForSingleObject(pump.event_handle(), 2000) == WAIT_OBJECT_0,
           "input pump did not signal");
    expect(pump.take() == "vt-input", "input pump changed bytes");
    pump.stop();
  }
  /* Exercise the cancellation window before and during a blocking ReadFile. */
  for (int iteration = 0; iteration < 64; ++iteration) {
    mosh::win32::ConsoleInputPump pump(read_pipe);
    pump.stop();
  }
  CloseHandle(write_pipe);
  CloseHandle(read_pipe);

  HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD input_mode = 0;
  DWORD output_mode = 0;
  if (GetConsoleMode(stdin_handle, &input_mode) &&
      GetConsoleMode(stdout_handle, &output_mode)) {
    const UINT input_cp = GetConsoleCP();
    const UINT output_cp = GetConsoleOutputCP();
    {
      mosh::win32::ConsoleSession session(stdin_handle, stdout_handle);
      DWORD active_input = 0;
      DWORD active_output = 0;
      expect(GetConsoleMode(stdin_handle, &active_input) &&
                 (active_input & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
             "VT input mode not enabled");
      expect(GetConsoleMode(stdout_handle, &active_output) &&
                 (active_output & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0,
             "VT output mode not enabled");
    }
    DWORD restored_input = 0;
    DWORD restored_output = 0;
    expect(GetConsoleMode(stdin_handle, &restored_input) &&
               restored_input == input_mode,
           "input mode not restored");
    expect(GetConsoleMode(stdout_handle, &restored_output) &&
               restored_output == output_mode,
           "output mode not restored");
    expect(GetConsoleCP() == input_cp && GetConsoleOutputCP() == output_cp,
           "console code pages not restored");
  }
  return 0;
}
