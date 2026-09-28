/* GPL-3.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mosh::win32::utf8 {

inline constexpr char32_t replacement_character = U'\uFFFD';

/* Encode one Unicode scalar value. Invalid scalar values become U+FFFD. */
void append(std::string &destination, char32_t codepoint);
[[nodiscard]] std::string encode(std::u32string_view text);

/*
 * Strict incremental UTF-8 decoder. It rejects overlong encodings,
 * surrogate code points, and values above U+10FFFF. Invalid input is
 * represented by U+FFFD and parsing resumes at the next possible lead byte.
 */
class Decoder final {
public:
  void feed(std::string_view bytes, std::u32string &output);
  void finish(std::u32string &output);
  void reset() noexcept { pending_.clear(); }
  [[nodiscard]] bool has_pending_input() const noexcept {
    return !pending_.empty();
  }

private:
  std::vector<std::uint8_t> pending_;
  void decode_available(std::u32string &output, bool finish);
};

[[nodiscard]] std::u32string decode(std::string_view bytes);

/* wcwidth-compatible result: -1 control, 0 combining, 1 narrow, 2 wide. */
[[nodiscard]] int display_width(char32_t codepoint) noexcept;

} // namespace mosh::win32::utf8
