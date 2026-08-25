#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "tokmon/chlog_compat.hpp"
#include "tokmon/cbor.hpp"
#include "tokmon/error.hpp"

namespace tokmon {

enum class LogLevel { trace, debug, info, warn, error, critical };

void log_message(LogLevel level, std::string_view message) noexcept;

template <typename Format>
concept RuntimeLogFormat =
    std::is_convertible_v<Format, std::string_view> &&
    !(std::is_array_v<std::remove_reference_t<Format>> &&
      std::is_same_v<std::remove_cv_t<std::remove_extent_t<
                         std::remove_reference_t<Format>>>, char>);

template <typename... Arguments>
void log_debug(const std::format_string<Arguments...> format,
               Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::debug,
                std::format(format, std::forward<Arguments>(arguments)...));
  } catch (...) {}
}

template <RuntimeLogFormat Format, typename... Arguments>
void log_debug(Format&& format, Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::debug,
        std::vformat(std::string_view(format),
                     std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_info(const std::format_string<Arguments...> format,
              Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::info,
                std::format(format, std::forward<Arguments>(arguments)...));
  } catch (...) {}
}

template <RuntimeLogFormat Format, typename... Arguments>
void log_info(Format&& format, Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::info,
        std::vformat(std::string_view(format),
                     std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_warn(const std::format_string<Arguments...> format,
              Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::warn,
                std::format(format, std::forward<Arguments>(arguments)...));
  } catch (...) {}
}

template <RuntimeLogFormat Format, typename... Arguments>
void log_warn(Format&& format, Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::warn,
        std::vformat(std::string_view(format),
                     std::make_format_args(arguments...)));
  } catch (...) {}
}

template <typename... Arguments>
void log_error(const std::format_string<Arguments...> format,
               Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::error,
                std::format(format, std::forward<Arguments>(arguments)...));
  } catch (...) {}
}

template <RuntimeLogFormat Format, typename... Arguments>
void log_error(Format&& format, Arguments&&... arguments) noexcept {
  try {
    log_message(LogLevel::error,
        std::vformat(std::string_view(format),
                     std::make_format_args(arguments...)));
  } catch (...) {}
}

Result<void> initialize_logging(const std::filesystem::path& log_directory,
                                std::string_view process_name,
                                std::string_view level = "info");
[[nodiscard]] std::string redact(std::string_view message);
[[nodiscard]] cbor::Value redact_value(const cbor::Value& value);

}  // namespace tokmon
