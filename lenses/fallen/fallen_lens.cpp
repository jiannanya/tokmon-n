#include "lenses/fallen/fallen_lens.hpp"

#include "tokmon/hash.hpp"

namespace tokmon::builtin {

FallenLens::FallenLens() : LensBase(make_manifest("fallen", "Fallen / Policy 偏振滤光镜",
    {"act.policy", "ui.approvals"},
    {{"act.proposed", "*"}, {"approval.*", "*"}, {"trust.*", "*"},
     {"policy.*", "*"}},
    {{"approval.decide", "tokmon.approval.decide.v1"},
     {"policy.evaluate", "tokmon.policy.evaluate.v1"}})) {}

Result<void> FallenLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* proposed = photons.latest("act.proposed");
  std::string act_hash;
  if (proposed) {
    const auto* encoded_act = cbor::find(proposed->payload, "act");
    act_hash = sha256_hex(cbor::encode(encoded_act ? *encoded_act : proposed->payload));
  }
  const Photon* decision = nullptr;
  for (auto iterator = photons.photons().rbegin(); iterator != photons.photons().rend();
       ++iterator) {
    if (iterator->kind != "approval.granted" && iterator->kind != "approval.denied") continue;
    const auto* decided_hash = cbor::find(iterator->payload, "act_hash");
    if (decided_hash && decided_hash->as_string() == act_hash) { decision = &*iterator; break; }
  }
  const bool pending = proposed && (!decision || decision->sequence < proposed->sequence);
  if (auto result = identify(surface, "act.policy", cbor::object({
      {"deny_precedence", true}, {"pending", pending},
      {"act_hash", act_hash},
      {"decision", !decision ? "ask" : decision->kind == "approval.granted" ? "allow" : "deny"}}));
      !result) return result;
  if (pending)
    return surface.add("ui.approvals", proposed->id, cbor::object({
        {"act", proposed->payload},
        {"epoch", static_cast<std::int64_t>(proposed->epoch)},
        {"reason", "需要按风险规则确认"}}), 100);
  return {};
}

Result<RefractionResult> FallenLens::refract(const PhotonWindow&, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto* act_hash = cbor::find(act.parameters, "act_hash");
  if (!act_hash || act_hash->as_string().size() != 64u)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "policy decision requires a SHA-256 act_hash"));
  const auto* approved = cbor::find(act.parameters, "approved");
  std::string kind;
  if (act.kind == "policy.evaluate") {
    const auto* decision = cbor::find(act.parameters, "decision");
    if (!decision || (decision->as_string() != "allow" &&
                      decision->as_string() != "ask" && decision->as_string() != "deny"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "policy.evaluate decision must be allow, ask or deny"));
    kind = "policy.evaluated";
  } else {
    if (!approved)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "approval.decide requires approved"));
    kind = approved->as_bool() ? "approval.granted" : "approval.denied";
  }
  return emit(beam, kind, "tokmon.policy.result.v1", cbor::object({
      {"act_hash", *act_hash},
      {"epoch", static_cast<std::int64_t>(act.epoch)},
      {"target_generation", cbor::find(act.parameters, "target_generation") ?
          *cbor::find(act.parameters, "target_generation") : cbor::Value(0)},
      {"parameter_bound", true}}));
}

}  // namespace tokmon::builtin
