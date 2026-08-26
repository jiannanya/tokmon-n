#include "tokmon/worker_lens_proxy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "tokmon/worker_protocol.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tokmon {
namespace {

ErrorCode worker_error_code(const std::string_view value) {
  if (value == "invalid_argument") return ErrorCode::invalid_argument;
  if (value == "invalid_state") return ErrorCode::invalid_state;
  if (value == "not_found") return ErrorCode::not_found;
  if (value == "permission_denied") return ErrorCode::permission_denied;
  if (value == "schema_mismatch") return ErrorCode::schema_mismatch;
  if (value == "timeout") return ErrorCode::timeout;
  if (value == "cancelled") return ErrorCode::cancelled;
  if (value == "approval_required") return ErrorCode::approval_required;
  if (value == "unsupported") return ErrorCode::unsupported;
  return ErrorCode::lens_crashed;
}

Error worker_error(const cbor::Value& payload, const std::string& fallback) {
  const auto* encoded = cbor::find(payload, "error");
  if (!encoded) return make_error(ErrorCode::lens_crashed, fallback);
  const auto* code = cbor::find(*encoded, "code");
  const auto* message = cbor::find(*encoded, "message");
  auto result = make_error(code ? worker_error_code(code->as_string())
                                : ErrorCode::lens_crashed,
      message ? std::string(message->as_string()) : fallback);
  if (const auto* retryable = cbor::find(*encoded, "retryable"))
    result.retryable = retryable->as_bool();
  return result;
}

RefractionStatus parse_status(const std::string_view value) {
  if (value == "passed") return RefractionStatus::passed;
  if (value == "rejected") return RefractionStatus::rejected;
  if (value == "failed") return RefractionStatus::failed;
  return RefractionStatus::completed;
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::wstring quote_argument(const std::wstring& value) {
  if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos) return value;
  std::wstring result(1, L'\"');
  std::size_t slashes = 0;
  for (const auto character : value) {
    if (character == L'\\') ++slashes;
    else if (character == L'\"') {
      result.append(slashes * 2u + 1u, L'\\'); result.push_back(L'\"'); slashes = 0;
    } else {
      result.append(slashes, L'\\'); result.push_back(character); slashes = 0;
    }
  }
  result.append(slashes * 2u, L'\\'); result.push_back(L'\"');
  return result;
}

void close_handle(HANDLE& handle) {
  if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
}
#endif

}  // namespace

struct WorkerLensProxy::Impl {
  LensManifest manifest;
  std::atomic_bool stopping{false};
  std::mutex mutex;
  std::uint64_t next_request{1};
#if defined(_WIN32)
  HANDLE process{nullptr};
  HANDLE job{nullptr};
  HANDLE input{nullptr};
  HANDLE output{nullptr};
#else
  pid_t process{-1};
  int input{-1};
  int output{-1};
#endif

  void terminate() noexcept {
#if defined(_WIN32)
    if (job) TerminateJobObject(job, 125);
    else if (process) TerminateProcess(process, 125);
    if (process) WaitForSingleObject(process, 2'000);
    close_handle(input); close_handle(output); close_handle(process); close_handle(job);
#else
    if (process > 0) { ::kill(-process, SIGKILL); ::waitpid(process, nullptr, 0); }
    if (input >= 0) ::close(input);
    if (output >= 0) ::close(output);
    process = -1; input = -1; output = -1;
#endif
  }

  Result<void> write_bytes(const std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#if defined(_WIN32)
      DWORD written = 0;
      const auto requested = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - offset, static_cast<std::size_t>(0x7fffffff)));
      if (!WriteFile(input, bytes.data() + offset, requested, &written, nullptr) || written == 0)
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker input pipe closed"));
#else
      const auto written = ::write(input, bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0)
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker input pipe closed"));
#endif
      offset += static_cast<std::size_t>(written);
    }
    return {};
  }

  Result<void> read_bytes(const std::span<std::uint8_t> bytes,
                          const std::chrono::steady_clock::time_point deadline,
                          const std::stop_token stop) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      if (stop.stop_requested()) {
        terminate();
        return tl::unexpected(make_error(ErrorCode::cancelled, "worker call cancelled"));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        terminate();
        return tl::unexpected(make_error(ErrorCode::timeout, "worker call timed out"));
      }
#if defined(_WIN32)
      DWORD available = 0;
      if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr))
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker output pipe closed"));
      if (available == 0) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
          return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                           "worker exited before responding"));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      DWORD read_count = 0;
      const auto requested = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - offset, static_cast<std::size_t>(available)));
      if (!ReadFile(output, bytes.data() + offset, requested, &read_count, nullptr) ||
          read_count == 0)
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker output pipe closed"));
      const auto read = static_cast<std::size_t>(read_count);
#else
      pollfd descriptor{output, POLLIN, 0};
      const auto polled = ::poll(&descriptor, 1, 10);
      if (polled < 0 && errno == EINTR) continue;
      if (polled < 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker output pipe failed"));
      if (polled == 0) continue;
      const auto read_count = ::read(output, bytes.data() + offset, bytes.size() - offset);
      if (read_count < 0 && errno == EINTR) continue;
      if (read_count <= 0)
        return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                         "worker output pipe closed"));
      const auto read = static_cast<std::size_t>(read_count);
#endif
      offset += read;
    }
    return {};
  }

  Result<WorkerFrame> request(std::string type, cbor::Value payload,
                              const std::chrono::steady_clock::time_point deadline,
                              const std::stop_token stop = {}) {
    std::scoped_lock lock(mutex);
    if (stopping.load(std::memory_order_acquire) && type != "worker.shutdown")
      return tl::unexpected(make_error(ErrorCode::cancelled, "worker Lens is stopping"));
    const auto request_id = next_request++;
    const auto encoded = cbor::encode(to_cbor(WorkerFrame{
        .type = std::move(type), .request_id = request_id, .payload = std::move(payload)}));
    if (encoded.empty() || encoded.size() > worker_max_frame)
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "worker request frame has invalid size"));
    std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(encoded.size() >> 24u),
        static_cast<std::uint8_t>(encoded.size() >> 16u),
        static_cast<std::uint8_t>(encoded.size() >> 8u),
        static_cast<std::uint8_t>(encoded.size())};
    if (auto written = write_bytes(header); !written) return tl::unexpected(written.error());
    if (auto written = write_bytes(encoded); !written) return tl::unexpected(written.error());
    if (auto read = read_bytes(header, deadline, stop); !read)
      return tl::unexpected(read.error());
    const auto size = (static_cast<std::uint32_t>(header[0]) << 24u) |
        (static_cast<std::uint32_t>(header[1]) << 16u) |
        (static_cast<std::uint32_t>(header[2]) << 8u) |
        static_cast<std::uint32_t>(header[3]);
    if (size == 0 || size > worker_max_frame)
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "worker response frame has invalid size"));
    std::vector<std::uint8_t> bytes(size);
    if (auto read = read_bytes(bytes, deadline, stop); !read)
      return tl::unexpected(read.error());
    auto value = cbor::decode(bytes);
    if (!value) return tl::unexpected(value.error());
    auto response = worker_frame_from_cbor(*value);
    if (!response) return tl::unexpected(response.error());
    if (response->request_id != request_id)
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "worker response request id mismatch"));
    return response;
  }
};

WorkerLensProxy::WorkerLensProxy() : impl_(std::make_unique<Impl>()) {}
WorkerLensProxy::~WorkerLensProxy() { request_stop(); }

Result<std::shared_ptr<WorkerLensProxy>> WorkerLensProxy::launch(WorkerLensOptions options) {
  if (options.manifest.runtime != RuntimeKind::native_worker &&
      options.manifest.runtime != RuntimeKind::node &&
      options.manifest.runtime != RuntimeKind::cpython)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "worker runtime must be node or cpython"));
  std::vector<std::filesystem::path> required{options.supervisor, options.entry};
  if (options.manifest.runtime != RuntimeKind::native_worker) {
    required.push_back(options.runtime_executable);
    required.push_back(options.adapter);
  }
  for (const auto& path : required)
    if (!std::filesystem::is_regular_file(path))
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "worker artifact is missing: " + path.string()));
  auto proxy = std::shared_ptr<WorkerLensProxy>(new WorkerLensProxy());
  proxy->impl_->manifest = std::move(options.manifest);
  const auto runtime = std::string(to_string(proxy->impl_->manifest.runtime));
  std::vector<std::string> arguments{options.supervisor.string()};
  arguments.push_back("--tokmon-internal-worker");
  arguments.insert(arguments.end(), {"--runtime", runtime,
                                     "--entry", options.entry.string()});
  if (options.manifest.runtime != RuntimeKind::native_worker) {
    arguments.insert(arguments.end(), {"--runtime-executable", options.runtime_executable.string(),
                                       "--adapter", options.adapter.string()});
  }
#if defined(_WIN32)
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE child_input = nullptr; HANDLE child_output = nullptr;
  if (!CreatePipe(&child_input, &proxy->impl_->input, &security, 0) ||
      !CreatePipe(&proxy->impl_->output, &child_output, &security, 0)) {
    close_handle(child_input); close_handle(child_output); proxy->impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot create worker pipes"));
  }
  SetHandleInformation(proxy->impl_->input, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(proxy->impl_->output, HANDLE_FLAG_INHERIT, 0);
  std::wstring command;
  for (const auto& argument : arguments) {
    if (!command.empty()) command.push_back(L' ');
    command.append(quote_argument(widen(argument)));
  }
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdInput = child_input;
  startup.hStdOutput = child_output; startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
  close_handle(child_input); close_handle(child_output);
  if (!created) {
    proxy->impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::io_error,
        "cannot launch Lens worker: " + std::to_string(GetLastError())));
  }
  proxy->impl_->process = process.hProcess;
  proxy->impl_->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!proxy->impl_->job || !SetInformationJobObject(proxy->impl_->job,
          JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
      !AssignProcessToJobObject(proxy->impl_->job, proxy->impl_->process)) {
    TerminateProcess(process.hProcess, 125); close_handle(process.hThread);
    proxy->impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "cannot contain Lens worker process tree"));
  }
  ResumeThread(process.hThread); close_handle(process.hThread);
#else
  int to_child[2]{}; int from_child[2]{};
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0)
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot create worker pipes"));
  const auto child = ::fork();
  if (child == 0) {
    ::setpgid(0, 0); ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::close(to_child[0]); ::close(to_child[1]);
    ::close(from_child[0]); ::close(from_child[1]);
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr); ::execv(argv.front(), argv.data()); _exit(127);
  }
  ::close(to_child[0]); ::close(from_child[1]);
  if (child < 0) {
    ::close(to_child[1]); ::close(from_child[0]);
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot fork Lens worker"));
  }
  proxy->impl_->process = child; proxy->impl_->input = to_child[1];
  proxy->impl_->output = from_child[0];
#endif
  auto ready = proxy->impl_->request("worker.hello", cbor::object({
      {"protocol_major", static_cast<std::int64_t>(worker_protocol_major)},
      {"protocol_minor", static_cast<std::int64_t>(worker_protocol_minor)},
      {"lens_id", proxy->impl_->manifest.id}}),
      std::chrono::steady_clock::now() + options.startup_timeout);
  if (!ready || ready->type != "worker.ready") {
    const auto failure = ready ? make_error(ErrorCode::protocol_error,
        "Lens worker did not return worker.ready") : ready.error();
    proxy->impl_->terminate(); return tl::unexpected(failure);
  }
  const auto* ready_id = cbor::find(ready->payload, "lens_id");
  const auto* major = cbor::find(ready->payload, "protocol_major");
  const auto* ready_runtime = cbor::find(ready->payload, "runtime");
  if (!ready_id || ready_id->as_string() != proxy->impl_->manifest.id || !major ||
      major->as_integer() != worker_protocol_major || !ready_runtime ||
      ready_runtime->as_string() != to_string(proxy->impl_->manifest.runtime)) {
    proxy->impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::integrity_error,
                                     "worker identity or protocol does not match manifest"));
  }
  if (!proxy->impl_->manifest.runtime_version.empty()) {
    const auto* ready_version = cbor::find(ready->payload, "runtime_version");
    if (!ready_version ||
        ready_version->as_string() != proxy->impl_->manifest.runtime_version) {
      proxy->impl_->terminate();
      return tl::unexpected(make_error(ErrorCode::integrity_error,
          "worker runtime version does not match the exact manifest version"));
    }
  }
  return proxy;
}

const LensManifest& WorkerLensProxy::manifest() const noexcept { return impl_->manifest; }

Result<void> WorkerLensProxy::view(const OpticalInput& input,
                                   WavefrontBuilder& outgoing) {
  const auto deadline = input.beat().deadline == std::chrono::steady_clock::time_point{}
      ? std::chrono::steady_clock::now() + impl_->manifest.resources.deadline
      : input.beat().deadline;
  auto response = impl_->request("lens.view.request",
      cbor::object({{"optical_input", to_cbor(input)}}), deadline);
  if (!response) return tl::unexpected(response.error());
  if (response->type != "lens.view.result") {
    impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "unexpected worker view response"));
  }
  const auto* ok = cbor::find(response->payload, "ok");
  if (!ok || !ok->as_bool())
    return tl::unexpected(worker_error(response->payload, "worker Lens view failed"));
  const auto* cells = cbor::find(response->payload, "wavefront_delta");
  if (!cells || !cells->as_array()) {
    impl_->terminate();
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "worker view result has no Wavefront delta"));
  }
  for (const auto& encoded : *cells->as_array()) {
    auto cell = field_cell_from_cbor(encoded);
    if (!cell) {
      impl_->terminate();
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "worker returned an invalid FieldCell"));
    }
    if (cell->band == "act.proposal") {
      if (std::find(impl_->manifest.light_permissions.begin(),
                    impl_->manifest.light_permissions.end(), "act.request") ==
          impl_->manifest.light_permissions.end())
        return tl::unexpected(make_error(ErrorCode::permission_denied,
                                         "worker Lens proposed without act.request"));
      auto act = act_from_cbor(cell->value);
      if (!act) return tl::unexpected(act.error());
      if (auto proposed = outgoing.propose(std::move(*act),
              cell->provenance.input_cells); !proposed)
        return proposed;
    } else {
      auto emitted = outgoing.emit(cell->provenance.output_port,
          std::move(cell->key), std::move(cell->value),
          cell->provenance.input_cells, cell->priority);
      if (!emitted) return tl::unexpected(emitted.error());
    }
  }
  return {};
}

Result<RefractionResult> WorkerLensProxy::refract(const PhotonWindow& photons,
                                                   const Act& act,
                                                   RefractionBeam& beam) {
  auto response = impl_->request("lens.refract.request", cbor::object({
      {"window", to_cbor(photons)}, {"act", to_cbor(act)}}),
      std::chrono::steady_clock::now() + act.timeout, beam.stop_token());
  if (!response) return tl::unexpected(response.error());
  if (response->type != "lens.refract.result")
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "unexpected worker refraction response"));
  const auto* ok = cbor::find(response->payload, "ok");
  if (!ok || !ok->as_bool())
    return tl::unexpected(worker_error(response->payload, "worker Lens refraction failed"));
  const auto* drafts = cbor::find(response->payload, "drafts");
  if (drafts && drafts->as_array() && !drafts->as_array()->empty() &&
      std::find(impl_->manifest.light_permissions.begin(),
                impl_->manifest.light_permissions.end(), "photon.emit") ==
          impl_->manifest.light_permissions.end())
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "worker Lens emitted without photon.emit"));
  RefractionResult result;
  if (const auto* status = cbor::find(response->payload, "status"))
    result.status = parse_status(status->as_string());
  if (const auto* detail = cbor::find(response->payload, "detail"))
    result.detail = detail->as_string();
  if (drafts && drafts->as_array()) {
    for (const auto& draft : *drafts->as_array()) {
      const auto* kind = cbor::find(draft, "kind");
      const auto* schema = cbor::find(draft, "schema");
      const auto* payload = cbor::find(draft, "payload");
      if (!kind || !schema || !payload)
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "worker PhotonDraft is incomplete"));
      auto emitted = beam.emit(std::string(kind->as_string()),
                               std::string(schema->as_string()), *payload);
      if (!emitted) return tl::unexpected(emitted.error());
      result.emitted.push_back(emitted->id);
    }
  }
  if (const auto* logs = cbor::find(response->payload, "logs"); logs && logs->as_array())
    for (const auto& item : *logs->as_array()) {
      const auto* level = cbor::find(item, "level");
      const auto* message = cbor::find(item, "message");
      if (level && message) beam.log(level->as_string(), message->as_string());
    }
  return result;
}

void WorkerLensProxy::request_stop() noexcept {
  if (!impl_ || impl_->stopping.exchange(true, std::memory_order_acq_rel)) return;
  auto stopped = impl_->request("worker.shutdown", cbor::object({}),
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
  (void)stopped;
  impl_->terminate();
}

}  // namespace tokmon
