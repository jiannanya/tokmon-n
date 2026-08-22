#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace tokmon {

enum class ErrorCode : std::uint16_t {
  invalid_argument,
  invalid_state,
  not_found,
  permission_denied,
  schema_mismatch,
  integrity_error,
  storage_error,
  io_error,
  timeout,
  cancelled,
  abi_mismatch,
  protocol_error,
  lens_crashed,
  sandbox_rejected,
  approval_required,
  outcome_unknown,
  unsupported,
  internal_error,
};

struct Error {
  ErrorCode code{ErrorCode::internal_error};
  std::string message;
  std::string lens;
  std::string ray;
  std::string act;
  std::string caused_by;
  bool retryable{false};

  [[nodiscard]] std::string describe() const;
};

template <typename T>
using Result = tl::expected<T, Error>;

[[nodiscard]] Error make_error(ErrorCode code, std::string message,
                               bool retryable = false);
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

}  // namespace tokmon

