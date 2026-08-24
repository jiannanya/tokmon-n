#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "tokmon/cbor.hpp"

namespace tokmon::yaml {

[[nodiscard]] Result<cbor::Value> parse(std::string_view text,
                                        std::string_view source = "YAML input");
[[nodiscard]] Result<cbor::Value> load(const std::filesystem::path& path);
[[nodiscard]] Result<std::string> stringify(const cbor::Value& value);

}  // namespace tokmon::yaml
