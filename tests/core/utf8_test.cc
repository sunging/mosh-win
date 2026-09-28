#include "platform/utf8.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "utf8_test: " << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main() {
  using mosh::win32::utf8::decode;
  using mosh::win32::utf8::display_width;
  using mosh::win32::utf8::encode;
  using mosh::win32::utf8::replacement_character;

  expect(display_width(U'中') == 2, "U+4E2D must be wide");
  expect(display_width(U'\u0301') == 0, "U+0301 must combine");
  expect(display_width(U'\U0001F600') == 2, "U+1F600 must be wide");
  expect(display_width(U'\u200D') == 0, "U+200D must be zero-width");
  expect(display_width(U'\U0001FAE8') == 2, "U+1FAE8 must be wide");
  expect(display_width(U'\U00020000') == 2, "CJK Extension B must be wide");
  expect(display_width(U'\U0001D400') == 1,
         "mathematical alphanumeric must be narrow");
  expect(display_width(U'\U0001F100') == 1,
         "enclosed alphanumeric must be narrow");
  expect(display_width(U'\u00AD') == 1, "soft hyphen policy must be narrow");
  expect(display_width(U'\U0001F3FB') == 2,
         "emoji modifier policy must be wide");

  const std::u32string scalars = U"A中\U0001F600\U00020000";
  expect(decode(encode(scalars)) == scalars, "four-byte UTF-8 round trip");

  mosh::win32::utf8::Decoder streaming;
  std::u32string streamed;
  streaming.feed("\xF0\x9F", streamed);
  expect(streamed.empty() && streaming.has_pending_input(),
         "split sequence must remain pending");
  streaming.feed("\x98\x80", streamed);
  expect(streamed == U"\U0001F600" && !streaming.has_pending_input(),
         "split four-byte sequence did not decode");

  const std::u32string surrogate = decode("\xED\xA0\x80");
  expect(!surrogate.empty(), "encoded surrogate must produce replacement");
  for (const char32_t value : surrogate) {
    expect(value == replacement_character,
           "encoded surrogate escaped strict validation");
  }
  expect(encode(std::u32string(1, static_cast<char32_t>(0xD800))) ==
             "\xEF\xBF\xBD",
         "surrogate scalar must encode as U+FFFD");

  streaming.reset();
  streamed.clear();
  streaming.feed("\xF0\x9F", streamed);
  streaming.finish(streamed);
  expect(streamed == std::u32string(1, replacement_character),
         "truncated sequence must finish as one replacement");

  /* A lead byte followed by a non-continuation byte is invalid as soon as
     both bytes are known; the ASCII byte must not wait for more input. */
  streaming.reset();
  streamed.clear();
  streaming.feed("\xE2" "A", streamed);
  expect(streamed == U"�A" && !streaming.has_pending_input(),
         "broken sequence must be replaced without further input");

  /* Streaming and one-shot decoding must agree byte-for-byte. */
  const std::string mixed = "a\xE2\x82" "b\xF0\x9F\x98\x80\xC3" "c\xED\xA0\x80";
  std::u32string incremental;
  streaming.reset();
  for (const char byte : mixed) {
    streaming.feed(std::string_view(&byte, 1), incremental);
  }
  streaming.finish(incremental);
  expect(incremental == decode(mixed),
         "byte-at-a-time decoding differs from one-shot decoding");
  return 0;
}
