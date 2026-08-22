#include "lenses/janus/janus_lens.hpp"

#include "tokmon/hash.hpp"

namespace tokmon::builtin {

JanusLens::JanusLens() : LensBase(make_manifest("janus", "Janus / 默认 Agent 双面反射镜",
    {"ray.status", "model.intent"},
    {{"user.input", "*"}, {"assistant.message", "*"}, {"model.tool-call", "*"},
     {"tool.result", "*"}, {"act.*", "*"}, {"ray.*", "*"}},
    {{"ray.cancel", "tokmon.ray.cancel.v1"},
     {"ray.steer", "tokmon.ray.steer.v1"}})) {}

Result<void> JanusLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* input = photons.latest("user.input");
  if (!input)
    return identify(surface, "ray.status", cbor::object({{"state", "idle"}}));
  const auto* cancelled = photons.latest("ray.cancel-requested");
  const auto* exhausted = photons.latest("ray.budget-exhausted");
  if ((cancelled && cancelled->sequence > input->sequence) ||
      (exhausted && exhausted->sequence > input->sequence))
    return identify(surface, "ray.status", cbor::object({
        {"state", cancelled && cancelled->sequence > input->sequence
                      ? "cancelled" : "budget_exhausted"},
        {"terminal", true}}));
  const auto* answer = photons.latest("assistant.message");
  const auto* call = photons.latest("model.tool-call");
  const auto* result = photons.latest("tool.result");
  const bool complete = answer && answer->sequence > input->sequence;
  const bool pending_tool = call && call->sequence > input->sequence &&
      (!result || result->sequence < call->sequence);
  const bool result_ready = result && result->sequence > input->sequence &&
      (!call || result->sequence > call->sequence);
  std::int64_t repeated_model_calls = 0;
  for (const auto& photon : photons.photons()) {
    if (photon.sequence <= input->sequence) continue;
    if (photon.kind == "model.dispatched") ++repeated_model_calls;
    if (photon.kind == "assistant.message" || photon.kind == "tool.result")
      repeated_model_calls = 0;
  }
  const bool oscillating = repeated_model_calls >= 3;
  const auto state = complete ? "complete" : pending_tool ? "waiting_tool" :
      oscillating ? "oscillation" : "need_model";
  if (auto added = identify(surface, "ray.status", cbor::object({
      {"state", state}, {"input_sequence", static_cast<std::int64_t>(input->sequence)},
      {"oscillation_count", repeated_model_calls}}));
      !added) return added;
  if (complete || pending_tool || oscillating) return {};
  const auto& source = result_ready ? *result : *input;
  const auto model_surface_hash = sha256_hex(cbor::encode(cbor::object({
      {"input", input->payload},
      {"tool_result", result_ready ? result->payload : cbor::Value(nullptr)}})));
  const auto tool_schema_hash = sha256_hex("calculate@tokmon.math.calculate.v1");
  auto act = propose(source, "model.call", "tokmon.model.call.v1",
      "org.tokmon.lens.rhea", cbor::object({
          {"prompt", text(*input)}, {"model", "local-deterministic"},
          {"input_photon", input->id},
          {"after_tool_result", result_ready},
          {"model_surface_hash", model_surface_hash},
          {"tool_schema_hash", tool_schema_hash},
          {"max_output_tokens", 4096}}), RiskClass::external);
  return surface.propose(std::move(act));
}

Result<RefractionResult> JanusLens::refract(const PhotonWindow&, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto kind = act.kind == "ray.cancel" ? "ray.cancel-requested" : "user.steering";
  return emit(beam, kind, "tokmon.ray.control.v1", act.parameters);
}

}  // namespace tokmon::builtin
