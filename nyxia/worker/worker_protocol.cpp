#include "tokmon/worker_protocol.hpp"

#include <array>
#include <span>

namespace tokmon {

cbor::Value to_cbor(const WorkerFrame& frame) {
  return cbor::object({{"type", frame.type},
                       {"request_id", static_cast<std::int64_t>(frame.request_id)},
                       {"payload", frame.payload}});
}

Result<WorkerFrame> worker_frame_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "worker frame must be a map"));
  WorkerFrame frame;
  if (const auto* field = cbor::find(value, "type")) frame.type = field->as_string();
  if (const auto* field = cbor::find(value, "request_id"))
    frame.request_id = static_cast<std::uint64_t>(field->as_integer());
  if (const auto* field = cbor::find(value, "payload")) frame.payload = *field;
  if (frame.type.empty())
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "worker frame type is required"));
  return frame;
}

Result<void> write_frame(std::ostream& output, const WorkerFrame& frame) {
  const auto payload = cbor::encode(to_cbor(frame));
  if (payload.size() > worker_max_frame)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "worker frame exceeds maximum size"));
  const auto size = static_cast<std::uint32_t>(payload.size());
  const std::array<char, 4> header = {
      static_cast<char>(size >> 24u), static_cast<char>(size >> 16u),
      static_cast<char>(size >> 8u), static_cast<char>(size)};
  output.write(header.data(), static_cast<std::streamsize>(header.size()));
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
  output.flush();
  if (!output)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "failed to write worker frame", true));
  return {};
}

Result<WorkerFrame> read_frame(std::istream& input, const std::uint32_t max_frame) {
  std::array<std::uint8_t, 4> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (!input)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "failed to read worker frame header", true));
  const auto size = (static_cast<std::uint32_t>(header[0]) << 24u) |
                    (static_cast<std::uint32_t>(header[1]) << 16u) |
                    (static_cast<std::uint32_t>(header[2]) << 8u) |
                    static_cast<std::uint32_t>(header[3]);
  if (size == 0 || size > max_frame)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "invalid worker frame size"));
  std::vector<std::uint8_t> payload(size);
  input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(size));
  if (!input)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "truncated worker frame", true));
  auto value = cbor::decode(payload);
  if (!value) return tl::unexpected(value.error());
  return worker_frame_from_cbor(*value);
}

}  // namespace tokmon

