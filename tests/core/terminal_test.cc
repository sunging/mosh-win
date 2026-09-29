/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "completeterminal.h"
#include "test_support.h"
#include "hostinput.pb.h"
#include "parseraction.h"
#include "terminaldisplay.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using mosh::test::expect;

std::string cell_text(const Terminal::Framebuffer &framebuffer, int row,
                      int column) {
  std::string result;
  framebuffer.get_cell(row, column)->print_grapheme(result);
  return result;
}

void expect_host_diff(const std::string &wire, bool expect_resize,
                      int expected_width = 0, int expected_height = 0) {
  HostBuffers::HostMessage message;
  expect(message.ParseFromString(wire), "terminal diff is not valid protobuf");

  bool saw_host_bytes = false;
  bool saw_resize = false;
  for (int index = 0; index < message.instruction_size(); ++index) {
    const HostBuffers::Instruction &instruction = message.instruction(index);
    if (instruction.HasExtension(HostBuffers::hostbytes)) {
      saw_host_bytes = true;
      expect(!instruction.GetExtension(HostBuffers::hostbytes)
                  .hoststring()
                  .empty(),
             "terminal diff contains an empty host-byte instruction");
    }
    if (instruction.HasExtension(HostBuffers::resize)) {
      saw_resize = true;
      const HostBuffers::ResizeMessage &resize =
          instruction.GetExtension(HostBuffers::resize);
      expect(static_cast<int>(resize.width()) == expected_width &&
                 static_cast<int>(resize.height()) == expected_height,
             "terminal diff contains the wrong resize dimensions");
    }
  }

  expect(saw_host_bytes, "terminal diff contains no framebuffer update");
  expect(saw_resize == expect_resize,
         expect_resize ? "terminal diff lost resize instruction"
                       : "same-size terminal diff unexpectedly resized");
}

void test_unicode_framebuffer() {
  const std::string chinese = "\xE4\xB8\xAD";       // U+4E2D
  const std::string combining = "e\xCC\x81";        // e + U+0301
  const std::string emoji = "\xF0\x9F\x98\x80";   // U+1F600

  Terminal::Complete terminal(16, 4);
  const std::string input = std::string("A") + chinese + combining + emoji;
  expect(terminal.act(input).empty(),
         "printable UTF-8 unexpectedly generated a host response");

  const Terminal::Framebuffer &framebuffer = terminal.get_fb();
  expect(framebuffer.ds.get_width() == 16 &&
             framebuffer.ds.get_height() == 4,
         "framebuffer dimensions changed while printing");
  expect(cell_text(framebuffer, 0, 0) == "A", "ASCII cell changed");
  expect(cell_text(framebuffer, 0, 1) == chinese,
         "Chinese cell did not preserve UTF-8");
  expect(framebuffer.get_cell(0, 1)->get_wide(),
         "Chinese cell was not marked wide");
  expect(framebuffer.get_cell(0, 2)->empty(),
         "Chinese trailing cell was not cleared");
  expect(cell_text(framebuffer, 0, 3) == combining,
         "combining mark was not attached to its base character");
  expect(!framebuffer.get_cell(0, 3)->get_wide(),
         "combining grapheme was marked wide");
  expect(cell_text(framebuffer, 0, 4) == emoji,
         "non-BMP emoji did not preserve UTF-8");
  expect(framebuffer.get_cell(0, 4)->get_wide(),
         "non-BMP emoji was not marked wide");
  expect(framebuffer.get_cell(0, 5)->empty(),
         "emoji trailing cell was not cleared");
  expect(framebuffer.ds.get_cursor_col() == 6 &&
             framebuffer.ds.get_cursor_row() == 0,
         "Unicode display widths produced the wrong cursor position");
}

void test_framebuffer_diff_and_resize() {
  const std::string unicode_line =
      std::string("A") + "\xE4\xB8\xAD" + "e\xCC\x81" +
      "\xF0\x9F\x98\x80";

  Terminal::Complete authoritative(16, 4);
  Terminal::Complete replica(16, 4);
  authoritative.act(unicode_line);

  std::string diff = authoritative.diff_from(replica);
  expect_host_diff(diff, false);
  replica.apply_string(diff);
  expect(!authoritative.compare(replica),
         "initial framebuffer diff did not reconstruct Unicode state");
  expect(authoritative.diff_from(replica).empty(),
         "synchronized terminals still produced a display diff");

  authoritative.act("\x1B[2;3H\x1B[38;5;196mR\x1B[0m\x07");
  diff = authoritative.diff_from(replica);
  expect_host_diff(diff, false);
  replica.apply_string(diff);
  expect(!authoritative.compare(replica),
         "incremental framebuffer diff did not reconstruct state");

  const Parser::Resize resize(10, 3);
  authoritative.act(resize);
  expect(authoritative.get_fb().ds.get_width() == 10 &&
             authoritative.get_fb().ds.get_height() == 3,
         "resize action did not update authoritative framebuffer");

  diff = authoritative.diff_from(replica);
  expect_host_diff(diff, true, 10, 3);
  replica.apply_string(diff);
  expect(replica.get_fb().ds.get_width() == 10 &&
             replica.get_fb().ds.get_height() == 3,
         "resize instruction did not update replica framebuffer");
  expect(!authoritative.compare(replica),
         "resize diff did not reconstruct terminal state");

  // The display protocol may redraw blank cells after a resize because a
  // literal space and an empty cell are intentionally display-equivalent.
  // Applying such a conservative repaint must preserve the reconstructed
  // framebuffer even when the encoded diff is not byte-empty.
  replica.apply_string(authoritative.diff_from(replica));
  expect(!authoritative.compare(replica),
         "post-resize conservative repaint changed terminal state");
}

void test_alternate_screen_sequences() {
  expect(_putenv_s("MOSH_NO_TERM_INIT", "") == 0,
         "could not clear MOSH_NO_TERM_INIT");
  const Terminal::Display display(false);
  expect(display.open() == "\x1B[?1049h\x1B[?1h",
         "display open did not enter alternate screen and application mode");
  expect(display.close() ==
             "\x1B[?1l\x1B[0m\x1B[?25h"
             "\x1B[?1003l\x1B[?1002l\x1B[?1001l\x1B[?1000l"
             "\x1B[?1015l\x1B[?1006l\x1B[?1005l\x1B[?1049l",
         "display close did not restore VT modes and primary screen");

  expect(_putenv_s("MOSH_NO_TERM_INIT", "1") == 0,
         "could not set MOSH_NO_TERM_INIT");
  const Terminal::Display no_init(false);
  expect(no_init.open() == "\x1B[?1h",
         "no-init display unexpectedly entered alternate screen");
  expect(no_init.close().find("\x1B[?1049l") == std::string::npos,
         "no-init display unexpectedly restored alternate screen");
  expect(no_init.close().find("\x1B[?25h") != std::string::npos,
         "no-init display did not restore cursor visibility");
  expect(_putenv_s("MOSH_NO_TERM_INIT", "") == 0,
         "could not restore MOSH_NO_TERM_INIT");
}

} // namespace

int main() {
  test_unicode_framebuffer();
  test_framebuffer_diff_and_resize();
  test_alternate_screen_sequences();
  return 0;
}
