#include "tokmon/optical_assembly.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stack>
#include <unordered_map>

#include "tokmon/hash.hpp"

namespace tokmon {
namespace {

const OpticalPortSpec* find_port(const std::vector<OpticalPortSpec>& ports,
                                 const std::string_view name) {
  const auto found = std::ranges::find_if(ports, [name](const auto& port) {
    return port.name == name;
  });
  return found == ports.end() ? nullptr : &*found;
}

bool schema_compatible(const std::string_view from, const std::string_view to) {
  return from.empty() || to.empty() || from == "*" || to == "*" || from == to;
}

bool resonator_law(const MergeLaw law) {
  return law == MergeLaw::set_union || law == MergeLaw::map_union_unique ||
      law == MergeLaw::top_k || law == MergeLaw::priority_then_path;
}

cbor::Value endpoint_value(const OpticalEndpoint& endpoint) {
  return cbor::object({{"lens", endpoint.lens}, {"port", endpoint.port}});
}

std::set<LensId> lens_set(const std::vector<LensId>& lenses) {
  return {lenses.begin(), lenses.end()};
}

std::vector<PhotonId> observed_photons(const LensManifest& manifest,
                                       const PhotonWindow& photons) {
  std::vector<PhotonId> result;
  for (const auto& photon : photons.photons())
    if (std::ranges::any_of(manifest.observes,
        [&photon](const PhotonPattern& pattern) { return pattern.matches(photon); }))
      result.push_back(photon.id);
  return result;
}

std::string input_prefix_hash(const PhotonWindow& photons) {
  return sha256_hex(cbor::encode(to_cbor(photons)));
}

}  // namespace

Result<std::shared_ptr<const OpticalAssemblySnapshot>> compile_optical_assembly(
    const MountEpoch epoch, const std::vector<MountedLens>& lenses,
    const OpticalAssemblySpec& spec) {
  auto assembly = std::make_shared<OpticalAssemblySnapshot>();
  assembly->id = spec.id;
  assembly->epoch = epoch;
  assembly->budget = spec.budget;
  if (assembly->id.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "OpticalAssembly id is required"));
  if (assembly->budget.max_cells == 0u || assembly->budget.max_bytes == 0u ||
      assembly->budget.max_lens_executions == 0u ||
      assembly->budget.deadline <= std::chrono::milliseconds::zero())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "OpticalAssembly budget must be positive"));

  std::unordered_map<LensId, std::size_t> indices;
  for (std::size_t index = 0; index < lenses.size(); ++index) {
    if (!lenses[index].lens)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "OpticalAssembly contains a null Lens"));
    const auto& id = lenses[index].lens->manifest().id;
    const auto& manifest = lenses[index].lens->manifest();
    if (manifest.abi_major != 2u || id.empty())
      return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                       "OpticalAssembly requires Wavefront ABI Lenses"));
    const auto validate_ports = [&](const std::vector<OpticalPortSpec>& ports,
                                    const bool output) -> Result<void> {
      std::set<PortName> names;
      for (const auto& port : ports) {
        if (port.name.empty() || port.band.empty() || port.schema.empty() ||
            port.max_cells == 0u || port.max_cell_bytes == 0u ||
            !names.insert(port.name).second)
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              "Lens has an invalid or duplicate optical port: " + id));
        if (output && port.surface &&
            port.sensitivity == FieldSensitivity::sensitive &&
            port.redaction_policy == "none")
          return tl::unexpected(make_error(ErrorCode::permission_denied,
              "sensitive Surface output requires a redaction policy: " + id));
      }
      return {};
    };
    if (auto valid = validate_ports(manifest.inputs, false); !valid)
      return tl::unexpected(valid.error());
    if (auto valid = validate_ports(manifest.outputs, true); !valid)
      return tl::unexpected(valid.error());
    if (!indices.emplace(id, index).second)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "duplicate Lens in OpticalAssembly: " + id));
  }

  std::set<std::tuple<std::size_t, std::string, std::size_t, std::string>> connections;
  const auto append_connection = [&](const OpticalConnectionSpec& connection)
      -> Result<void> {
    const auto from_index = indices.find(connection.from.lens);
    const auto to_index = indices.find(connection.to.lens);
    if (from_index == indices.end() || to_index == indices.end())
      return tl::unexpected(make_error(ErrorCode::not_found,
          "OpticalConnection references a Lens outside the Assembly"));
    const auto& from_manifest = lenses[from_index->second].lens->manifest();
    const auto& to_manifest = lenses[to_index->second].lens->manifest();
    const auto* output = find_port(from_manifest.outputs, connection.from.port);
    const auto* input = find_port(to_manifest.inputs, connection.to.port);
    if (!output || !input)
      return tl::unexpected(make_error(ErrorCode::not_found,
          "OpticalConnection references an undeclared port"));
    if (output->band != input->band || !schema_compatible(output->schema, input->schema))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "OpticalConnection band/schema mismatch from " + connection.from.lens + "." +
              connection.from.port + " to " + connection.to.lens + "." +
              connection.to.port));
    if (static_cast<std::uint8_t>(from_manifest.trust) > input->maximum_trust_tier)
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "OpticalConnection does not meet the input port trust requirement"));
    if (static_cast<std::uint8_t>(output->sensitivity) >
        static_cast<std::uint8_t>(input->sensitivity))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "OpticalConnection would implicitly lower field sensitivity"));
    if (!output->allowed_audiences.empty() &&
        !std::ranges::any_of(output->allowed_audiences,
            [&to_manifest](const LensId& audience) {
              return audience == "*" || audience == to_manifest.id;
            }))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "OpticalConnection target is outside the output audience"));
    const bool process_boundary = to_manifest.runtime == RuntimeKind::native_worker ||
        to_manifest.runtime == RuntimeKind::node ||
        to_manifest.runtime == RuntimeKind::cpython ||
        to_manifest.runtime == RuntimeKind::wasm;
    if (process_boundary && (!output->exportable || output->transient_handle))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "non-exportable or transient optical field cannot cross a process boundary"));
    if (process_boundary && output->sensitivity == FieldSensitivity::sensitive &&
        static_cast<std::uint8_t>(to_manifest.trust) >=
            static_cast<std::uint8_t>(TrustLevel::t2))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "sensitive optical field cannot enter a low-trust Worker"));
    const auto key = std::tuple(from_index->second, connection.from.port,
                                to_index->second, connection.to.port);
    if (!connections.insert(key).second) return {};
    assembly->connections.push_back(CompiledConnection{
        .from_lens = from_index->second, .from_port = connection.from.port,
        .to_lens = to_index->second, .to_port = connection.to.port});
    return {};
  };

  for (const auto& connection : spec.connections)
    if (auto appended = append_connection(connection); !appended)
      return tl::unexpected(appended.error());

  std::set<std::pair<PortName, std::pair<std::size_t, PortName>>> input_bindings;
  for (const auto& binding : spec.inputs) {
    const auto target = indices.find(binding.to.lens);
    if (binding.assembly_port.empty() || target == indices.end())
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "OpticalAssembly input binding is invalid"));
    const auto* port = find_port(lenses[target->second].lens->manifest().inputs,
                                 binding.to.port);
    if (!port)
      return tl::unexpected(make_error(ErrorCode::not_found,
          "OpticalAssembly input binding references an undeclared input port"));
    const auto key = std::pair(binding.assembly_port,
        std::pair(target->second, binding.to.port));
    if (!input_bindings.insert(key).second)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "duplicate OpticalAssembly input binding"));
    assembly->inputs.push_back(CompiledInputBinding{
        .assembly_port = binding.assembly_port, .to_lens = target->second,
        .to_port = binding.to.port});
  }
  std::set<std::tuple<std::size_t, PortName, PortName>> output_bindings;
  for (const auto& binding : spec.outputs) {
    const auto source = indices.find(binding.from.lens);
    if (binding.assembly_port.empty() || source == indices.end())
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "OpticalAssembly output binding is invalid"));
    const auto* port = find_port(lenses[source->second].lens->manifest().outputs,
                                 binding.from.port);
    if (!port)
      return tl::unexpected(make_error(ErrorCode::not_found,
          "OpticalAssembly output binding references an undeclared output port"));
    const auto key = std::tuple(source->second, binding.from.port,
                                binding.assembly_port);
    if (!output_bindings.insert(key).second)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "duplicate OpticalAssembly output binding"));
    assembly->outputs.push_back(CompiledOutputBinding{
        .from_lens = source->second, .from_port = binding.from.port,
        .assembly_port = binding.assembly_port});
  }

  for (std::size_t to_index = 0; to_index < lenses.size(); ++to_index) {
    const auto& input_manifest = lenses[to_index].lens->manifest();
    for (const auto& input : input_manifest.inputs) {
      const bool already_connected = std::ranges::any_of(assembly->connections,
          [to_index, &input](const auto& connection) {
            return connection.to_lens == to_index && connection.to_port == input.name;
          });
      const bool externally_bound = std::ranges::any_of(assembly->inputs,
          [to_index, &input](const CompiledInputBinding& binding) {
            return binding.to_lens == to_index && binding.to_port == input.name;
          });
      if (already_connected || externally_bound) continue;
      std::vector<OpticalConnectionSpec> compatible;
      if (spec.autowire_unique) {
        for (std::size_t from_index = 0; from_index < lenses.size(); ++from_index) {
          if (from_index == to_index) continue;
          for (const auto& output : lenses[from_index].lens->manifest().outputs)
            if (output.band == input.band && schema_compatible(output.schema, input.schema))
              compatible.push_back(OpticalConnectionSpec{
                  .from = {lenses[from_index].lens->manifest().id, output.name},
                  .to = {input_manifest.id, input.name}});
        }
      }
      if (compatible.size() == 1u) {
        if (auto appended = append_connection(compatible.front()); !appended)
          return tl::unexpected(appended.error());
      } else if (compatible.size() > 1u) {
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "ambiguous automatic optical connection for " + input_manifest.id + "." +
                input.name + "; declare a splitter/merge connection explicitly"));
      } else if (input.requirement == PortRequirement::required) {
        return tl::unexpected(make_error(ErrorCode::not_found,
            "required optical input is unconnected: " + input_manifest.id + "." +
                input.name));
      }
    }
  }

  std::set<std::pair<std::string, std::string>> act_routes;
  for (std::size_t index = 0; index < lenses.size(); ++index) {
    const auto& manifest = lenses[index].lens->manifest();
    if (manifest.trigger == TriggerPolicy::on_delta && !manifest.monotone)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
          "on_delta Lens must declare monotone idempotent output: " + manifest.id));
    for (const auto& output : manifest.outputs)
      if (output.max_cell_bytes > assembly->budget.max_cell_bytes ||
          output.max_cells > assembly->budget.max_cells)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "Lens output budget is unsatisfiable in OpticalAssembly: " + manifest.id));
    for (const auto& input : manifest.inputs) {
      const auto connected = std::ranges::count_if(assembly->connections,
          [index, &input](const CompiledConnection& connection) {
            return connection.to_lens == index && connection.to_port == input.name;
          }) + std::ranges::count_if(assembly->inputs,
          [index, &input](const CompiledInputBinding& binding) {
            return binding.to_lens == index && binding.to_port == input.name;
          });
      if (input.cardinality == PortCardinality::one && connected > 1)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "single-cardinality optical input has multiple sources: " +
                manifest.id + "." + input.name));
    }
    for (const auto& pattern : manifest.refracts) {
      if (pattern.kind == "*" || pattern.kind.ends_with('*')) continue;
      if (!act_routes.emplace(pattern.kind, pattern.schema).second)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "Act route is ambiguous in OpticalAssembly: " + pattern.kind));
    }
  }

  const auto node_count = lenses.size();
  std::vector<std::vector<std::size_t>> adjacency(node_count);
  std::vector<bool> self_loop(node_count, false);
  for (const auto& connection : assembly->connections) {
    adjacency[connection.from_lens].push_back(connection.to_lens);
    if (connection.from_lens == connection.to_lens) self_loop[connection.from_lens] = true;
  }
  for (auto& edges : adjacency) {
    std::ranges::sort(edges);
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  }

  std::vector<int> discovery(node_count, -1), low(node_count, -1);
  std::vector<bool> on_stack(node_count, false);
  std::vector<std::size_t> stack;
  std::vector<std::vector<std::size_t>> components;
  int next_discovery = 0;
  std::function<void(std::size_t)> strong_connect = [&](const std::size_t node) {
    discovery[node] = low[node] = next_discovery++;
    stack.push_back(node);
    on_stack[node] = true;
    for (const auto target : adjacency[node]) {
      if (discovery[target] < 0) {
        strong_connect(target);
        low[node] = std::min(low[node], low[target]);
      } else if (on_stack[target]) {
        low[node] = std::min(low[node], discovery[target]);
      }
    }
    if (low[node] != discovery[node]) return;
    std::vector<std::size_t> component;
    while (!stack.empty()) {
      const auto member = stack.back();
      stack.pop_back();
      on_stack[member] = false;
      component.push_back(member);
      if (member == node) break;
    }
    std::ranges::sort(component);
    components.push_back(std::move(component));
  };
  for (std::size_t node = 0; node < node_count; ++node)
    if (discovery[node] < 0) strong_connect(node);

  std::vector<PropagationStep> steps;
  std::vector<std::size_t> component_for_node(node_count, 0);
  steps.reserve(components.size());
  for (std::size_t component_index = 0; component_index < components.size();
       ++component_index) {
    const auto& members = components[component_index];
    for (const auto member : members) component_for_node[member] = component_index;
    const bool cyclic = members.size() > 1u || (members.size() == 1u && self_loop[members[0]]);
    PropagationStep step{.lenses = members};
    if (cyclic) {
      std::set<LensId> ids;
      for (const auto member : members) ids.insert(lenses[member].lens->manifest().id);
      const auto found = std::ranges::find_if(spec.resonators,
          [&ids](const ResonatorSpec& resonator) { return lens_set(resonator.lenses) == ids; });
      if (found == spec.resonators.end())
        return tl::unexpected(make_error(ErrorCode::invalid_state,
                                         "unguarded cycle in OpticalAssembly"));
      for (const auto member : members) {
        const auto& manifest = lenses[member].lens->manifest();
        if (!manifest.monotone)
          return tl::unexpected(make_error(ErrorCode::invalid_state,
              "Resonator Lens must declare monotone view: " + manifest.id));
        if (manifest.trigger != TriggerPolicy::on_delta)
          return tl::unexpected(make_error(ErrorCode::invalid_state,
              "Resonator Lens must use on_delta triggering: " + manifest.id));
        if (manifest.runtime != RuntimeKind::in_process &&
            manifest.runtime != RuntimeKind::wasm)
          return tl::unexpected(make_error(ErrorCode::invalid_state,
              "Resonator cannot cross a process boundary: " + manifest.id));
        for (const auto& port : manifest.inputs)
          if (!resonator_law(port.merge))
            return tl::unexpected(make_error(ErrorCode::invalid_state,
                "Resonator input port uses a non-lattice merge law"));
        for (const auto& port : manifest.outputs)
          if (!resonator_law(port.merge))
            return tl::unexpected(make_error(ErrorCode::invalid_state,
                "Resonator output port uses a non-lattice merge law"));
      }
      if (found->budget.max_rounds == 0u)
        return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                         "Resonator max_rounds must be positive"));
      if (found->budget.max_cells > assembly->budget.max_cells ||
          found->budget.max_bytes > assembly->budget.max_bytes ||
          found->budget.max_lens_executions >
              assembly->budget.max_lens_executions)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "Resonator budget cannot exceed its OpticalAssembly budget"));
      step.resonator = true;
      step.resonance = *found;
    }
    steps.push_back(std::move(step));
  }

  std::vector<std::set<std::size_t>> component_edges(components.size());
  std::vector<std::size_t> indegree(components.size(), 0u);
  for (std::size_t source = 0; source < adjacency.size(); ++source)
    for (const auto target : adjacency[source]) {
      const auto from_component = component_for_node[source];
      const auto to_component = component_for_node[target];
      if (from_component != to_component &&
          component_edges[from_component].insert(to_component).second)
        ++indegree[to_component];
    }
  std::set<std::size_t> ready;
  for (std::size_t component = 0; component < indegree.size(); ++component)
    if (indegree[component] == 0u) ready.insert(component);
  std::size_t scheduled = 0;
  while (!ready.empty()) {
    std::vector<std::size_t> current(ready.begin(), ready.end());
    ready.clear();
    std::vector<PropagationStep> layer;
    for (const auto component : current) {
      layer.push_back(steps[component]);
      ++scheduled;
      for (const auto target : component_edges[component])
        if (--indegree[target] == 0u) ready.insert(target);
    }
    assembly->layers.push_back(std::move(layer));
  }
  if (scheduled != components.size())
    return tl::unexpected(make_error(ErrorCode::internal_error,
                                     "OpticalAssembly component scheduling failed"));

  cbor::Value::Array lens_values;
  for (std::size_t index = 0; index < lenses.size(); ++index) {
    cbor::Value::Array inputs;
    cbor::Value::Array outputs;
    for (const auto& port : lenses[index].lens->manifest().inputs)
      inputs.push_back(to_cbor(port));
    for (const auto& port : lenses[index].lens->manifest().outputs)
      outputs.push_back(to_cbor(port));
    lens_values.push_back(cbor::object({
        {"id", lenses[index].lens->manifest().id},
        {"generation", static_cast<std::int64_t>(lenses[index].generation)},
        {"artifact", lenses[index].artifact_hash}, {"inputs", std::move(inputs)},
        {"outputs", std::move(outputs)}}));
  }
  cbor::Value::Array connection_values;
  for (const auto& connection : assembly->connections)
    connection_values.push_back(cbor::object({
      {"from", endpoint_value({lenses[connection.from_lens].lens->manifest().id,
                                  connection.from_port})},
        {"to", endpoint_value({lenses[connection.to_lens].lens->manifest().id,
                                connection.to_port})}}));
  cbor::Value::Array input_values;
  for (const auto& binding : assembly->inputs)
    input_values.push_back(cbor::object({{"assembly_port", binding.assembly_port},
        {"to", endpoint_value({lenses[binding.to_lens].lens->manifest().id,
                                binding.to_port})}}));
  cbor::Value::Array output_values;
  for (const auto& binding : assembly->outputs)
    output_values.push_back(cbor::object({{"from", endpoint_value({
        lenses[binding.from_lens].lens->manifest().id, binding.from_port})},
        {"assembly_port", binding.assembly_port}}));
  assembly->hash = sha256_hex(cbor::encode(cbor::object({
      {"id", assembly->id}, {"epoch", static_cast<std::int64_t>(epoch)},
      {"lenses", std::move(lens_values)},
      {"connections", std::move(connection_values)},
      {"inputs", std::move(input_values)}, {"outputs", std::move(output_values)},
      {"max_cells", static_cast<std::int64_t>(assembly->budget.max_cells)},
      {"max_bytes", static_cast<std::int64_t>(assembly->budget.max_bytes)},
      {"max_rounds", static_cast<std::int64_t>(assembly->budget.max_rounds)}})));
  return std::shared_ptr<const OpticalAssemblySnapshot>(std::move(assembly));
}

Result<OpticalBeatResult> OpticalPropagator::propagate(
    const RayId& ray, const PhotonWindow& photons,
    const std::vector<MountedLens>& lenses,
    const OpticalAssemblySnapshot& assembly,
    const IncidentWave* external_incident) const {
  const BeatKey key{.ray = ray, .epoch = assembly.epoch,
      .input_prefix_hash = input_prefix_hash(photons), .assembly_hash = assembly.hash};
  Wavefront wavefront(key);
  std::vector<OpticalTraceEntry> trace;
  std::vector<std::set<std::string>> executed_inputs(lenses.size());
  std::size_t lens_executions = 0;
  std::size_t max_round_seen = 0;
  const auto assembly_deadline = std::chrono::steady_clock::now() + assembly.budget.deadline;

  const auto execute_lens = [&](const std::size_t index, const std::uint32_t round)
      -> Result<bool> {
    if (index >= lenses.size() || !lenses[index].lens)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "compiled optical Lens index is invalid"));
    const auto& mounted = lenses[index];
    const auto& manifest = mounted.lens->manifest();
    IncidentWave incident;
    bool missing_required = false;
    for (const auto& input : manifest.inputs) {
      std::vector<FieldCell> cells;
      bool connected = false;
      if (external_incident) {
        for (const auto& binding : assembly.inputs) {
          if (binding.to_lens != index || binding.to_port != input.name) continue;
          connected = true;
          for (const auto& cell : external_incident->cells(binding.assembly_port)) {
            if (cell.band != input.band || !schema_compatible(cell.schema, input.schema))
              return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                               "external incident field schema mismatch"));
            if (cell.producer_trust > input.maximum_trust_tier ||
                static_cast<std::uint8_t>(cell.sensitivity) >
                    static_cast<std::uint8_t>(input.sensitivity))
              return tl::unexpected(make_error(ErrorCode::permission_denied,
                                               "external incident information flow denied"));
            cells.push_back(cell);
          }
        }
      }
      for (const auto& connection : assembly.connections) {
        if (connection.to_lens != index || connection.to_port != input.name) continue;
        connected = true;
        auto source_cells = wavefront.select(
            lenses[connection.from_lens].lens->manifest().id, connection.from_port);
        for (const auto& cell : source_cells) {
          if (cell.band != input.band || !schema_compatible(cell.schema, input.schema))
            return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                             "incident field schema mismatch"));
          if (cell.producer_trust > input.maximum_trust_tier)
            return tl::unexpected(make_error(ErrorCode::permission_denied,
                                             "incident field trust requirement denied"));
          if (static_cast<std::uint8_t>(cell.sensitivity) >
              static_cast<std::uint8_t>(input.sensitivity))
            return tl::unexpected(make_error(ErrorCode::permission_denied,
                                             "incident field sensitivity denied"));
          if (!cell.allowed_audiences.empty() &&
              !std::ranges::any_of(cell.allowed_audiences,
                  [&manifest](const LensId& audience) {
                    return audience == "*" || audience == manifest.id;
                  }))
            return tl::unexpected(make_error(ErrorCode::permission_denied,
                                             "incident field audience denied"));
          cells.push_back(cell);
        }
      }
      if (connected) {
        if (input.merge == MergeLaw::top_k && cells.size() > input.max_cells) {
          std::ranges::sort(cells, [](const FieldCell& left, const FieldCell& right) {
            if (left.priority != right.priority) return left.priority > right.priority;
            return left.id < right.id;
          });
          cells.resize(input.max_cells);
        }
        if (auto result = incident.connect(input.name, std::move(cells), input); !result)
          return tl::unexpected(result.error());
      }
      if (input.requirement == PortRequirement::required &&
          (!connected || incident.cells(input.name).empty()))
        missing_required = true;
    }
    if (missing_required) {
      OpticalTraceEntry entry{.lens = manifest.id, .generation = mounted.generation,
                               .path_index = index, .round = round,
                               .input_cells = incident.cell_ids().size()};
      entry.status = "waiting_required_input";
      trace.push_back(std::move(entry));
      return false;
    }

    std::vector<IncidentWave> invocations;
    if (manifest.trigger != TriggerPolicy::per_key_join) {
      invocations.push_back(std::move(incident));
    } else {
      std::set<std::string, std::less<>> keys;
      for (const auto& port : manifest.inputs)
        for (const auto& cell : incident.cells(port.name)) keys.insert(cell.key);
      for (const auto& join_key : keys) {
        IncidentWave joined;
        bool ready = true;
        for (const auto& port : manifest.inputs) {
          if (!incident.connected(port.name)) {
            if (port.requirement == PortRequirement::required) ready = false;
            continue;
          }
          std::vector<FieldCell> cells;
          for (const auto& cell : incident.cells(port.name))
            if (cell.key == join_key) cells.push_back(cell);
          if (port.requirement == PortRequirement::required && cells.empty()) ready = false;
          if (auto connected = joined.connect(port.name, std::move(cells), port);
              !connected)
            return tl::unexpected(connected.error());
        }
        if (ready) invocations.push_back(std::move(joined));
      }
    }
    bool any_changed = false;
    for (const auto& invocation : invocations) {
      OpticalTraceEntry entry{.lens = manifest.id, .generation = mounted.generation,
                               .path_index = index, .round = round,
                               .input_cells = invocation.cell_ids().size()};
      const auto input_hash = invocation.canonical_hash();
      if (!executed_inputs[index].insert(input_hash).second) {
        entry.cache_hit = true;
        trace.push_back(std::move(entry));
        continue;
      }
      if (++lens_executions > assembly.budget.max_lens_executions)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "OpticalAssembly Lens execution budget exceeded"));
      if (std::chrono::steady_clock::now() >= assembly_deadline)
        return tl::unexpected(make_error(ErrorCode::timeout,
            "OpticalAssembly propagation deadline exceeded"));
      OpticalBudget lens_budget = assembly.budget;
      lens_budget.max_bytes = std::min(lens_budget.max_bytes,
                                       manifest.resources.output_bytes);
      lens_budget.deadline = std::min(lens_budget.deadline,
                                      manifest.resources.deadline);
      BeatContext context{.key = key, .budget = lens_budget, .round = round,
          .deadline = std::min(assembly_deadline,
              std::chrono::steady_clock::now() + lens_budget.deadline)};
      auto input_ids = invocation.cell_ids();
      auto photon_ids = observed_photons(manifest, photons);
      WavefrontBuilder outgoing(manifest.id, mounted.generation, index,
                                manifest.outputs, context, input_ids, photon_ids,
                                static_cast<std::uint8_t>(manifest.trust));
      const OpticalInput input(photons, invocation, context);
      const auto started = std::chrono::steady_clock::now();
      Result<void> viewed = tl::unexpected(make_error(
          ErrorCode::internal_error, "Lens view did not return"));
      try {
        viewed = mounted.lens->view(input, outgoing);
      } catch (const std::exception& exception) {
        viewed = tl::unexpected(make_error(ErrorCode::lens_crashed,
            "Lens view exception: " + std::string(exception.what())));
      } catch (...) {
        viewed = tl::unexpected(make_error(ErrorCode::lens_crashed,
            "Lens view raised an unknown exception"));
      }
      entry.duration = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started);
      if (!viewed) {
        entry.status = "failed";
        entry.detail = viewed.error().describe();
        trace.push_back(std::move(entry));
        return tl::unexpected(viewed.error());
      }
      for (const auto& cell : outgoing.cells()) {
        const auto* output = find_port(manifest.outputs,
                                       cell.provenance.output_port);
        const auto law = cell.band == "act.proposal" ? MergeLaw::map_union_unique :
            output ? output->merge : MergeLaw::set_union;
        const auto max_cells = output ? output->max_cells :
            assembly.budget.max_cells;
        auto merged = wavefront.merge(cell, law, max_cells);
        if (!merged) return tl::unexpected(merged.error());
        any_changed = any_changed || *merged;
      }
      if (wavefront.cell_count() > assembly.budget.max_cells ||
          wavefront.bytes() > assembly.budget.max_bytes)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "OpticalAssembly wavefront budget exceeded"));
      entry.output_cells = outgoing.cells().size();
      entry.output_bytes = outgoing.bytes();
      trace.push_back(std::move(entry));
    }
    return any_changed;
  };

  for (const auto& layer : assembly.layers) {
    for (const auto& step : layer) {
      if (!step.resonator) {
        for (const auto index : step.lenses) {
          auto executed = execute_lens(index, 0);
          if (!executed) return tl::unexpected(executed.error());
        }
        continue;
      }
      bool converged = false;
      const auto executions_before = lens_executions;
      const auto resonator_deadline = std::min(assembly_deadline,
          std::chrono::steady_clock::now() + step.resonance.budget.deadline);
      for (std::uint32_t round = 0; round < step.resonance.budget.max_rounds; ++round) {
        const auto before = wavefront.canonical_hash();
        for (const auto index : step.lenses) {
          auto executed = execute_lens(index, round);
          if (!executed) return tl::unexpected(executed.error());
        }
        for (const auto& [band, cells] : wavefront.bands())
          for (const auto& cell : cells)
            if (std::ranges::find(step.lenses, cell.provenance.path_index) !=
                    step.lenses.end() &&
                band == "act.proposal")
              return tl::unexpected(make_error(ErrorCode::permission_denied,
                  "Resonator Lens cannot produce an Act proposal"));
        max_round_seen = std::max<std::size_t>(max_round_seen,
                                               static_cast<std::size_t>(round + 1u));
        if (wavefront.canonical_hash() == before) {
          converged = true;
          break;
        }
        std::size_t resonator_cells = 0;
        std::size_t resonator_bytes = 0;
        for (const auto& [band, cells] : wavefront.bands()) {
          (void)band;
          for (const auto& cell : cells)
            if (std::ranges::find(step.lenses, cell.provenance.path_index) !=
                step.lenses.end()) {
              ++resonator_cells;
              resonator_bytes += cbor::encode(cell.value).size();
            }
        }
        if (std::chrono::steady_clock::now() >= resonator_deadline ||
            resonator_cells > step.resonance.budget.max_cells ||
            resonator_bytes > step.resonance.budget.max_bytes ||
            lens_executions - executions_before >
                step.resonance.budget.max_lens_executions)
          return tl::unexpected(make_error(ErrorCode::timeout,
                                           "Optical resonator budget exceeded"));
      }
      if (!converged)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
                                         "Optical resonator did not converge"));
    }
  }

  SurfaceSnapshot surface;
  surface.epoch = assembly.epoch;
  surface.assembly_hash = assembly.hash;
  surface.wavefront_hash = wavefront.canonical_hash();
  surface.wavefront_cells = wavefront.cell_count();
  surface.propagation_rounds = max_round_seen;
  for (const auto& [band, cells] : wavefront.bands()) {
    for (const auto& cell : cells) {
      if (band == "act.proposal") {
        auto act = act_from_cbor(cell.value);
        if (!act) return tl::unexpected(act.error());
        act->epoch = assembly.epoch;
        act->assembly_hash = assembly.hash;
        act->proposal_cell = cell.id;
        act->optical_inputs = cell.provenance.input_cells;
        surface.proposals.push_back(std::move(*act));
      } else if (cell.surface) {
        surface.contributions.push_back(SurfaceContribution{
            .lens = cell.provenance.producer,
            .generation = cell.provenance.generation,
            .channel = cell.band,
            .key = cell.key,
            .value = cell.value,
            .priority = cell.priority,
            .field_cell = cell.id,
            .input_cells = cell.provenance.input_cells,
            .assembly_hash = cell.provenance.assembly_hash});
      }
    }
  }
  std::ranges::stable_sort(surface.contributions,
      [](const SurfaceContribution& left, const SurfaceContribution& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        if (left.lens != right.lens) return left.lens < right.lens;
        if (left.key != right.key) return left.key < right.key;
        return left.field_cell < right.field_cell;
      });
  return OpticalBeatResult{.wavefront = std::move(wavefront),
                           .surface = std::move(surface), .trace = std::move(trace)};
}

cbor::Value to_cbor(const OpticalAssemblySnapshot& assembly) {
  cbor::Value::Array connections;
  for (const auto& connection : assembly.connections)
    connections.push_back(cbor::object({
        {"from_lens", static_cast<std::int64_t>(connection.from_lens)},
        {"from_port", connection.from_port},
        {"to_lens", static_cast<std::int64_t>(connection.to_lens)},
        {"to_port", connection.to_port}}));
  cbor::Value::Array layers;
  for (const auto& layer : assembly.layers) {
    cbor::Value::Array steps;
    for (const auto& step : layer) {
      cbor::Value::Array lenses;
      for (const auto index : step.lenses)
        lenses.emplace_back(static_cast<std::int64_t>(index));
      steps.push_back(cbor::object({{"lenses", std::move(lenses)},
          {"resonator", step.resonator}, {"resonator_id", step.resonance.id}}));
    }
    layers.emplace_back(std::move(steps));
  }
  cbor::Value::Array inputs;
  for (const auto& binding : assembly.inputs)
    inputs.push_back(cbor::object({{"assembly_port", binding.assembly_port},
        {"to_lens", static_cast<std::int64_t>(binding.to_lens)},
        {"to_port", binding.to_port}}));
  cbor::Value::Array outputs;
  for (const auto& binding : assembly.outputs)
    outputs.push_back(cbor::object({
        {"from_lens", static_cast<std::int64_t>(binding.from_lens)},
        {"from_port", binding.from_port}, {"assembly_port", binding.assembly_port}}));
  return cbor::object({{"id", assembly.id},
      {"epoch", static_cast<std::int64_t>(assembly.epoch)}, {"hash", assembly.hash},
      {"connections", std::move(connections)}, {"inputs", std::move(inputs)},
      {"outputs", std::move(outputs)}, {"layers", std::move(layers)}});
}

}  // namespace tokmon
