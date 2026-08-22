#include "lenses/common/schema_validator.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace tokmon::builtin {
namespace {

bool number(const cbor::Value& value) {
  return std::holds_alternative<std::int64_t>(value.data) ||
      std::holds_alternative<double>(value.data);
}

double as_number(const cbor::Value& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value.data))
    return static_cast<double>(*integer);
  if (const auto* floating = std::get_if<double>(&value.data)) return *floating;
  return 0.0;
}

bool type_matches(const cbor::Value& value, const std::string_view type) {
  if (type == "null") return value.is_null();
  if (type == "boolean") return std::holds_alternative<bool>(value.data);
  if (type == "integer") return std::holds_alternative<std::int64_t>(value.data);
  if (type == "number") return number(value);
  if (type == "string") return std::holds_alternative<std::string>(value.data);
  if (type == "array") return value.is_array();
  if (type == "object") return value.is_map();
  return false;
}

Result<void> invalid(const std::string_view path, const std::string& message) {
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
      std::string(path) + ": " + message));
}

}  // namespace

Result<void> validate_schema(const cbor::Value& value, const cbor::Value& schema,
                             const std::string_view path) {
  if (!schema.is_map()) return invalid(path, "schema must be an object");
  if (const auto* alternatives = cbor::find(schema, "oneOf")) {
    if (!alternatives->as_array() || alternatives->as_array()->empty())
      return invalid(path, "oneOf must be a non-empty array");
    std::size_t matches = 0;
    for (const auto& alternative : *alternatives->as_array())
      if (validate_schema(value, alternative, path)) ++matches;
    if (matches != 1u) return invalid(path, "oneOf must match exactly one schema");
  }
  if (const auto* allowed = cbor::find(schema, "enum")) {
    if (!allowed->as_array()) return invalid(path, "enum must be an array");
    const auto encoded = cbor::encode(value);
    if (std::none_of(allowed->as_array()->begin(), allowed->as_array()->end(),
        [&encoded](const auto& item) { return cbor::encode(item) == encoded; }))
      return invalid(path, "value is not in enum");
  }

  if (const auto* type = cbor::find(schema, "type")) {
    bool matches = false;
    if (std::holds_alternative<std::string>(type->data)) {
      matches = type_matches(value, type->as_string());
    } else if (type->as_array()) {
      for (const auto& item : *type->as_array()) matches = matches ||
          type_matches(value, item.as_string());
    } else {
      return invalid(path, "type must be a string or string array");
    }
    if (!matches) return invalid(path, "value has the wrong type");
  }

  if (const auto* text = std::get_if<std::string>(&value.data)) {
    if (const auto* minimum = cbor::find(schema, "minLength");
        minimum && text->size() < static_cast<std::size_t>(minimum->as_integer()))
      return invalid(path, "string is shorter than minLength");
    if (const auto* maximum = cbor::find(schema, "maxLength");
        maximum && text->size() > static_cast<std::size_t>(maximum->as_integer()))
      return invalid(path, "string exceeds maxLength");
    if (const auto* pattern = cbor::find(schema, "pattern")) {
      try {
        if (!std::regex_search(*text, std::regex(std::string(pattern->as_string()))))
          return invalid(path, "string does not match pattern");
      } catch (const std::regex_error&) {
        return invalid(path, "schema pattern is invalid");
      }
    }
  }

  if (number(value)) {
    if (const auto* minimum = cbor::find(schema, "minimum");
        minimum && as_number(value) < as_number(*minimum))
      return invalid(path, "number is below minimum");
    if (const auto* maximum = cbor::find(schema, "maximum");
        maximum && as_number(value) > as_number(*maximum))
      return invalid(path, "number exceeds maximum");
  }

  if (const auto* array = value.as_array()) {
    if (const auto* minimum = cbor::find(schema, "minItems");
        minimum && array->size() < static_cast<std::size_t>(minimum->as_integer()))
      return invalid(path, "array is shorter than minItems");
    if (const auto* maximum = cbor::find(schema, "maxItems");
        maximum && array->size() > static_cast<std::size_t>(maximum->as_integer()))
      return invalid(path, "array exceeds maxItems");
    if (const auto* item_schema = cbor::find(schema, "items"))
      for (std::size_t index = 0; index < array->size(); ++index)
        if (auto result = validate_schema((*array)[index], *item_schema,
            std::string(path) + "[" + std::to_string(index) + "]"); !result) return result;
  }

  if (const auto* object = value.as_map()) {
    std::set<std::string, std::less<>> required;
    if (const auto* fields = cbor::find(schema, "required")) {
      if (!fields->as_array()) return invalid(path, "required must be an array");
      for (const auto& field : *fields->as_array())
        required.emplace(field.as_string());
    }
    for (const auto& field : required)
      if (!object->contains(field)) return invalid(path, "missing required field '" + field + "'");
    const auto* properties = cbor::find(schema, "properties");
    if (properties && !properties->is_map()) return invalid(path, "properties must be an object");
    const auto allow_additional = !cbor::find(schema, "additionalProperties") ||
        cbor::find(schema, "additionalProperties")->as_bool(true);
    for (const auto& [key, child] : *object) {
      const auto* child_schema = properties ? cbor::find(*properties, key) : nullptr;
      if (!child_schema) {
        if (!allow_additional) return invalid(path, "unknown field '" + key + "'");
        continue;
      }
      if (auto result = validate_schema(child, *child_schema,
          std::string(path) + "." + key); !result) return result;
    }
  }
  return {};
}

}  // namespace tokmon::builtin
