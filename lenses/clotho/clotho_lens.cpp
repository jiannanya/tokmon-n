#include "lenses/clotho/clotho_lens.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "tokmon/hash.hpp"

namespace tokmon::builtin {
namespace {

struct WorkflowNode {
  std::string id;
  cbor::Value definition;
  std::vector<std::string> dependencies;
};

cbor::Value yaml_value(const YAML::Node& node) {
  if (!node || node.IsNull()) return nullptr;
  if (node.IsSequence()) {
    cbor::Value::Array result;
    for (const auto& item : node) result.push_back(yaml_value(item));
    return result;
  }
  if (node.IsMap()) {
    cbor::Value::Map result;
    for (const auto& item : node)
      result[item.first.as<std::string>()] = yaml_value(item.second);
    return result;
  }
  const auto text = node.Scalar();
  if (text == "true") return true;
  if (text == "false") return false;
  try {
    std::size_t used = 0;
    const auto integer = std::stoll(text, &used);
    if (used == text.size()) return static_cast<std::int64_t>(integer);
  } catch (const std::exception&) {}
  try {
    std::size_t used = 0;
    const auto number = std::stod(text, &used);
    if (used == text.size()) return number;
  } catch (const std::exception&) {}
  return text;
}

Result<cbor::Value> parse_definition(const std::string& yaml) {
  try {
    const auto root = YAML::Load(yaml);
    if (!root.IsMap() || !root["api"] || root["api"].as<std::string>() != "tokmon.workflow/v1" ||
        !root["name"] || !root["nodes"] || !root["nodes"].IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "workflow YAML requires api tokmon.workflow/v1, name and nodes"));
    const auto templates = root["templates"];
    if (templates && !templates.IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workflow templates must be a map"));
    cbor::Value::Array nodes;
    for (const auto& entry : root["nodes"]) {
      auto value = cbor::Value(cbor::Value::Map{});
      if (entry.second["uses"]) {
        const auto name = entry.second["uses"].as<std::string>();
        if (!templates || !templates[name] || !templates[name].IsMap())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "workflow node references an unknown template"));
        value = yaml_value(templates[name]);
      }
      auto override = yaml_value(entry.second);
      if (!override.is_map())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "workflow node must be a map"));
      for (const auto& [key, item] : *override.as_map())
        if (key != "uses") (*value.as_map())[key] = item;
      if (!value.is_map()) value = cbor::Value::Map{};
      const auto id = entry.first.as<std::string>();
      (*value.as_map())["id"] = id;
      if (const auto* needs = cbor::find(value, "needs"))
        (*value.as_map())["depends_on"] = *needs;
      else if (!cbor::find(value, "depends_on"))
        (*value.as_map())["depends_on"] = cbor::Value::Array{};
      const auto* each = cbor::find(value, "for_each");
      if (each) {
        if (!each->as_array() || each->as_array()->empty() || each->as_array()->size() > 1024u)
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              "workflow for_each must be a non-empty array with at most 1024 items"));
        cbor::Value::Array expanded_ids;
        for (std::size_t index = 0; index < each->as_array()->size(); ++index) {
          auto expanded = value;
          expanded.as_map()->erase("for_each");
          std::ostringstream suffix;
          suffix << id << '[' << std::setw(4) << std::setfill('0') << index << ']';
          (*expanded.as_map())["id"] = suffix.str();
          (*expanded.as_map())["fanout_parent"] = id;
          (*expanded.as_map())["fanout_index"] = static_cast<std::int64_t>(index);
          (*expanded.as_map())["item"] = (*each->as_array())[index];
          expanded_ids.emplace_back(suffix.str());
          nodes.push_back(std::move(expanded));
        }
        nodes.push_back(cbor::object({{"id", id}, {"depends_on", std::move(expanded_ids)},
            {"join", true}, {"fan_in", true}}));
      } else {
        nodes.push_back(std::move(value));
      }
    }
    return cbor::object({{"api", "tokmon.workflow/v1"},
        {"name", root["name"].as<std::string>()}, {"nodes", std::move(nodes)},
        {"inputs", root["inputs"] ? yaml_value(root["inputs"]) : cbor::Value::Map{}},
        {"failure", root["failure"] ? yaml_value(root["failure"]) : cbor::Value("stop")},
        {"max_parallel", root["max_parallel"] ? yaml_value(root["max_parallel"]) : cbor::Value(4)},
        {"groups", root["groups"] ? yaml_value(root["groups"]) : cbor::Value::Map{}},
        {"permissions", root["permissions"] ? yaml_value(root["permissions"])
                                               : cbor::Value::Array{}}});
  } catch (const YAML::Exception& error) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "invalid workflow YAML: " + std::string(error.what())));
  }
}

Result<std::vector<WorkflowNode>> workflow_nodes(const Photon& definition) {
  const auto* nodes_field = cbor::find(definition.payload, "nodes");
  if (!nodes_field || !nodes_field->as_array() || nodes_field->as_array()->empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "workflow definition requires nodes"));
  std::vector<WorkflowNode> nodes;
  std::set<std::string> ids;
  const auto* permissions = cbor::find(definition.payload, "permissions");
  const auto permission_allows = [&](const std::string_view kind) {
    if (!permissions || !permissions->as_array() || permissions->as_array()->empty()) return true;
    return std::any_of(permissions->as_array()->begin(), permissions->as_array()->end(),
        [&](const cbor::Value& value) {
          const auto pattern = value.as_string();
          return pattern == "*" || pattern == kind ||
              (pattern.ends_with('*') && kind.starts_with(pattern.substr(0, pattern.size() - 1u)));
        });
  };
  for (const auto& item : *nodes_field->as_array()) {
    const auto* id = cbor::find(item, "id");
    if (!id || id->as_string().empty() || !ids.insert(std::string(id->as_string())).second)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workflow node ids must be unique"));
    WorkflowNode node{std::string(id->as_string()), item, {}};
    const auto* dependencies = cbor::find(item, "depends_on");
    if (!dependencies) dependencies = cbor::find(item, "needs");
    if (dependencies) {
      if (!dependencies->as_array())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "workflow dependencies must be an array"));
      for (const auto& dependency : *dependencies->as_array()) {
        if (!std::holds_alternative<std::string>(dependency.data))
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "workflow dependency must be a string"));
        node.dependencies.emplace_back(dependency.as_string());
      }
    }
    if (const auto* act = cbor::find(item, "act")) {
      if (act->as_string().empty() || !permission_allows(act->as_string()))
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            "workflow node Act exceeds the workflow permission upper bound"));
      const auto* schema = cbor::find(item, "schema");
      if (!schema || schema->as_string().empty() || schema->as_string() == "*")
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "workflow Act node requires a concrete schema"));
    }
    if (const auto* timeout = cbor::find(item, "timeout_ms");
        timeout && (timeout->as_integer() <= 0 || timeout->as_integer() > 86'400'000))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workflow node timeout is outside safe bounds"));
    if (const auto* policy = cbor::find(item, "on_failure")) {
      const auto text = policy->as_string();
      if (text != "retry" && text != "continue" && text != "stop" && text != "compensate")
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "unknown workflow failure policy"));
    }
    if (const auto* retry = cbor::find(item, "retry")) {
      const auto* attempts = cbor::find(*retry, "max_attempts");
      if (!retry->is_map() || (attempts &&
          (attempts->as_integer() < 1 || attempts->as_integer() > 100)))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "workflow retry policy is invalid"));
    }
    nodes.push_back(std::move(node));
  }
  for (const auto& node : nodes)
    for (const auto& dependency : node.dependencies)
      if (!ids.contains(dependency) || dependency == node.id)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "workflow dependency is invalid"));
  std::set<std::string> resolved;
  while (resolved.size() != nodes.size()) {
    bool progress = false;
    for (const auto& node : nodes)
      if (!resolved.contains(node.id) && std::ranges::all_of(node.dependencies,
          [&](const auto& id) { return resolved.contains(id); })) {
        resolved.insert(node.id); progress = true;
      }
    if (!progress) return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                                     "workflow graph contains a cycle"));
  }
  std::ranges::sort(nodes, {}, &WorkflowNode::id);
  return nodes;
}

struct WorkflowState {
  std::set<std::string> succeeded;
  std::set<std::string> skipped;
  std::set<std::string> failed;
  std::map<std::string, std::int64_t, std::less<>> attempts;
  std::map<std::string, std::string, std::less<>> inflight;
  std::map<std::string, Photon, std::less<>> terminal_acts;
  std::map<std::string, Photon, std::less<>> compensation_terminal;
  std::map<std::string, cbor::Value, std::less<>> outputs;
  std::set<std::string> compensated;
  bool paused{false};
  bool cancelled{false};
};

WorkflowState state(const PhotonWindow& photons) {
  WorkflowState result;
  std::map<std::string, std::pair<std::string, bool>, std::less<>> act_nodes;
  for (const auto& photon : photons.photons()) {
    if (photon.kind == "workflow.paused") result.paused = true;
    if (photon.kind == "workflow.resumed") result.paused = false;
    if (photon.kind == "workflow.cancelled") result.cancelled = true;
    const auto* node = cbor::find(photon.payload, "node_id");
    if (photon.kind == "workflow.step-completed" && node) result.succeeded.emplace(node->as_string());
    if (photon.kind == "workflow.step-skipped" && node) result.skipped.emplace(node->as_string());
    if (photon.kind == "workflow.step-failed" && node) result.failed.emplace(node->as_string());
    if (photon.kind == "workflow.step-completed" && node)
      if (const auto* output = cbor::find(photon.payload, "output"))
        result.outputs[std::string(node->as_string())] = *output;
    if (photon.kind == "workflow.compensation-completed" && node)
      result.compensated.emplace(node->as_string());
    if (photon.kind == "workflow.step-retry-requested") {
      const auto* request = cbor::find(photon.payload, "request");
      const auto* retried = request ? cbor::find(*request, "node_id") : node;
      if (retried) {
        result.failed.erase(std::string(retried->as_string()));
        result.terminal_acts.erase(std::string(retried->as_string()));
        result.inflight.erase(std::string(retried->as_string()));
      }
    }
    if (photon.kind == "workflow.step-dispatched" && node) {
      result.failed.erase(std::string(node->as_string()));
      result.terminal_acts.erase(std::string(node->as_string()));
      result.attempts[std::string(node->as_string())]++;
      result.inflight[std::string(node->as_string())] = photon.id;
    }
    if (photon.kind == "act.started") {
      const auto* encoded = cbor::find(photon.payload, "act");
      const auto* parameters = encoded ? cbor::find(*encoded, "parameters") : nullptr;
      const auto* workflow_node = parameters ? cbor::find(*parameters, "_workflow_node") : nullptr;
      if (workflow_node) act_nodes[photon.caused_by_act] = {
          std::string(workflow_node->as_string()),
          parameters && cbor::find(*parameters, "_workflow_compensation") &&
              cbor::find(*parameters, "_workflow_compensation")->as_bool()};
    }
    if ((photon.kind == "act.completed" || photon.kind == "act.failed") &&
        act_nodes.contains(photon.caused_by_act)) {
      const auto& [node_id, compensation] = act_nodes[photon.caused_by_act];
      if (compensation) result.compensation_terminal[node_id] = photon;
      else result.terminal_acts[node_id] = photon;
    }
  }
  for (const auto& node : result.succeeded) result.inflight.erase(node);
  for (const auto& node : result.skipped) result.inflight.erase(node);
  for (const auto& node : result.failed) result.inflight.erase(node);
  return result;
}

bool scalar_equal(const cbor::Value& value, std::string expected) {
  if (expected.size() >= 2u && expected.front() == '\'' && expected.back() == '\'')
    expected = expected.substr(1, expected.size() - 2u);
  if (std::holds_alternative<std::string>(value.data)) return value.as_string() == expected;
  if (std::holds_alternative<bool>(value.data)) return (value.as_bool() ? "true" : "false") == expected;
  if (std::holds_alternative<std::int64_t>(value.data)) return std::to_string(value.as_integer()) == expected;
  return false;
}

bool condition(const cbor::Value& node, const cbor::Value& definition,
               const WorkflowState& current) {
  const auto* when = cbor::find(node, "when");
  if (!when || when->as_string().empty()) return true;
  const std::string expression(when->as_string());
  constexpr std::string_view prefix = "steps.";
  constexpr std::string_view status_marker = ".status == ";
  if (expression.starts_with(prefix)) {
    const auto marker = expression.find(status_marker);
    if (marker == std::string::npos) return false;
    const auto id = expression.substr(prefix.size(), marker - prefix.size());
    auto expected = expression.substr(marker + status_marker.size());
    if (expected.size() >= 2u && expected.front() == '\'' && expected.back() == '\'')
      expected = expected.substr(1, expected.size() - 2u);
    if (expected == "succeeded") return current.succeeded.contains(id);
    if (expected == "failed") return current.failed.contains(id);
    if (expected == "skipped") return current.skipped.contains(id);
    return false;
  }
  constexpr std::string_view input_prefix = "inputs.";
  constexpr std::string_view equals = " == ";
  if (expression.starts_with(input_prefix)) {
    const auto marker = expression.find(equals);
    const auto* inputs = cbor::find(definition, "inputs");
    if (marker == std::string::npos || !inputs) return false;
    const auto name = expression.substr(input_prefix.size(), marker - input_prefix.size());
    const auto* value = cbor::find(*inputs, name);
    return value && scalar_equal(*value, expression.substr(marker + equals.size()));
  }
  return expression == "true";
}

std::string failure_policy(const cbor::Value& node, const cbor::Value& definition) {
  if (const auto* policy = cbor::find(node, "on_failure")) return std::string(policy->as_string());
  if (const auto* policy = cbor::find(definition, "failure")) return std::string(policy->as_string());
  return "stop";
}

const cbor::Value* value_path(const cbor::Value& root, std::string_view path) {
  const cbor::Value* current = &root;
  while (!path.empty()) {
    const auto separator = path.find('.');
    const auto component = path.substr(0, separator);
    current = cbor::find(*current, component);
    if (!current) return nullptr;
    if (separator == std::string_view::npos) break;
    path.remove_prefix(separator + 1u);
  }
  return current;
}

cbor::Value resolve_templates(const cbor::Value& value, const cbor::Value& node,
                              const cbor::Value& definition,
                              const WorkflowState& current) {
  if (const auto* map = value.as_map()) {
    cbor::Value::Map resolved;
    for (const auto& [key, item] : *map)
      resolved[key] = resolve_templates(item, node, definition, current);
    return resolved;
  }
  if (const auto* array = value.as_array()) {
    cbor::Value::Array resolved;
    for (const auto& item : *array)
      resolved.push_back(resolve_templates(item, node, definition, current));
    return resolved;
  }
  if (!std::holds_alternative<std::string>(value.data)) return value;
  const auto text = value.as_string();
  if (!text.starts_with("${") || !text.ends_with('}')) return value;
  auto expression = text.substr(2u, text.size() - 3u);
  if (expression.starts_with("{ ") && expression.ends_with(" }"))
    expression = expression.substr(2u, expression.size() - 4u);
  if (expression == "item") {
    if (const auto* item = cbor::find(node, "item")) return *item;
    return nullptr;
  }
  if (expression.starts_with("inputs.")) {
    const auto* inputs = cbor::find(definition, "inputs");
    const auto* found = inputs ? value_path(*inputs, expression.substr(7u)) : nullptr;
    return found ? *found : cbor::Value(nullptr);
  }
  if (expression.starts_with("steps.")) {
    auto remainder = expression.substr(6u);
    const auto marker = remainder.find(".output");
    if (marker == std::string_view::npos) return nullptr;
    const auto id = std::string(remainder.substr(0, marker));
    const auto output = current.outputs.find(id);
    if (output == current.outputs.end()) return nullptr;
    remainder.remove_prefix(marker + 7u);
    if (!remainder.empty() && remainder.front() == '.') remainder.remove_prefix(1u);
    const auto* found = remainder.empty() ? &output->second
                                           : value_path(output->second, remainder);
    return found ? *found : cbor::Value(nullptr);
  }
  return nullptr;
}

}  // namespace

ClothoLens::ClothoLens() : LensBase(make_manifest("clotho", "Clotho / 显式工作流光栅",
    {"workflow.graph", "workflow.status"},
    {{"workflow.*", "*"}, {"act.started", "*"}, {"act.completed", "*"},
     {"act.failed", "*"}},
    {{"workflow.define", "tokmon.workflow.define.v1"},
     {"workflow.step", "tokmon.workflow.step.v1"},
     {"workflow.record-step", "tokmon.workflow.record-step.v1"},
     {"workflow.compensate", "tokmon.workflow.compensate.v1"},
     {"workflow.record-compensation", "tokmon.workflow.record-compensation.v1"},
     {"workflow.retry", "tokmon.workflow.retry.v1"},
     {"workflow.pause", "tokmon.workflow.pause.v1"},
     {"workflow.resume", "tokmon.workflow.resume.v1"},
     {"workflow.cancel", "tokmon.workflow.cancel.v1"}})) {}

Result<void> ClothoLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* definition = photons.latest("workflow.defined");
  if (!definition) return {};
  auto nodes = workflow_nodes(*definition);
  if (!nodes) return tl::unexpected(nodes.error());
  const auto current = state(photons);
  std::map<std::string, const WorkflowNode*, std::less<>> by_id;
  for (const auto& node : *nodes) by_id[node.id] = &node;
  std::size_t continued_failures = 0;
  for (const auto& id : current.failed)
    if (const auto found = by_id.find(id); found != by_id.end() &&
        (failure_policy(found->second->definition, definition->payload) == "continue" ||
         (failure_policy(found->second->definition, definition->payload) == "compensate" &&
          current.compensated.contains(id)))) ++continued_failures;
  const auto finished = current.succeeded.size() + current.skipped.size() + continued_failures;
  const auto status = current.cancelled ? "cancelled" : current.paused ? "paused" :
      finished == nodes->size() ? "completed" : !current.failed.empty() ? "failed" : "active";
  if (auto result = surface.add("workflow.graph", definition->id, definition->payload, 10);
      !result) return result;
  if (auto result = identify(surface, "workflow.status", cbor::object({
      {"state", status}, {"definition_hash", definition->hash},
      {"completed", static_cast<std::int64_t>(finished)},
      {"inflight", static_cast<std::int64_t>(current.inflight.size())},
      {"total", static_cast<std::int64_t>(nodes->size())}})); !result) return result;
  if (current.cancelled || current.paused || finished == nodes->size()) return {};

  for (const auto& [node, terminal] : current.terminal_acts) {
    if (current.succeeded.contains(node) || current.skipped.contains(node) ||
        current.failed.contains(node)) continue;
    auto act = propose(*definition, "workflow.record-step", "tokmon.workflow.record-step.v1",
        manifest().id, cbor::object({{"node_id", node},
          {"status", terminal.kind == "act.completed" ? "succeeded" : "failed"},
          {"output", terminal.payload}}),
        RiskClass::reversible);
    return surface.propose(std::move(act));
  }
  for (const auto& [node, terminal] : current.compensation_terminal) {
    if (current.compensated.contains(node)) continue;
    auto act = propose(*definition, "workflow.record-compensation",
        "tokmon.workflow.record-compensation.v1", manifest().id,
        cbor::object({{"node_id", node},
          {"status", terminal.kind == "act.completed" ? "succeeded" : "failed"},
          {"output", terminal.payload}}), RiskClass::reversible);
    return surface.propose(std::move(act));
  }

  for (const auto& failed : current.failed) {
    const auto found = by_id.find(failed);
    if (found == by_id.end()) continue;
    const auto& node = *found->second;
    const auto* retry = cbor::find(node.definition, "retry");
    const auto max_attempts = retry && cbor::find(*retry, "max_attempts")
        ? cbor::find(*retry, "max_attempts")->as_integer(1) : 1;
    const auto attempts = current.attempts.contains(node.id) ? current.attempts.at(node.id) : 0;
    if (attempts < max_attempts) continue;
    const auto policy = failure_policy(node.definition, definition->payload);
    if (policy == "compensate" && !current.compensated.contains(failed)) {
      const auto* compensation = cbor::find(node.definition, "compensate");
      if (!compensation || !compensation->is_map())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "compensating workflow node requires a compensate Act definition"));
      auto act = propose(*definition, "workflow.compensate", "tokmon.workflow.compensate.v1",
          manifest().id, cbor::object({{"node_id", failed}, {"act", *compensation},
            {"failed_attempts", attempts}}), RiskClass::reversible);
      return surface.propose(std::move(act));
    }
    if (policy == "stop") return {};
  }

  const auto max_parallel = static_cast<std::size_t>(std::max<std::int64_t>(1,
      cbor::find(definition->payload, "max_parallel")
          ? cbor::find(definition->payload, "max_parallel")->as_integer(4) : 4));
  auto available = max_parallel > current.inflight.size() ? max_parallel - current.inflight.size() : 0;
  std::map<std::string, std::size_t, std::less<>> group_inflight;
  for (const auto& [id, _] : current.inflight)
    if (const auto found = by_id.find(id); found != by_id.end())
      if (const auto* group = cbor::find(found->second->definition, "group");
          group && !group->as_string().empty()) ++group_inflight[std::string(group->as_string())];
  const auto group_limit = [&](const std::string_view group) {
    const auto* groups = cbor::find(definition->payload, "groups");
    const auto* encoded = groups ? cbor::find(*groups, group) : nullptr;
    if (!encoded) return max_parallel;
    if (std::holds_alternative<std::int64_t>(encoded->data))
      return static_cast<std::size_t>(std::max<std::int64_t>(1, encoded->as_integer()));
    const auto* value = cbor::find(*encoded, "max_parallel");
    return static_cast<std::size_t>(std::max<std::int64_t>(1,
        value ? value->as_integer(1) : 1));
  };
  for (const auto& node : *nodes) {
    if (available == 0) break;
    const auto* retry = cbor::find(node.definition, "retry");
    const auto max_attempts = retry && cbor::find(*retry, "max_attempts")
        ? cbor::find(*retry, "max_attempts")->as_integer(1) : 1;
    const auto attempts = current.attempts.contains(node.id) ? current.attempts.at(node.id) : 0;
    const bool retryable_failure = current.failed.contains(node.id) && attempts < max_attempts;
    if (current.succeeded.contains(node.id) || current.skipped.contains(node.id) ||
        (current.failed.contains(node.id) && !retryable_failure) || current.inflight.contains(node.id) ||
        !std::ranges::all_of(node.dependencies, [&](const auto& dependency) {
          if (current.succeeded.contains(dependency) || current.skipped.contains(dependency))
            return true;
          const auto found = by_id.find(dependency);
          if (found == by_id.end() || !current.failed.contains(dependency)) return false;
          const auto policy = failure_policy(found->second->definition, definition->payload);
          return policy == "continue" ||
              (policy == "compensate" && current.compensated.contains(dependency));
        })) continue;
    const auto group = cbor::find(node.definition, "group")
        ? std::string(cbor::find(node.definition, "group")->as_string()) : std::string{};
    if (!group.empty() && group_inflight[group] >= group_limit(group)) continue;
    auto parameters = cbor::object({{"node_id", node.id}, {"definition_hash", definition->hash},
        {"node", node.definition}, {"attempt", attempts + 1},
        {"skip", !condition(node.definition, definition->payload, current)}});
    auto act = propose(*definition, "workflow.step", "tokmon.workflow.step.v1",
        manifest().id, std::move(parameters), RiskClass::reversible);
    if (auto result = surface.propose(std::move(act)); !result) return result;
    if (!group.empty()) ++group_inflight[group];
    --available;
  }
  return {};
}

Result<RefractionResult> ClothoLens::refract(const PhotonWindow& photons, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "workflow.define") {
    const auto* yaml = cbor::find(act.parameters, "yaml");
    if (!yaml || yaml->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workflow.define requires YAML text"));
    auto definition = parse_definition(std::string(yaml->as_string()));
    if (!definition) return tl::unexpected(definition.error());
    Photon synthetic{.payload = *definition};
    if (auto valid = workflow_nodes(synthetic); !valid) return tl::unexpected(valid.error());
    const auto hash = sha256_hex(cbor::encode(*definition));
    (*definition->as_map())["definition_hash"] = hash;
    return emit(beam, "workflow.defined", "tokmon.workflow.definition.v1", *definition);
  }
  if (act.kind == "workflow.pause" || act.kind == "workflow.resume" ||
      act.kind == "workflow.cancel") {
    const auto kind = act.kind == "workflow.pause" ? "workflow.paused" :
        act.kind == "workflow.resume" ? "workflow.resumed" : "workflow.cancelled";
    return emit(beam, kind, "tokmon.workflow.control.v1",
                cbor::object({{"request", act.parameters}, {"history_deleted", false}}));
  }
  if (act.kind == "workflow.retry")
    return emit(beam, "workflow.step-retry-requested", "tokmon.workflow.control.v1",
                cbor::object({{"request", act.parameters}, {"new_attempt", true}}));
  const auto node_id = cbor::find(act.parameters, "node_id");
  if (!node_id || node_id->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "workflow step requires node_id"));
  if (act.kind == "workflow.record-compensation") {
    const auto success = cbor::find(act.parameters, "status") &&
        cbor::find(act.parameters, "status")->as_string() == "succeeded";
    return emit(beam, success ? "workflow.compensation-completed"
                              : "workflow.compensation-failed",
        "tokmon.workflow.compensation-result.v1", cbor::object({{"node_id", *node_id},
          {"status", success ? "succeeded" : "failed"},
          {"output", cbor::find(act.parameters, "output")
              ? *cbor::find(act.parameters, "output") : cbor::Value(nullptr)}}));
  }
  if (act.kind == "workflow.compensate") {
    const auto* compensation = cbor::find(act.parameters, "act");
    if (!compensation || !compensation->is_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workflow.compensate requires an Act definition"));
    return emit(beam, "workflow.compensation-dispatched", "tokmon.workflow.dispatch.v1",
        cbor::object({{"node_id", *node_id}, {"act", *compensation},
          {"compensation", true}}));
  }
  if (act.kind == "workflow.record-step") {
    const auto success = cbor::find(act.parameters, "status") &&
        cbor::find(act.parameters, "status")->as_string() == "succeeded";
    return emit(beam, success ? "workflow.step-completed" : "workflow.step-failed",
        "tokmon.workflow.result.v1", cbor::object({{"node_id", *node_id},
          {"status", success ? "succeeded" : "failed"},
          {"output", cbor::find(act.parameters, "output")
              ? *cbor::find(act.parameters, "output") : cbor::Value(nullptr)},
          {"attempt_preserved", true}}));
  }
  if (cbor::find(act.parameters, "skip") && cbor::find(act.parameters, "skip")->as_bool())
    return emit(beam, "workflow.step-skipped", "tokmon.workflow.result.v1",
                cbor::object({{"node_id", *node_id}, {"condition", false}}));
  const auto* node = cbor::find(act.parameters, "node");
  const auto* act_kind = node ? cbor::find(*node, "act") : nullptr;
  const auto* child = node ? cbor::find(*node, "child") : nullptr;
  if ((!act_kind && !child) || (cbor::find(*node, "join") && cbor::find(*node, "join")->as_bool())) {
    cbor::Value::Array joined;
    if (const auto* dependencies = cbor::find(*node, "depends_on");
        dependencies && dependencies->as_array()) {
      const auto current = state(photons);
      for (const auto& dependency : *dependencies->as_array()) {
        const auto found = current.outputs.find(std::string(dependency.as_string()));
        joined.push_back(cbor::object({{"node_id", std::string(dependency.as_string())},
          {"output", found == current.outputs.end() ? cbor::Value(nullptr)
                                                     : found->second}}));
      }
    }
    return emit(beam, "workflow.step-completed", "tokmon.workflow.result.v1",
                cbor::object({{"node_id", *node_id}, {"status", "succeeded"},
                              {"output", std::move(joined)}, {"empty_node", true}}));
  }
  const auto* definition_photon = photons.latest("workflow.defined");
  const auto current = state(photons);
  const auto parameters = child ? resolve_templates(*child, *node,
          definition_photon ? definition_photon->payload : cbor::Value(cbor::Value::Map{}),
          current)
      : resolve_templates(cbor::find(*node, "with") ? *cbor::find(*node, "with")
                                                     : cbor::Value(cbor::Value::Map{}),
          *node, definition_photon ? definition_photon->payload
                                   : cbor::Value(cbor::Value::Map{}), current);
  auto dispatch = cbor::object({{"kind", child ? "child.spawn" : std::string(act_kind->as_string())},
      {"schema", child ? "tokmon.child.spawn.v1" :
          (cbor::find(*node, "schema") ? *cbor::find(*node, "schema") : cbor::Value("*"))},
      {"target", child ? "org.tokmon.lens.aya" :
          (cbor::find(*node, "target") ? *cbor::find(*node, "target") : cbor::Value(""))},
      {"parameters", parameters},
      {"risk", cbor::find(*node, "risk") ? *cbor::find(*node, "risk") : cbor::Value("reversible")},
      {"timeout_ms", cbor::find(*node, "timeout_ms")
          ? *cbor::find(*node, "timeout_ms") : cbor::Value(30'000)}});
  return emit(beam, "workflow.step-dispatched", "tokmon.workflow.dispatch.v1",
      cbor::object({{"node_id", *node_id}, {"attempt", cbor::find(act.parameters, "attempt")
          ? *cbor::find(act.parameters, "attempt") : cbor::Value(1)}, {"act", std::move(dispatch)}}));
}

}  // namespace tokmon::builtin
