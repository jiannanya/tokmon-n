#include "lenses/clotho/clotho_lens.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace {

struct WorkflowNode {
  std::string id;
  tokmon::cbor::Value parameters;
  std::vector<std::string> dependencies;
};

tokmon::Result<std::vector<WorkflowNode>> workflow_nodes(
    const tokmon::Photon& definition) {
  const auto* nodes_field = tokmon::cbor::find(definition.payload, "nodes");
  if (!nodes_field || !nodes_field->as_array() || nodes_field->as_array()->empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                             "workflow definition requires nodes"));
  std::vector<WorkflowNode> nodes;
  std::set<std::string> ids;
  for (const auto& item : *nodes_field->as_array()) {
    const auto* id = tokmon::cbor::find(item, "id");
    if (!id || id->as_string().empty() || !ids.insert(std::string(id->as_string())).second)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                               "workflow node ids must be unique"));
    WorkflowNode node{.id = std::string(id->as_string()), .parameters = item};
    if (const auto* dependencies = tokmon::cbor::find(item, "depends_on")) {
      if (!dependencies->as_array())
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                                 "depends_on must be an array"));
      for (const auto& dependency : *dependencies->as_array())
        node.dependencies.emplace_back(dependency.as_string());
    }
    nodes.push_back(std::move(node));
  }
  for (const auto& node : nodes)
    for (const auto& dependency : node.dependencies)
      if (!ids.contains(dependency) || dependency == node.id)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                                 "workflow dependency is invalid"));
  std::set<std::string> resolved;
  while (resolved.size() != nodes.size()) {
    bool progress = false;
    for (const auto& node : nodes) {
      if (resolved.contains(node.id)) continue;
      if (std::all_of(node.dependencies.begin(), node.dependencies.end(),
                      [&resolved](const auto& id) { return resolved.contains(id); })) {
        resolved.insert(node.id); progress = true;
      }
    }
    if (!progress)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                               "workflow graph contains a cycle"));
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; });
  return nodes;
}

std::set<std::string> completed_nodes(const tokmon::PhotonWindow& photons) {
  std::set<std::string> completed;
  for (const auto& photon : photons.photons()) {
    if (photon.kind != "workflow.step-completed") continue;
    if (const auto* node = tokmon::cbor::find(photon.payload, "node_id"))
      completed.emplace(node->as_string());
  }
  return completed;
}

}  // namespace

namespace tokmon::builtin {

ClothoLens::ClothoLens() : LensBase(make_manifest("clotho", "Clotho / 显式工作流光栅",
    {"workflow.graph", "workflow.status"},
    {{"workflow.defined", "*"}, {"workflow.started", "*"},
     {"workflow.step-*", "*"}, {"workflow.join-*", "*"}},
    {{"workflow.step", "tokmon.workflow.step.v1"},
     {"workflow.retry", "tokmon.workflow.retry.v1"},
     {"workflow.cancel", "tokmon.workflow.cancel.v1"}})) {}

Result<void> ClothoLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* definition = photons.latest("workflow.defined");
  if (!definition) return {};
  auto nodes = workflow_nodes(*definition);
  if (!nodes) return tl::unexpected(nodes.error());
  const auto completed = completed_nodes(photons);
  if (auto result = surface.add("workflow.graph", definition->id, definition->payload, 10);
      !result) return result;
  const bool done = completed.size() == nodes->size();
  if (auto result = identify(surface, "workflow.status", cbor::object({
      {"state", done ? "completed" : "active"}, {"definition_hash", definition->hash},
      {"completed", static_cast<std::int64_t>(completed.size())},
      {"total", static_cast<std::int64_t>(nodes->size())}})); !result) return result;
  if (done) return {};
  const auto ready_node = std::find_if(nodes->begin(), nodes->end(), [&](const auto& node) {
    return !completed.contains(node.id) &&
        std::all_of(node.dependencies.begin(), node.dependencies.end(),
                    [&completed](const auto& id) { return completed.contains(id); });
  });
  if (ready_node == nodes->end()) return {};
  auto act = propose(*definition, "workflow.step", "tokmon.workflow.step.v1",
      manifest().id, cbor::object({{"node_id", ready_node->id},
                                   {"definition_hash", definition->hash},
                                   {"node", ready_node->parameters}}),
      RiskClass::reversible);
  return surface.propose(std::move(act));
}

Result<RefractionResult> ClothoLens::refract(const PhotonWindow& photons, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const std::string kind = act.kind == "workflow.cancel" ? "workflow.cancelled" :
      act.kind == "workflow.retry" ? "workflow.step-retried" :
                                     "workflow.step-completed";
  if (act.kind == "workflow.step" &&
      (!cbor::find(act.parameters, "node_id") ||
       cbor::find(act.parameters, "node_id")->as_string().empty()))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "workflow.step requires node_id"));
  auto payload = cbor::object({{"step", act.parameters}, {"act_id", act.id}});
  if (const auto* node = cbor::find(act.parameters, "node_id"))
    (*payload.as_map())["node_id"] = std::string(node->as_string());
  auto result = emit(beam, kind, "tokmon.workflow.result.v1", std::move(payload));
  if (!result) return result;
  (void)photons;
  return result;
}

}  // namespace tokmon::builtin
