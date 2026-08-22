#include "lenses/nota/nota_lens.hpp"

namespace tokmon::builtin {

NotaLens::NotaLens() : LensBase(make_manifest("nota", "Nota / 可观测性光谱分析仪",
    {"diagnostic.metrics", "diagnostic.health", "ui.diagnostics"},
    {{"act.*", "*"}, {"lens.*", "*"}, {"worker.*", "*"},
     {"waveguide.*", "*"}, {"system.*", "*"}},
    {{"telemetry.export", "tokmon.telemetry.export.v1"},
     {"profile.capture", "tokmon.profile.capture.v1"},
     {"diagnostic.bundle", "tokmon.diagnostic.bundle.v1"}})) {}

Result<void> NotaLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  std::int64_t acts = 0; std::int64_t failures = 0; std::int64_t workers = 0;
  for (const auto& photon : photons.photons()) {
    if (photon.kind.starts_with("act.")) ++acts;
    if (photon.kind.ends_with("failed") || photon.kind.ends_with("rejected")) ++failures;
    if (photon.kind.starts_with("worker.")) ++workers;
  }
  if (auto result = surface.add("diagnostic.metrics", "active-ray", cbor::object({
      {"acts", acts}, {"failures", failures}, {"worker_events", workers},
      {"payload_captured", false}}), 0); !result) return result;
  if (auto result = identify(surface, "diagnostic.health", cbor::object({
      {"healthy", failures == 0}, {"recovery_source", false},
      {"exporter_blocks_commit", false}})); !result) return result;
  return surface.add("ui.diagnostics", "active-ray", cbor::object({
      {"status", failures == 0 ? "healthy" : "degraded"},
      {"failure_count", failures}, {"sensitive_payloads", false}}), 10);
}

Result<RefractionResult> NotaLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const std::string kind = act.kind == "telemetry.export" ? "telemetry.exported" :
      act.kind == "profile.capture" ? "profile.captured" : "diagnostic.bundle-created";
  return emit(beam, kind, "tokmon.diagnostic.result.v1", cbor::object({
      {"request", act.parameters}, {"secrets_included", false},
      {"fact_source", false}}));
}

}  // namespace tokmon::builtin
