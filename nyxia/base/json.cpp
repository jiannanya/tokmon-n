#include "tokmon/json.hpp"

#include <limits>

#include <nlohmann/json.hpp>

namespace tokmon::json {
namespace {

cbor::Value from_json(const nlohmann::json& value) {
  if (value.is_null()) return nullptr;
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(number);
    return static_cast<double>(number);
  }
  if (value.is_number_float()) return value.get<double>();
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    cbor::Value::Array result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(from_json(item));
    return result;
  }
  cbor::Value::Map result;
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
    result.emplace(iterator.key(), from_json(iterator.value()));
  return result;
}

nlohmann::json to_json(const cbor::Value& value) {
  return std::visit([](const auto& item) -> nlohmann::json {
    using Type = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<Type, std::monostate>) return nullptr;
    else if constexpr (std::is_same_v<Type, bool> ||
                       std::is_same_v<Type, std::int64_t> ||
                       std::is_same_v<Type, double> ||
                       std::is_same_v<Type, std::string>) return item;
    else if constexpr (std::is_same_v<Type, cbor::Value::Bytes>)
      return nlohmann::json{{"$bytes", nlohmann::json::binary(item)}};
    else if constexpr (std::is_same_v<Type, cbor::Value::Array>) {
      auto result = nlohmann::json::array();
      for (const auto& child : item) result.push_back(to_json(child));
      return result;
    } else {
      auto result = nlohmann::json::object();
      for (const auto& [key, child] : item) result[key] = to_json(child);
      return result;
    }
  }, value.data);
}

}  // namespace

Result<cbor::Value> parse(const std::string_view text) {
  try {
    return from_json(nlohmann::json::parse(text.begin(), text.end()));
  } catch (const nlohmann::json::exception& exception) {
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "invalid JSON: " + std::string(exception.what())));
  }
}

std::string stringify(const cbor::Value& value, const bool pretty) {
  return to_json(value).dump(pretty ? 2 : -1, ' ', false,
                             nlohmann::json::error_handler_t::replace);
}

}  // namespace tokmon::json
