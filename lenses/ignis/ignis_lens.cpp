#include "lenses/ignis/ignis_lens.hpp"

namespace tokmon::builtin {

IgnisLens::IgnisLens() : LensBase(make_manifest("ignis", "Ignis / 光圈调焦环",
    {"diagnostic.light-path", "ui.lenses"},
    {{"config.light-path-observed", "*"}, {"lens.candidate-*", "*"},
     {"mount.*", "*"}, {"lens.afterglow-*", "*"}},
    {{"lens.verify", "tokmon.lens.verify.v1"},
     {"lens.reconcile", "tokmon.lens.reconcile.v1"}},
    {"photon.emit", "artifact.read", "log.write"})) {}

Result<void> IgnisLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* desired = photons.latest("config.light-path-observed");
  const auto* committed = photons.latest("mount.epoch-committed");
  const bool pending = desired && (!committed || committed->sequence < desired->sequence);
  if (auto result = identify(surface, "diagnostic.light-path", cbor::object({
      {"desired_sequence", desired ? static_cast<std::int64_t>(desired->sequence) : 0},
      {"committed_sequence", committed ? static_cast<std::int64_t>(committed->sequence) : 0},
      {"pending", pending},
      {"desired", desired ? desired->payload : cbor::Value(nullptr)},
      {"active", committed ? committed->payload : cbor::Value(nullptr)}})); !result) return result;
  if (auto result = surface.add("ui.lenses", "light-path", cbor::object({
      {"pending", pending}, {"epoch", committed ?
          static_cast<std::int64_t>(committed->epoch) : 0},
      {"candidate", desired ? desired->payload : cbor::Value(nullptr)}}), 50); !result)
    return result;
  if (pending) {
    auto act = propose(*desired, "lens.reconcile", "tokmon.lens.reconcile.v1",
        manifest().id, desired->payload, RiskClass::reversible);
    return surface.propose(std::move(act));
  }
  return {};
}

Result<RefractionResult> IgnisLens::refract(const PhotonWindow&, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto kind = act.kind == "lens.verify" ? "lens.verification-requested"
                                               : "mount.reconcile-requested";
  return emit(beam, kind, "tokmon.lens.control.v1",
              cbor::object({{"candidate", act.parameters}, {"act_id", act.id}}));
}

}  // namespace tokmon::builtin
