#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "tokmon/cbor.hpp"
#include "tokmon/error.hpp"

namespace tokmon {

enum class LogLevel { trace, debug, info, warn, error, critical };

void log_message(LogLevel level, std::string_view message) noexcept;

template <typename... Arguments>
void log_debug(const std::string_view format,
               Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::debug, std::vformat(
        format, std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_info(const std::string_view format,
              Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::info, std::vformat(
        format, std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_warn(const std::string_view format,
              Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::warn, std::vformat(
        format, std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_error(const std::string_view format,
               Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::error, std::vformat(
        format, std::make_format_args(arguments...)));
  } catch (...) {}
}

Result<void> initialize_logging(const std::filesystem::path& log_directory,
                                std::string_view process_name,
                                std::string_view level = "info");
[[nodiscard]] std::string redact(std::string_view message);
[[nodiscard]] cbor::Value redact_value(const cbor::Value& value);

}  // namespace tokmon
