#pragma once

#include <string_view>

#include "tokmon/cbor.hpp"

namespace tokmon::builtin {

[[nodiscard]] Result<void> validate_schema(const cbor::Value& value,
                                           const cbor::Value& schema,
                                           std::string_view path = "$");

}  // namespace tokmon::builtin
