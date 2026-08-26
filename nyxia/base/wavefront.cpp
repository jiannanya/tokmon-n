#include "tokmon/wavefront.hpp"

#include <algorithm>
#include <limits>

#include "tokmon/hash.hpp"

namespace tokmon {
namespace {

const std::vector<FieldCell> empty_cells;

std::string required_string(const cbor::Value& value, const std::string_view key) {
  if (const auto* field = cbor::find(value, key)) return std::string(field->as_string());
  return {};
}

std::int64_t required_integer(const cbor::Value& value, const std::string_view key) {
  if (const auto* field = cbor::find(value, key)) return field->as_integer();
  return 0;
}

cbor::Value strings(const std::vector<std::string>& values) {
  cbor::Value::Array encoded;
  encoded.reserve(values.size());
  for (const auto& value : values) encoded.emplace_back(value);
  return encoded;
}

std::vector<std::string> string_array(const cbor::Value* value) {
  std::vector<std::string> result;
  if (!value || !value->as_array()) return result;
  result.reserve(value->as_array()->size());
  for (const auto& item : *value->as_array()) result.emplace_back(item.as_string());
  return result;
}

bool stable_cell_less(const FieldCell& left, const FieldCell& right) {
  if (left.priority != right.priority) return left.priority > right.priority;
  if (left.provenance.path_index != right.provenance.path_index)
    return left.provenance.path_index < right.provenance.path_index;
  if (left.key != right.key) return left.key < right.key;
  return left.id < right.id;
}

}  // namespace

std::string BeatKey::canonical() const {
  return sha256_hex(cbor::encode(cbor::object({
      {"ray", ray}, {"epoch", static_cast<std::int64_t>(epoch)},
      {"input_prefix_hash", input_prefix_hash}, {"assembly_hash", assembly_hash}})));
}

const std::vector<FieldCell>& IncidentWave::cells(const PortName port) const noexcept {
  const auto found = ports_.find(port);
  return found == ports_.end() ? empty_cells : found->second;
}

const FieldCell* IncidentWave::one(const PortName port) const noexcept {
  const auto& values = cells(port);
  return values.size() == 1u ? &values.front() : nullptr;
}

bool IncidentWave::connected(const PortName port) const noexcept {
  return ports_.contains(port);
}

bool IncidentWave::sealed(const PortName port) const noexcept {
  return sealed_.contains(port);
}

std::vector<FieldCellId> IncidentWave::cell_ids() const {
  std::vector<FieldCellId> result;
  for (const auto& [port, cells_for_port] : ports_) {
    (void)port;
    for (const auto& cell : cells_for_port) result.push_back(cell.id);
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string IncidentWave::canonical_hash() const {
  const auto ids = cell_ids();
  return sha256_hex(cbor::encode(strings(ids)));
}

Result<void> IncidentWave::connect(PortName port, std::vector<FieldCell> cells_for_port,
                                   const OpticalPortSpec& spec, const bool sealed_value) {
  std::ranges::sort(cells_for_port, stable_cell_less);
  cells_for_port.erase(std::unique(cells_for_port.begin(), cells_for_port.end(),
      [](const FieldCell& left, const FieldCell& right) { return left.id == right.id; }),
      cells_for_port.end());
  const auto same_value = [](const FieldCell& left, const FieldCell& right) {
    return left.schema == right.schema && left.key == right.key &&
        cbor::encode(left.value) == cbor::encode(right.value);
  };
  if (spec.merge == MergeLaw::set_union || spec.merge == MergeLaw::top_k) {
    std::vector<FieldCell> unique;
    for (auto& cell : cells_for_port) {
      if (!std::ranges::any_of(unique, [&cell, &same_value](const FieldCell& value) {
            return same_value(value, cell);
          }))
        unique.push_back(std::move(cell));
    }
    cells_for_port = std::move(unique);
  } else if (spec.merge == MergeLaw::map_union_unique) {
    std::vector<FieldCell> unique;
    for (auto& cell : cells_for_port) {
      const auto found = std::ranges::find_if(unique, [&cell](const FieldCell& value) {
        return value.key == cell.key;
      });
      if (found == unique.end()) {
        unique.push_back(std::move(cell));
      } else if (!same_value(*found, cell)) {
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            "map_union_unique conflict on input port '" + port +
                "' key '" + cell.key + "'"));
      }
    }
    cells_for_port = std::move(unique);
  } else if (spec.merge == MergeLaw::priority_then_path) {
    std::vector<FieldCell> selected;
    for (auto& cell : cells_for_port)
      if (!std::ranges::any_of(selected, [&cell](const FieldCell& value) {
            return value.key == cell.key;
          }))
        selected.push_back(std::move(cell));
    cells_for_port = std::move(selected);
  }
  if (spec.merge == MergeLaw::top_k && cells_for_port.size() > spec.max_cells)
    cells_for_port.resize(spec.max_cells);
  if ((spec.cardinality == PortCardinality::one ||
       spec.merge == MergeLaw::optional_single || spec.merge == MergeLaw::product) &&
      cells_for_port.size() > 1u)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
        "input port '" + port + "' received more than one field cell"));
  if (cells_for_port.size() > spec.max_cells)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
        "input port '" + port + "' exceeded its cell budget"));
  ports_[std::move(port)] = std::move(cells_for_port);
  if (sealed_value) sealed_.insert(spec.name);
  return {};
}

bool BeatContext::expired() const noexcept {
  return deadline != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= deadline;
}

OpticalInput::OpticalInput(const PhotonWindow& photons, const IncidentWave& incident,
                           const BeatContext& beat) noexcept
    : photons_(photons), incident_(incident), beat_(beat) {}
const PhotonWindow& OpticalInput::photon_window() const noexcept { return photons_; }
const IncidentWave& OpticalInput::incident() const noexcept { return incident_; }
const BeatContext& OpticalInput::beat() const noexcept { return beat_; }
const std::vector<Photon>& OpticalInput::photons() const noexcept {
  return photons_.photons();
}
const Photon* OpticalInput::latest() const noexcept { return photons_.latest(); }
const Photon* OpticalInput::latest(const std::string_view kind) const noexcept {
  return photons_.latest(kind);
}
bool OpticalInput::contains_after(const std::string_view kind,
                                  const std::uint64_t sequence) const noexcept {
  return photons_.contains_after(kind, sequence);
}

WavefrontBuilder::WavefrontBuilder(LensId source, const GenerationId generation,
                                   const std::size_t path_index,
                                   std::vector<OpticalPortSpec> outputs,
                                   BeatContext context,
                                   std::vector<FieldCellId> visible_inputs,
                                   std::vector<PhotonId> visible_photons,
                                   const std::uint8_t source_trust)
    : source_(std::move(source)), generation_(generation), path_index_(path_index),
      outputs_(std::move(outputs)), context_(std::move(context)),
      visible_inputs_(visible_inputs.begin(), visible_inputs.end()),
      visible_photons_(std::move(visible_photons)), source_trust_(source_trust) {}

const OpticalPortSpec* WavefrontBuilder::output_spec(
    const std::string_view output) const noexcept {
  const auto found = std::ranges::find_if(outputs_, [output](const auto& spec) {
    return spec.name == output;
  });
  return found == outputs_.end() ? nullptr : &*found;
}

Result<FieldCellId> WavefrontBuilder::append_cell(
    const OpticalPortSpec& spec, std::string key, cbor::Value value,
    const std::span<const FieldCellId> caused_by, const std::int32_t priority) {
  if (context_.expired())
    return tl::unexpected(make_error(ErrorCode::timeout,
                                     "wavefront view deadline exceeded"));
  if (key.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "field cell key is required"));
  const auto payload_bytes = cbor::encode(value);
  if (spec.sensitivity == FieldSensitivity::secret_reference &&
      !std::holds_alternative<std::string>(value.data) &&
      !cbor::find(value, "secret_ref"))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "secret_reference output must contain only an opaque reference"));
  const auto max_cell = std::min(spec.max_cell_bytes, context_.budget.max_cell_bytes);
  if (payload_bytes.size() > max_cell)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "field cell exceeds output port byte budget"));
  if (cells_.size() >= context_.budget.max_cells ||
      cells_.size() >= spec.max_cells || bytes_ + payload_bytes.size() > context_.budget.max_bytes)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "wavefront output budget exceeded"));
  std::vector<FieldCellId> inputs(caused_by.begin(), caused_by.end());
  for (const auto& id : inputs)
    if (!visible_inputs_.contains(id))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "field provenance references an input outside the incident wave"));
  std::ranges::sort(inputs);
  inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
  const auto identity = sha256_hex(cbor::encode(cbor::object({
      {"beat", context_.key.canonical()}, {"producer", source_},
      {"generation", static_cast<std::int64_t>(generation_)},
      {"port", spec.name}, {"band", spec.band}, {"schema", spec.schema},
      {"key", key}, {"value", value}, {"priority", priority},
      {"surface", spec.surface},
      {"sensitivity", std::string(to_string(spec.sensitivity))},
      {"allowed_audiences", strings(spec.allowed_audiences)},
      {"redaction_policy", spec.redaction_policy}, {"exportable", spec.exportable},
      {"transient_handle", spec.transient_handle}, {"inputs", strings(inputs)}})));
  FieldCell cell{
      .id = "field-" + identity.substr(0, 40),
      .band = spec.band,
      .schema = spec.schema,
      .key = std::move(key),
      .value = std::move(value),
      .priority = priority,
      .surface = spec.surface,
      .sensitivity = spec.sensitivity,
      .producer_trust = source_trust_,
      .allowed_audiences = spec.allowed_audiences,
      .redaction_policy = spec.redaction_policy,
      .exportable = spec.exportable,
      .transient_handle = spec.transient_handle,
      .provenance = FieldProvenance{
          .producer = source_, .generation = generation_, .epoch = context_.key.epoch,
          .path_index = path_index_, .output_port = spec.name,
          .input_cells = std::move(inputs), .input_photons = visible_photons_,
          .assembly_hash = context_.key.assembly_hash}};
  bytes_ += payload_bytes.size();
  const auto id = cell.id;
  cells_.push_back(std::move(cell));
  return id;
}

Result<FieldCellId> WavefrontBuilder::emit(
    PortName output, std::string key, cbor::Value value,
    const std::span<const FieldCellId> caused_by, const std::int32_t priority) {
  const auto* spec = output_spec(output);
  if (!spec)
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "Lens attempted to emit undeclared output port '" + output + "'"));
  return append_cell(*spec, std::move(key), std::move(value), caused_by, priority);
}

Result<void> WavefrontBuilder::add(std::string channel, std::string key,
                                   cbor::Value value, const std::int32_t priority) {
  auto emitted = emit(std::move(channel), std::move(key), std::move(value), {}, priority);
  if (!emitted) return tl::unexpected(emitted.error());
  return {};
}

Result<void> WavefrontBuilder::propose(Act act,
                                       const std::span<const FieldCellId> caused_by) {
  if (act.kind.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "proposed Act kind is required"));
  if (act.id.empty()) act.id = make_id("act");
  const OpticalPortSpec proposal{
      .name = "act.proposal", .band = "act.proposal",
      .schema = "tokmon.act.proposal.v1", .cardinality = PortCardinality::many,
      .merge = MergeLaw::map_union_unique, .max_cells = context_.budget.max_cells,
      .max_cell_bytes = context_.budget.max_cell_bytes, .surface = true};
  auto emitted = append_cell(proposal, act.id, to_cbor(act), caused_by, 0);
  if (!emitted) return tl::unexpected(emitted.error());
  return {};
}

const std::vector<FieldCell>& WavefrontBuilder::cells() const noexcept { return cells_; }
std::size_t WavefrontBuilder::bytes() const noexcept { return bytes_; }

Wavefront::Wavefront(BeatKey key) : key_(std::move(key)) {}

Result<bool> Wavefront::merge(FieldCell cell, const MergeLaw law,
                              const std::size_t max_cells) {
  if (ids_.contains(cell.id)) return false;
  auto& target = bands_[cell.band];
  const auto scope_producer = cell.provenance.producer;
  const auto scope_port = cell.provenance.output_port;
  const auto same_scope = [&scope_producer, &scope_port](const FieldCell& existing) {
    return existing.provenance.producer == scope_producer &&
        existing.provenance.output_port == scope_port;
  };
  const auto same_value = [&cell](const FieldCell& existing) {
    return existing.schema == cell.schema && existing.key == cell.key &&
        cbor::encode(existing.value) == cbor::encode(cell.value);
  };
  if (law == MergeLaw::set_union || law == MergeLaw::top_k) {
    if (std::ranges::any_of(target, [&same_scope, &same_value](const FieldCell& value) {
          return same_scope(value) && same_value(value);
        })) return false;
  }
  if (law == MergeLaw::map_union_unique) {
    const auto conflict = std::ranges::find_if(target,
        [&cell, &same_scope](const FieldCell& existing) {
      return same_scope(existing) && existing.key == cell.key;
    });
    if (conflict != target.end() && same_value(*conflict)) return false;
    if (conflict != target.end())
      return tl::unexpected(make_error(ErrorCode::invalid_state,
          "map_union_unique conflict on band '" + cell.band + "' key '" + cell.key + "'"));
  }
  const auto scoped_count = static_cast<std::size_t>(std::ranges::count_if(
      target, same_scope));
  if ((law == MergeLaw::optional_single || law == MergeLaw::product) &&
      scoped_count > 0u) {
    const auto existing = std::ranges::find_if(target, same_scope);
    if (existing != target.end() && same_value(*existing)) return false;
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "single-value merge conflict on band '" + cell.band + "'"));
  }
  if (law == MergeLaw::priority_then_path) {
    const auto existing = std::ranges::find_if(target,
        [&cell, &same_scope](const FieldCell& value) {
      return same_scope(value) && value.key == cell.key;
    });
    if (existing != target.end()) {
      if (!stable_cell_less(cell, *existing)) return false;
      bytes_ -= cbor::encode(existing->value).size();
      ids_.erase(existing->id);
      bytes_ += cbor::encode(cell.value).size();
      ids_.insert(cell.id);
      *existing = std::move(cell);
      std::ranges::sort(target, stable_cell_less);
      return true;
    }
  }
  if (law != MergeLaw::top_k && max_cells > 0u && scoped_count >= max_cells)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
        "wavefront band '" + cell.band + "' exceeded its cell budget"));
  const auto inserted_id = cell.id;
  const auto encoded_size = cbor::encode(cell.value).size();
  ids_.insert(cell.id);
  bytes_ += encoded_size;
  target.push_back(std::move(cell));
  std::ranges::sort(target, stable_cell_less);
  if (law == MergeLaw::top_k && max_cells > 0u) {
    auto count = scoped_count + 1u;
    while (count > max_cells) {
      const auto worst = std::find_if(target.rbegin(), target.rend(), same_scope);
      if (worst == target.rend()) break;
      bytes_ -= cbor::encode(worst->value).size();
      ids_.erase(worst->id);
      target.erase(std::next(worst).base());
      --count;
    }
  }
  return ids_.contains(inserted_id);
}

const std::vector<FieldCell>& Wavefront::band(const BandId band_id) const noexcept {
  const auto found = bands_.find(band_id);
  return found == bands_.end() ? empty_cells : found->second;
}

std::vector<FieldCell> Wavefront::select(const LensId& producer,
                                         const PortName output) const {
  std::vector<FieldCell> result;
  for (const auto& [band_id, cells_for_band] : bands_) {
    (void)band_id;
    for (const auto& cell : cells_for_band)
      if (cell.provenance.producer == producer &&
          cell.provenance.output_port == output)
        result.push_back(cell);
  }
  std::ranges::sort(result, stable_cell_less);
  return result;
}

const std::map<BandId, std::vector<FieldCell>, std::less<>>&
Wavefront::bands() const noexcept { return bands_; }
bool Wavefront::contains(const FieldCellId& id) const noexcept { return ids_.contains(id); }
std::size_t Wavefront::cell_count() const noexcept { return ids_.size(); }
std::size_t Wavefront::bytes() const noexcept { return bytes_; }
std::string Wavefront::canonical_hash() const {
  return sha256_hex(cbor::encode(to_cbor(*this)));
}
const BeatKey& Wavefront::beat() const noexcept { return key_; }

std::string_view to_string(const PortCardinality value) noexcept {
  return value == PortCardinality::one ? "one" : "many";
}
std::string_view to_string(const PortRequirement value) noexcept {
  return value == PortRequirement::required ? "required" : "optional";
}
std::string_view to_string(const MergeLaw value) noexcept {
  switch (value) {
    case MergeLaw::set_union: return "set_union";
    case MergeLaw::map_union_unique: return "map_union_unique";
    case MergeLaw::priority_then_path: return "priority_then_path";
    case MergeLaw::top_k: return "top_k";
    case MergeLaw::product: return "product";
    case MergeLaw::optional_single: return "optional_single";
    case MergeLaw::stable_concat: return "stable_concat";
  }
  return "set_union";
}
std::string_view to_string(const TriggerPolicy value) noexcept {
  switch (value) {
    case TriggerPolicy::once_when_ready: return "once_when_ready";
    case TriggerPolicy::on_delta: return "on_delta";
    case TriggerPolicy::on_seal: return "on_seal";
    case TriggerPolicy::per_key_join: return "per_key_join";
  }
  return "once_when_ready";
}
std::string_view to_string(const FieldSensitivity value) noexcept {
  switch (value) {
    case FieldSensitivity::public_data: return "public";
    case FieldSensitivity::normal: return "normal";
    case FieldSensitivity::sensitive: return "sensitive";
    case FieldSensitivity::secret_reference: return "secret_reference";
  }
  return "normal";
}

Result<PortCardinality> port_cardinality_from_string(const std::string_view value) {
  if (value == "one") return PortCardinality::one;
  if (value == "many") return PortCardinality::many;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown port cardinality: " + std::string(value)));
}
Result<PortRequirement> port_requirement_from_string(const std::string_view value) {
  if (value == "required") return PortRequirement::required;
  if (value == "optional") return PortRequirement::optional;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown port requirement: " + std::string(value)));
}
Result<MergeLaw> merge_law_from_string(const std::string_view value) {
  for (const auto law : {MergeLaw::set_union, MergeLaw::map_union_unique,
       MergeLaw::priority_then_path, MergeLaw::top_k, MergeLaw::product,
       MergeLaw::optional_single, MergeLaw::stable_concat})
    if (to_string(law) == value) return law;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown merge law: " + std::string(value)));
}
Result<TriggerPolicy> trigger_policy_from_string(const std::string_view value) {
  for (const auto policy : {TriggerPolicy::once_when_ready, TriggerPolicy::on_delta,
       TriggerPolicy::on_seal, TriggerPolicy::per_key_join})
    if (to_string(policy) == value) return policy;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown trigger policy: " + std::string(value)));
}
Result<FieldSensitivity> field_sensitivity_from_string(const std::string_view value) {
  for (const auto sensitivity : {FieldSensitivity::public_data, FieldSensitivity::normal,
       FieldSensitivity::sensitive, FieldSensitivity::secret_reference})
    if (to_string(sensitivity) == value) return sensitivity;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown field sensitivity: " + std::string(value)));
}

cbor::Value to_cbor(const OpticalPortSpec& port) {
  return cbor::object({{"name", port.name}, {"band", port.band}, {"schema", port.schema},
      {"cardinality", std::string(to_string(port.cardinality))},
      {"requirement", std::string(to_string(port.requirement))},
      {"merge", std::string(to_string(port.merge))},
      {"sensitivity", std::string(to_string(port.sensitivity))},
      {"maximum_trust_tier", static_cast<std::int64_t>(port.maximum_trust_tier)},
      {"allowed_audiences", strings(port.allowed_audiences)},
      {"redaction_policy", port.redaction_policy},
      {"exportable", port.exportable}, {"transient_handle", port.transient_handle},
      {"max_cells", static_cast<std::int64_t>(port.max_cells)},
      {"max_cell_bytes", static_cast<std::int64_t>(port.max_cell_bytes)},
      {"surface", port.surface}});
}

Result<OpticalPortSpec> optical_port_spec_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "OpticalPortSpec must be a map"));
  OpticalPortSpec port;
  port.name = required_string(value, "name");
  port.band = required_string(value, "band");
  port.schema = required_string(value, "schema");
  auto cardinality = port_cardinality_from_string(required_string(value, "cardinality"));
  auto requirement = port_requirement_from_string(required_string(value, "requirement"));
  auto merge = merge_law_from_string(required_string(value, "merge"));
  auto sensitivity = field_sensitivity_from_string(required_string(value, "sensitivity"));
  if (!cardinality) return tl::unexpected(cardinality.error());
  if (!requirement) return tl::unexpected(requirement.error());
  if (!merge) return tl::unexpected(merge.error());
  if (!sensitivity) return tl::unexpected(sensitivity.error());
  port.cardinality = *cardinality;
  port.requirement = *requirement;
  port.merge = *merge;
  port.sensitivity = *sensitivity;
  if (const auto* field = cbor::find(value, "maximum_trust_tier"))
    port.maximum_trust_tier = static_cast<std::uint8_t>(field->as_integer(3));
  port.allowed_audiences = string_array(cbor::find(value, "allowed_audiences"));
  if (const auto* field = cbor::find(value, "redaction_policy"))
    port.redaction_policy = std::string(field->as_string("none"));
  if (const auto* field = cbor::find(value, "exportable"))
    port.exportable = field->as_bool(true);
  if (const auto* field = cbor::find(value, "transient_handle"))
    port.transient_handle = field->as_bool();
  port.max_cells = static_cast<std::size_t>(required_integer(value, "max_cells"));
  port.max_cell_bytes = static_cast<std::size_t>(required_integer(value, "max_cell_bytes"));
  if (const auto* field = cbor::find(value, "surface")) port.surface = field->as_bool();
  if (port.name.empty() || port.band.empty() || port.schema.empty() ||
      port.max_cells == 0u || port.max_cell_bytes == 0u)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "OpticalPortSpec has invalid required fields"));
  return port;
}

cbor::Value to_cbor(const FieldCell& cell) {
  return cbor::object({{"id", cell.id}, {"band", cell.band}, {"schema", cell.schema},
      {"key", cell.key}, {"value", cell.value}, {"priority", cell.priority},
      {"surface", cell.surface},
      {"sensitivity", std::string(to_string(cell.sensitivity))},
      {"producer_trust", static_cast<std::int64_t>(cell.producer_trust)},
      {"allowed_audiences", strings(cell.allowed_audiences)},
      {"redaction_policy", cell.redaction_policy},
      {"exportable", cell.exportable}, {"transient_handle", cell.transient_handle},
      {"provenance", cbor::object({
          {"producer", cell.provenance.producer},
          {"generation", static_cast<std::int64_t>(cell.provenance.generation)},
          {"epoch", static_cast<std::int64_t>(cell.provenance.epoch)},
          {"path_index", static_cast<std::int64_t>(cell.provenance.path_index)},
          {"output_port", cell.provenance.output_port},
          {"input_cells", strings(cell.provenance.input_cells)},
          {"input_photons", strings(cell.provenance.input_photons)},
          {"assembly_hash", cell.provenance.assembly_hash}})}});
}

Result<FieldCell> field_cell_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "FieldCell must be a map"));
  FieldCell cell;
  cell.id = required_string(value, "id");
  cell.band = required_string(value, "band");
  cell.schema = required_string(value, "schema");
  cell.key = required_string(value, "key");
  if (const auto* field = cbor::find(value, "value")) cell.value = *field;
  cell.priority = static_cast<std::int32_t>(required_integer(value, "priority"));
  if (const auto* field = cbor::find(value, "surface")) cell.surface = field->as_bool();
  auto sensitivity = field_sensitivity_from_string(required_string(value, "sensitivity"));
  if (!sensitivity) return tl::unexpected(sensitivity.error());
  cell.sensitivity = *sensitivity;
  cell.producer_trust = static_cast<std::uint8_t>(required_integer(value, "producer_trust"));
  cell.allowed_audiences = string_array(cbor::find(value, "allowed_audiences"));
  if (const auto* field = cbor::find(value, "redaction_policy"))
    cell.redaction_policy = std::string(field->as_string("none"));
  if (const auto* field = cbor::find(value, "exportable")) cell.exportable = field->as_bool(true);
  if (const auto* field = cbor::find(value, "transient_handle"))
    cell.transient_handle = field->as_bool();
  const auto* provenance = cbor::find(value, "provenance");
  if (!provenance || !provenance->is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "FieldCell provenance is required"));
  cell.provenance.producer = required_string(*provenance, "producer");
  cell.provenance.generation = static_cast<GenerationId>(
      required_integer(*provenance, "generation"));
  cell.provenance.epoch = static_cast<MountEpoch>(required_integer(*provenance, "epoch"));
  cell.provenance.path_index = static_cast<std::size_t>(
      required_integer(*provenance, "path_index"));
  cell.provenance.output_port = required_string(*provenance, "output_port");
  cell.provenance.input_cells = string_array(cbor::find(*provenance, "input_cells"));
  cell.provenance.input_photons = string_array(cbor::find(*provenance, "input_photons"));
  cell.provenance.assembly_hash = required_string(*provenance, "assembly_hash");
  if (cell.id.empty() || cell.band.empty() || cell.schema.empty() ||
      cell.provenance.producer.empty() || cell.provenance.output_port.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "FieldCell identity and provenance are required"));
  return cell;
}

cbor::Value to_cbor(const IncidentWave& wave) {
  cbor::Value::Map ports;
  for (const auto& [name, values] : wave.ports_) {
    cbor::Value::Array cells;
    cells.reserve(values.size());
    for (const auto& cell : values) cells.push_back(to_cbor(cell));
    ports.emplace(name, cbor::object({
        {"sealed", wave.sealed_.contains(name)}, {"cells", std::move(cells)}}));
  }
  return cbor::Value(std::move(ports));
}

Result<IncidentWave> incident_wave_from_cbor(const cbor::Value& value) {
  const auto* ports = value.as_map();
  if (!ports)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "IncidentWave must be a port map"));
  IncidentWave wave;
  for (const auto& [name, encoded] : *ports) {
    const auto* items = cbor::find(encoded, "cells");
    if (!items || !items->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "IncidentWave port cells are required"));
    auto& target = wave.ports_[name];
    for (const auto& item : *items->as_array()) {
      auto cell = field_cell_from_cbor(item);
      if (!cell) return tl::unexpected(cell.error());
      target.push_back(std::move(*cell));
    }
    std::ranges::sort(target, stable_cell_less);
    if (const auto* sealed = cbor::find(encoded, "sealed");
        sealed && sealed->as_bool())
      wave.sealed_.insert(name);
  }
  return wave;
}

cbor::Value to_cbor(const BeatContext& beat) {
  const auto remaining = beat.deadline == std::chrono::steady_clock::time_point{}
      ? beat.budget.deadline
      : std::max(std::chrono::milliseconds::zero(),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              beat.deadline - std::chrono::steady_clock::now()));
  return cbor::object({
      {"key", cbor::object({{"ray", beat.key.ray},
          {"epoch", static_cast<std::int64_t>(beat.key.epoch)},
          {"input_prefix_hash", beat.key.input_prefix_hash},
          {"assembly_hash", beat.key.assembly_hash}})},
      {"budget", cbor::object({
          {"max_cells", static_cast<std::int64_t>(beat.budget.max_cells)},
          {"max_bytes", static_cast<std::int64_t>(beat.budget.max_bytes)},
          {"max_cell_bytes", static_cast<std::int64_t>(beat.budget.max_cell_bytes)},
          {"max_lens_executions", static_cast<std::int64_t>(beat.budget.max_lens_executions)},
          {"max_rounds", static_cast<std::int64_t>(beat.budget.max_rounds)},
          {"deadline_ms", static_cast<std::int64_t>(remaining.count())}})},
      {"round", static_cast<std::int64_t>(beat.round)}});
}

Result<BeatContext> beat_context_from_cbor(const cbor::Value& value) {
  const auto* key = cbor::find(value, "key");
  const auto* budget = cbor::find(value, "budget");
  if (!key || !key->is_map() || !budget || !budget->is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "BeatContext key and budget are required"));
  BeatContext result;
  result.key.ray = required_string(*key, "ray");
  result.key.epoch = static_cast<MountEpoch>(required_integer(*key, "epoch"));
  result.key.input_prefix_hash = required_string(*key, "input_prefix_hash");
  result.key.assembly_hash = required_string(*key, "assembly_hash");
  result.budget.max_cells = static_cast<std::size_t>(required_integer(*budget, "max_cells"));
  result.budget.max_bytes = static_cast<std::size_t>(required_integer(*budget, "max_bytes"));
  result.budget.max_cell_bytes = static_cast<std::size_t>(required_integer(*budget, "max_cell_bytes"));
  result.budget.max_lens_executions = static_cast<std::size_t>(required_integer(*budget, "max_lens_executions"));
  result.budget.max_rounds = static_cast<std::uint32_t>(required_integer(*budget, "max_rounds"));
  result.budget.deadline = std::chrono::milliseconds(required_integer(*budget, "deadline_ms"));
  result.round = static_cast<std::uint32_t>(required_integer(value, "round"));
  result.deadline = std::chrono::steady_clock::now() + result.budget.deadline;
  if (result.key.ray.empty() || result.key.assembly_hash.empty() ||
      result.budget.max_cells == 0u || result.budget.max_bytes == 0u ||
      result.budget.max_cell_bytes == 0u || result.budget.deadline <= std::chrono::milliseconds::zero())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "BeatContext has invalid required fields"));
  return result;
}

cbor::Value to_cbor(const OpticalInput& input) {
  return cbor::object({{"photon_window", to_cbor(input.photon_window())},
                       {"incident", to_cbor(input.incident())},
                       {"beat", to_cbor(input.beat())}});
}

cbor::Value to_cbor(const Wavefront& wavefront) {
  cbor::Value::Array bands;
  for (const auto& [band_id, cells_for_band] : wavefront.bands()) {
    cbor::Value::Array cells;
    cells.reserve(cells_for_band.size());
    for (const auto& cell : cells_for_band) cells.push_back(to_cbor(cell));
    bands.push_back(cbor::object({{"band", band_id}, {"cells", std::move(cells)}}));
  }
  return cbor::object({{"beat", wavefront.beat().canonical()},
                       {"bands", std::move(bands)}});
}

}  // namespace tokmon
