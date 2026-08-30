#pragma once

#include <cstddef>
#include <string_view>

namespace tokmon::desk {

[[nodiscard]] std::size_t previous_grapheme_boundary(
    std::string_view utf8, std::size_t offset) noexcept;
[[nodiscard]] std::size_t next_grapheme_boundary(
    std::string_view utf8, std::size_t offset) noexcept;
[[nodiscard]] std::size_t clamp_grapheme_boundary(
    std::string_view utf8, std::size_t offset) noexcept;
[[nodiscard]] std::size_t grapheme_count(std::string_view utf8) noexcept;

} // namespace tokmon::desk
