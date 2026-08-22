#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tokmon {

using Sha256 = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256 sha256(std::span<const std::uint8_t> bytes);
[[nodiscard]] Sha256 sha256(std::string_view text);
[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string sha256_hex(std::string_view text);
[[nodiscard]] std::string hmac_sha256_hex(std::string_view key,
                                          std::string_view message);

}  // namespace tokmon
