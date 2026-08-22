#include "lenses/fallen/fallen_lens.hpp"

#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"

#include <algorithm>
#include <cctype>

namespace tokmon::builtin {

FallenLens::FallenLens() : LensBase(make_manifest("fallen", "Fallen / Policy 偏振滤光镜",
    {"act.policy", "ui.approvals"},
    {{"act.proposed", "*"}, {"approval.*", "*"}, {"trust.*", "*"},
     {"policy.*", "*"}},
    {{"approval.decide", "tokmon.approval.decide.v1"},
     {"policy.evaluate", "tokmon.policy.evaluate.v1"},
     {"content.classify", "tokmon.content.classify.v1"}})) {}

Result<void> FallenLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* proposed = photons.latest("act.proposed");
  std::string act_hash;
  if (proposed) {
    const auto* bound_hash = cbor::find(proposed->payload, "act_hash");
    act_hash = bound_hash ? std::string(bound_hash->as_string()) : std::string{};
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
        {"act", redact_value(proposed->payload)},
        {"epoch", static_cast<std::int64_t>(proposed->epoch)},
        {"act_hash", act_hash},
        {"reason", "需要按风险规则确认"}}), 100);
  return {};
}

Result<RefractionResult> FallenLens::refract(const PhotonWindow& photons, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "content.classify") {
    const auto* content = cbor::find(act.parameters, "content");
    if (!content)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "content.classify requires content"));
    auto normalized = std::string(content->as_string());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    cbor::Value::Array labels;
    const auto contains_any = [&normalized](const std::initializer_list<std::string_view> terms) {
      return std::any_of(terms.begin(), terms.end(), [&normalized](const auto term) {
        return normalized.find(term) != std::string::npos;
      });
    };
    if (contains_any({"ignore previous", "ignore all", "system prompt", "developer message"}))
      labels.emplace_back("instruction-manipulation");
    if (contains_any({"authorization: bearer", "api_key=", "password=", "secret="}))
      labels.emplace_back("credential-like");
    if (contains_any({"powershell -enc", "rm -rf", "format c:", "curl | sh"}))
      labels.emplace_back("dangerous-command");
    if (normalized.find("-----begin private key-----") != std::string::npos)
      labels.emplace_back("private-key");
    const auto severity = labels.empty() ? "clear" : labels.size() == 1 ? "review" : "block";
    return emit(beam, "content.classified", "tokmon.content.classification.v1",
                cbor::object({{"content_hash", sha256_hex(content->as_string())},
                              {"labels", std::move(labels)}, {"severity", severity},
                              {"classifier", "fallen-deterministic-v1"}}));
  }
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
  const auto deadline_ms = cbor::find(act.parameters, "deadline_ms")
      ? cbor::find(act.parameters, "deadline_ms")->as_integer() : 0;
  if (kind.starts_with("approval.") &&
      (!cbor::find(act.parameters, "target_generation") ||
       cbor::find(act.parameters, "target_generation")->as_integer() <= 0 ||
       deadline_ms <= 0))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "approval decision requires target_generation and deadline_ms"));
  cbor::Value::Array completed_approvers;
  if (kind == "approval.granted") {
    const auto approver = cbor::find(act.parameters, "approver")
        ? std::string(cbor::find(act.parameters, "approver")->as_string()) : "user";
    for (const auto& photon : photons.photons()) {
      if (photon.kind != "approval.stage-granted") continue;
      const auto* previous_hash = cbor::find(photon.payload, "act_hash");
      const auto* previous_approver = cbor::find(photon.payload, "approver");
      if (previous_hash && previous_hash->as_string() == act_hash->as_string() && previous_approver)
        completed_approvers.emplace_back(std::string(previous_approver->as_string()));
    }
    completed_approvers.emplace_back(approver);
    if (const auto* required = cbor::find(act.parameters, "required_approvers");
        required && required->as_array()) {
      const auto complete = std::all_of(required->as_array()->begin(), required->as_array()->end(),
          [&completed_approvers](const cbor::Value& expected) {
            return std::any_of(completed_approvers.begin(), completed_approvers.end(),
                [&expected](const cbor::Value& actual) {
                  return actual.as_string() == expected.as_string();
                });
          });
      if (!complete) kind = "approval.stage-granted";
    }
  }
  const auto scope = cbor::find(act.parameters, "scope")
      ? std::string(cbor::find(act.parameters, "scope")->as_string()) : "one_shot";
  if (scope != "one_shot" && scope != "session")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "approval scope must be one_shot or session"));
  return emit(beam, kind, "tokmon.policy.result.v1", cbor::object({
      {"act_hash", *act_hash},
      {"epoch", cbor::find(act.parameters, "target_epoch") ?
          *cbor::find(act.parameters, "target_epoch") :
          cbor::Value(static_cast<std::int64_t>(act.epoch))},
      {"target_generation", cbor::find(act.parameters, "target_generation") ?
          *cbor::find(act.parameters, "target_generation") : cbor::Value(0)},
      {"deadline_ms", deadline_ms}, {"parameter_bound", true}, {"scope", scope},
      {"act_kind", cbor::find(act.parameters, "act_kind") ?
          *cbor::find(act.parameters, "act_kind") : cbor::Value("")},
      {"target", cbor::find(act.parameters, "target") ?
          *cbor::find(act.parameters, "target") : cbor::Value("")},
      {"approvers", std::move(completed_approvers)},
      {"approver", cbor::find(act.parameters, "approver") ?
          *cbor::find(act.parameters, "approver") : cbor::Value("user")}}));
}

}  // namespace tokmon::builtin
