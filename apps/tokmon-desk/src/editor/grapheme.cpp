#include "editor/grapheme.hpp"

#include <algorithm>
#include <cstdint>

namespace tokmon::desk {
namespace {

struct CodePoint {
  char32_t value{0xfffd};
  std::size_t begin{0};
  std::size_t end{0};
};

CodePoint decode_next(const std::string_view text, std::size_t offset) noexcept {
  offset = std::min(offset, text.size());
  CodePoint result{0xfffd, offset, std::min(offset + 1, text.size())};
  if (offset >= text.size()) {
    result.end = offset;
    return result;
  }
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80) {
    result.value = first;
    return result;
  }
  int length = 0;
  char32_t value = 0;
  if ((first & 0xe0u) == 0xc0u) { length = 2; value = first & 0x1fu; }
  else if ((first & 0xf0u) == 0xe0u) { length = 3; value = first & 0x0fu; }
  else if ((first & 0xf8u) == 0xf0u) { length = 4; value = first & 0x07u; }
  else return result;
  if (offset + static_cast<std::size_t>(length) > text.size())
    return result;
  for (int index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xc0u) != 0x80u)
      return result;
    value = (value << 6u) | (continuation & 0x3fu);
  }
  result.value = value;
  result.end = offset + static_cast<std::size_t>(length);
  return result;
}

CodePoint decode_previous(const std::string_view text,
                          std::size_t offset) noexcept {
  offset = std::min(offset, text.size());
  if (offset == 0)
    return {0, 0, 0};
  auto begin = offset - 1;
  while (begin > 0 &&
         (static_cast<unsigned char>(text[begin]) & 0xc0u) == 0x80u)
    --begin;
  auto result = decode_next(text, begin);
  if (result.end > offset)
    return {0xfffd, offset - 1, offset};
  return result;
}

bool in(const char32_t value, const char32_t first,
        const char32_t last) noexcept {
  return value >= first && value <= last;
}

bool extend(const char32_t value) noexcept {
  return in(value, 0x0300, 0x036f) || in(value, 0x0483, 0x0489) ||
         in(value, 0x0591, 0x05bd) || in(value, 0x05bf, 0x05bf) ||
         in(value, 0x05c1, 0x05c2) || in(value, 0x0610, 0x061a) ||
         in(value, 0x064b, 0x065f) || in(value, 0x0670, 0x0670) ||
         in(value, 0x06d6, 0x06ed) || in(value, 0x0900, 0x0903) ||
         in(value, 0x093a, 0x094f) || in(value, 0x0981, 0x0983) ||
         in(value, 0x1ab0, 0x1aff) || in(value, 0x1dc0, 0x1dff) ||
         in(value, 0x20d0, 0x20ff) || in(value, 0xfe00, 0xfe0f) ||
         in(value, 0xfe20, 0xfe2f) || in(value, 0xe0100, 0xe01ef) ||
         in(value, 0x1f3fb, 0x1f3ff);
}

bool regional(const char32_t value) noexcept {
  return in(value, 0x1f1e6, 0x1f1ff);
}

bool hangul_l(const char32_t value) noexcept {
  return in(value, 0x1100, 0x115f) || in(value, 0xa960, 0xa97c);
}
bool hangul_v(const char32_t value) noexcept {
  return in(value, 0x1160, 0x11a7) || in(value, 0xd7b0, 0xd7c6);
}
bool hangul_t(const char32_t value) noexcept {
  return in(value, 0x11a8, 0x11ff) || in(value, 0xd7cb, 0xd7fb);
}
bool hangul_lv(const char32_t value) noexcept {
  return in(value, 0xac00, 0xd7a3) && (value - 0xac00) % 28 == 0;
}
bool hangul_lvt(const char32_t value) noexcept {
  return in(value, 0xac00, 0xd7a3) && (value - 0xac00) % 28 != 0;
}

bool joins(const char32_t left, const char32_t right) noexcept {
  if (left == '\r' && right == '\n')
    return true;
  if (extend(right) || right == 0x200d)
    return true;
  if (hangul_l(left) && (hangul_l(right) || hangul_v(right) ||
                         hangul_lv(right) || hangul_lvt(right)))
    return true;
  if ((hangul_lv(left) || hangul_v(left)) &&
      (hangul_v(right) || hangul_t(right)))
    return true;
  if ((hangul_lvt(left) || hangul_t(left)) && hangul_t(right))
    return true;
  return left == 0x200d;
}

std::size_t next_from_boundary(const std::string_view text,
                               const std::size_t offset) noexcept {
  if (offset >= text.size())
    return text.size();
  auto current = decode_next(text, offset);
  auto end = current.end;
  int regional_count = regional(current.value) ? 1 : 0;
  while (end < text.size()) {
    const auto following = decode_next(text, end);
    if (regional(current.value) && regional(following.value)) {
      if ((regional_count % 2) == 1) {
        ++regional_count;
        end = following.end;
        current = following;
        continue;
      }
      break;
    }
    if (!joins(current.value, following.value))
      break;
    end = following.end;
    current = following;
  }
  return end;
}

} // namespace

std::size_t next_grapheme_boundary(const std::string_view text,
                                   std::size_t offset) noexcept {
  offset = clamp_grapheme_boundary(text, offset);
  return next_from_boundary(text, offset);
}

std::size_t previous_grapheme_boundary(const std::string_view text,
                                       std::size_t offset) noexcept {
  offset = std::min(offset, text.size());
  if (offset == 0)
    return 0;
  // Walking forward from the nearest safe line boundary keeps the backward
  // rules consistent for ZWJ and regional-indicator clusters without scanning
  // the complete document.
  auto scan = text.rfind('\n', offset - 1);
  scan = scan == std::string_view::npos ? 0 : scan + 1;
  std::size_t previous = scan;
  while (scan < offset) {
    previous = scan;
    const auto next = next_grapheme_boundary(text, scan);
    if (next >= offset)
      return previous;
    scan = next > scan ? next : scan + 1;
  }
  return previous;
}

std::size_t clamp_grapheme_boundary(const std::string_view text,
                                    std::size_t offset) noexcept {
  offset = std::min(offset, text.size());
  while (offset > 0 && offset < text.size() &&
         (static_cast<unsigned char>(text[offset]) & 0xc0u) == 0x80u)
    --offset;
  const auto line_break = offset == 0 ? std::string_view::npos
      : text.rfind('\n', offset - 1);
  auto scan = line_break == std::string_view::npos ? 0 : line_break + 1;
  while (scan < offset) {
    const auto next = next_from_boundary(text, scan);
    if (next > offset)
      return scan;
    scan = next > scan ? next : scan + 1;
  }
  return scan;
}

std::size_t grapheme_count(const std::string_view text) noexcept {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto next = next_grapheme_boundary(text, offset);
    offset = next > offset ? next : offset + 1;
    ++count;
  }
  return count;
}

} // namespace tokmon::desk
