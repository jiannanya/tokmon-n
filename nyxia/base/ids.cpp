#include "tokmon/ids.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace tokmon {
namespace {
std::atomic<std::uint64_t> sequence{0};
thread_local std::mt19937_64 generator{std::random_device{}()};
}

std::int64_t unix_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string make_id(const std::string_view prefix) {
  const auto now = static_cast<std::uint64_t>(unix_time_ms());
  const auto ordinal = sequence.fetch_add(1, std::memory_order_relaxed);
  const auto random = generator();
  std::ostringstream stream;
  stream << prefix << '_' << std::hex << std::setfill('0') << std::setw(12) << now
         << std::setw(8) << ordinal << std::setw(16) << random;
  return stream.str();
}

}  // namespace tokmon

