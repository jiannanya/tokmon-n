#include "lenses/tracket/tracket_lens.hpp"

#include <set>

#include "tokmon/ids.hpp"

namespace tokmon::builtin {

TracketLens::TracketLens() : LensBase(make_manifest("tracket", "Tracket / 因果记录与回放光路镜",
    {"fact.integrity", "ui.trajectory", "ui.causality"},
    {{"*", "*"}},
    {{"integrity.verify", "tokmon.integrity.verify.v1"},
     {"replay.create", "tokmon.replay.create.v1"},
     {"ray.fork", "tokmon.ray.fork.v1"},
     {"trajectory.export", "tokmon.trajectory.export.v1"}})) {}

Result<void> TracketLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  bool ordered = true; bool linked = true; bool parents_valid = true;
  std::uint64_t previous = 0;
  cbor::Value::Array timeline; cbor::Value::Array edges;
  std::set<std::string> seen_ids;
  for (const auto& photon : photons.photons()) {
    if (previous != 0 && photon.sequence <= previous) ordered = false;
    if (photon.hash.size() != 64u ||
        (!photon.previous_hash.empty() && photon.previous_hash.size() != 64u)) linked = false;
    if (!seen_ids.insert(photon.id).second) linked = false;
    if (photon.parent) {
      if (!seen_ids.contains(*photon.parent)) parents_valid = false;
      edges.push_back(cbor::object({{"from", *photon.parent}, {"to", photon.id},
                                    {"relation", "parent"}}));
    }
    if (!photon.caused_by_act.empty())
      edges.push_back(cbor::object({{"from", photon.caused_by_act}, {"to", photon.id},
                                    {"relation", "caused_by_act"}}));
    previous = photon.sequence;
    timeline.push_back(cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"id", photon.id}, {"parent", photon.parent ? *photon.parent : ""},
        {"kind", photon.kind}, {"caused_by_act", photon.caused_by_act}}));
  }
  if (auto result = surface.add("ui.trajectory", "active-ray", std::move(timeline), 20);
      !result) return result;
  if (auto result = surface.add("ui.causality", "active-ray", std::move(edges), 15);
      !result) return result;
  return identify(surface, "fact.integrity", cbor::object({
      {"ordered", ordered}, {"hashes_present", linked}, {"parents_valid", parents_valid},
      {"valid", ordered && linked && parents_valid},
      {"photon_count", static_cast<std::int64_t>(photons.photons().size())}}));
}

Result<RefractionResult> TracketLens::refract(const PhotonWindow&, const Act& act,
                                               RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "replay.create") {
    const auto* level = cbor::find(act.parameters, "level");
    if (!level || (level->as_string() != "R0" && level->as_string() != "R1" &&
                   level->as_string() != "R2" && level->as_string() != "R3"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "replay level must be R0, R1, R2 or R3"));
    if (level->as_string() == "R3" && !act.approved)
      return tl::unexpected(make_error(ErrorCode::approval_required,
                                       "R3 replay requires explicit approval"));
  }
  const std::string kind = act.kind == "integrity.verify" ? "integrity.verified" :
      act.kind == "replay.create" ? "replay.created" :
      act.kind == "ray.fork" ? "ray.forked" : "trajectory.exported";
  auto payload = cbor::object({
      {"request", act.parameters}, {"source_mutated", false},
      {"reality_actions_executed", false},
      {"fork_ray", act.kind == "ray.fork" ? make_id("ray") : ""}});
  return emit(beam, kind, "tokmon.trajectory.result.v1", std::move(payload));
}

}  // namespace tokmon::builtin
