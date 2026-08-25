#include "tokmon/logging.hpp"

#include <atomic>
#include <memory>

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

}  // namespace

void log_message(const LogLevel level, const std::string_view message) noexcept {
  try {
    const auto logger = active_logger().load(std::memory_order_acquire);
    if (logger) logger->log(chlog_level(level), "{}", message);
  } catch (...) {}
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
