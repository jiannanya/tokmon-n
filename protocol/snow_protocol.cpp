#include "tokmon/snow_protocol.hpp"

#include <atomic>
#include <random>

namespace tokmon {

std::uint64_t next_snow_request_id() {
  static const std::uint64_t prefix = [] {
    std::random_device random;
    return (static_cast<std::uint64_t>(random()) << 32u) |
           static_cast<std::uint64_t>(random());
  }();
  static std::atomic_uint64_t sequence{1};
  const auto value = prefix ^ sequence.fetch_add(1, std::memory_order_relaxed);
  return value == 0 ? sequence.fetch_add(1, std::memory_order_relaxed) : value;
}

std::string_view to_string(const SnowMessageKind kind) noexcept {
  switch (kind) {
    case SnowMessageKind::hello: return "hello";
    case SnowMessageKind::snapshot_request: return "snapshot.request";
    case SnowMessageKind::snapshot: return "snapshot";
    case SnowMessageKind::delta: return "delta";
    case SnowMessageKind::stream: return "stream";
    case SnowMessageKind::intent: return "intent";
    case SnowMessageKind::intent_result: return "intent.result";
    case SnowMessageKind::cancel: return "cancel";
    case SnowMessageKind::close: return "close";
    case SnowMessageKind::closed: return "closed";
    case SnowMessageKind::ping: return "ping";
    case SnowMessageKind::pong: return "pong";
    case SnowMessageKind::error: return "error";
  }
  return "error";
}

cbor::Value to_cbor(const SnowMessage& message) {
  return cbor::object({{"kind", std::string(to_string(message.kind))},
      {"request_id", static_cast<std::int64_t>(message.request_id)},
      {"cursor", static_cast<std::int64_t>(message.cursor)},
      {"payload", message.payload}});
}

Result<SnowMessage> snow_message_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Snow message must be a map"));
  SnowMessage message;
  const auto* kind = cbor::find(value, "kind");
  const auto text = kind ? kind->as_string() : std::string_view{};
  if (text == "hello") message.kind = SnowMessageKind::hello;
  else if (text == "snapshot.request") message.kind = SnowMessageKind::snapshot_request;
  else if (text == "snapshot") message.kind = SnowMessageKind::snapshot;
  else if (text == "delta") message.kind = SnowMessageKind::delta;
  else if (text == "stream") message.kind = SnowMessageKind::stream;
  else if (text == "intent") message.kind = SnowMessageKind::intent;
  else if (text == "intent.result") message.kind = SnowMessageKind::intent_result;
  else if (text == "cancel") message.kind = SnowMessageKind::cancel;
  else if (text == "close") message.kind = SnowMessageKind::close;
  else if (text == "closed") message.kind = SnowMessageKind::closed;
  else if (text == "ping") message.kind = SnowMessageKind::ping;
  else if (text == "pong") message.kind = SnowMessageKind::pong;
  else if (text == "error") message.kind = SnowMessageKind::error;
  else return tl::unexpected(make_error(ErrorCode::protocol_error,
                                        "unknown Snow message kind"));
  if (const auto* field = cbor::find(value, "request_id"))
    message.request_id = static_cast<std::uint64_t>(field->as_integer());
  if (const auto* field = cbor::find(value, "cursor"))
    message.cursor = static_cast<std::uint64_t>(field->as_integer());
  if (const auto* field = cbor::find(value, "payload")) message.payload = *field;
  return message;
}

}  // namespace tokmon
