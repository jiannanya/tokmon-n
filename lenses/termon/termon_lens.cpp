#include "lenses/termon/termon_lens.hpp"

#include "tokmon/logging.hpp"

namespace tokmon::builtin {

TermonLens::TermonLens() : LensBase(make_manifest("termon", "Termon / Slint 全息显像屏",
    {"ui.conversation", "ui.trajectory", "ui.code", "ui.terminal",
     "ui.approval", "ui.lenses"},
    {{"*", "*"}},
    {{"ui.intent", "tokmon.ui.intent.v1"}},
    {"photon.emit", "snow.intent", "log.write"}, RuntimeKind::desktop)) {}

Result<void> TermonLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array trajectory; cbor::Value::Array conversation;
  cbor::Value::Array terminal; cbor::Value::Array approvals;
  cbor::Value::Array code;
  for (const auto& photon : photons.photons()) {
    if (trajectory.size() == 10'000u) trajectory.erase(trajectory.begin());
    trajectory.push_back(cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"kind", photon.kind}, {"payload", redact(cbor::diagnostic(photon.payload))},
        {"time", photon.committed_at_ms}}));
    if (photon.kind == "user.input" || photon.kind == "assistant.message")
      conversation.push_back(cbor::object({
          {"role", photon.kind == "user.input" ? "user" : "assistant"},
          {"text", text(photon)}, {"source", photon.id}}));
    if (photon.kind == "process.stdout" || photon.kind == "process.stderr" ||
        photon.kind == "process.exited")
      terminal.push_back(cbor::object({{"kind", photon.kind},
                                       {"text", redact(cbor::diagnostic(photon.payload))}}));
    if (photon.kind == "approval.granted" || photon.kind == "approval.denied" ||
        photon.kind == "act.proposed")
      approvals.push_back(cbor::object({{"kind", photon.kind}, {"source", photon.id},
                                        {"detail", redact(cbor::diagnostic(photon.payload))}}));
    if (photon.kind == "fs.changed") code.push_back(photon.payload);
  }
  if (auto result = surface.add("ui.conversation", "active-ray",
                                std::move(conversation), 50); !result) return result;
  if (auto result = surface.add("ui.trajectory", "active-ray",
                                std::move(trajectory), 40); !result) return result;
  if (auto result = surface.add("ui.terminal", "active-ray",
                                std::move(terminal), 30); !result) return result;
  if (auto result = surface.add("ui.approval", "active-ray",
                                std::move(approvals), 30); !result) return result;
  return surface.add("ui.code", "active-ray", std::move(code), 20);
}

Result<RefractionResult> TermonLens::refract(const PhotonWindow&, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  return emit(beam, "ui.intent-forwarded", "tokmon.ui.result.v1", cbor::object({
      {"intent", act.parameters}, {"fact_source", false}}));
}

}  // namespace tokmon::builtin
