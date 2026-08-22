#include "lenses/styx/styx_lens.hpp"

#include <algorithm>
#include <filesystem>

#include "lenses/common/process_runner.hpp"

namespace tokmon::builtin {

StyxLens::StyxLens() : LensBase(make_manifest("styx", "Styx / 执行隔离暗室",
    {"act.sandbox", "ui.terminal"},
    {{"act.admitted", "*"}, {"sandbox.*", "*"}, {"process.*", "*"},
     {"worker.*", "*"}},
    {{"process.exec", "tokmon.process.exec.v1"},
     {"process.cancel", "tokmon.process.cancel.v1"},
     {"worker.launch", "tokmon.worker.launch.v1"},
     {"wasm.invoke", "tokmon.wasm.invoke.v1"}},
    {"photon.emit", "io.process", "log.write"})) {}

Result<void> StyxLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
#if defined(_WIN32)
  constexpr auto backend = "windows-job-object";
#else
  constexpr auto backend = "posix-process-group";
#endif
  if (auto result = identify(surface, "act.sandbox", cbor::object({
      {"backend", backend}, {"strength", "process-tree"}, {"silent_downgrade", false},
      {"bounded_output", true}, {"process_tree_owned", true}})); !result) return result;
  cbor::Value::Array terminal;
  for (const auto& photon : photons.photons()) {
    if (!photon.kind.starts_with("process.") && !photon.kind.starts_with("worker.")) continue;
    if (terminal.size() == 256u) terminal.erase(terminal.begin());
    terminal.push_back(cbor::object({
        {"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"kind", photon.kind}, {"payload", photon.payload}}));
  }
  return surface.add("ui.terminal", "active-ray", std::move(terminal), 20);
}

Result<RefractionResult> StyxLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "process.cancel")
    return emit(beam, "process.cancel-requested", "tokmon.sandbox.result.v1",
                cbor::object({{"request", act.parameters}, {"history_deleted", false}}));
  if (act.kind == "wasm.invoke")
    return tl::unexpected(make_error(ErrorCode::unsupported,
        "no WASM runtime is mounted for wasm.invoke"));

  const auto* argv_field = cbor::find(act.parameters, "argv");
  const auto* cwd_field = cbor::find(act.parameters, "cwd");
  if (!argv_field || !argv_field->as_array() || argv_field->as_array()->empty() ||
      !cwd_field || cwd_field->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "process execution requires argv and cwd"));
  std::vector<std::string> argv;
  for (const auto& argument : *argv_field->as_array()) {
    if (!std::holds_alternative<std::string>(argument.data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "every argv item must be a string"));
    argv.emplace_back(argument.as_string());
  }
  if (const auto* strength = cbor::find(act.parameters, "require_strength");
      strength && strength->as_string() != "process-tree")
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "requested sandbox strength is unavailable"));
  auto output_limit = std::int64_t{262'144};
  if (const auto* configured = cbor::find(act.parameters, "max_output_bytes"))
    output_limit = std::clamp(configured->as_integer(output_limit),
                              std::int64_t{1}, std::int64_t{4 * 1024 * 1024});
  auto run_timeout = act.timeout > std::chrono::milliseconds(50)
      ? act.timeout - std::chrono::milliseconds(50) : act.timeout;
  auto output = run_process(argv, std::filesystem::path(cwd_field->as_string()),
                            run_timeout, static_cast<std::size_t>(output_limit),
                            beam.stop_token());
  if (!output) return tl::unexpected(output.error());
  if (output->timed_out)
    return tl::unexpected(make_error(ErrorCode::timeout, "process deadline exceeded"));
  if (output->cancelled)
    return tl::unexpected(make_error(ErrorCode::cancelled, "process cancelled"));

  std::vector<PhotonId> emitted;
  const auto append = [&](std::string kind, cbor::Value payload) -> Result<void> {
    auto photon = beam.emit(std::move(kind), "tokmon.process.event.v1", std::move(payload));
    if (!photon) return tl::unexpected(photon.error());
    emitted.push_back(photon->id);
    return {};
  };
  if (auto started = append(act.kind == "worker.launch" ? "worker.started" : "process.started",
      cbor::object({{"argv", *argv_field}, {"cwd", std::string(cwd_field->as_string())},
                    {"sandbox_strength", output->sandbox_strength}})); !started)
    return tl::unexpected(started.error());
  if (!output->stdout_text.empty())
    if (auto chunk = append("process.stdout", cbor::object({
        {"text", output->stdout_text}, {"truncated", output->stdout_truncated}})); !chunk)
      return tl::unexpected(chunk.error());
  if (!output->stderr_text.empty())
    if (auto chunk = append("process.stderr", cbor::object({
        {"text", output->stderr_text}, {"truncated", output->stderr_truncated}})); !chunk)
      return tl::unexpected(chunk.error());
  const auto completed_kind = act.kind == "worker.launch" ? "worker.exited" : "process.exited";
  if (auto completed = append(completed_kind, cbor::object({
      {"exit_code", output->exit_code}, {"sandbox_strength", output->sandbox_strength},
      {"stdout_truncated", output->stdout_truncated},
      {"stderr_truncated", output->stderr_truncated}})); !completed)
    return tl::unexpected(completed.error());
  return RefractionResult{.status = RefractionStatus::completed,
                           .emitted = std::move(emitted),
                           .detail = "isolated process completed"};
}

}  // namespace tokmon::builtin
