#include "tokmon/hash.hpp"

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace tokmon {
namespace {

constexpr std::array<std::uint32_t, 64> constants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotate_right(const std::uint32_t value,
                                     const unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

}  // namespace

Sha256 sha256(const std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
  const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8u;
  padded.push_back(0x80u);
  while ((padded.size() % 64u) != 56u) padded.push_back(0u);
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffu));
  }

  std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u,
                                        0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu,
                                        0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t offset = 0; offset < padded.size(); offset += 64u) {
    for (std::size_t index = 0; index < 16u; ++index) {
      const auto base = offset + index * 4u;
      schedule[index] = (static_cast<std::uint32_t>(padded[base]) << 24u) |
                        (static_cast<std::uint32_t>(padded[base + 1]) << 16u) |
                        (static_cast<std::uint32_t>(padded[base + 2]) << 8u) |
                        static_cast<std::uint32_t>(padded[base + 3]);
    }
    for (std::size_t index = 16u; index < 64u; ++index) {
      const auto s0 = rotate_right(schedule[index - 15u], 7u) ^
                      rotate_right(schedule[index - 15u], 18u) ^
                      (schedule[index - 15u] >> 3u);
      const auto s1 = rotate_right(schedule[index - 2u], 17u) ^
                      rotate_right(schedule[index - 2u], 19u) ^
                      (schedule[index - 2u] >> 10u);
      schedule[index] = schedule[index - 16u] + s0 + schedule[index - 7u] + s1;
    }

    auto a = state[0]; auto b = state[1]; auto c = state[2]; auto d = state[3];
    auto e = state[4]; auto f = state[5]; auto g = state[6]; auto h = state[7];
    for (std::size_t index = 0; index < 64u; ++index) {
      const auto sum1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
      const auto choice = (e & f) ^ ((~e) & g);
      const auto temporary1 = h + sum1 + choice + constants[index] + schedule[index];
      const auto sum0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g; g = f; f = e; e = d + temporary1;
      d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
  }

  Sha256 digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4u] = static_cast<std::uint8_t>(state[index] >> 24u);
    digest[index * 4u + 1u] = static_cast<std::uint8_t>(state[index] >> 16u);
    digest[index * 4u + 2u] = static_cast<std::uint8_t>(state[index] >> 8u);
    digest[index * 4u + 3u] = static_cast<std::uint8_t>(state[index]);
  }
  return digest;
}

Sha256 sha256(const std::string_view text) {
  return sha256(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string hex(const std::span<const std::uint8_t> bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto byte : bytes) stream << std::setw(2) << static_cast<unsigned>(byte);
  return stream.str();
}

std::string sha256_hex(const std::span<const std::uint8_t> bytes) {
  const auto digest = sha256(bytes);
  return hex(digest);
}

std::string sha256_hex(const std::string_view text) {
  const auto digest = sha256(text);
  return hex(digest);
}

}  // namespace tokmon

