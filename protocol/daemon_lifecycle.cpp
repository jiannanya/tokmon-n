#include "tokmon/daemon_lifecycle.hpp"

#include <algorithm>
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
        "tokmond executable was not found beside the client: " +
        options.executable.string()));
#if defined(_WIN32)
  auto command = quote_windows_argument(options.executable.wstring()) +
      L" --workspace " + quote_windows_argument(options.workspace.wstring()) +
      L" --endpoint " + quote_windows_argument(options.endpoint.wstring());
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const auto working_directory = options.executable.parent_path().wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
          working_directory.c_str(), &startup, &process))
    return tl::unexpected(make_error(ErrorCode::io_error,
        "cannot start tokmond (Win32 error " + std::to_string(GetLastError()) + ")"));
  const auto id = static_cast<std::uint64_t>(process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return id;
#else
  const auto process = ::fork();
  if (process < 0)
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot fork tokmond"));
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
    (void)::execl(executable.c_str(), executable.c_str(), "--workspace",
                  workspace.c_str(), "--endpoint", endpoint.c_str(),
                  static_cast<char*>(nullptr));
    _exit(127);
  }
  return static_cast<std::uint64_t>(process);
#endif
}

}  // namespace

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
                                   "tokmond did not become ready before the startup deadline"));
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
        message ? std::string(message->as_string()) : "tokmond rejected shutdown"));
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto available = daemon_available(endpoint, std::chrono::milliseconds(40));
    if (!available) return tl::unexpected(available.error());
    if (!*available) return {};
  }
  return tl::unexpected(make_error(ErrorCode::timeout, "tokmond did not stop in time"));
}

}  // namespace tokmon
