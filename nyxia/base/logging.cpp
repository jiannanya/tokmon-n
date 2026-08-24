#include "tokmon/logging.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <regex>

#include "tokmon/chlog_compat.hpp"
#include <chlog/chlog.hpp>

namespace tokmon {

namespace {

std::atomic<std::shared_ptr<chlog::logger>>& active_logger() {
  static std::atomic logger{[] {
    chlog::logger_config config;
    config.name = "tokmon";
    config.parallel_sinks = false;
    auto result = std::make_shared<chlog::logger>(std::move(config));
    result->add_sink(std::make_shared<chlog::console_sink>(
        chlog::console_sink::style::color));
    return result;
  }()};
  return logger;
}

chlog::level chlog_level(const LogLevel level) noexcept {
  switch (level) {
    case LogLevel::trace: return chlog::level::trace;
    case LogLevel::debug: return chlog::level::debug;
    case LogLevel::info: return chlog::level::info;
    case LogLevel::warn: return chlog::level::warn;
    case LogLevel::error: return chlog::level::error;
    case LogLevel::critical: return chlog::level::critical;
  }
  return chlog::level::info;
}

chlog::level parse_level(const std::string_view level) noexcept {
  if (level == "trace") return chlog::level::trace;
  if (level == "debug") return chlog::level::debug;
  if (level == "warn" || level == "warning") return chlog::level::warn;
  if (level == "error") return chlog::level::error;
  if (level == "critical") return chlog::level::critical;
  if (level == "off") return chlog::level::off;
  return chlog::level::info;
}

bool sensitive_key(std::string key) {
  std::ranges::transform(key, key.begin(),
      [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return key == "value" || key == "secret_value" || key == "api_key" ||
      key == "apikey" || key == "password" || key == "authorization" ||
      key == "access_token" || key == "refresh_token" || key == "credential";
}

}  // namespace

void log_message(const LogLevel level, const std::string_view message) noexcept {
  try {
    const auto logger = active_logger().load(std::memory_order_acquire);
    if (logger) logger->log(chlog_level(level), std::string_view("{}"), message);
  } catch (...) {}
}

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
    chlog::logger_config config;
    config.name = std::string(process_name);
    config.level = parse_level(level);
    config.pattern = "{date}T{time}.{ms} [{lvl}] [{name}] {msg}";
    config.flush_on_level = chlog::level::warn;
    config.parallel_sinks = false;
    auto logger = std::make_shared<chlog::logger>(std::move(config));
    logger->add_sink(std::make_shared<chlog::console_sink>(
        chlog::console_sink::style::color));
    logger->add_sink(std::make_shared<chlog::rotating_file_sink>(
        log_directory / (std::string(process_name) + ".log"),
        5u * 1024u * 1024u, 4u));
    active_logger().store(std::move(logger), std::memory_order_release);
    return {};
  } catch (const std::exception& exception) {
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "logging initialization failed: " +
                                         std::string(exception.what())));
  }
}

}  // namespace tokmon
