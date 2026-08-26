#include "tokmon/engine.hpp"

#include <set>

namespace tokmon {

ActPipeline::ActPipeline(Admission admission) : admission_(std::move(admission)) {}

Result<Act> ActPipeline::admit(Act act, const LightPathSnapshot& path) const {
  if (act.id.empty()) act.id = make_id("act");
  if (act.ray.empty() || act.kind.empty() || act.schema.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Act ray, kind and schema are required"));
  if (!act.parameters.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Act parameters must be a map"));
  if (act.epoch == 0) act.epoch = path.epoch;
  if (act.epoch != path.epoch)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "Act mount epoch is stale"));
  if (!act.assembly_hash.empty() &&
      (!path.assembly || act.assembly_hash != path.assembly->hash))
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "Act OpticalAssembly snapshot is stale"));
  if (!act.assembly_hash.empty() && act.proposal_cell.empty())
    return tl::unexpected(make_error(ErrorCode::integrity_error,
                                     "optically proposed Act has no proposal cell"));

  const MountedLens* target = nullptr;
  for (const auto& mounted : path.lenses) {
    if (!act.target.empty() && mounted.lens->manifest().id != act.target) continue;
    if (std::any_of(mounted.lens->manifest().refracts.begin(),
                    mounted.lens->manifest().refracts.end(),
                    [&act](const ActPattern& pattern) { return pattern.matches(act); })) {
      if (target != nullptr)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
                                         "Act resolves to more than one Lens"));
      target = &mounted;
    }
  }
  if (target == nullptr)
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "no Lens refracts Act " + act.kind));
  act.target = target->lens->manifest().id;
  act.generation = target->generation;
  if (act.idempotency_key.empty()) act.idempotency_key = act.id;

  const auto decision = admission_ ? admission_(act) :
      act.risk == RiskClass::external_irreversible ? AdmissionDecision::ask :
                                                    AdmissionDecision::allow;
  if (decision == AdmissionDecision::deny)
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "Fallen policy denied Act " + act.kind));
  if (decision == AdmissionDecision::ask)
    return tl::unexpected(make_error(ErrorCode::approval_required,
        "Act requires a bound approval Photon"));
  if (target->lens->manifest().trust == TrustLevel::t3)
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "quarantined Lens cannot receive Acts"));
  return act;
}

}  // namespace tokmon
