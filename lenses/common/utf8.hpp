#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace tokmon::builtin {

namespace utf8_detail {

inline std::size_t valid_sequence_width(const std::string_view text,
                                        const std::size_t index) noexcept {
  const auto byte = [](const char value) {
    return static_cast<unsigned char>(value);
  };
  const auto first = byte(text[index]);
  if (first < 0x80u) return 1u;
  const auto continuation = [&](const std::size_t offset) {
    return index + offset < text.size() &&
        (byte(text[index + offset]) & 0xc0u) == 0x80u;
  };
  if (first >= 0xc2u && first <= 0xdfu)
    return continuation(1u) ? 2u : 0u;
  if (first >= 0xe0u && first <= 0xefu) {
    if (!continuation(1u) || !continuation(2u)) return 0u;
    const auto second = byte(text[index + 1u]);
    if ((first == 0xe0u && second < 0xa0u) ||
        (first == 0xedu && second >= 0xa0u)) return 0u;
    return 3u;
  }
  if (first >= 0xf0u && first <= 0xf4u) {
    if (!continuation(1u) || !continuation(2u) || !continuation(3u)) return 0u;
    const auto second = byte(text[index + 1u]);
    if ((first == 0xf0u && second < 0x90u) ||
        (first == 0xf4u && second >= 0x90u)) return 0u;
    return 4u;
  }
  return 0u;
}

}  // namespace utf8_detail

inline bool valid_utf8(const std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto width = utf8_detail::valid_sequence_width(text, index);
    if (width == 0u) return false;
    index += width;
  }
  return true;
}

// Returns valid UTF-8 no larger than max_bytes. Invalid source bytes are
// replaced with U+FFFD, while a valid multi-byte sequence is never split.
inline std::string bounded_utf8(const std::string_view text,
                                const std::size_t max_bytes) {
  constexpr std::string_view replacement{"\xef\xbf\xbd", 3u};
  std::string result;
  result.reserve(std::min(text.size(), max_bytes));
  for (std::size_t index = 0; index < text.size();) {
    const auto width = utf8_detail::valid_sequence_width(text, index);
    if (width != 0u) {
      if (result.size() + width > max_bytes) break;
      result.append(text, index, width);
      index += width;
      continue;
    }
    if (result.size() + replacement.size() > max_bytes) break;
    result.append(replacement);
    ++index;
  }
  return result;
}

inline std::string repair_utf8(const std::string_view text) {
  if (valid_utf8(text)) return std::string(text);
  // Every invalid byte can expand from one byte to the three-byte replacement.
  return bounded_utf8(text, text.size() * 3u);
}

}  // namespace tokmon::builtin
