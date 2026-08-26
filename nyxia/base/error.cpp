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
    case ErrorCode::provider_not_found: return "provider_not_found";
    case ErrorCode::ambiguous_provider: return "ambiguous_provider";
    case ErrorCode::deadline_exceeded: return "deadline_exceeded";
    case ErrorCode::budget_exceeded: return "budget_exceeded";
    case ErrorCode::provider_failed: return "provider_failed";
    case ErrorCode::stale_generation: return "stale_generation";
    case ErrorCode::recursive_query_denied: return "recursive_query_denied";
    case ErrorCode::nondeterministic_result: return "nondeterministic_result";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::internal_error: return "internal_error";
  }
  return "internal_error";
}

ErrorCode error_code_from_string(const std::string_view value,
                                 const ErrorCode fallback) noexcept {
  if (value == "invalid_argument") return ErrorCode::invalid_argument;
  if (value == "invalid_state") return ErrorCode::invalid_state;
  if (value == "not_found") return ErrorCode::not_found;
  if (value == "permission_denied") return ErrorCode::permission_denied;
  if (value == "schema_mismatch") return ErrorCode::schema_mismatch;
  if (value == "integrity_error") return ErrorCode::integrity_error;
  if (value == "storage_error") return ErrorCode::storage_error;
  if (value == "io_error") return ErrorCode::io_error;
  if (value == "timeout") return ErrorCode::timeout;
  if (value == "cancelled") return ErrorCode::cancelled;
  if (value == "abi_mismatch") return ErrorCode::abi_mismatch;
  if (value == "protocol_error") return ErrorCode::protocol_error;
  if (value == "lens_crashed") return ErrorCode::lens_crashed;
  if (value == "sandbox_rejected") return ErrorCode::sandbox_rejected;
  if (value == "approval_required") return ErrorCode::approval_required;
  if (value == "outcome_unknown") return ErrorCode::outcome_unknown;
  if (value == "provider_not_found") return ErrorCode::provider_not_found;
  if (value == "ambiguous_provider") return ErrorCode::ambiguous_provider;
  if (value == "deadline_exceeded") return ErrorCode::deadline_exceeded;
  if (value == "budget_exceeded") return ErrorCode::budget_exceeded;
  if (value == "provider_failed") return ErrorCode::provider_failed;
  if (value == "stale_generation") return ErrorCode::stale_generation;
  if (value == "recursive_query_denied") return ErrorCode::recursive_query_denied;
  if (value == "nondeterministic_result") return ErrorCode::nondeterministic_result;
  if (value == "unsupported") return ErrorCode::unsupported;
  if (value == "internal_error") return ErrorCode::internal_error;
  return fallback;
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

