#include "lenses/cista/cista_lens.hpp"

#include "tokmon/ids.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {

CistaLens::CistaLens() : LensBase(make_manifest("cista", "Cista / Secret 遮光秘盒",
    {"act.secrets", "diagnostic.redaction"},
    {{"secret.ref-observed", "*"}, {"secret.*", "*"}, {"redaction.*", "*"}},
    {{"secret.bind", "tokmon.secret.bind.v1"},
     {"redaction.apply", "tokmon.redaction.apply.v1"}},
    {"photon.emit", "secret.bind", "log.write"})) {}

Result<void> CistaLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array references;
  for (const auto& photon : photons.photons()) {
    if (photon.kind != "secret.ref-observed") continue;
    references.push_back(cbor::object({{"ref", text(photon, "ref")},
        {"available", cbor::find(photon.payload, "available") ?
            cbor::find(photon.payload, "available")->as_bool() : false},
        {"plaintext", false}}));
  }
  if (auto result = identify(surface, "act.secrets", cbor::object({
      {"references", std::move(references)}, {"plaintext_visible", false}})); !result)
    return result;
  return surface.add("diagnostic.redaction", "policy", cbor::object({
      {"schema_aware", true}, {"fail_closed", true}, {"plaintext_logged", false}}), 10);
}

Result<RefractionResult> CistaLens::refract(const PhotonWindow&, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "redaction.apply") {
    const auto* content = cbor::find(act.parameters, "content");
    if (!content || !std::holds_alternative<std::string>(content->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "redaction.apply requires string content"));
    return emit(beam, "redaction.applied", "tokmon.redaction.result.v1",
        cbor::object({{"content", redact(content->as_string())},
                      {"plaintext_retained", false}}));
  }
  const auto* purpose = cbor::find(act.parameters, "purpose");
  const auto* act_hash = cbor::find(act.parameters, "act_hash");
  const auto* target_generation = cbor::find(act.parameters, "target_generation");
  const auto* lifetime = cbor::find(act.parameters, "lifetime_ms");
  if (!purpose || purpose->as_string().empty() || !act_hash ||
      act_hash->as_string().size() != 64u || !target_generation ||
      target_generation->as_integer() <= 0 || !lifetime ||
      lifetime->as_integer() <= 0 || lifetime->as_integer() > 300'000)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "secret binding requires purpose, act_hash, target_generation and bounded lifetime"));
  return emit(beam, "secret.bound", "tokmon.secret.binding.v1", cbor::object({
      {"binding_id", make_id("secret-binding")}, {"purpose", std::string(purpose->as_string())},
      {"act_hash", std::string(act_hash->as_string())},
      {"target", act.target}, {"target_generation", *target_generation},
      {"lifetime_ms", *lifetime},
      {"epoch", static_cast<std::int64_t>(act.epoch)}, {"plaintext", false}}));
}

}  // namespace tokmon::builtin
