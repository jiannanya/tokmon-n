#include "lenses/styx/styx_lens.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "lenses/common/process_runner.hpp"
#include "lenses/common/pty_session.hpp"
#include "lenses/common/secret_store.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

std::string field(const cbor::Value& value, const std::string_view key,
                  const std::string_view fallback = {}) {
  const auto* item = cbor::find(value, key);
  return item ? std::string(item->as_string(fallback)) : std::string(fallback);
}

bool environment_name(const std::string_view value) {
  return !value.empty() && value.size() <= 128u &&
      std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
      });
}

bool within_root(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

Result<std::filesystem::path> bounded_path(const cbor::Value& parameters,
                                           const std::string_view field_name,
                                           const bool must_exist) {
  const auto* root_value = cbor::find(parameters, "allowed_root");
  const auto* path_value = cbor::find(parameters, field_name);
  if (!root_value || !path_value || root_value->as_string().empty() ||
      path_value->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "allowed_root and bounded path are required"));
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(root_value->as_string()), error);
  if (error || !std::filesystem::is_directory(root))
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "allowed_root is not an existing directory"));
  const auto supplied = std::filesystem::path(path_value->as_string());
  const auto path = std::filesystem::weakly_canonical(
      supplied.is_absolute() ? supplied : root / supplied, error);
  if (error || !within_root(root, path) || (must_exist && !std::filesystem::exists(path)))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "path escapes allowed_root or is absent"));
  return path;
}

Result<std::string> file_hash(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return tl::unexpected(make_error(ErrorCode::io_error,
                                                "cannot read execution artifact"));
  const std::string content(std::istreambuf_iterator<char>(input), {});
  return sha256_hex(content);
}

}  // namespace

struct StyxLens::Impl {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<PtySession>, std::less<>> terminals;
};

StyxLens::StyxLens() : LensBase(make_manifest("styx", "Styx / 执行隔离暗室",
    {"act.sandbox", "ui.terminal"},
    {{"act.admitted", "*"}, {"sandbox.*", "*"}, {"process.*", "*"},
     {"pty.*", "*"}, {"worker.*", "*"}, {"wasm.*", "*"}, {"remote.*", "*"}},
    {{"process.exec", "tokmon.process.exec.v1"},
     {"process.cancel", "tokmon.process.cancel.v1"},
     {"pty.open", "tokmon.pty.open.v1"}, {"pty.write", "tokmon.pty.write.v1"},
     {"pty.resize", "tokmon.pty.resize.v1"}, {"pty.close", "tokmon.pty.close.v1"},
     {"worker.launch", "tokmon.worker.launch.v1"},
     {"wasm.invoke", "tokmon.wasm.invoke.v1"},
     {"remote.execute", "tokmon.remote.execute.v1"}},
    {"photon.emit", "io.process", "secret.consume", "log.write"})),
    impl_(std::make_unique<Impl>()) {
  mark_stateful();
}

StyxLens::~StyxLens() {
  std::map<std::string, std::shared_ptr<PtySession>, std::less<>> terminals;
  {
    std::scoped_lock lock(impl_->mutex);
    terminals.swap(impl_->terminals);
  }
  for (auto& [_, terminal] : terminals)
    (void)terminal->close(std::chrono::milliseconds(100));
}

Result<void> StyxLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
#if defined(_WIN32)
  constexpr auto backend = "windows-job-object";
#else
  constexpr auto backend = "posix-process-group-rlimit";
#endif
  if (auto result = identify(surface, "act.sandbox", cbor::object({
      {"backend", backend}, {"strength", "process-tree/resource-limits"},
      {"silent_downgrade", false}, {"bounded_output", true},
      {"process_tree_owned", true}, {"network_isolation", false},
      {"pty_available", true}, {"pty_backend",
#if defined(_WIN32)
       "windows-conpty/job-object"
#else
       "unix-pty/process-group"
#endif
      }, {"wasm_runtime", "mount-required"},
      {"remote_backend", "not-configured"}})); !result) return result;
  cbor::Value::Array terminal;
  for (const auto& photon : photons.photons()) {
    if (!photon.kind.starts_with("process.") && !photon.kind.starts_with("pty.") &&
        !photon.kind.starts_with("worker.")) continue;
    if (terminal.size() == 1024u) terminal.erase(terminal.begin());
    terminal.push_back(cbor::object({
        {"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"kind", photon.kind}, {"payload", redact_value(photon.payload)}}));
  }
  return surface.add("ui.terminal", "active-ray", std::move(terminal), 20);
}

Result<RefractionResult> StyxLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "process.cancel")
    return emit(beam, "process.cancel-requested", "tokmon.sandbox.result.v1",
                cbor::object({{"process_ref", field(act.parameters, "process_ref")},
                  {"grace_ms", cbor::find(act.parameters, "grace_ms")
                      ? *cbor::find(act.parameters, "grace_ms") : cbor::Value(1000)},
                  {"history_deleted", false}}));
  if (act.kind.starts_with("pty.")) {
    std::vector<PhotonId> emitted;
    const auto append = [&](std::string kind, cbor::Value payload) -> Result<void> {
      auto photon = beam.emit(std::move(kind), "tokmon.pty.event.v1", std::move(payload));
      if (!photon) return tl::unexpected(photon.error());
      emitted.push_back(photon->id);
      return {};
    };
    const auto emit_snapshot = [&](const std::string& session_ref,
                                   PtySnapshot snapshot) -> Result<void> {
      if (!snapshot.output.empty()) {
        if (auto result = append("pty.chunk", cbor::object({
            {"session_ref", session_ref}, {"text", redact(snapshot.output)},
            {"stream", "combined"}, {"utf8", true}})); !result) return result;
      }
      if (snapshot.truncated)
        if (auto result = append("pty.output-truncated", cbor::object({
            {"session_ref", session_ref}, {"ring_bytes", static_cast<std::int64_t>(
                snapshot.output.size())}, {"explicit", true}})); !result) return result;
      if (!snapshot.running)
        return append("pty.exited", cbor::object({{"session_ref", session_ref},
            {"exit_code", snapshot.exit_code},
            {"sandbox_strength", snapshot.sandbox_strength}}));
      return {};
    };
    if (act.kind == "pty.open") {
      const auto* argv_value = cbor::find(act.parameters, "argv");
      const auto* cwd_value = cbor::find(act.parameters, "cwd");
      if (!argv_value || !argv_value->as_array() || argv_value->as_array()->empty() ||
          !cwd_value || cwd_value->as_string().empty())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "pty.open requires argv and cwd"));
      PtyOptions options;
      options.cwd = std::filesystem::path(cwd_value->as_string());
      for (const auto& argument : *argv_value->as_array()) {
        if (!std::holds_alternative<std::string>(argument.data) ||
            argument.as_string().find('\0') != std::string_view::npos)
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "PTY argv must contain NUL-free strings"));
        options.argv.emplace_back(argument.as_string());
      }
      options.columns = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "columns")
              ? cbor::find(act.parameters, "columns")->as_integer(80) : 80, 1, 500));
      options.rows = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "rows")
              ? cbor::find(act.parameters, "rows")->as_integer(24) : 24, 1, 300));
      options.max_output_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "max_output_bytes")
              ? cbor::find(act.parameters, "max_output_bytes")->as_integer(262'144) : 262'144,
          1, 4 * 1024 * 1024));
      options.idle_timeout = std::chrono::milliseconds(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "idle_timeout_ms")
              ? cbor::find(act.parameters, "idle_timeout_ms")->as_integer(300'000) : 300'000,
          100, 3'600'000));
      if (const auto* environment = cbor::find(act.parameters, "env"); environment) {
        if (!environment->as_map())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "PTY env must be an object"));
        for (const auto& [name, value] : *environment->as_map()) {
          if (!environment_name(name) || !std::holds_alternative<std::string>(value.data))
            return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                             "PTY env contains an invalid entry"));
          options.environment[name] = std::string(value.as_string());
        }
      }
      if (const auto* limits = cbor::find(act.parameters, "limits"); limits && limits->is_map()) {
        if (const auto* value = cbor::find(*limits, "memory_bytes"))
          options.max_memory_bytes = static_cast<std::size_t>(std::max<std::int64_t>(
              0, value->as_integer()));
        if (const auto* value = cbor::find(*limits, "cpu_ms"))
          options.max_cpu_time = std::chrono::milliseconds(std::max<std::int64_t>(
              0, value->as_integer()));
        if (const auto* value = cbor::find(*limits, "max_processes"))
          options.max_processes = static_cast<std::size_t>(std::max<std::int64_t>(
              0, value->as_integer()));
      }
      auto terminal = PtySession::open(std::move(options));
      if (!terminal) return tl::unexpected(terminal.error());
      const auto session_ref = field(act.parameters, "session_ref", make_id("pty"));
      {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->terminals.contains(session_ref))
          return tl::unexpected(make_error(ErrorCode::invalid_state,
                                           "PTY session_ref is already active"));
        impl_->terminals.emplace(session_ref, *terminal);
      }
      if (auto result = append("pty.opened", cbor::object({
          {"session_ref", session_ref}, {"columns", options.columns}, {"rows", options.rows},
#if defined(_WIN32)
          {"backend", "windows-conpty/job-object"},
#else
          {"backend", "unix-pty/process-group"},
#endif
          {"environment_inherited", false}, {"idle_timeout_ms", options.idle_timeout.count()}}));
          !result) return tl::unexpected(result.error());
      std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "settle_ms")
              ? cbor::find(act.parameters, "settle_ms")->as_integer(50) : 50, 0, 1000)));
      if (auto result = emit_snapshot(session_ref, (*terminal)->take_output()); !result)
        return tl::unexpected(result.error());
      return RefractionResult{.status = RefractionStatus::completed,
                               .emitted = std::move(emitted), .detail = "PTY opened"};
    }
    const auto session_ref = field(act.parameters, "session_ref");
    if (session_ref.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "PTY operation requires session_ref"));
    std::shared_ptr<PtySession> terminal;
    {
      std::scoped_lock lock(impl_->mutex);
      const auto found = impl_->terminals.find(session_ref);
      if (found == impl_->terminals.end())
        return tl::unexpected(make_error(ErrorCode::not_found, "PTY session was not found"));
      terminal = found->second;
      if (act.kind == "pty.close") impl_->terminals.erase(found);
    }
    if (act.kind == "pty.write") {
      const auto* input = cbor::find(act.parameters, "input");
      if (!input || !std::holds_alternative<std::string>(input->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "pty.write requires string input"));
      if (auto result = terminal->write(input->as_string()); !result)
        return tl::unexpected(result.error());
      std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "settle_ms")
              ? cbor::find(act.parameters, "settle_ms")->as_integer(50) : 50, 0, 1000)));
      if (auto result = append("pty.input-written", cbor::object({
          {"session_ref", session_ref},
          {"bytes", static_cast<std::int64_t>(input->as_string().size())}})); !result)
        return tl::unexpected(result.error());
      if (auto result = emit_snapshot(session_ref, terminal->take_output()); !result)
        return tl::unexpected(result.error());
    } else if (act.kind == "pty.resize") {
      const auto columns = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "columns")
              ? cbor::find(act.parameters, "columns")->as_integer() : 0, 0, 500));
      const auto rows = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "rows")
              ? cbor::find(act.parameters, "rows")->as_integer() : 0, 0, 300));
      if (auto result = terminal->resize(columns, rows); !result)
        return tl::unexpected(result.error());
      if (auto result = append("pty.resized", cbor::object({
          {"session_ref", session_ref}, {"columns", columns}, {"rows", rows}})); !result)
        return tl::unexpected(result.error());
    } else {
      auto closed = terminal->close(std::chrono::milliseconds(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "grace_ms")
              ? cbor::find(act.parameters, "grace_ms")->as_integer(500) : 500, 0, 5000)));
      if (!closed) return tl::unexpected(closed.error());
      if (auto result = emit_snapshot(session_ref, *closed); !result)
        return tl::unexpected(result.error());
      if (auto result = append("pty.closed", cbor::object({
          {"session_ref", session_ref}, {"exit_code", closed->exit_code},
          {"tree_terminated", true}})); !result)
        return tl::unexpected(result.error());
    }
    return RefractionResult{.status = RefractionStatus::completed,
                             .emitted = std::move(emitted),
                             .detail = "PTY operation completed"};
  }
  if (act.kind == "wasm.invoke") {
    if (field(act.parameters, "runtime_kind") != "wasmtime")
      return tl::unexpected(make_error(ErrorCode::unsupported,
          "wasm.invoke requires an explicitly mounted wasmtime runtime"));
    const auto runtime = field(act.parameters, "runtime_executable");
    if (runtime.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "runtime_executable is required"));
    auto module = bounded_path(act.parameters, "module_path", true);
    if (!module) return tl::unexpected(module.error());
    if (module->extension() != ".wasm")
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "wasmtime module must use the .wasm extension"));
    auto digest = file_hash(*module);
    if (!digest) return tl::unexpected(digest.error());
    const auto expected = field(act.parameters, "module_sha256");
    if (expected.empty() || expected != *digest)
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "WASM module hash does not match"));
    std::vector<std::string> argv{runtime, "run"};
    if (const auto export_name = field(act.parameters, "export"); !export_name.empty()) {
      argv.emplace_back("--invoke");
      argv.push_back(export_name);
    }
    if (const auto* directories = cbor::find(act.parameters, "allowed_directories"); directories) {
      if (!directories->as_array())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "allowed_directories must be an array"));
      for (const auto& value : *directories->as_array()) {
        auto directory = bounded_path(cbor::object({
            {"allowed_root", *cbor::find(act.parameters, "allowed_root")},
            {"directory", value}}), "directory", true);
        if (!directory || !std::filesystem::is_directory(*directory))
          return tl::unexpected(directory ? make_error(ErrorCode::permission_denied,
              "WASI directory is not a directory") : directory.error());
        argv.emplace_back("--dir");
        argv.push_back(directory->generic_string());
      }
    }
    argv.push_back(module->generic_string());
    if (const auto* arguments = cbor::find(act.parameters, "arguments"); arguments) {
      if (!arguments->as_array())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "WASM arguments must be an array"));
      for (const auto& argument : *arguments->as_array()) {
        if (!std::holds_alternative<std::string>(argument.data))
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "WASM argument must be a string"));
        argv.emplace_back(argument.as_string());
      }
    }
    std::vector<PhotonId> emitted;
    std::optional<Error> stream_error;
    const auto append = [&](std::string kind, cbor::Value payload) {
      if (stream_error) return;
      auto photon = beam.emit(std::move(kind), "tokmon.wasm.event.v1", std::move(payload));
      if (!photon) stream_error = photon.error();
      else emitted.push_back(photon->id);
    };
    append("wasm.started", cbor::object({{"module_sha256", *digest},
        {"runtime", "wasmtime"}, {"network", "wasi-disabled"},
        {"host_process_fallback", false}}));
    ProcessRequest request{.argv = std::move(argv),
        .cwd = std::filesystem::path(cbor::find(act.parameters, "allowed_root")->as_string()),
        .timeout = act.timeout,
        .max_output_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
            cbor::find(act.parameters, "max_output_bytes")
                ? cbor::find(act.parameters, "max_output_bytes")->as_integer(262'144) : 262'144,
            1, 4 * 1024 * 1024)), .inherit_environment = false, .stop = beam.stop_token()};
    request.on_stdout = [&](const std::string_view value) {
      append("wasm.stdout", cbor::object({{"text", redact(value)}, {"streaming", true}}));
    };
    request.on_stderr = [&](const std::string_view value) {
      append("wasm.stderr", cbor::object({{"text", redact(value)}, {"streaming", true}}));
    };
    auto result = run_process(std::move(request));
    if (stream_error) return tl::unexpected(*stream_error);
    if (!result) return tl::unexpected(result.error());
    append(result->exit_code == 0 ? "wasm.completed" : "wasm.failed", cbor::object({
        {"module_sha256", *digest}, {"exit_code", result->exit_code},
        {"timed_out", result->timed_out}, {"cancelled", result->cancelled},
        {"stdout_truncated", result->stdout_truncated},
        {"stderr_truncated", result->stderr_truncated},
        {"sandbox_strength", result->sandbox_strength}}));
    return RefractionResult{.status = result->exit_code == 0 ? RefractionStatus::completed
                                                              : RefractionStatus::failed,
        .emitted = std::move(emitted), .detail = "WASI invocation completed"};
  }
  if (act.kind == "remote.execute") {
    if (field(act.parameters, "backend") != "docker")
      return tl::unexpected(make_error(ErrorCode::unsupported,
                                       "only the mounted Docker adapter is supported"));
    if (cbor::find(act.parameters, "secret_bindings"))
      return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
          "container secret injection requires a credential-file adapter, not command-line env"));
    const auto image = field(act.parameters, "image");
    if (image.empty() || image.find("@sha256:") == std::string::npos)
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "remote image must be pinned by sha256 digest"));
    const auto* command = cbor::find(act.parameters, "argv");
    const auto* root_value = cbor::find(act.parameters, "allowed_root");
    if (!command || !command->as_array() || command->as_array()->empty() ||
        !root_value || root_value->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "remote.execute requires argv and allowed_root"));
    if (field(act.parameters, "network_mode", "none") != "none")
      return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
          "the Docker adapter currently guarantees only network_mode=none"));
    std::error_code root_error;
    const auto allowed_root = std::filesystem::weakly_canonical(
        std::filesystem::path(root_value->as_string()), root_error);
    if (root_error || !std::filesystem::is_directory(allowed_root))
      return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                       "remote allowed_root is invalid"));
    const auto docker = field(act.parameters, "adapter_executable", "docker");
    const auto container = "tokmon-" + sha256_hex(act.id).substr(0, 24);
    std::vector<PhotonId> emitted;
    std::optional<Error> stream_error;
    const auto append = [&](std::string kind, cbor::Value payload) {
      if (stream_error) return;
      auto photon = beam.emit(std::move(kind), "tokmon.remote.event.v1", std::move(payload));
      if (!photon) stream_error = photon.error();
      else emitted.push_back(photon->id);
    };
    const auto invoke = [&](std::vector<std::string> arguments,
                            const bool stream) -> Result<ProcessOutput> {
      arguments.insert(arguments.begin(), docker);
      ProcessRequest request{.argv = std::move(arguments), .cwd = allowed_root,
          .timeout = act.timeout, .max_output_bytes = 2u * 1024u * 1024u,
          .inherit_environment = true, .stop = beam.stop_token()};
      if (stream) {
        request.on_stdout = [&](const std::string_view value) {
          append("remote.stdout", cbor::object({{"container", container},
                                                 {"text", redact(value)}}));
        };
        request.on_stderr = [&](const std::string_view value) {
          append("remote.stderr", cbor::object({{"container", container},
                                                 {"text", redact(value)}}));
        };
      }
      return run_process(std::move(request));
    };
    const auto cleanup = [&]() -> Result<ProcessOutput> {
      return invoke({"rm", "--force", container}, false);
    };
    std::vector<std::string> create{"create", "--name", container,
        "--network", "none", "--read-only", "--security-opt", "no-new-privileges",
        "--cap-drop", "ALL", "--pids-limit", std::to_string(std::clamp<std::int64_t>(
            cbor::find(act.parameters, "max_processes")
                ? cbor::find(act.parameters, "max_processes")->as_integer(64) : 64, 1, 4096))};
    if (const auto memory = cbor::find(act.parameters, "memory_bytes")) {
      create.emplace_back("--memory");
      create.push_back(std::to_string(std::max<std::int64_t>(16 * 1024 * 1024,
                                                              memory->as_integer())));
    }
    create.push_back(image);
    for (const auto& argument : *command->as_array()) {
      if (!std::holds_alternative<std::string>(argument.data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "remote argv item must be a string"));
      create.emplace_back(argument.as_string());
    }
    auto created = invoke(std::move(create), false);
    if (!created) return tl::unexpected(created.error());
    if (created->exit_code != 0) {
      append("remote.failed", cbor::object({{"phase", "create"},
          {"backend", "docker"}, {"region", field(act.parameters, "region", "local")},
          {"stderr", redact(created->stderr_text)}, {"exit_code", created->exit_code}}));
      return RefractionResult{.status = RefractionStatus::failed,
          .emitted = std::move(emitted), .detail = "container creation failed"};
    }
    append("remote.created", cbor::object({{"container", container}, {"backend", "docker"},
        {"region", field(act.parameters, "region", "local")}, {"image", image},
        {"network", "none"}, {"root_read_only", true}, {"capabilities_dropped", true}}));
    const auto* inputs = cbor::find(act.parameters, "inputs");
    if (inputs && !inputs->as_array()) {
      (void)cleanup();
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "remote inputs must be an array"));
    }
    if (inputs) {
      for (const auto& input : *inputs->as_array()) {
        const auto destination = field(input, "destination");
        if (!destination.starts_with('/') || destination.find("..") != std::string::npos) {
          (void)cleanup();
          return tl::unexpected(make_error(ErrorCode::permission_denied,
                                           "container input destination is invalid"));
        }
        auto source = bounded_path(cbor::object({{"allowed_root", allowed_root.generic_string()},
            {"source", cbor::find(input, "source") ? *cbor::find(input, "source")
                                                    : cbor::Value("")}}), "source", true);
        if (!source) { (void)cleanup(); return tl::unexpected(source.error()); }
        auto copied = invoke({"cp", source->generic_string(), container + ":" + destination}, false);
        if (!copied || copied->exit_code != 0) {
          append("remote.failed", cbor::object({{"phase", "upload"},
              {"source", source->generic_string()}, {"destination", destination}}));
          (void)cleanup();
          return RefractionResult{.status = RefractionStatus::failed,
              .emitted = std::move(emitted), .detail = "container upload failed"};
        }
        auto hash = std::filesystem::is_regular_file(*source) ? file_hash(*source)
                                                               : Result<std::string>("");
        append("remote.input-uploaded", cbor::object({{"container", container},
            {"source", source->generic_string()}, {"destination", destination},
            {"sha256", hash ? *hash : ""}}));
      }
    }
    auto executed = invoke({"start", "--attach", container}, true);
    if (!executed) { (void)cleanup(); return tl::unexpected(executed.error()); }
    if (executed->cancelled || beam.stop_requested()) {
      (void)invoke({"kill", container}, false);
      append("remote.cancelled", cbor::object({{"container", container},
                                                {"cooperative", true}}));
    }
    const auto* outputs = cbor::find(act.parameters, "outputs");
    if (outputs && !outputs->as_array()) {
      (void)cleanup();
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "remote outputs must be an array"));
    }
    if (outputs && executed->exit_code == 0) {
      for (const auto& output : *outputs->as_array()) {
        const auto source = field(output, "source");
        if (!source.starts_with('/') || source.find("..") != std::string::npos) {
          (void)cleanup();
          return tl::unexpected(make_error(ErrorCode::permission_denied,
                                           "container output source is invalid"));
        }
        auto destination = bounded_path(cbor::object({
            {"allowed_root", allowed_root.generic_string()},
            {"destination", cbor::find(output, "destination")
                ? *cbor::find(output, "destination") : cbor::Value("")}}),
            "destination", false);
        if (!destination) { (void)cleanup(); return tl::unexpected(destination.error()); }
        std::filesystem::create_directories(destination->parent_path(), root_error);
        auto copied = invoke({"cp", container + ":" + source,
                              destination->generic_string()}, false);
        if (!copied || copied->exit_code != 0) {
          append("remote.failed", cbor::object({{"phase", "download"}, {"source", source}}));
          (void)cleanup();
          return RefractionResult{.status = RefractionStatus::failed,
              .emitted = std::move(emitted), .detail = "container download failed"};
        }
        auto hash = file_hash(*destination);
        if (!hash) { (void)cleanup(); return tl::unexpected(hash.error()); }
        append("remote.artifact-downloaded", cbor::object({{"container", container},
            {"source", source}, {"destination", destination->generic_string()},
            {"sha256", *hash}}));
      }
    }
    auto destroyed = cleanup();
    append("remote.destroyed", cbor::object({{"container", container},
        {"verified", destroyed && destroyed->exit_code == 0}}));
    append(executed->exit_code == 0 ? "remote.completed" :
        executed->cancelled ? "remote.cancelled" : "remote.failed", cbor::object({
          {"container", container}, {"backend", "docker"},
          {"region", field(act.parameters, "region", "local")},
          {"exit_code", executed->exit_code}, {"timed_out", executed->timed_out},
          {"cancelled", executed->cancelled},
          {"stdout_truncated", executed->stdout_truncated},
          {"stderr_truncated", executed->stderr_truncated},
          {"destroyed", destroyed && destroyed->exit_code == 0}}));
    if (stream_error) return tl::unexpected(*stream_error);
    return RefractionResult{.status = executed->exit_code == 0 ? RefractionStatus::completed
        : executed->cancelled ? RefractionStatus::rejected : RefractionStatus::failed,
        .emitted = std::move(emitted), .detail = "remote container lifecycle completed"};
  }

  const auto* argv_field = cbor::find(act.parameters, "argv");
  const auto* cwd_field = cbor::find(act.parameters, "cwd");
  if (!argv_field || !argv_field->as_array() || argv_field->as_array()->empty() ||
      !cwd_field || cwd_field->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "process execution requires argv and cwd"));
  std::vector<std::string> argv;
  for (const auto& argument : *argv_field->as_array()) {
    if (!std::holds_alternative<std::string>(argument.data) ||
        argument.as_string().find('\0') != std::string_view::npos)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "every argv item must be a NUL-free string"));
    argv.emplace_back(argument.as_string());
  }
  const auto required_strength = field(act.parameters, "require_strength", "process-tree");
  if (required_strength != "process-tree" &&
      required_strength != "process-tree/resource-limits")
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "requested sandbox strength is unavailable"));
  if (const auto* network = cbor::find(act.parameters, "network_allowlist");
      network && network->as_array() && !network->as_array()->empty())
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
        "this local backend cannot enforce network allowlists"));
  const auto network_mode = field(act.parameters, "network_mode", "deny");
  if (network_mode != "host")
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
        "local execution has no network namespace; explicit approved network_mode=host is required"));
  if (const auto* scopes = cbor::find(act.parameters, "file_scopes");
      scopes && scopes->as_array() && !scopes->as_array()->empty())
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
        "local execution cannot enforce declared file scopes; use the container backend"));

  ProcessRequest request{.argv = std::move(argv),
      .cwd = std::filesystem::path(cwd_field->as_string()), .timeout = act.timeout,
      .max_output_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "max_output_bytes")
              ? cbor::find(act.parameters, "max_output_bytes")->as_integer(262'144) : 262'144,
          1, 4 * 1024 * 1024)), .stdin_text = field(act.parameters, "stdin"),
      .inherit_environment = false, .stop = beam.stop_token()};
  if (const auto* environment = cbor::find(act.parameters, "env"); environment) {
    if (!environment->as_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "process env must be an object"));
    for (const auto& [name, value] : *environment->as_map()) {
      if (!environment_name(name) || !std::holds_alternative<std::string>(value.data) ||
          value.as_string().find('\0') != std::string_view::npos)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "process env contains an invalid entry"));
      request.environment[name] = std::string(value.as_string());
    }
  }
  if (const auto* bindings = cbor::find(act.parameters, "secret_bindings"); bindings) {
    if (!bindings->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "secret_bindings must be an array"));
    for (const auto& binding : *bindings->as_array()) {
      const auto name = field(binding, "env");
      if (!environment_name(name))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "secret binding env name is invalid"));
      auto secret = resolve_secret_binding(field(binding, "binding_id"),
          field(binding, "purpose", "process-env"), act_secret_scope_hash(act),
          act.target, act.generation, act.epoch);
      if (!secret) return tl::unexpected(secret.error());
      request.environment[name] = std::move(*secret);
    }
  }
  if (const auto* limits = cbor::find(act.parameters, "limits"); limits && limits->is_map()) {
    if (const auto* value = cbor::find(*limits, "memory_bytes"))
      request.max_memory_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
          value->as_integer(), 16 * 1024 * 1024, 16LL * 1024 * 1024 * 1024));
    if (const auto* value = cbor::find(*limits, "cpu_ms"))
      request.max_cpu_time = std::chrono::milliseconds(std::clamp<std::int64_t>(
          value->as_integer(), 10, act.timeout.count()));
    if (const auto* value = cbor::find(*limits, "max_processes"))
      request.max_processes = static_cast<std::size_t>(std::clamp<std::int64_t>(
          value->as_integer(), 1, 1024));
  }

  std::vector<PhotonId> emitted;
  std::optional<Error> stream_error;
  const auto append = [&](std::string kind, cbor::Value payload) {
    if (stream_error) return;
    auto photon = beam.emit(std::move(kind), "tokmon.process.event.v1", std::move(payload));
    if (!photon) stream_error = photon.error();
    else emitted.push_back(photon->id);
  };
  cbor::Value::Array environment_names;
  for (const auto& [name, _] : request.environment) environment_names.emplace_back(name);
  append("sandbox.plan-created", cbor::object({
      {"argv", *argv_field}, {"cwd", std::string(cwd_field->as_string())},
      {"environment_allowlist", std::move(environment_names)},
      {"file_scopes", cbor::Value::Array{}}, {"network_mode", network_mode},
      {"memory_bytes", static_cast<std::int64_t>(request.max_memory_bytes)},
      {"cpu_ms", request.max_cpu_time.count()},
      {"max_processes", static_cast<std::int64_t>(request.max_processes)},
      {"max_output_bytes", static_cast<std::int64_t>(request.max_output_bytes)},
      {"deadline_ms", act.timeout.count()}, {"shell", false},
      {"sandbox_strength", "process-tree/resource-limits"}}));
  append(act.kind == "worker.launch" ? "worker.started" : "process.started",
      cbor::object({{"argv", *argv_field}, {"cwd", std::string(cwd_field->as_string())},
        {"sandbox_strength", "process-tree/resource-limits"},
        {"environment_inherited", false}, {"network", "host-unrestricted"}}));
  const auto stream_limit = request.max_output_bytes;
  std::size_t stdout_streamed = 0;
  std::size_t stderr_streamed = 0;
  request.on_stdout = [&](const std::string_view chunk) {
    const auto remaining = stdout_streamed < stream_limit ? stream_limit - stdout_streamed : 0u;
    const auto count = std::min(remaining, chunk.size());
    if (count != 0u) append("process.stdout", cbor::object({
        {"text", redact(chunk.substr(0, count))}, {"streaming", true}}));
    stdout_streamed += count;
  };
  request.on_stderr = [&](const std::string_view chunk) {
    const auto remaining = stderr_streamed < stream_limit ? stream_limit - stderr_streamed : 0u;
    const auto count = std::min(remaining, chunk.size());
    if (count != 0u) append("process.stderr", cbor::object({
        {"text", redact(chunk.substr(0, count))}, {"streaming", true}}));
    stderr_streamed += count;
  };
  auto output = run_process(std::move(request));
  if (stream_error) return tl::unexpected(*stream_error);
  if (!output) return tl::unexpected(output.error());
  if (output->timed_out) {
    append("process.timed-out", cbor::object({{"exit_code", output->exit_code},
        {"cooperative_stop_attempted", output->cooperative_stop_attempted},
        {"forced_tree_termination", output->forced_tree_termination},
        {"tree_terminated", true}}));
    return RefractionResult{.status = RefractionStatus::failed, .emitted = std::move(emitted),
                             .detail = "process deadline exceeded"};
  }
  if (output->cancelled) {
    append("process.cancelled", cbor::object({{"exit_code", output->exit_code},
        {"cooperative_stop_attempted", output->cooperative_stop_attempted},
        {"forced_tree_termination", output->forced_tree_termination},
        {"tree_terminated", true}}));
    return RefractionResult{.status = RefractionStatus::rejected,
                             .emitted = std::move(emitted), .detail = "process cancelled"};
  }
  const auto completed_kind = act.kind == "worker.launch" ? "worker.exited" : "process.exited";
  if (output->stdout_truncated || output->stderr_truncated)
    append("process.output-truncated", cbor::object({
        {"stdout", output->stdout_truncated}, {"stderr", output->stderr_truncated},
        {"ring_bytes", static_cast<std::int64_t>(request.max_output_bytes)},
        {"explicit", true}}));
  append(completed_kind, cbor::object({{"exit_code", output->exit_code},
      {"sandbox_strength", output->sandbox_strength},
      {"stdout_truncated", output->stdout_truncated},
      {"stderr_truncated", output->stderr_truncated},
      {"cooperative_stop_attempted", output->cooperative_stop_attempted},
      {"forced_tree_termination", output->forced_tree_termination},
      {"output_bytes", static_cast<std::int64_t>(
          output->stdout_text.size() + output->stderr_text.size())}}));
  return RefractionResult{.status = output->exit_code == 0 ? RefractionStatus::completed
                                                           : RefractionStatus::failed,
                           .emitted = std::move(emitted),
                           .detail = "isolated process completed"};
}

}  // namespace tokmon::builtin
