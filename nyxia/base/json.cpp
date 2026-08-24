#include "tokmon/json.hpp"

#include <chjson/chjson.hpp>

namespace tokmon::json {
namespace {

cbor::Value from_json(const chjson::sv_value& value) {
  if (value.is_null()) return nullptr;
  if (value.is_bool()) return value.as_bool();
  if (value.is_number())
    return value.is_int() ? cbor::Value(value.as_int())
                          : cbor::Value(value.as_double());
  if (value.is_string()) return std::string(value.as_string_view());
  if (value.is_array()) {
    cbor::Value::Array result;
    const auto array = value.as_array();
    result.reserve(array.size());
    for (const auto& item : array) result.push_back(from_json(item));
    return result;
  }
  cbor::Value::Map result;
  for (const auto member : value.as_object())
    result.emplace(std::string(member.first), from_json(member.second));
  return result;
}

chjson::value to_json(const cbor::Value& value) {
  return std::visit([](const auto& item) -> chjson::value {
    using Type = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<Type, std::monostate>) return nullptr;
    else if constexpr (std::is_same_v<Type, bool>) return item;
    else if constexpr (std::is_same_v<Type, std::int64_t>)
      return chjson::value::integer(item);
    else if constexpr (std::is_same_v<Type, double>)
      return chjson::value::number(item);
    else if constexpr (std::is_same_v<Type, std::string>) return item;
    else if constexpr (std::is_same_v<Type, cbor::Value::Bytes>) {
      chjson::value::array bytes;
      bytes.reserve(item.size());
      for (const auto byte : item)
        bytes.push_back(chjson::value::integer(byte));
      // Preserve the established text representation for a binary value so
      // protocol output remains wire-compatible across the library change.
      return chjson::value::object{
          {"bytes", std::move(bytes)}, {"subtype", nullptr}};
    }
    else if constexpr (std::is_same_v<Type, cbor::Value::Array>) {
      chjson::value::array result;
      result.reserve(item.size());
      for (const auto& child : item) result.push_back(to_json(child));
      return result;
    } else {
      chjson::value::object result;
      result.reserve(item.size());
      for (const auto& [key, child] : item)
        result.emplace_back(key, to_json(child));
      return result;
    }
  }, value.data);
}

}  // namespace

Result<cbor::Value> parse(const std::string_view text) {
  auto parsed = chjson::parse(text);
  if (parsed.err)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
        "invalid JSON at " + std::to_string(parsed.err.line) + ":" +
            std::to_string(parsed.err.column) + " (code " +
            std::to_string(static_cast<int>(parsed.err.code)) + ")"));
  return from_json(parsed.doc.root());
}

std::string stringify(const cbor::Value& value, const bool pretty) {
  return chjson::dump(to_json(value), pretty);
}

}  // namespace tokmon::json
