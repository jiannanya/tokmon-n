#include "tokmon/engine.hpp"

#include <algorithm>
#include <exception>

#include <spdlog/spdlog.h>

#include "tokmon/logging.hpp"

namespace tokmon {

RayTracingEngine::RayTracingEngine(PhotonStore& store, LightPath& path,
                                   BeamRegistry& beams)
    : store_(store), path_(path), beams_(beams) {}

Result<Photon> RayTracingEngine::emit(PhotonDraft draft) {
  if (stopping_.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     "Nyxia is stopping"));
  return store_.append(std::move(draft));
}

void RayTracingEngine::log(const std::string_view level, const std::string_view message,
                           const LensId& lens) {
  const auto safe = redact(message);
  if (level == "error") spdlog::error("lens={} {}", lens, safe);
  else if (level == "warn") spdlog::warn("lens={} {}", lens, safe);
  else if (level == "debug") spdlog::debug("lens={} {}", lens, safe);
  else spdlog::info("lens={} {}", lens, safe);
}

Result<SurfaceSnapshot> RayTracingEngine::view(const RayId& ray) {
  auto photons = store_.read_ray(ray);
  if (!photons) return tl::unexpected(photons.error());
  PhotonWindow window(std::move(*photons));
  const auto path = path_.snapshot();
  SurfaceSnapshot snapshot;
  snapshot.epoch = path->epoch;
  for (const auto& mounted : path->lenses) {
    SurfaceBuilder builder(mounted.lens->manifest().id);
    try {
      auto result = mounted.lens->view(window, builder);
      if (!result) {
        log("warn", result.error().describe(), mounted.lens->manifest().id);
        snapshot.contributions.push_back(SurfaceContribution{
            .lens = mounted.lens->manifest().id,
            .channel = "diagnostic",
            .key = "view.error",
            .value = cbor::object({{"message", result.error().describe()}}),
            .priority = 100});
        continue;
      }
    } catch (const std::exception& exception) {
      log("error", std::string("view exception: ") + exception.what(),
          mounted.lens->manifest().id);
      continue;
    } catch (...) {
      log("error", "view unknown exception", mounted.lens->manifest().id);
      continue;
    }
    snapshot.contributions.insert(snapshot.contributions.end(),
                                  builder.contributions().begin(),
                                  builder.contributions().end());
    snapshot.proposals.insert(snapshot.proposals.end(),
                              builder.proposals().begin(), builder.proposals().end());
  }
  std::stable_sort(snapshot.contributions.begin(), snapshot.contributions.end(),
      [](const auto& left, const auto& right) { return left.priority > right.priority; });
  return snapshot;
}

Result<void> RayTracingEngine::audit_act(const Act& act, std::string kind,
                                         cbor::Value payload) {
  auto map = payload.as_map();
  if (!map) payload = cbor::Value::Map{};
  map = payload.as_map();
  (*map)["act"] = to_cbor(act);
  auto appended = emit(PhotonDraft{.ray = act.ray, .kind = std::move(kind),
      .schema = "tokmon.act.audit.v1", .payload = std::move(payload), .epoch = act.epoch,
      .caused_by_act = act.id});
  if (!appended) return tl::unexpected(appended.error());
  return {};
}

Result<RefractionResult> RayTracingEngine::execute(const PhotonWindow& window, Act act,
                                                    const MountedLens& mounted) {
  auto ticket = beams_.acquire(mounted.lens->manifest().id, mounted.generation, act.timeout);
  act.target = mounted.lens->manifest().id;
  act.generation = mounted.generation;
  if (auto audit = audit_act(act, "act.started"); !audit) {
    beams_.release(ticket->id); return tl::unexpected(audit.error());
  }
  RefractionBeam beam(*this, act, ticket->stop.get_token(), ticket->deadline);
  Result<RefractionResult> result = tl::unexpected(
      make_error(ErrorCode::internal_error, "Lens refract did not return"));
  try {
    result = mounted.lens->refract(window, act, beam);
  } catch (const std::exception& exception) {
    result = tl::unexpected(make_error(ErrorCode::lens_crashed,
                                       "Lens exception: " + std::string(exception.what())));
  } catch (...) {
    result = tl::unexpected(make_error(ErrorCode::lens_crashed,
                                       "Lens raised an unknown exception"));
  }
  beams_.release(ticket->id);
  if (!result) {
    auto failure = result.error();
    failure.lens = act.target; failure.ray = act.ray; failure.act = act.id;
    auto audit = audit_act(act, "act.failed",
                           cbor::object({{"error", failure.describe()}}));
    if (!audit) return tl::unexpected(audit.error());
    return tl::unexpected(std::move(failure));
  }
  if (auto audit = audit_act(act, "act.completed",
      cbor::object({{"status", std::string(to_string(result->status))},
                    {"detail", result->detail}})); !audit)
    return tl::unexpected(audit.error());
  return result;
}

Result<std::size_t> RayTracingEngine::advance(const RayId& ray,
                                               const std::size_t max_beats) {
  std::size_t beats = 0;
  while (beats < max_beats && !stopping_.load(std::memory_order_acquire)) {
    auto surface = view(ray);
    if (!surface) return tl::unexpected(surface.error());
    if (surface->proposals.empty()) {
      auto dark = emit(PhotonDraft{.ray = ray, .kind = "ray.darkened",
          .schema = "tokmon.ray.darkened.v1",
          .payload = cbor::object({{"beats", static_cast<std::int64_t>(beats)}}),
          .epoch = surface->epoch});
      if (!dark) return tl::unexpected(dark.error());
      return beats;
    }

    auto act = surface->proposals.front();
    act.ray = ray;
    const auto current = path_.snapshot();
    ActPipeline pipeline(approval_);
    if (auto proposed = audit_act(act, "act.proposed"); !proposed)
      return tl::unexpected(proposed.error());
    auto admitted = pipeline.admit(std::move(act), *current);
    if (!admitted) {
      auto rejected = emit(PhotonDraft{.ray = ray, .kind = "act.rejected",
          .schema = "tokmon.act.audit.v1",
          .payload = cbor::object({{"error", admitted.error().describe()}}),
          .epoch = current->epoch});
      if (!rejected) return tl::unexpected(rejected.error());
      return tl::unexpected(admitted.error());
    }
    if (auto audit = audit_act(*admitted, "act.admitted"); !audit)
      return tl::unexpected(audit.error());

    const MountedLens* target = nullptr;
    for (const auto& mounted : current->lenses)
      if (mounted.lens->manifest().id == admitted->target &&
          mounted.generation == admitted->generation) { target = &mounted; break; }
    if (!target)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "admitted Lens generation left active path"));
    auto photons = store_.read_ray(ray);
    if (!photons) return tl::unexpected(photons.error());
    auto result = execute(PhotonWindow(std::move(*photons)), *admitted, *target);
    if (!result) return tl::unexpected(result.error());
    ++beats;
  }
  if (stopping_.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled, "engine stopping"));
  auto exhausted = emit(PhotonDraft{.ray = ray, .kind = "ray.budget-exhausted",
      .schema = "tokmon.ray.budget.v1",
      .payload = cbor::object({{"max_beats", static_cast<std::int64_t>(max_beats)}}),
      .epoch = path_.snapshot()->epoch});
  if (!exhausted) return tl::unexpected(exhausted.error());
  return tl::unexpected(make_error(ErrorCode::invalid_state,
                                   "ray exceeded maximum beats"));
}

Result<RayId> RayTracingEngine::begin(std::string input, MountEpoch epoch) {
  const auto ray = make_id("ray");
  if (epoch == 0) epoch = path_.snapshot()->epoch;
  auto photon = emit(PhotonDraft{.ray = ray, .kind = "user.input",
      .schema = "tokmon.user.input.v1",
      .payload = cbor::object({{"text", std::move(input)}}), .epoch = epoch});
  if (!photon) return tl::unexpected(photon.error());
  return ray;
}

void RayTracingEngine::request_stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  const auto current = path_.snapshot();
  for (const auto& mounted : current->lenses) mounted.lens->request_stop();
}

void RayTracingEngine::set_approval(std::function<bool(const Act&)> approval) {
  approval_ = std::move(approval);
}

}  // namespace tokmon

