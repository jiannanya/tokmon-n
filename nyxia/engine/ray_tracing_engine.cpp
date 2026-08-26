#include "tokmon/engine.hpp"

#include <algorithm>
#include <exception>

#include "tokmon/hash.hpp"
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
  if (level == "error") log_error("lens={} {}", lens, safe);
  else if (level == "warn") log_warn("lens={} {}", lens, safe);
  else if (level == "debug") log_debug("lens={} {}", lens, safe);
  else log_info("lens={} {}", lens, safe);
}

Result<SurfaceSnapshot> RayTracingEngine::view(const RayId& ray) {
  auto photons = store_.read_ray(ray);
  if (!photons) return tl::unexpected(photons.error());
  PhotonWindow window(std::move(*photons));
  const auto path = path_.snapshot();
  SurfaceSnapshot snapshot;
  snapshot.epoch = path->epoch;
  snapshot.path_hash = path->hash;
  snapshot.input_prefix_hash = sha256_hex(cbor::encode(to_cbor(window)));
  snapshot.beat = "beat-" + sha256_hex(ray + "\n" + std::to_string(path->epoch) +
      "\n" + path->hash + "\n" + snapshot.input_prefix_hash).substr(0, 32);
  BeatBoardBuilder board(BeatMetadata{.beat = snapshot.beat, .ray = ray,
      .epoch = path->epoch, .path_hash = path->hash,
      .input_prefix_hash = snapshot.input_prefix_hash});

  // Phase 1: every Lens observes one immutable Photon prefix. Legacy view()
  // remains the compatibility derive operation.
  for (std::size_t path_index = 0; path_index < path->lenses.size(); ++path_index) {
    const auto& mounted = path->lenses[path_index];
    SurfaceBuilder builder(mounted.lens->manifest().id);
    bool view_succeeded = false;
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
      } else {
        view_succeeded = true;
      }
    } catch (const std::exception& exception) {
      log("error", std::string("view exception: ") + exception.what(),
          mounted.lens->manifest().id);
      snapshot.contributions.push_back(SurfaceContribution{
          .lens = mounted.lens->manifest().id, .channel = "diagnostic",
          .key = "view.exception",
          .value = cbor::object({{"message", exception.what()}}), .priority = 100});
    } catch (...) {
      log("error", "view unknown exception", mounted.lens->manifest().id);
      snapshot.contributions.push_back(SurfaceContribution{
          .lens = mounted.lens->manifest().id, .channel = "diagnostic",
          .key = "view.exception",
          .value = cbor::object({{"message", "unknown exception"}}), .priority = 100});
    }
    if (view_succeeded) {
      snapshot.contributions.insert(snapshot.contributions.end(),
                                    builder.contributions().begin(),
                                    builder.contributions().end());
      snapshot.proposals.insert(snapshot.proposals.end(),
                                builder.proposals().begin(), builder.proposals().end());
    }

    auto* raw_extension = dynamic_cast<IOpticalLensExtension*>(mounted.lens.get());
    std::shared_ptr<IOpticalLensExtension> extension;
    if (raw_extension)
      extension = std::shared_ptr<IOpticalLensExtension>(mounted.lens, raw_extension);
    cbor::Value frozen_state;
    bool derive_succeeded = true;
    if (extension && extension->supports_derive()) {
      try {
        auto derived = extension->derive(window);
        if (derived) frozen_state = std::move(*derived);
        else {
          derive_succeeded = false;
          log("warn", derived.error().describe(), mounted.lens->manifest().id);
          snapshot.contributions.push_back(SurfaceContribution{
              .lens = mounted.lens->manifest().id, .channel = "diagnostic",
              .key = "derive.error",
              .value = cbor::object({{"message", derived.error().describe()}}),
              .priority = 100});
        }
      } catch (const std::exception& exception) {
        derive_succeeded = false;
        log("error", std::string("derive exception: ") + exception.what(),
            mounted.lens->manifest().id);
      } catch (...) {
        derive_succeeded = false;
        log("error", "derive unknown exception", mounted.lens->manifest().id);
      }
    }
    auto published = board.publish(mounted.lens->manifest().id, mounted.artifact_hash,
        mounted.generation, path_index,
        derive_succeeded ? mounted.lens->manifest().provides_queries :
                           std::vector<OpticalQueryCapability>{},
        std::move(extension), std::move(frozen_state));
    if (!published) {
      log("warn", published.error().describe(), mounted.lens->manifest().id);
      snapshot.contributions.push_back(SurfaceContribution{
          .lens = mounted.lens->manifest().id, .channel = "diagnostic",
          .key = "beatboard.publish.error",
          .value = cbor::object({{"message", published.error().describe()}}),
          .priority = 100});
    }
  }

  auto frozen = std::move(board).freeze(snapshot);
  if (!frozen) return tl::unexpected(frozen.error());

  // Phase 2: only opted-in Lens extensions run. All get/query operations are
  // served from the frozen board above, including providers later in the path.
  for (const auto& mounted : path->lenses) {
    auto* raw_extension = dynamic_cast<IOpticalLensExtension*>(mounted.lens.get());
    if (!raw_extension || !raw_extension->supports_coordinate()) continue;
    OpticalContext optical(*frozen, mounted.lens->manifest().id, mounted.generation,
                           mounted.lens->manifest().consumes_queries);
    SurfaceBuilder builder(mounted.lens->manifest().id);
    bool succeeded = false;
    std::string failure_message = "coordinate phase failed";
    try {
      auto result = raw_extension->coordinate(window, optical, builder);
      if (!result) {
        failure_message = result.error().describe();
        log("warn", result.error().describe(), mounted.lens->manifest().id);
      } else succeeded = true;
    } catch (const std::exception& exception) {
      failure_message = std::string("coordinate exception: ") + exception.what();
      log("error", std::string("coordinate exception: ") + exception.what(),
          mounted.lens->manifest().id);
    } catch (...) {
      failure_message = "coordinate unknown exception";
      log("error", "coordinate unknown exception", mounted.lens->manifest().id);
    }
    if (!succeeded) {
      snapshot.contributions.push_back(SurfaceContribution{
          .lens = mounted.lens->manifest().id, .channel = "diagnostic",
          .key = "coordinate.error",
          .value = cbor::object({{"message", std::move(failure_message)}}),
          .priority = 100});
      continue;
    }
    snapshot.contributions.insert(snapshot.contributions.end(),
        builder.contributions().begin(), builder.contributions().end());
    const auto traces = optical.query_traces();
    cbor::Value::Array provenance;
    for (const auto& trace : traces)
      provenance.push_back(cbor::object({
          {"capability", trace.capability}, {"provider", trace.provider},
          {"provider_generation", static_cast<std::int64_t>(trace.provider_generation)},
          {"request_schema", trace.request_schema},
          {"response_schema", trace.response_schema},
          {"request_hash", trace.request_hash}, {"response_hash", trace.response_hash}}));
    for (auto proposal : builder.proposals()) {
      if (!provenance.empty()) {
        if (!proposal.provenance.as_map()) proposal.provenance = cbor::Value::Map{};
        (*proposal.provenance.as_map())["optical_queries"] = provenance;
        (*proposal.provenance.as_map())["beat"] = snapshot.beat;
        (*proposal.provenance.as_map())["path_hash"] = snapshot.path_hash;
      }
      snapshot.proposals.push_back(std::move(proposal));
    }
  }
  snapshot.query_traces = (*frozen)->traces();
  std::stable_sort(snapshot.contributions.begin(), snapshot.contributions.end(),
      [](const auto& left, const auto& right) { return left.priority > right.priority; });
  return snapshot;
}

Result<void> RayTracingEngine::audit_act(const Act& act, std::string kind,
                                         cbor::Value payload) {
  auto map = payload.as_map();
  if (!map) payload = cbor::Value::Map{};
  map = payload.as_map();
  auto safe_act = redact_value(to_cbor(act));
  if (act.kind == "blob.put" && cbor::find(act.parameters, "sensitive") &&
      cbor::find(act.parameters, "sensitive")->as_bool()) {
    if (auto* root = safe_act.as_map()) {
      const auto found = root->find("parameters");
      if (found != root->end() && found->second.as_map())
        (*found->second.as_map())["content"] = "<redacted>";
    }
  }
  (*map)["act"] = std::move(safe_act);
  (*map)["act_hash"] = act_binding_hash(act);
  auto appended = emit(PhotonDraft{.ray = act.ray, .kind = std::move(kind),
      .schema = "tokmon.act.audit.v1", .payload = std::move(payload), .epoch = act.epoch,
      .caused_by_act = act.id});
  if (!appended) return tl::unexpected(appended.error());
  return {};
}

Result<RefractionResult> RayTracingEngine::execute(const PhotonWindow& window, Act act,
                                                    const MountedLens& mounted) {
  auto ticket = beams_.acquire(mounted.lens->manifest().id, mounted.generation,
                               act.ray, act.timeout);
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
    if (failure.code == ErrorCode::cancelled) {
      auto cancelled = emit(PhotonDraft{.ray = act.ray, .kind = "ray.cancelled",
          .schema = "tokmon.ray.cancelled.v1",
          .payload = cbor::object({{"act", act.id}, {"history_deleted", false}}),
          .epoch = act.epoch, .caused_by_act = act.id});
      if (!cancelled) return tl::unexpected(cancelled.error());
    }
    return tl::unexpected(std::move(failure));
  }
  const auto terminal_kind = result->status == RefractionStatus::completed ? "act.completed" :
      result->status == RefractionStatus::rejected ? "act.rejected" : "act.failed";
  if (auto audit = audit_act(act, terminal_kind,
      cbor::object({{"status", std::string(to_string(result->status))},
                    {"detail", result->detail}})); !audit)
    return tl::unexpected(audit.error());
  return result;
}

Result<RefractionResult> RayTracingEngine::refract(Act act) {
  if (stopping_.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled, "engine stopping"));
  const auto current = path_.snapshot();
  const auto proposed_ray = act.ray;
  ActPipeline pipeline(admission_);
  if (auto proposed = audit_act(act, "act.proposed"); !proposed)
    return tl::unexpected(proposed.error());
  auto admitted = pipeline.admit(std::move(act), *current);
  if (!admitted) {
    auto rejected = emit(PhotonDraft{.ray = proposed_ray, .kind = "act.rejected",
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
        mounted.generation == admitted->generation) {
      target = &mounted;
      break;
    }
  if (!target)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "admitted Lens generation left active path"));
  auto photons = store_.read_ray(admitted->ray);
  if (!photons) return tl::unexpected(photons.error());
  return execute(PhotonWindow(std::move(*photons)), *admitted, *target);
}

Result<std::size_t> RayTracingEngine::advance(const RayId& ray,
                                               const std::size_t max_beats) {
  std::size_t beats = 0;
  while (beats < max_beats && !stopping_.load(std::memory_order_acquire)) {
    {
      std::scoped_lock lock(cancelled_mutex_);
      if (cancelled_rays_.contains(ray)) {
        auto cancelled = emit(PhotonDraft{.ray = ray, .kind = "ray.cancelled",
            .schema = "tokmon.ray.cancelled.v1",
            .payload = cbor::object({{"beats", static_cast<std::int64_t>(beats)},
                                      {"history_deleted", false}}),
            .epoch = path_.snapshot()->epoch});
        if (!cancelled) return tl::unexpected(cancelled.error());
        return tl::unexpected(make_error(ErrorCode::cancelled, "ray was cancelled"));
      }
    }
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
    auto result = refract(std::move(act));
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

void RayTracingEngine::cancel_ray(const RayId& ray) noexcept {
  {
    std::scoped_lock lock(cancelled_mutex_);
    cancelled_rays_.insert(ray);
  }
  (void)beams_.stop_ray(ray);
}

Result<RayId> RayTracingEngine::begin(std::string input, MountEpoch epoch,
                                      cbor::Value context) {
  const auto ray = make_id("ray");
  auto photon = continue_ray(ray, std::move(input), epoch, std::move(context));
  if (!photon) return tl::unexpected(photon.error());
  return ray;
}

Result<Photon> RayTracingEngine::continue_ray(const RayId& ray, std::string input,
                                               MountEpoch epoch, cbor::Value context) {
  if (ray.empty() || input.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "ray and user input must be non-empty"));
  {
    std::scoped_lock lock(cancelled_mutex_);
    if (cancelled_rays_.contains(ray))
      return tl::unexpected(make_error(ErrorCode::cancelled,
                                       "a cancelled ray cannot accept new input"));
  }
  if (epoch == 0) epoch = path_.snapshot()->epoch;
  if (!context.as_map()) context = cbor::Value::Map{};
  (*context.as_map())["text"] = std::move(input);
  return emit(PhotonDraft{.ray = ray, .kind = "user.input",
      .schema = "tokmon.user.input.v1",
      .payload = std::move(context), .epoch = epoch});
}

void RayTracingEngine::request_stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  const auto current = path_.snapshot();
  for (const auto& mounted : current->lenses) mounted.lens->request_stop();
}

void RayTracingEngine::set_admission(
    std::function<AdmissionDecision(const Act&)> admission) {
  admission_ = std::move(admission);
}

}  // namespace tokmon
