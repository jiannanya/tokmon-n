#include "tokmon/logging.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace tokmon {
namespace {

bool sensitive_key(std::string key) {
  std::ranges::transform(key, key.begin(),
      [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return key == "value" || key == "secret_value" || key == "api_key" ||
      key == "apikey" || key == "password" || key == "authorization" ||
      key == "access_token" || key == "refresh_token" || key == "credential";
}

}  // namespace

std::string redact(const std::string_view message) {
  std::string value(message);
  static const std::regex assignment_pattern(
      R"((authorization|api[_-]?key|access[_-]?token|refresh[_-]?token|token|password|secret)\s*["']?\s*[:=]\s*["']?([^\s,;&"'}]+))",
      std::regex::icase);
  static const std::regex bearer_pattern(R"((bearer)\s+[A-Za-z0-9._~+/=-]+)",
                                         std::regex::icase);
  static const std::regex url_pattern(
      R"(([?&](?:api[_-]?key|access[_-]?token|token|password|secret)=)[^&#\s]+)",
      std::regex::icase);
  value = std::regex_replace(value, bearer_pattern, "$1 <redacted>");
  value = std::regex_replace(value, assignment_pattern, "$1=<redacted>");
  value = std::regex_replace(value, url_pattern, "$1<redacted>");
  return value;
}

cbor::Value redact_value(const cbor::Value& value) {
  if (const auto* map = value.as_map()) {
    cbor::Value::Map result;
    for (const auto& [key, child] : *map)
      result.emplace(key, sensitive_key(key) ? cbor::Value("<redacted>")
                                              : redact_value(child));
    return result;
  }
  if (const auto* array = value.as_array()) {
    cbor::Value::Array result;
    result.reserve(array->size());
    for (const auto& child : *array) result.push_back(redact_value(child));
    return result;
  }
  if (const auto* text = std::get_if<std::string>(&value.data)) return redact(*text);
  return value;
}

}  // namespace tokmon
