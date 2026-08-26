#include "lenses/termon/termon_lens.hpp"

#include "tokmon/logging.hpp"

namespace tokmon::builtin {

TermonLens::TermonLens() : LensBase(make_manifest("termon", "Termon / Slint 全息显像屏",
    {"ui.conversation", "ui.trajectory", "ui.code", "ui.terminal",
     "ui.approval", "ui.context", "ui.models", "ui.tools", "ui.workspace",
     "ui.children", "ui.lenses", "ui.diagnostics", "ui.settings"},
    {{"*", "*"}},
    {{"ui.intent", "tokmon.ui.intent.v1"}},
    {"photon.emit", "snow.intent", "log.write"}, RuntimeKind::desktop)) {}

Result<void> TermonLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array trajectory; cbor::Value::Array conversation;
  cbor::Value::Array terminal; cbor::Value::Array approvals;
  cbor::Value::Array code; cbor::Value::Array context; cbor::Value::Array models;
  cbor::Value::Array tools; cbor::Value::Array workspace; cbor::Value::Array children;
  cbor::Value::Array lenses; cbor::Value::Array diagnostics; cbor::Value::Array settings;
  const auto bounded_push = [](cbor::Value::Array& channel, cbor::Value value,
                               const std::size_t capacity = 10'000u) {
    if (channel.size() == capacity) channel.erase(channel.begin());
    channel.push_back(std::move(value));
  };
  for (const auto& photon : photons.photons()) {
    const auto projected = cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"id", photon.id}, {"kind", photon.kind}, {"schema", photon.schema},
        {"payload", redact_value(photon.payload)}, {"time", photon.committed_at_ms},
        {"caused_by_act", photon.caused_by_act}});
    bounded_push(trajectory, projected);
    if (photon.kind == "user.input" || photon.kind == "assistant.message")
      bounded_push(conversation, cbor::object({
          {"role", photon.kind == "user.input" ? "user" : "assistant"},
          {"text", text(photon)}, {"source", photon.id},
          {"status", "committed"}, {"sequence", static_cast<std::int64_t>(photon.sequence)}}));
    if (photon.kind == "process.stdout" || photon.kind == "process.stderr" ||
        photon.kind == "process.exited")
      bounded_push(terminal, projected, 4096u);
    if (photon.kind == "approval.granted" || photon.kind == "approval.denied" ||
        photon.kind == "approval.stage-granted" || photon.kind == "act.proposed")
      bounded_push(approvals, projected, 1024u);
    if (photon.kind.starts_with("fs.") || photon.kind.starts_with("git.") ||
        photon.kind.starts_with("artifact.") || photon.kind.starts_with("workspace.")) {
      bounded_push(workspace, projected, 4096u);
      if (photon.kind == "fs.changed" || photon.kind == "fs.written" ||
          photon.kind == "artifact.previewed") bounded_push(code, projected, 4096u);
    }
    if (photon.kind.starts_with("model.")) bounded_push(models, projected, 2048u);
    if (photon.kind == "model-surface.built" || photon.kind.starts_with("context.") ||
        photon.kind.starts_with("summary.") || photon.kind.starts_with("rag.") ||
        photon.kind.starts_with("memory.") || photon.kind.starts_with("skill."))
      bounded_push(context, projected, 2048u);
    if (photon.kind.starts_with("tool.") || photon.kind.starts_with("external."))
      bounded_push(tools, projected, 4096u);
    if (photon.kind.starts_with("child.") || photon.kind.starts_with("workspace.merge"))
      bounded_push(children, projected, 2048u);
    if (photon.kind.starts_with("lens.") || photon.kind.starts_with("mount.") ||
        photon.kind == "config.light-path-observed") bounded_push(lenses, projected, 2048u);
    if (photon.kind.starts_with("diagnostic.") || photon.kind.starts_with("telemetry.") ||
        photon.kind.starts_with("integrity.") || photon.kind == "act.failed" ||
        photon.kind == "act.rejected") bounded_push(diagnostics, projected, 4096u);
    if (photon.kind.starts_with("config.") || photon.kind.starts_with("policy."))
      bounded_push(settings, projected, 1024u);
  }
  if (auto result = surface.add("ui.conversation", "active-ray",
                                std::move(conversation), 50); !result) return result;
  if (auto result = surface.add("ui.trajectory", "active-ray",
                                std::move(trajectory), 40); !result) return result;
  if (auto result = surface.add("ui.terminal", "active-ray",
                                std::move(terminal), 30); !result) return result;
  if (auto result = surface.add("ui.approval", "active-ray",
                                std::move(approvals), 30); !result) return result;
  if (auto result = surface.add("ui.code", "active-ray", std::move(code), 20); !result)
    return result;
  if (auto result = surface.add("ui.context", "active-ray", std::move(context), 25); !result)
    return result;
  if (auto result = surface.add("ui.models", "active-ray", std::move(models), 25); !result)
    return result;
  if (auto result = surface.add("ui.tools", "active-ray", std::move(tools), 25); !result)
    return result;
  if (auto result = surface.add("ui.workspace", "active-ray", std::move(workspace), 25); !result)
    return result;
  if (auto result = surface.add("ui.children", "active-ray", std::move(children), 25); !result)
    return result;
  if (auto result = surface.add("ui.lenses", "active-ray", std::move(lenses), 25); !result)
    return result;
  if (auto result = surface.add("ui.diagnostics", "active-ray", std::move(diagnostics), 25);
      !result) return result;
  return surface.add("ui.settings", "active-ray", std::move(settings), 20);
}

Result<RefractionResult> TermonLens::refract(const PhotonWindow&, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  return emit(beam, "ui.intent-forwarded", "tokmon.ui.result.v1", cbor::object({
      {"intent", act.parameters}, {"fact_source", false}}));
}

}  // namespace tokmon::builtin
