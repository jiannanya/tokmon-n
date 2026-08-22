#include "tokmon/logging.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <regex>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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

Result<void> initialize_logging(const std::filesystem::path& log_directory,
                                const std::string_view process_name,
                                const std::string_view level) {
  try {
    std::filesystem::create_directories(log_directory);
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (log_directory / (std::string(process_name) + ".log")).string(),
        5 * 1024 * 1024, 4));
    auto logger = std::make_shared<spdlog::logger>(std::string(process_name),
                                                   sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z [%l] [%n] %v");
    logger->set_level(spdlog::level::from_str(std::string(level)));
    spdlog::set_default_logger(std::move(logger));
    spdlog::flush_on(spdlog::level::warn);
    return {};
  } catch (const std::exception& exception) {
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "logging initialization failed: " +
                                         std::string(exception.what())));
  }
}

}  // namespace tokmon
