#include "lenses/techor/techor_lens.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <sstream>

#include "lenses/common/schema_validator.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/json.hpp"

namespace tokmon::builtin {
namespace {

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string act_kind;
  std::string act_schema;
  std::string target;
  cbor::Value input_schema;
  RiskClass risk{RiskClass::reversible};
  bool external{false};
  std::string connection_ref;
  std::string remote_name;
  std::string remote_schema_hash;
  std::uint64_t sequence{0};
};

std::string string_field(const cbor::Value& value, const std::string_view key,
                         const std::string_view fallback = {}) {
  const auto* field = cbor::find(value, key);
  return field ? std::string(field->as_string(fallback)) : std::string(fallback);
}

RiskClass risk_field(const cbor::Value& value) {
  const auto risk = string_field(value, "risk", "reversible");
  if (risk == "observe") return RiskClass::observe;
  if (risk == "external") return RiskClass::external;
  if (risk == "external_irreversible") return RiskClass::external_irreversible;
  return RiskClass::reversible;
}

ToolDefinition calculator() {
  return ToolDefinition{.name = "calculate",
      .description = "计算一个确定性的二元算术表达式",
      .act_kind = "tool.calculate", .act_schema = "tokmon.math.calculate.v1",
      .target = "org.tokmon.lens.calculator",
      .input_schema = cbor::object({{"type", "object"},
          {"properties", cbor::object({{"expression", cbor::object({
              {"type", "string"}, {"minLength", 3}, {"maxLength", 256}})}})},
          {"required", cbor::Value::Array{"expression"}},
          {"additionalProperties", false}}),
      .risk = RiskClass::observe};
}

ToolDefinition registered_tool(const Photon& photon) {
  return ToolDefinition{.name = string_field(photon.payload, "name"),
      .description = string_field(photon.payload, "description"),
      .act_kind = string_field(photon.payload, "act_kind"),
      .act_schema = string_field(photon.payload, "act_schema"),
      .target = string_field(photon.payload, "target"),
      .input_schema = cbor::find(photon.payload, "input_schema")
          ? *cbor::find(photon.payload, "input_schema") : cbor::object({{"type", "object"}}),
      .risk = risk_field(photon.payload), .sequence = photon.sequence};
}

std::vector<ToolDefinition> external_tools(const Photon& photon) {
  std::vector<ToolDefinition> result;
  const auto* tools = cbor::find(photon.payload, "tools");
  if (!tools || !tools->as_array()) return result;
  const auto connection_ref = string_field(photon.payload, "connection_ref");
  const auto catalog_hash = string_field(photon.payload, "schema_hash", photon.hash);
  for (const auto& tool : *tools->as_array()) {
    const auto name = string_field(tool, "name");
    if (name.empty()) continue;
    const auto* schema = cbor::find(tool, "inputSchema");
    if (!schema) schema = cbor::find(tool, "input_schema");
    const auto schema_value = schema ? *schema : cbor::object({{"type", "object"}});
    result.push_back(ToolDefinition{.name = name,
        .description = string_field(tool, "description", "External MCP tool"),
        .act_kind = "external.call", .act_schema = "tokmon.external.call.v1",
        .target = "org.tokmon.lens.iris", .input_schema = schema_value,
        .risk = RiskClass::external, .external = true,
        .connection_ref = connection_ref, .remote_name = name,
        .remote_schema_hash = catalog_hash.size() == 64u ? catalog_hash
                                                         : sha256_hex(cbor::encode(schema_value)),
        .sequence = photon.sequence});
  }
  return result;
}

std::map<std::string, std::vector<ToolDefinition>, std::less<>> tool_catalog(
    const PhotonWindow& photons) {
  std::map<std::string, std::vector<ToolDefinition>, std::less<>> catalog;
  catalog["calculate"].push_back(calculator());
  std::set<std::string, std::less<>> unregistered;
  for (const auto& photon : photons.photons()) {
    if (photon.kind == "tool.unregistered") {
      const auto name = string_field(photon.payload, "name");
      if (!name.empty()) { unregistered.insert(name); catalog.erase(name); }
      continue;
    }
    if (photon.kind == "tool.registered") {
      auto definition = registered_tool(photon);
      if (!definition.name.empty()) {
        unregistered.erase(definition.name);
        catalog[definition.name].push_back(std::move(definition));
      }
    }
    if (photon.kind == "external.catalog-observed") {
      for (auto definition : external_tools(photon)) {
        if (unregistered.contains(definition.name)) continue;
        catalog[definition.name].push_back(std::move(definition));
      }
    }
  }
  return catalog;
}

cbor::Value model_schema(const ToolDefinition& tool) {
  return cbor::object({{"name", tool.name}, {"description", tool.description},
      {"arguments_schema", tool.act_schema}, {"input_schema", tool.input_schema},
      {"target", tool.target}, {"act_kind", tool.act_kind},
      {"risk", std::string(to_string(tool.risk))},
      {"source", tool.external ? "mcp" : "light-path"},
      {"schema_hash", sha256_hex(cbor::encode(tool.input_schema))}});
}

Result<Act> decode_call(const Photon& call, const ToolDefinition& tool,
                        const cbor::Value& arguments) {
  if (auto valid = validate_schema(arguments, tool.input_schema); !valid)
    return tl::unexpected(valid.error());
  cbor::Value parameters = arguments;
  if (tool.external) {
    parameters = cbor::object({{"connection_ref", tool.connection_ref},
        {"operation", "tools/call"}, {"schema_hash", tool.remote_schema_hash},
        {"arguments", cbor::object({{"name", tool.remote_name},
                                    {"arguments", arguments}})}});
  }
  const auto identity = sha256_hex(cbor::encode(cbor::object({
      {"source", call.id}, {"ray", call.ray}, {"kind", tool.act_kind},
      {"schema", tool.act_schema}, {"target", tool.target},
      {"parameters", parameters}, {"epoch", static_cast<std::int64_t>(call.epoch)}})));
  auto act = Act{.id = "act-" + identity.substr(0, 32), .ray = call.ray,
      .kind = tool.act_kind, .schema = tool.act_schema,
      .parameters = std::move(parameters), .target = tool.target,
      .epoch = call.epoch, .risk = tool.risk, .idempotency_key = identity};
  if (const auto call_id = string_field(call.payload, "call_id"); !call_id.empty())
    act.idempotency_key = sha256_hex(act.idempotency_key + call_id);
  return act;
}

struct CodeInstruction {
  std::string tool;
  cbor::Value arguments;
};

Result<std::vector<CodeInstruction>> parse_code_frame(const Photon& frame) {
  if (string_field(frame.payload, "mode") != "tokmon-act-v1")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "Code Mode accepts only the declarative tokmon-act-v1 language"));
  const auto source = string_field(frame.payload, "source");
  if (source.empty() || source.size() > 64u * 1024u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "Code Mode source is empty or too large"));
  std::vector<CodeInstruction> instructions;
  std::istringstream input(source);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.starts_with('#')) continue;
    const auto separator = line.find_first_of(" \t");
    if (separator == std::string::npos)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Code Mode line requires tool and JSON arguments"));
    auto name = line.substr(0, separator);
    auto json_text = line.substr(separator + 1u);
    const auto first = json_text.find_first_not_of(" \t");
    if (first == std::string::npos)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Code Mode arguments are missing"));
    json_text.erase(0, first);
    auto arguments = json::parse(json_text);
    if (!arguments || !arguments->is_map())
      return tl::unexpected(arguments ? make_error(ErrorCode::schema_mismatch,
          "Code Mode arguments must be a JSON object") : arguments.error());
    instructions.push_back(CodeInstruction{std::move(name), std::move(*arguments)});
  }
  if (instructions.empty() || instructions.size() > 256u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "Code Mode instruction count is outside 1..256"));
  return instructions;
}

}  // namespace

TechorLens::TechorLens() : LensBase(make_manifest("techor", "Techor / Tool 光能作动镜",
    {"model.tools", "act.candidates", "diagnostic.tool-decode"},
    {{"model.tool-call", "*"}, {"tool.result", "*"}, {"code.frame", "*"},
     {"tool.registered", "*"}, {"tool.unregistered", "*"},
     {"external.catalog-observed", "*"}, {"workflow.step-dispatched", "*"},
     {"workflow.compensation-dispatched", "*"}},
    {{"tool.decode", "tokmon.tool.decode.v1"}})) {}

Result<void> TechorLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  auto catalog = tool_catalog(photons);
  for (const auto& [name, definitions] : catalog) {
    if (definitions.size() == 1u) {
      if (auto result = surface.add("model.tools", name, model_schema(definitions.front()), 40);
          !result) return result;
    } else if (auto result = surface.add("diagnostic.tool-decode", name,
        cbor::object({{"status", "conflict"},
          {"definitions", static_cast<std::int64_t>(definitions.size())},
          {"reason", "tool name resolves to multiple targets"}}), 100); !result) return result;
  }

  if (const auto* code = photons.latest("code.frame")) {
    auto instructions = parse_code_frame(*code);
    if (!instructions)
      return surface.add("diagnostic.tool-decode", code->id,
          cbor::object({{"status", "rejected"}, {"mode", "tokmon-act-v1"},
                        {"reason", instructions.error().describe()},
                        {"direct_io", false}}), 100);
    for (std::size_t index = 0; index < instructions->size(); ++index) {
      const Photon* started = nullptr;
      const Photon* terminal = nullptr;
      for (const auto& photon : photons.photons()) {
        if (photon.sequence <= code->sequence || photon.kind != "act.started") continue;
        const auto* encoded = cbor::find(photon.payload, "act");
        const auto* parameters = encoded ? cbor::find(*encoded, "parameters") : nullptr;
        const auto* source = parameters ? cbor::find(*parameters, "_code_frame") : nullptr;
        const auto* encoded_index = parameters ? cbor::find(*parameters, "_code_index") : nullptr;
        if (source && source->as_string() == code->id && encoded_index &&
            encoded_index->as_integer() == static_cast<std::int64_t>(index)) started = &photon;
      }
      if (started) {
        for (const auto& photon : photons.photons())
          if (photon.sequence > started->sequence && photon.caused_by_act == started->caused_by_act &&
              (photon.kind == "act.completed" || photon.kind == "act.failed" ||
               photon.kind == "act.rejected")) terminal = &photon;
        if (!terminal) return {};
        if (terminal->kind != "act.completed")
          return surface.add("diagnostic.tool-decode", code->id,
              cbor::object({{"status", "stopped"},
                            {"failed_index", static_cast<std::int64_t>(index)},
                            {"terminal", terminal->kind}}), 100);
        continue;
      }
      const auto& instruction = (*instructions)[index];
      const auto found = catalog.find(instruction.tool);
      if (found == catalog.end() || found->second.size() != 1u)
        return surface.add("diagnostic.tool-decode", code->id,
            cbor::object({{"status", "rejected"}, {"tool", instruction.tool},
              {"index", static_cast<std::int64_t>(index)},
              {"reason", found == catalog.end() ? "unknown tool" : "ambiguous tool"}}), 100);
      auto decoded = decode_call(*code, found->second.front(), instruction.arguments);
      if (!decoded)
        return surface.add("diagnostic.tool-decode", code->id,
            cbor::object({{"status", "rejected"},
              {"index", static_cast<std::int64_t>(index)},
              {"reason", decoded.error().describe()}}), 100);
      (*decoded->parameters.as_map())["_code_frame"] = code->id;
      (*decoded->parameters.as_map())["_code_index"] = static_cast<std::int64_t>(index);
      decoded->id = make_id("act-code");
      decoded->idempotency_key = sha256_hex(code->id + ":" + std::to_string(index));
      if (auto added = surface.add("act.candidates", decoded->id,
          cbor::object({{"act", to_cbor(*decoded)}, {"source", code->id},
                        {"index", static_cast<std::int64_t>(index)},
                        {"direct_io", false}}), 70); !added) return added;
      return surface.propose(std::move(*decoded));
    }
    if (auto complete = surface.add("diagnostic.tool-decode", code->id,
        cbor::object({{"status", "complete"},
          {"instructions", static_cast<std::int64_t>(instructions->size())},
          {"direct_io", false}}), 60); !complete) return complete;
  }

  const auto* dispatched = photons.latest("workflow.step-dispatched");
  const auto* compensation = photons.latest("workflow.compensation-dispatched");
  if (compensation && (!dispatched || compensation->sequence > dispatched->sequence))
    dispatched = compensation;
  if (dispatched) {
    bool started = false;
    for (const auto& photon : photons.photons()) {
      if (photon.sequence <= dispatched->sequence || photon.kind != "act.started") continue;
      const auto* encoded = cbor::find(photon.payload, "act");
      const auto* parameters = encoded ? cbor::find(*encoded, "parameters") : nullptr;
      const auto* source = parameters ? cbor::find(*parameters, "_workflow_dispatch") : nullptr;
      if (source && source->as_string() == dispatched->id) { started = true; break; }
    }
    if (!started) {
      const auto* encoded = cbor::find(dispatched->payload, "act");
      const auto kind = encoded ? string_field(*encoded, "kind") : std::string{};
      const auto schema = encoded ? string_field(*encoded, "schema") : std::string{};
      const auto target = encoded ? string_field(*encoded, "target") : std::string{};
      const auto* supplied = encoded ? cbor::find(*encoded, "parameters") : nullptr;
      if (kind.empty() || schema.empty() || schema == "*" || !supplied || !supplied->is_map())
        return surface.add("diagnostic.tool-decode", dispatched->id,
            cbor::object({{"status", "rejected"},
                          {"reason", "workflow dispatch requires concrete kind/schema and object parameters"}}), 100);
      auto parameters = *supplied;
      (*parameters.as_map())["_workflow_node"] = string_field(dispatched->payload, "node_id");
      (*parameters.as_map())["_workflow_dispatch"] = dispatched->id;
      (*parameters.as_map())["_workflow_compensation"] =
          dispatched->kind == "workflow.compensation-dispatched";
      auto act = propose(*dispatched, kind, schema, target, std::move(parameters),
                         encoded ? risk_field(*encoded) : RiskClass::reversible);
      if (const auto* timeout = cbor::find(*encoded, "timeout_ms"))
        act.timeout = std::chrono::milliseconds(std::clamp<std::int64_t>(
            timeout->as_integer(30'000), 1, 86'400'000));
      if (auto added = surface.add("act.candidates", act.id, to_cbor(act), 60); !added)
        return added;
      return surface.propose(std::move(act));
    }
  }

  const auto* call = photons.latest("model.tool-call");
  const auto* result = photons.latest("tool.result");
  if (!call || (result && result->sequence > call->sequence)) return {};
  const auto name = string_field(call->payload, "tool");
  const auto found = catalog.find(name);
  if (name.empty() || found == catalog.end() || found->second.size() != 1u)
    return surface.add("diagnostic.tool-decode", call->id,
        cbor::object({{"status", "rejected"},
          {"reason", found == catalog.end() ? "unknown tool" : "ambiguous tool"},
          {"tool", name}}), 100);
  const auto& definition = found->second.front();
  if (const auto declared_schema = string_field(call->payload, "schema");
      !declared_schema.empty() && declared_schema != definition.act_schema)
    return surface.add("diagnostic.tool-decode", call->id,
        cbor::object({{"status", "rejected"}, {"reason", "schema drift"},
          {"declared", declared_schema}, {"current", definition.act_schema}}), 100);
  const auto* arguments = cbor::find(call->payload, "arguments");
  if (!arguments || !arguments->is_map())
    return surface.add("diagnostic.tool-decode", call->id,
        cbor::object({{"status", "rejected"},
                      {"reason", "tool arguments must be an object"}}), 100);
  auto decoded = decode_call(*call, definition, *arguments);
  if (!decoded)
    return surface.add("diagnostic.tool-decode", call->id,
        cbor::object({{"status", "rejected"}, {"reason", decoded.error().describe()}}), 100);
  if (auto added = surface.add("act.candidates", decoded->id, to_cbor(*decoded), 50); !added)
    return added;
  return surface.propose(std::move(*decoded));
}

Result<RefractionResult> TechorLens::refract(const PhotonWindow&, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto* call = cbor::find(act.parameters, "call");
  if (call && !call->is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "tool.decode call must be an object"));
  return emit(beam, "tool.decoded", "tokmon.tool.decoded.v1",
      cbor::object({{"normalized", call ? *call : act.parameters},
          {"epoch", static_cast<std::int64_t>(act.epoch)},
          {"schema_validated", true}, {"unique_target", true}}));
}

}  // namespace tokmon::builtin
