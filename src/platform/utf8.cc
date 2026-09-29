/*
 * Unicode helpers for the native Windows Mosh client.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utf8.h"
#include "unicode_width_table.h"

#include <algorithm>

namespace mosh::win32::utf8 {
namespace {

template <std::size_t N>
[[nodiscard]] bool in_intervals(
    char32_t codepoint,
    const std::array<detail::UnicodeInterval, N> &intervals) noexcept {
  const auto it = std::lower_bound(
      intervals.begin(), intervals.end(), codepoint,
      [](const detail::UnicodeInterval &interval, char32_t value) {
        return interval.last < value;
      });
  return it != intervals.end() && it->first <= codepoint;
}

[[nodiscard]] bool continuation(std::uint8_t byte) noexcept {
  return (byte & 0xC0U) == 0x80U;
}

} // namespace

void append(std::string &destination, char32_t codepoint) {
  const auto scalar = static_cast<std::uint32_t>(codepoint);
  if (scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
    codepoint = replacement_character;
  }

  const auto value = static_cast<std::uint32_t>(codepoint);
  if (value <= 0x7FU) {
    destination.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    destination.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    destination.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    destination.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    destination.push_back(
        static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    destination.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    destination.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    destination.push_back(
        static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    destination.push_back(
        static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    destination.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

std::string encode(std::u32string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char32_t codepoint : text) {
    append(result, codepoint);
  }
  return result;
}

void Decoder::feed(std::string_view bytes, std::u32string &output) {
  pending_.reserve(pending_.size() + bytes.size());
  for (const unsigned char byte : bytes) {
    pending_.push_back(byte);
  }
  decode_available(output, false);
}

void Decoder::finish(std::u32string &output) {
  decode_available(output, true);
  pending_.clear();
}

void Decoder::decode_available(std::u32string &output, bool finish) {
  std::size_t offset = 0;
  while (offset < pending_.size()) {
    const std::uint8_t lead = pending_[offset];
    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;

    if (lead <= 0x7FU) {
      length = 1;
      value = lead;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
      value = lead & 0x1FU;
      minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
      value = lead & 0x0FU;
      minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
      value = lead & 0x07U;
      minimum = 0x10000U;
    } else {
      output.push_back(replacement_character);
      ++offset;
      continue;
    }

    /* Validate the continuation bytes that are already available, so a
       sequence that is provably broken (for example "\xE2" followed by
       ASCII) is replaced immediately instead of waiting for more input.
       The result is identical to decoding the complete byte string. */
    const std::size_t available = std::min(length, pending_.size() - offset);
    bool valid = true;
    for (std::size_t index = 1; index < available; ++index) {
      const std::uint8_t byte = pending_[offset + index];
      if (!continuation(byte)) {
        valid = false;
        break;
      }
      value = (value << 6U) | (byte & 0x3FU);
    }
    if (!valid) {
      output.push_back(replacement_character);
      ++offset;
      continue;
    }

    if (available < length) {
      if (finish) {
        output.push_back(replacement_character);
        offset = pending_.size();
      }
      break;
    }

    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
      output.push_back(replacement_character);
      ++offset;
      continue;
    }

    output.push_back(static_cast<char32_t>(value));
    offset += length;
  }

  if (offset != 0) {
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(offset));
  }
}

std::u32string decode(std::string_view bytes) {
  Decoder decoder;
  std::u32string result;
  result.reserve(bytes.size());
  decoder.feed(bytes, result);
  decoder.finish(result);
  return result;
}

int display_width(char32_t codepoint) noexcept {
  const auto scalar = static_cast<std::uint32_t>(codepoint);
  if (scalar == 0) {
    return 0;
  }
  if (scalar < 0x20U || (scalar >= 0x7FU && scalar < 0xA0U) ||
      scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
    return -1;
  }
  if (scalar == 0x200BU || scalar == 0x200CU || scalar == 0x200DU ||
      scalar == 0x2060U || scalar == 0xFEFFU ||
      (scalar >= 0xE0100U && scalar <= 0xE01EFU) ||
      in_intervals(codepoint, detail::kUnicodeZeroWidth)) {
    return 0;
  }
  return in_intervals(codepoint, detail::kUnicodeWide) ? 2 : 1;
}

} // namespace mosh::win32::utf8
