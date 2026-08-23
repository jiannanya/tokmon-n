#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "tokmon/error.hpp"

namespace tokmon {

struct DaemonLaunchOptions {
  std::filesystem::path endpoint;
  std::filesystem::path workspace;
  std::filesystem::path executable;
  std::chrono::milliseconds startup_timeout{std::chrono::seconds(8)};
};

struct DaemonConnection {
  bool started{false};
  std::optional<std::uint64_t> process_id;
};

[[nodiscard]] std::filesystem::path workspace_snow_endpoint(
    const std::filesystem::path& run_directory,
    const std::filesystem::path& workspace);

[[nodiscard]] Result<bool> daemon_available(
    const std::filesystem::path& endpoint,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(80));

[[nodiscard]] Result<DaemonConnection> ensure_daemon(
    const DaemonLaunchOptions& options);

[[nodiscard]] Result<void> shutdown_daemon(
    const std::filesystem::path& endpoint,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

}  // namespace tokmon
