#include "tokmon/error.hpp"

#include <sstream>

namespace tokmon {

std::string_view to_string(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_state: return "invalid_state";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::permission_denied: return "permission_denied";
    case ErrorCode::schema_mismatch: return "schema_mismatch";
    case ErrorCode::integrity_error: return "integrity_error";
    case ErrorCode::storage_error: return "storage_error";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::abi_mismatch: return "abi_mismatch";
    case ErrorCode::protocol_error: return "protocol_error";
    case ErrorCode::lens_crashed: return "lens_crashed";
    case ErrorCode::sandbox_rejected: return "sandbox_rejected";
    case ErrorCode::approval_required: return "approval_required";
    case ErrorCode::outcome_unknown: return "outcome_unknown";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::internal_error: return "internal_error";
  }
  return "internal_error";
}

Error make_error(const ErrorCode code, std::string message, const bool retryable) {
  return Error{.code = code, .message = std::move(message), .retryable = retryable};
}

std::string Error::describe() const {
  std::ostringstream stream;
  stream << to_string(code) << ": " << message;
  if (!lens.empty()) stream << " lens=" << lens;
  if (!ray.empty()) stream << " ray=" << ray;
  if (!act.empty()) stream << " act=" << act;
  if (!caused_by.empty()) stream << " caused_by=" << caused_by;
  return stream.str();
}

}  // namespace tokmon

