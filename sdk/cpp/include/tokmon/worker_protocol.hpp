#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

#include "tokmon/cbor.hpp"
#include "tokmon/error.hpp"

namespace tokmon {

inline constexpr std::uint32_t worker_protocol_major = 1;
inline constexpr std::uint32_t worker_protocol_minor = 1;
inline constexpr std::uint32_t worker_max_frame = 16u * 1024u * 1024u;

struct WorkerFrame {
  std::string type;
  std::uint64_t request_id{0};
  cbor::Value payload{cbor::Value::Map{}};
};

[[nodiscard]] cbor::Value to_cbor(const WorkerFrame& frame);
[[nodiscard]] Result<WorkerFrame> worker_frame_from_cbor(const cbor::Value& value);
Result<void> write_frame(std::ostream& output, const WorkerFrame& frame);
[[nodiscard]] Result<WorkerFrame> read_frame(std::istream& input,
                                             std::uint32_t max_frame = worker_max_frame);

}  // namespace tokmon

