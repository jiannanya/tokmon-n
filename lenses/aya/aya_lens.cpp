#include "lenses/aya/aya_lens.hpp"

#include "tokmon/ids.hpp"

namespace tokmon::builtin {

AyaLens::AyaLens() : LensBase(make_manifest("aya", "Aya / 子运行分形复眼镜",
    {"child.runs", "ui.child-runs"},
    {{"child.requested", "*"}, {"child.started", "*"}, {"child.progress", "*"},
     {"child.completed", "*"}, {"child.joined", "*"}},
    {{"child.spawn", "tokmon.child.spawn.v1"},
     {"child.join", "tokmon.child.join.v1"},
     {"child.cancel", "tokmon.child.cancel.v1"}})) {}

Result<void> AyaLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array children;
  for (const auto& photon : photons.photons()) {
    if (!photon.kind.starts_with("child.")) continue;
    children.push_back(cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"kind", photon.kind}, {"detail", photon.payload}}));
  }
  if (auto result = identify(surface, "child.runs", cbor::object({
      {"items", children}, {"default_workspace_mode", "read_only"}})); !result)
    return result;
  return surface.add("ui.child-runs", "active-ray", std::move(children), 20);
}

Result<RefractionResult> AyaLens::refract(const PhotonWindow&, const Act& act,
                                           RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "child.spawn") {
    const auto* parent_budget = cbor::find(act.parameters, "parent_budget");
    const auto* child_budget = cbor::find(act.parameters, "budget");
    const auto* allowed = cbor::find(act.parameters, "allowed_acts");
    const auto* workspace = cbor::find(act.parameters, "workspace_mode");
    const auto* join = cbor::find(act.parameters, "join_policy");
    if (!parent_budget || !child_budget || parent_budget->as_integer() <= 0 ||
        child_budget->as_integer() <= 0 || !allowed || !allowed->as_array() ||
        !workspace || !join || join->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "child.spawn requires budgets, allowed_acts, workspace_mode and join_policy"));
    if (child_budget->as_integer() > parent_budget->as_integer())
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "child budget exceeds parent budget"));
    if (workspace->as_string() != "read_only" && workspace->as_string() != "isolated_write")
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "child workspace mode is not allowed"));
  } else if (!cbor::find(act.parameters, "child_ray") ||
             cbor::find(act.parameters, "child_ray")->as_string().empty()) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "child action requires child_ray"));
  }
  const std::string kind = act.kind == "child.spawn" ? "child.started" :
      act.kind == "child.join" ? "child.joined" : "child.cancelled";
  return emit(beam, kind, "tokmon.child.result.v1",
              cbor::object({{"parent_ray", act.ray}, {"request", act.parameters},
                            {"child_ray", act.kind == "child.spawn" ? make_id("ray") :
                                std::string(cbor::find(act.parameters, "child_ray")->as_string())},
                            {"history_deleted", false}}));
}

}  // namespace tokmon::builtin
