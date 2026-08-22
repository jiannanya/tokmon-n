#pragma once

#include <string>
#include <string_view>

#include "tokmon/cbor.hpp"

namespace tokmon::json {

[[nodiscard]] Result<cbor::Value> parse(std::string_view text);
[[nodiscard]] std::string stringify(const cbor::Value& value,
                                    bool pretty = false);

}  // namespace tokmon::json
