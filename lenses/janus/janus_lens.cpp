#include "lenses/janus/janus_lens.hpp"

#include <algorithm>
#include <map>

#include "tokmon/hash.hpp"

namespace tokmon::builtin {

JanusLens::JanusLens() : LensBase(make_manifest("janus", "Janus / 默认 Agent 双面反射镜",
    {"ray.status", "model.intent"},
    {{"user.input", "*"}, {"assistant.message", "*"}, {"model.tool-call", "*"},
     {"tool.result", "*"}, {"model.*", "*"}, {"act.*", "*"}, {"ray.*", "*"},
     {"user.steering", "*"}},
    {{"ray.cancel", "tokmon.ray.cancel.v1"},
     {"ray.steer", "tokmon.ray.steer.v1"},
     {"ray.terminate", "tokmon.ray.terminate.v1"}})) {}

Result<void> JanusLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* input = photons.latest("user.input");
  if (!input)
    return identify(surface, "ray.status", cbor::object({{"state", "idle"}}));
  const auto* steering = photons.latest("user.steering");
  const auto start_sequence = steering && steering->sequence > input->sequence
      ? steering->sequence : input->sequence;
  const auto* cancelled = photons.latest("ray.cancel-requested");
  const auto* exhausted = photons.latest("ray.budget-exhausted");
  const auto* terminated = photons.latest("ray.terminated");
  if ((cancelled && cancelled->sequence > start_sequence) ||
      (exhausted && exhausted->sequence > start_sequence) ||
      (terminated && terminated->sequence > start_sequence))
    return identify(surface, "ray.status", cbor::object({
        {"state", cancelled && cancelled->sequence > start_sequence ? "cancelled" :
            exhausted && exhausted->sequence > start_sequence ? "budget_exhausted" : "terminated"},
        {"terminal", true}}));
  const auto* answer = photons.latest("assistant.message");
  const auto* call = photons.latest("model.tool-call");
  const auto* result = photons.latest("tool.result");
  const bool complete = answer && answer->sequence > start_sequence;
  const bool pending_tool = call && call->sequence > start_sequence &&
      (!result || result->sequence < call->sequence);
  const bool result_ready = result && result->sequence > start_sequence &&
      (!call || result->sequence > call->sequence);
  std::int64_t model_calls = 0; std::int64_t tool_calls = 0;
  std::int64_t used_tokens = 0; std::int64_t cost_microunits = 0;
  std::int64_t steps = 0; std::int64_t consecutive_failures = 0;
  std::map<std::string, std::int64_t, std::less<>> normalized_acts;
  for (const auto& photon : photons.photons()) {
    if (photon.sequence <= start_sequence) continue;
    if (photon.kind == "model.requested") ++model_calls;
    if (photon.kind == "model.tool-call") ++tool_calls;
    if (photon.kind == "act.completed") { ++steps; consecutive_failures = 0; }
    if (photon.kind == "act.failed" || photon.kind == "model.failed") ++consecutive_failures;
    if (photon.kind == "model.usage") {
      if (const auto* value = cbor::find(photon.payload, "input_tokens"))
        used_tokens += value->as_integer();
      if (const auto* value = cbor::find(photon.payload, "output_tokens"))
        used_tokens += value->as_integer();
      if (const auto* value = cbor::find(photon.payload, "cost_microunits"))
        cost_microunits += value->as_integer();
    }
    if (photon.kind == "act.proposed")
      if (const auto* value = cbor::find(photon.payload, "act_hash"))
        ++normalized_acts[std::string(value->as_string())];
  }
  const auto* limits = photons.latest("ray.budget-configured");
  const auto limit = [limits](const std::string_view name, const std::int64_t fallback) {
    const auto* value = limits ? cbor::find(limits->payload, name) : nullptr;
    return value ? value->as_integer(fallback) : fallback;
  };
  const bool budget_exceeded = steps >= limit("max_steps", 64) ||
      model_calls >= limit("max_model_calls", 16) || tool_calls >= limit("max_tool_calls", 32) ||
      used_tokens >= limit("max_tokens", 100'000) ||
      cost_microunits >= limit("max_cost_microunits", 1'000'000'000);
  const bool oscillating = std::ranges::any_of(normalized_acts,
      [](const auto& item) { return item.second >= 3; }) || consecutive_failures >= 3;
  const auto* requested = photons.latest("model.requested");
  const auto* model_terminal = photons.latest("model.completed");
  const bool waiting_model = requested && requested->sequence > start_sequence &&
      (!model_terminal || model_terminal->sequence < requested->sequence);
  const auto state = complete ? "complete" : pending_tool ? "waiting_tool" :
      waiting_model ? "waiting_model" : budget_exceeded ? "budget_exhausted" :
      oscillating ? "oscillation_stopped" : "need_model";
  if (auto added = identify(surface, "ray.status", cbor::object({
      {"state", state}, {"input_sequence", static_cast<std::int64_t>(input->sequence)},
      {"steps", steps}, {"model_calls", model_calls}, {"tool_calls", tool_calls},
      {"tokens", used_tokens}, {"cost_microunits", cost_microunits},
      {"consecutive_failures", consecutive_failures}, {"terminal", complete}}));
      !added) return added;
  if (complete || pending_tool || waiting_model) return {};
  if (budget_exceeded || oscillating) {
    auto act = propose(photons.latest() ? *photons.latest() : *input, "ray.terminate",
        "tokmon.ray.terminate.v1", manifest().id,
        cbor::object({{"reason", budget_exceeded ? "budget_exhausted" : "oscillation"},
          {"steps", steps}, {"tokens", used_tokens}, {"tool_calls", tool_calls}}),
        RiskClass::observe);
    return surface.propose(std::move(act));
  }
  const auto& source = result_ready ? *result : *input;
  const auto model_surface_hash = sha256_hex(cbor::encode(cbor::object({
      {"input", input->payload},
      {"tool_result", result_ready ? result->payload : cbor::Value(nullptr)}})));
  const auto tool_schema_hash = sha256_hex("calculate@tokmon.math.calculate.v1");
  const auto* selected_model = cbor::find(input->payload, "model");
  const auto* access_mode = cbor::find(input->payload, "access_mode");
  const auto* effort = cbor::find(input->payload, "effort");
  auto act = propose(source, "model.call", "tokmon.model.call.v1",
      "org.tokmon.lens.rhea", cbor::object({
          {"prompt", steering && steering->sequence > input->sequence ? text(*steering) : text(*input)},
          {"model", "local-deterministic"},
          {"requested_model", selected_model ? std::string(selected_model->as_string())
                                               : std::string("local-deterministic")},
          {"access_mode", access_mode ? std::string(access_mode->as_string())
                                       : std::string("完全访问")},
          {"effort", effort ? std::string(effort->as_string()) : std::string("标准")},
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
  const auto kind = act.kind == "ray.cancel" ? "ray.cancel-requested" :
      act.kind == "ray.terminate" ?
          (cbor::find(act.parameters, "reason") &&
           cbor::find(act.parameters, "reason")->as_string() == "budget_exhausted"
              ? "ray.budget-exhausted" : "ray.terminated") : "user.steering";
  return emit(beam, kind, "tokmon.ray.control.v1", act.parameters);
}

}  // namespace tokmon::builtin
