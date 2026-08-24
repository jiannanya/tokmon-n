#include "tokmon/yaml.hpp"

#include <charconv>
#include <limits>

#include <chyaml.hpp>

namespace tokmon::yaml {
namespace {

Result<cbor::Value> from_node(const chyaml::node node, const std::size_t depth) {
  if (depth > 128u)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "YAML nesting exceeds 128 levels"));
  if (!node)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "invalid YAML node"));
  if (node.is_alias()) {
    const auto resolved = node.resolve_alias();
    if (!resolved)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "unresolved YAML alias"));
    return from_node(resolved, depth + 1u);
  }
  if (node.is_sequence()) {
    cbor::Value::Array result;
    result.reserve(node.size());
    for (std::size_t index = 0; index < node.size(); ++index) {
      auto child = from_node(node[static_cast<std::ptrdiff_t>(index)], depth + 1u);
      if (!child) return tl::unexpected(child.error());
      result.push_back(std::move(*child));
    }
    return result;
  }
  if (node.is_mapping()) {
    cbor::Value::Map result;
    for (std::size_t index = 0; index < node.size(); ++index) {
      const auto pair = node.pair_at(static_cast<std::ptrdiff_t>(index));
      if (!pair || !pair.key.is_scalar())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "YAML map keys must be scalars"));
      auto child = from_node(pair.value, depth + 1u);
      if (!child) return tl::unexpected(child.error());
      result[std::string(pair.key.scalar())] = std::move(*child);
    }
    return result;
  }

  const auto scalar = node.scalar();
  const auto style = node.style();
  const auto tag = node.tag();
  const bool explicitly_string = tag.find("str") != std::string_view::npos;
  const bool plain = style == chyaml::node_style::plain ||
                     style == chyaml::node_style::any;
  if (!explicitly_string && plain && node.is_null()) return nullptr;
  if (!explicitly_string && plain) {
    bool boolean = false;
    if (node.as_bool(boolean)) return boolean;
    std::int64_t integer = 0;
    if (node.as_int64(integer)) return integer;
    double number = 0.0;
    if (node.as_double(number)) return number;
  }
  return std::string(scalar);
}

void append_quoted(std::string& output, const std::string_view text) {
  static constexpr char hexadecimal[] = "0123456789abcdef";
  output.push_back('"');
  for (const char raw_character : text) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (character < 0x20u) {
          output += "\\u00";
          output.push_back(hexadecimal[character >> 4u]);
          output.push_back(hexadecimal[character & 0x0fu]);
        } else {
          output.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output.push_back('"');
}

bool nonempty_collection(const cbor::Value& value) {
  if (const auto* map = value.as_map()) return !map->empty();
  if (const auto* array = value.as_array()) return !array->empty();
  return false;
}

bool emit_value(std::string& output, const cbor::Value& value,
                const std::size_t depth) {
  if (depth > 128u) return false;
  const auto indent = [&] { output.append(depth * 2u, ' '); };
  if (const auto* map = value.as_map()) {
    if (map->empty()) { output += "{}"; return true; }
    bool first = true;
    for (const auto& [key, child] : *map) {
      if (!first) output.push_back('\n');
      first = false;
      indent();
      append_quoted(output, key);
      output.push_back(':');
      if (nonempty_collection(child)) {
        output.push_back('\n');
        if (!emit_value(output, child, depth + 1u)) return false;
      } else {
        output.push_back(' ');
        if (!emit_value(output, child, depth + 1u)) return false;
      }
    }
    return true;
  }
  if (const auto* array = value.as_array()) {
    if (array->empty()) { output += "[]"; return true; }
    bool first = true;
    for (const auto& child : *array) {
      if (!first) output.push_back('\n');
      first = false;
      indent();
      output.push_back('-');
      if (nonempty_collection(child)) {
        output.push_back('\n');
        if (!emit_value(output, child, depth + 1u)) return false;
      } else {
        output.push_back(' ');
        if (!emit_value(output, child, depth + 1u)) return false;
      }
    }
    return true;
  }
  if (const auto* text = std::get_if<std::string>(&value.data)) {
    append_quoted(output, *text);
    return true;
  }
  if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
    output += std::to_string(*integer);
    return true;
  }
  if (const auto* number = std::get_if<double>(&value.data)) {
    char buffer[64]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), *number,
                                         std::chars_format::general,
                                         std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{}) return false;
    output.append(buffer, converted.ptr);
    return true;
  }
  if (const auto* boolean = std::get_if<bool>(&value.data)) {
    output += *boolean ? "true" : "false";
    return true;
  }
  if (std::holds_alternative<std::monostate>(value.data)) {
    output += "null";
    return true;
  }
  return false;
}

std::string parse_failure(const chyaml::parse_error& error,
                          const std::string_view source) {
  std::string result = "cannot parse " + std::string(source);
  if (error.line != 0u || error.column != 0u)
    result += " at " + std::to_string(error.line) + ":" +
              std::to_string(error.column);
  if (!error.message.empty()) result += ": " + error.message;
  return result;
}

}  // namespace

Result<cbor::Value> parse(const std::string_view text, const std::string_view source) {
  chyaml::document document;
  // The compact profile preserves scalar presentation, which is required to
  // distinguish plain YAML booleans/numbers from quoted strings.
  if (!document.parse_borrowed(text, {
          .profile = chyaml::parse_profile::compact,
          .resolve_aliases = true,
          .allow_duplicate_keys = false}))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     parse_failure(document.error(), source)));
  return from_node(document.root(), 0u);
}

Result<cbor::Value> load(const std::filesystem::path& path) {
  chyaml::document document;
  const auto path_text = path.string();
  if (!document.parse_file(path_text, {
          .profile = chyaml::parse_profile::compact,
          .resolve_aliases = true,
          .allow_duplicate_keys = false}))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     parse_failure(document.error(), path_text)));
  return from_node(document.root(), 0u);
}

Result<std::string> stringify(const cbor::Value& value) {
  // chYAML's DOM intentionally stores scalar text without a semantic type and
  // therefore quotes values such as true/null on emission.  Emit the small
  // CBOR value model directly so configuration edits preserve those types;
  // parsing and validation continue to use chYAML.
  std::string output;
  if (!emit_value(output, value, 0u))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot represent binary or over-nested value as YAML"));
  output.push_back('\n');
  return output;
}

}  // namespace tokmon::yaml
