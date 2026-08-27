#include "tokmon/daemon_lifecycle.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "tokmon/cbor.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/snow_protocol.hpp"
#include "tokmon/snow_transport.hpp"

namespace tokmon {
namespace {

SnowMessage ping_request() {
  return SnowMessage{.kind = SnowMessageKind::ping,
      .request_id = next_snow_request_id(),
      .payload = cbor::object({{"client", "daemon-lifecycle"}})};
}

Result<SnowMessage> lifecycle_request(const std::filesystem::path& endpoint,
                                      cbor::Value payload,
                                      const std::chrono::milliseconds timeout) {
  SnowClient client(endpoint, timeout);
  SnowMessage request{.kind = SnowMessageKind::intent,
      .request_id = next_snow_request_id(), .payload = std::move(payload)};
  auto response = client.request(request);
  if (!response) return tl::unexpected(response.error());
  if (response->kind == SnowMessageKind::error) {
    const auto* message = cbor::find(response->payload, "message");
    return tl::unexpected(make_error(ErrorCode::protocol_error,
        message ? std::string(message->as_string())
                : "Tokmon daemon rejected the lifecycle request"));
  }
  return response;
}

#if defined(_WIN32)
std::wstring quote_windows_argument(const std::wstring& value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
  std::wstring quoted(1, L'\"');
  std::size_t slashes = 0;
  for (const auto character : value) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      quoted.append(slashes * 2u + 1u, L'\\');
      quoted.push_back(L'\"');
      slashes = 0;
    } else {
      quoted.append(slashes, L'\\');
      slashes = 0;
      quoted.push_back(character);
    }
  }
  quoted.append(slashes * 2u, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}
#endif

Result<std::uint64_t> spawn_daemon(const DaemonLaunchOptions& options) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(options.executable, error))
    return tl::unexpected(make_error(ErrorCode::not_found,
        "Tokmon runtime executable was not found beside the client: " +
        options.executable.string()));
#if defined(_WIN32)
  auto command = quote_windows_argument(options.executable.wstring()) +
      L" --tokmon-internal-daemon" +
      L" --workspace " + quote_windows_argument(options.workspace.wstring()) +
      L" --endpoint " + quote_windows_argument(options.endpoint.wstring());
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const auto working_directory = options.executable.parent_path().wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
          working_directory.c_str(), &startup, &process))
    return tl::unexpected(make_error(ErrorCode::io_error,
        "cannot start Tokmon daemon (Win32 error " +
        std::to_string(GetLastError()) + ")"));
  const auto id = static_cast<std::uint64_t>(process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return id;
#else
  const auto process = ::fork();
  if (process < 0)
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot fork Tokmon daemon"));
  if (process == 0) {
    (void)::setsid();
    const auto null = ::open("/dev/null", O_RDWR);
    if (null >= 0) {
      (void)::dup2(null, STDIN_FILENO);
      (void)::dup2(null, STDOUT_FILENO);
      (void)::dup2(null, STDERR_FILENO);
      if (null > STDERR_FILENO) (void)::close(null);
    }
    const auto executable = options.executable.string();
    const auto workspace = options.workspace.string();
    const auto endpoint = options.endpoint.string();
    (void)::execl(executable.c_str(), executable.c_str(), "--tokmon-internal-daemon",
                  "--workspace", workspace.c_str(), "--endpoint", endpoint.c_str(),
                  static_cast<char*>(nullptr));
    _exit(127);
  }
  return static_cast<std::uint64_t>(process);
#endif
}

}  // namespace

struct DaemonClientLease::State {
  explicit State(DaemonClientOptions value) : options(std::move(value)) {}

  DaemonClientOptions options;
  std::atomic_bool detached{false};
  std::mutex wait_mutex;
  std::condition_variable_any wait_condition;
  std::jthread heartbeat;
};

DaemonClientLease::DaemonClientLease() = default;

DaemonClientLease::DaemonClientLease(DaemonClientLease&& other) noexcept
    : state_(std::move(other.state_)) {}

DaemonClientLease& DaemonClientLease::operator=(DaemonClientLease&& other) noexcept {
  if (this == &other) return *this;
  (void)detach();
  state_ = std::move(other.state_);
  return *this;
}

DaemonClientLease::~DaemonClientLease() { (void)detach(); }

Result<DaemonClientLease> DaemonClientLease::attach(DaemonClientOptions options) {
  if (options.endpoint.empty() || options.client_id.empty() ||
      options.client_kind.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
        "daemon client endpoint, id, and kind are required"));
  if (options.lease_ttl < std::chrono::seconds(2) ||
      options.lease_ttl > std::chrono::seconds(30) ||
      options.idle_timeout < std::chrono::milliseconds(0) ||
      options.idle_timeout > std::chrono::minutes(5))
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
        "daemon client lease timing is outside the supported range"));

  auto attached = lifecycle_request(options.endpoint, cbor::object({
      {"action", "daemon.client.attach"}, {"client_id", options.client_id},
      {"client_kind", options.client_kind},
      {"shutdown_when_idle", options.shutdown_when_idle},
      {"idle_timeout_ms", static_cast<std::int64_t>(options.idle_timeout.count())},
      {"lease_ttl_ms", static_cast<std::int64_t>(options.lease_ttl.count())}}),
      std::chrono::milliseconds(750));
  if (!attached) return tl::unexpected(attached.error());

  DaemonClientLease lease;
  lease.state_ = std::make_shared<State>(std::move(options));
  auto state = lease.state_;
  state->heartbeat = std::jthread([state](const std::stop_token stop) {
    const auto interval = std::max(std::chrono::milliseconds(500),
                                   state->options.lease_ttl / 3);
    while (!stop.stop_requested()) {
      std::unique_lock lock(state->wait_mutex);
      state->wait_condition.wait_for(lock, stop, interval, [] { return false; });
      if (stop.stop_requested() || state->detached.load(std::memory_order_acquire)) return;
      lock.unlock();
      auto renewed = lifecycle_request(state->options.endpoint, cbor::object({
          {"action", "daemon.client.heartbeat"},
          {"client_id", state->options.client_id},
          {"lease_ttl_ms", static_cast<std::int64_t>(
              state->options.lease_ttl.count())}}), std::chrono::milliseconds(500));
      if (!renewed && !stop.stop_requested())
        (void)lifecycle_request(state->options.endpoint, cbor::object({
            {"action", "daemon.client.attach"},
            {"client_id", state->options.client_id},
            {"client_kind", state->options.client_kind},
            {"shutdown_when_idle", state->options.shutdown_when_idle},
            {"idle_timeout_ms", static_cast<std::int64_t>(
                state->options.idle_timeout.count())},
            {"lease_ttl_ms", static_cast<std::int64_t>(
                state->options.lease_ttl.count())}}), std::chrono::milliseconds(500));
    }
  });
  return lease;
}

Result<void> DaemonClientLease::detach() {
  auto state = std::move(state_);
  if (!state || state->detached.exchange(true, std::memory_order_acq_rel)) return {};
  state->heartbeat.request_stop();
  state->wait_condition.notify_all();
  if (state->heartbeat.joinable()) state->heartbeat.join();
  auto detached = lifecycle_request(state->options.endpoint, cbor::object({
      {"action", "daemon.client.detach"},
      {"client_id", state->options.client_id},
      {"shutdown_when_idle", state->options.shutdown_when_idle},
      {"idle_timeout_ms", static_cast<std::int64_t>(
          state->options.idle_timeout.count())}}), std::chrono::milliseconds(750));
  if (!detached) return tl::unexpected(detached.error());
  return {};
}

std::filesystem::path workspace_snow_endpoint(
    const std::filesystem::path& run_directory,
    const std::filesystem::path& workspace) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(workspace, error);
  if (error) normalized = std::filesystem::absolute(workspace, error).lexically_normal();
  const auto key = sha256_hex(normalized.generic_string()).substr(0, 16);
#if defined(_WIN32)
  return default_snow_endpoint(run_directory / key);
#else
  return run_directory / ("snow-" + key + ".sock");
#endif
}

Result<bool> daemon_available(const std::filesystem::path& endpoint,
                              const std::chrono::milliseconds timeout) {
  SnowClient client(endpoint, timeout);
  auto response = client.request(ping_request());
  if (!response) {
    if (response.error().code == ErrorCode::io_error ||
        response.error().code == ErrorCode::timeout) return false;
    return tl::unexpected(response.error());
  }
  return response->kind == SnowMessageKind::pong;
}

Result<DaemonConnection> ensure_daemon(const DaemonLaunchOptions& options) {
  if (options.endpoint.empty() || options.workspace.empty() || options.executable.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "daemon launch paths cannot be empty"));
  auto available = daemon_available(options.endpoint);
  if (!available) return tl::unexpected(available.error());
  if (*available) return DaemonConnection{};

  auto process = spawn_daemon(options);
  if (!process) return tl::unexpected(process.error());
  const auto deadline = std::chrono::steady_clock::now() + options.startup_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    available = daemon_available(options.endpoint);
    if (!available) return tl::unexpected(available.error());
    if (*available)
      return DaemonConnection{.started = true, .process_id = *process};
  }
  return tl::unexpected(make_error(ErrorCode::timeout,
                                   "Tokmon daemon endpoint did not become reachable before the startup deadline"));
}

Result<DaemonStatus> daemon_status(const std::filesystem::path& endpoint,
                                   const std::chrono::milliseconds timeout) {
  auto response = lifecycle_request(endpoint,
      cbor::object({{"action", "daemon.status"}}), timeout);
  if (!response) return tl::unexpected(response.error());
  const auto* state = cbor::find(response->payload, "state");
  if (!state)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Tokmon daemon status omitted state"));
  DaemonStatus result;
  if (state->as_string() == "ready")
    result.state = DaemonStartupState::ready;
  else if (state->as_string() == "failed")
    result.state = DaemonStartupState::failed;
  else if (state->as_string() != "starting")
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Tokmon daemon returned an unknown state"));
  if (const auto* code = cbor::find(response->payload, "error_code"))
    result.error_code = std::string(code->as_string());
  if (const auto* error = cbor::find(response->payload, "error"))
    result.error = std::string(error->as_string());
  return result;
}

Result<DaemonStatus> wait_for_daemon_ready(
    const std::filesystem::path& endpoint,
    const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = daemon_status(endpoint);
    if (!status) return tl::unexpected(status.error());
    if (status->state != DaemonStartupState::starting) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return tl::unexpected(make_error(
      ErrorCode::timeout, "Tokmon daemon configuration check timed out"));
}

Result<void> shutdown_daemon(const std::filesystem::path& endpoint,
                             const std::chrono::milliseconds timeout) {
  SnowClient client(endpoint, std::min(timeout, std::chrono::milliseconds(500)));
  SnowMessage request{.kind = SnowMessageKind::intent,
      .request_id = next_snow_request_id(),
      .payload = cbor::object({{"action", "daemon.shutdown"}})};
  auto response = client.request(request);
  if (!response) return tl::unexpected(response.error());
  if (response->kind == SnowMessageKind::error) {
    const auto* message = cbor::find(response->payload, "message");
    return tl::unexpected(make_error(ErrorCode::protocol_error,
        message ? std::string(message->as_string()) : "Tokmon daemon rejected shutdown"));
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto available = daemon_available(endpoint, std::chrono::milliseconds(40));
    if (!available) return tl::unexpected(available.error());
    if (!*available) return {};
  }
  return tl::unexpected(make_error(ErrorCode::timeout,
                                   "Tokmon daemon did not stop in time"));
}

Result<void> pin_daemon(const std::filesystem::path& endpoint,
                        const std::chrono::milliseconds timeout) {
  auto pinned = lifecycle_request(endpoint,
      cbor::object({{"action", "daemon.pin"}}), timeout);
  if (!pinned) return tl::unexpected(pinned.error());
  return {};
}

}  // namespace tokmon
