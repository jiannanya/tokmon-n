#include "lenses/janus/janus_lens.hpp"

#include <algorithm>
#include <chrono>
#include <map>

#include "tokmon/hash.hpp"

namespace tokmon::builtin {
namespace {

cbor::Value agent_tool(std::string name, std::string description,
                       cbor::Value input_schema) {
  return cbor::object({{"name", std::move(name)},
      {"description", std::move(description)},
      {"input_schema", std::move(input_schema)}});
}

cbor::Value agent_tools(const std::string_view access_mode) {
  cbor::Value::Array tools;
  tools.push_back(agent_tool("read_file",
      "Read a UTF-8 text file inside the active workspace and return verified content",
      cbor::object({{"type", "object"}, {"properties", cbor::object({
          {"path", cbor::object({{"type", "string"}, {"minLength", 1},
                                   {"maxLength", 4096}})}})},
          {"required", cbor::Value::Array{"path"}}, {"additionalProperties", false}})));
  const bool read_only = access_mode == "read-only" || access_mode == "只读" ||
      access_mode == "只读访问";
  if (!read_only) {
    tools.push_back(agent_tool("write_file",
        "Create or replace a UTF-8 text file inside the active workspace; the write is read back and hashed",
        cbor::object({{"type", "object"}, {"properties", cbor::object({
            {"path", cbor::object({{"type", "string"}, {"minLength", 1},
                                     {"maxLength", 4096}})},
            {"content", cbor::object({{"type", "string"},
                                        {"maxLength", 1'048'576}})}})},
            {"required", cbor::Value::Array{"path", "content"}},
            {"additionalProperties", false}})));
    tools.push_back(agent_tool("run_command",
        "Run one executable directly without an implicit shell in the active workspace and return bounded output and exit status. Put the executable in argv[0]; for shell expressions invoke the platform shell explicitly (powershell.exe or cmd.exe on Windows, sh on Unix)",
        cbor::object({{"type", "object"}, {"properties", cbor::object({
            {"argv", cbor::object({{"type", "array"}, {"minItems", 1},
                {"maxItems", 128}, {"items", cbor::object({
                    {"type", "string"}, {"maxLength", 8192}})}})}})},
            {"required", cbor::Value::Array{"argv"}},
            {"additionalProperties", false}})));
  }
  tools.push_back(agent_tool("calculate", "Calculate a deterministic binary arithmetic expression",
      cbor::object({{"type", "object"}, {"properties", cbor::object({
          {"expression", cbor::object({{"type", "string"}, {"minLength", 3},
                                         {"maxLength", 256}})}})},
          {"required", cbor::Value::Array{"expression"}},
          {"additionalProperties", false}})));
  return tools;
}

bool agent_tool_result(const Photon& photon) {
  return photon.kind == "tool.result" || photon.kind == "fs.changed" ||
      photon.kind == "fs.read-completed" || photon.kind == "process.exited" ||
      photon.kind == "process.timed-out" || photon.kind == "process.cancelled";
}

std::chrono::milliseconds model_call_deadline(const cbor::Value& parameters) {
  // The Beam deadline must cover every HTTP attempt plus all deterministic
  // retry waits. Otherwise the outer scheduler can cancel Rhea halfway through
  // the configured 5s -> 10s -> 20s -> 40s -> 60s recovery sequence.
  const auto* attempts_value = cbor::find(parameters, "max_attempts");
  const auto* backoff_value = cbor::find(parameters, "retry_backoff_ms");
  const auto attempts = std::clamp<std::int64_t>(
      attempts_value ? attempts_value->as_integer(6) : 6, 1, 10);
  const auto base_backoff = std::clamp<std::int64_t>(
      backoff_value ? backoff_value->as_integer(5'000) : 5'000, 0, 60'000);
  std::int64_t retry_wait_ms = 0;
  std::int64_t wait_ms = base_backoff;
  for (std::int64_t retry = 0; retry + 1 < attempts; ++retry) {
    retry_wait_ms += std::min<std::int64_t>(wait_ms, 60'000);
    wait_ms = std::min<std::int64_t>(wait_ms * 2, 60'000);
  }
  const auto* first_token_value = cbor::find(parameters, "first_token_timeout_ms");
  const auto per_attempt_ms = std::clamp<std::int64_t>(
      first_token_value ? first_token_value->as_integer(60'000) : 60'000,
      1'000, 300'000);
  constexpr std::int64_t scheduler_margin_ms = 15'000;
  return std::chrono::milliseconds(
      attempts * per_attempt_ms + retry_wait_ms + scheduler_margin_ms);
}

}  // namespace

JanusLens::JanusLens() : LensBase(make_manifest("janus", "Janus / 默认 Agent 双面反射镜",
    {"ray.status", "model.intent"},
    {{"user.input", "*"}, {"assistant.message", "*"}, {"model.tool-call", "*"},
     {"tool.result", "*"}, {"model.*", "*"}, {"act.*", "*"}, {"ray.*", "*"},
     {"user.steering", "*"}},
    {{"ray.cancel", "tokmon.ray.cancel.v1"},
     {"ray.steer", "tokmon.ray.steer.v1"},
     {"ray.terminate", "tokmon.ray.terminate.v1"}})) {}

Result<void> JanusLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
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
  const Photon* result = nullptr;
  const Photon* process_output = nullptr;
  for (const auto& photon : photons.photons()) {
    if (photon.sequence <= start_sequence) continue;
    if (agent_tool_result(photon) && (!result || photon.sequence > result->sequence))
      result = &photon;
    if ((photon.kind == "process.stdout" || photon.kind == "process.stderr") &&
        (!process_output || photon.sequence > process_output->sequence))
      process_output = &photon;
  }
  const bool pending_tool = call && call->sequence > start_sequence &&
      (!result || result->sequence < call->sequence);
  const bool result_ready = result && result->sequence > start_sequence &&
      (!call || result->sequence > call->sequence);
  const bool complete = answer && answer->sequence > start_sequence && !pending_tool &&
      (!result_ready || answer->sequence > result->sequence);
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
  const auto* access_mode = cbor::find(input->payload, "access_mode");
  const auto access = access_mode ? std::string(access_mode->as_string())
                                  : std::string("full");
  auto tools = agent_tools(access);
  const auto model_surface_hash = sha256_hex(cbor::encode(cbor::object({
      {"input", input->payload},
      {"tool_result", result_ready ? result->payload : cbor::Value(nullptr)}})));
  const auto tool_schema_hash = sha256_hex(cbor::encode(tools));
  const auto* selected_model = cbor::find(input->payload, "model");
  const auto* effort = cbor::find(input->payload, "effort");
  cbor::Value::Array messages{cbor::object({{"role", "system"},
      {"content", "You are the Tokmon workspace agent. Use the provided tools for file, command, or calculation work. Call only one tool at a time. Never claim that an action is complete until its tool result has been returned and verified. Never repeat a successfully completed tool unless its verified result requires corrective work. After a tool result, continue with the next required tool or provide the final answer."}}),
      cbor::object({{"role", "user"}, {"content", text(*input)}})};
  if (result_ready) {
    std::string evidence = "Completed action ledger for this turn, in chronological order:";
    for (const auto& photon : photons.photons()) {
      if (photon.sequence <= start_sequence) continue;
      const bool requested_tool = photon.kind == "model.tool-call";
      const bool verified_result = agent_tool_result(photon);
      const bool command_output = photon.kind == "process.stdout" ||
          photon.kind == "process.stderr";
      if (!requested_tool && !verified_result && !command_output) continue;
      auto entry = cbor::diagnostic(photon.payload);
      if (entry.size() > 2'048u) entry.resize(2'048u);
      evidence.append("\n- ");
      evidence.append(requested_tool ? "Agent requested " :
          verified_result ? "Tokmon verified " : "Command output ");
      evidence.append(photon.kind);
      evidence.append(": ");
      evidence.append(entry);
      if (evidence.size() > 24'576u) {
        evidence.resize(24'576u);
        evidence.append(" [ledger truncated]");
        break;
      }
    }
    if (process_output && evidence.find("Command output") == std::string::npos)
      evidence.append("\n- Command output: " + cbor::diagnostic(process_output->payload));
    messages.push_back(cbor::object({{"role", "system"},
        {"content", evidence +
            "\nUse this ledger as authoritative state. Do not redo a successful entry. Continue the original task by calling the next missing tool, or give the final answer only when every requested action and verification is complete."}}));
  }
  auto parameters = cbor::object({
          {"prompt", steering && steering->sequence > input->sequence ? text(*steering) : text(*input)},
          {"messages", std::move(messages)}, {"tools", std::move(tools)},
          {"model", selected_model ? std::string(selected_model->as_string())
                                    : std::string("local-deterministic")},
          {"access_mode", access},
          {"effort", effort ? std::string(effort->as_string()) : std::string("标准")},
          {"input_photon", input->id},
          {"after_tool_result", result_ready},
          {"model_surface_hash", model_surface_hash},
          {"tool_schema_hash", tool_schema_hash},
          {"max_output_tokens", 4096}});
  // tokmond resolved this envelope from trusted YAML. Plaintext credentials
  // are never part of a Photon or Act.
  for (const auto* key : {"provider", "protocol", "endpoint", "secret_ref", "auth",
                          "allow_anonymous", "thinking", "reasoning_effort",
                          "max_output_tokens", "max_attempts", "retry_backoff_ms",
                          "first_token_timeout_ms", "idle_timeout_ms", "request_parameters",
                          "workspace_root"})
    if (const auto* value = cbor::find(input->payload, key))
      (*parameters.as_map())[key] = *value;
  auto act = propose(source, "model.call", "tokmon.model.call.v1",
      "org.tokmon.lens.rhea", std::move(parameters), RiskClass::external);
  act.timeout = model_call_deadline(act.parameters);
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
