#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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

struct DaemonClientOptions {
  std::filesystem::path endpoint;
  std::string client_id;
  std::string client_kind;
  bool shutdown_when_idle{true};
  std::chrono::milliseconds idle_timeout{std::chrono::seconds(15)};
  std::chrono::milliseconds lease_ttl{std::chrono::seconds(6)};
};

// A client lease keeps an automatically managed workspace daemon alive. The
// lease is renewed in the background and its destruction requests a safe idle
// shutdown without interrupting another client or active runtime work.
class DaemonClientLease final {
 public:
  DaemonClientLease(const DaemonClientLease&) = delete;
  DaemonClientLease& operator=(const DaemonClientLease&) = delete;
  DaemonClientLease(DaemonClientLease&& other) noexcept;
  DaemonClientLease& operator=(DaemonClientLease&& other) noexcept;
  ~DaemonClientLease();

  [[nodiscard]] static Result<DaemonClientLease> attach(
      DaemonClientOptions options);
  [[nodiscard]] Result<void> detach();

 private:
  struct State;
  DaemonClientLease();
  std::shared_ptr<State> state_;
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

// Explicit daemon start is a user decision and therefore pins the workspace
// daemon until `tokmon daemon stop` is issued.
[[nodiscard]] Result<void> pin_daemon(
    const std::filesystem::path& endpoint,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

}  // namespace tokmon
