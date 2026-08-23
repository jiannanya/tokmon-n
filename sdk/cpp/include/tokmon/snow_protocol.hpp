#pragma once

#include <cstdint>
#include <string>

#include "tokmon/cbor.hpp"
#include "tokmon/error.hpp"

namespace tokmon {

enum class SnowMessageKind : std::uint8_t {
  hello,
  snapshot_request,
  snapshot,
  delta,
  stream,
  intent,
  intent_result,
  cancel,
  close,
  closed,
  ping,
  pong,
  error,
};

struct SnowMessage {
  SnowMessageKind kind{SnowMessageKind::error};
  std::uint64_t request_id{0};
  std::uint64_t cursor{0};
  cbor::Value payload{cbor::Value::Map{}};
};

[[nodiscard]] std::string_view to_string(SnowMessageKind kind) noexcept;
[[nodiscard]] std::uint64_t next_snow_request_id();
[[nodiscard]] cbor::Value to_cbor(const SnowMessage& message);
[[nodiscard]] Result<SnowMessage> snow_message_from_cbor(const cbor::Value& value);

}  // namespace tokmon
