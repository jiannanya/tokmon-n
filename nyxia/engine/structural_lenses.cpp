#include "tokmon/structural_lenses.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "tokmon/hash.hpp"

namespace tokmon {
namespace {

LensManifest structural_manifest(LensId id, std::string display,
                                 std::vector<OpticalPortSpec> inputs,
                                 std::vector<OpticalPortSpec> outputs) {
  return LensManifest{.id = std::move(id), .display_name = std::move(display),
      .version = "1.0.0", .abi_major = 2, .abi_minor = 0,
      .runtime = RuntimeKind::in_process, .trust = TrustLevel::t1,
      .inputs = std::move(inputs), .outputs = std::move(outputs),
      .trigger = TriggerPolicy::on_delta, .monotone = true,
      .stateless = true};
}

Result<void> active(const std::atomic_bool& stopping, const BeatContext& beat,
                    const LensId& lens) {
  if (stopping.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled, lens + " is stopping"));
  if (beat.expired())
    return tl::unexpected(make_error(ErrorCode::timeout, lens + " view expired"));
  return {};
}

Result<RefractionResult> pass() {
  return RefractionResult{.status = RefractionStatus::passed};
}

bool branch_matches(const PrismBranch& branch, const FieldCell& cell) {
  if (!branch.key_prefix.empty() && !cell.key.starts_with(branch.key_prefix))
    return false;
  if (branch.value_field.empty()) return true;
  const auto* value = cbor::find(cell.value, branch.value_field);
  return value && cbor::encode(*value) == cbor::encode(branch.equals);
}

const OpticalPortSpec* port_named(const std::vector<OpticalPortSpec>& ports,
                                  const std::string_view name) {
  const auto found = std::ranges::find_if(ports, [name](const auto& port) {
    return port.name == name;
  });
  return found == ports.end() ? nullptr : &*found;
}

bool compatible(const OpticalPortSpec& outer, const OpticalPortSpec& inner) {
  return outer.band == inner.band &&
      (outer.schema == "*" || inner.schema == "*" || outer.schema == inner.schema);
}

}  // namespace

IdentityLens::IdentityLens(LensId id, OpticalPortSpec input,
                           OpticalPortSpec output)
    : manifest_(structural_manifest(std::move(id), "Identity Lens",
          {std::move(input)}, {std::move(output)})) {}
const LensManifest& IdentityLens::manifest() const noexcept { return manifest_; }
Result<void> IdentityLens::view(const OpticalInput& input,
                                WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  for (const auto& cell : input.incident().cells(manifest_.inputs.front().name)) {
    const std::array<FieldCellId, 1> cause{cell.id};
    auto emitted = outgoing.emit(manifest_.outputs.front().name, cell.key, cell.value,
                                 cause, cell.priority);
    if (!emitted) return tl::unexpected(emitted.error());
  }
  return {};
}
Result<RefractionResult> IdentityLens::refract(const PhotonWindow&, const Act&,
                                               RefractionBeam&) { return pass(); }
void IdentityLens::request_stop() noexcept { stopping_.store(true); }

SplitterLens::SplitterLens(LensId id, OpticalPortSpec input,
                           std::vector<OpticalPortSpec> outputs)
    : manifest_(structural_manifest(std::move(id), "Splitter Lens",
          {std::move(input)}, std::move(outputs))) {}
const LensManifest& SplitterLens::manifest() const noexcept { return manifest_; }
Result<void> SplitterLens::view(const OpticalInput& input,
                                WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  for (const auto& cell : input.incident().cells(manifest_.inputs.front().name)) {
    const std::array<FieldCellId, 1> cause{cell.id};
    for (const auto& output : manifest_.outputs) {
      auto emitted = outgoing.emit(output.name, cell.key, cell.value, cause, cell.priority);
      if (!emitted) return tl::unexpected(emitted.error());
    }
  }
  return {};
}
Result<RefractionResult> SplitterLens::refract(const PhotonWindow&, const Act&,
                                               RefractionBeam&) { return pass(); }
void SplitterLens::request_stop() noexcept { stopping_.store(true); }

PrismLens::PrismLens(LensId id, OpticalPortSpec input,
                     std::vector<PrismBranch> branches)
    : branches_(std::move(branches)) {
  std::vector<OpticalPortSpec> outputs;
  outputs.reserve(branches_.size());
  for (const auto& branch : branches_) outputs.push_back(branch.output);
  manifest_ = structural_manifest(std::move(id), "Prism Lens",
                                  {std::move(input)}, std::move(outputs));
}
const LensManifest& PrismLens::manifest() const noexcept { return manifest_; }
Result<void> PrismLens::view(const OpticalInput& input,
                            WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  for (const auto& cell : input.incident().cells(manifest_.inputs.front().name)) {
    const std::array<FieldCellId, 1> cause{cell.id};
    for (const auto& branch : branches_) {
      if (!branch_matches(branch, cell)) continue;
      auto emitted = outgoing.emit(branch.output.name, cell.key, cell.value,
                                   cause, cell.priority);
      if (!emitted) return tl::unexpected(emitted.error());
    }
  }
  return {};
}
Result<RefractionResult> PrismLens::refract(const PhotonWindow&, const Act&,
                                           RefractionBeam&) { return pass(); }
void PrismLens::request_stop() noexcept { stopping_.store(true); }

MergeLens::MergeLens(LensId id, std::vector<OpticalPortSpec> inputs,
                     OpticalPortSpec output)
    : manifest_(structural_manifest(std::move(id), "Merge Lens",
          std::move(inputs), {std::move(output)})) {}
const LensManifest& MergeLens::manifest() const noexcept { return manifest_; }
Result<void> MergeLens::view(const OpticalInput& input,
                            WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  for (const auto& port : manifest_.inputs)
    for (const auto& cell : input.incident().cells(port.name)) {
      const std::array<FieldCellId, 1> cause{cell.id};
      auto emitted = outgoing.emit(manifest_.outputs.front().name, cell.key,
                                   cell.value, cause, cell.priority);
      if (!emitted) return tl::unexpected(emitted.error());
    }
  return {};
}
Result<RefractionResult> MergeLens::refract(const PhotonWindow&, const Act&,
                                           RefractionBeam&) { return pass(); }
void MergeLens::request_stop() noexcept { stopping_.store(true); }

ApertureLens::ApertureLens(LensId id, OpticalPortSpec input,
                           OpticalPortSpec output, const std::size_t limit)
    : manifest_(structural_manifest(std::move(id), "Aperture Lens",
          {std::move(input)}, {std::move(output), OpticalPortSpec{
              .name = "diagnostic", .band = "diagnostic.optical",
              .schema = "tokmon.optical.aperture.v1",
              .merge = MergeLaw::map_union_unique,
              .sensitivity = FieldSensitivity::normal,
              .surface = false}})), limit_(limit) {}
const LensManifest& ApertureLens::manifest() const noexcept { return manifest_; }
Result<void> ApertureLens::view(const OpticalInput& input,
                               WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  if (limit_ == 0u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "Aperture limit must be positive"));
  auto cells = input.incident().cells(manifest_.inputs.front().name);
  std::vector<const FieldCell*> selected;
  selected.reserve(cells.size());
  for (const auto& cell : cells) selected.push_back(&cell);
  std::ranges::sort(selected, [](const FieldCell* left, const FieldCell* right) {
    if (left->priority != right->priority) return left->priority > right->priority;
    if (left->provenance.path_index != right->provenance.path_index)
      return left->provenance.path_index < right->provenance.path_index;
    return left->id < right->id;
  });
  if (selected.size() > limit_) selected.resize(limit_);
  cbor::Value::Array selected_ids;
  for (const auto* cell : selected) {
    const std::array<FieldCellId, 1> cause{cell->id};
    auto emitted = outgoing.emit(manifest_.outputs.front().name, cell->key,
                                 cell->value, cause, cell->priority);
    if (!emitted) return tl::unexpected(emitted.error());
    selected_ids.emplace_back(*emitted);
  }
  cbor::Value::Array input_ids;
  std::vector<FieldCellId> causes;
  input_ids.reserve(cells.size());
  causes.reserve(cells.size());
  for (const auto& cell : cells) {
    input_ids.emplace_back(cell.id);
    causes.push_back(cell.id);
  }
  const auto input_hash = sha256_hex(cbor::encode(input_ids));
  const auto output_hash = sha256_hex(cbor::encode(selected_ids));
  auto diagnostic = outgoing.emit("diagnostic", manifest_.id,
      cbor::object({{"rule", "priority-path-cell-id"}, {"rule_version", 1},
          {"limit", static_cast<std::int64_t>(limit_)},
          {"input_count", static_cast<std::int64_t>(cells.size())},
          {"selected_count", static_cast<std::int64_t>(selected.size())},
          {"dropped_count", static_cast<std::int64_t>(cells.size() - selected.size())},
          {"input_hash", input_hash}, {"output_hash", output_hash}}), causes);
  if (!diagnostic) return tl::unexpected(diagnostic.error());
  return {};
}
Result<RefractionResult> ApertureLens::refract(const PhotonWindow&, const Act&,
                                              RefractionBeam&) { return pass(); }
void ApertureLens::request_stop() noexcept { stopping_.store(true); }

ProjectionLens::ProjectionLens(LensId id, OpticalPortSpec input,
                               OpticalPortSpec surface_output) {
  surface_output.surface = true;
  manifest_ = structural_manifest(std::move(id), "Projection Lens",
                                  {std::move(input)}, {std::move(surface_output)});
}
const LensManifest& ProjectionLens::manifest() const noexcept { return manifest_; }
Result<void> ProjectionLens::view(const OpticalInput& input,
                                 WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  for (const auto& cell : input.incident().cells(manifest_.inputs.front().name)) {
    const std::array<FieldCellId, 1> cause{cell.id};
    auto emitted = outgoing.emit(manifest_.outputs.front().name, cell.key,
                                 cell.value, cause, cell.priority);
    if (!emitted) return tl::unexpected(emitted.error());
  }
  return {};
}
Result<RefractionResult> ProjectionLens::refract(const PhotonWindow&, const Act&,
                                                RefractionBeam&) { return pass(); }
void ProjectionLens::request_stop() noexcept { stopping_.store(true); }

CausalDelayLens::CausalDelayLens(LensId id, OpticalPortSpec input,
                                 OpticalPortSpec output) {
  manifest_ = structural_manifest(std::move(id), "Causal Delay Lens",
                                  {std::move(input)}, {std::move(output)});
  manifest_.observes = {{"optical.delay-committed", "tokmon.optical.delay.v1"}};
  manifest_.refracts = {{"optical.delay.commit", "tokmon.optical.delay.commit.v1"}};
  manifest_.light_permissions = {"photon.emit"};
  manifest_.trigger = TriggerPolicy::once_when_ready;
  manifest_.monotone = false;
}
const LensManifest& CausalDelayLens::manifest() const noexcept { return manifest_; }
Result<void> CausalDelayLens::view(const OpticalInput& input,
                                  WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  std::map<std::string, const Photon*, std::less<>> committed;
  for (const auto& photon : input.photons()) {
    if (photon.kind != "optical.delay-committed" ||
        photon.schema != "tokmon.optical.delay.v1") continue;
    const auto* lens = cbor::find(photon.payload, "lens_id");
    const auto* key = cbor::find(photon.payload, "delay_key");
    if (lens && key && lens->as_string() == manifest_.id)
      committed[std::string(key->as_string())] = &photon;
  }
  for (const auto& [delay_key, photon] : committed) {
    const auto* key = cbor::find(photon->payload, "key");
    const auto* value = cbor::find(photon->payload, "value");
    const auto* priority = cbor::find(photon->payload, "priority");
    if (!key || !value) continue;
    auto emitted = outgoing.emit(manifest_.outputs.front().name,
        std::string(key->as_string()), *value, {},
        priority ? static_cast<std::int32_t>(priority->as_integer()) : 0);
    if (!emitted) return tl::unexpected(emitted.error());
  }
  for (const auto& cell : input.incident().cells(manifest_.inputs.front().name)) {
    if (cell.sensitivity == FieldSensitivity::secret_reference &&
        !std::holds_alternative<std::string>(cell.value.data) &&
        !cbor::find(cell.value, "secret_ref"))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "CausalDelay accepts only opaque references on secret_reference bands"));
    const auto delay_key = sha256_hex(cbor::encode(cbor::object({
        {"lens", manifest_.id}, {"schema", cell.schema}, {"key", cell.key},
        {"value", cell.value}})));
    if (committed.contains(delay_key)) continue;
    Act act{.id = "act-delay-" + delay_key.substr(0, 32),
        .ray = input.beat().key.ray, .kind = "optical.delay.commit",
        .schema = "tokmon.optical.delay.commit.v1",
        .parameters = cbor::object({{"lens_id", manifest_.id},
            {"delay_key", delay_key}, {"key", cell.key}, {"value", cell.value},
            {"priority", cell.priority}}),
        .target = manifest_.id, .epoch = input.beat().key.epoch,
        .risk = RiskClass::reversible, .idempotency_key = delay_key};
    const std::array<FieldCellId, 1> cause{cell.id};
    if (auto proposed = outgoing.propose(std::move(act), cause); !proposed)
      return proposed;
  }
  return {};
}
Result<RefractionResult> CausalDelayLens::refract(const PhotonWindow&,
                                                 const Act& act,
                                                 RefractionBeam& beam) {
  if (!manifest_.refracts.front().matches(act)) return pass();
  const auto* lens = cbor::find(act.parameters, "lens_id");
  const auto* delay_key = cbor::find(act.parameters, "delay_key");
  const auto* key = cbor::find(act.parameters, "key");
  const auto* value = cbor::find(act.parameters, "value");
  if (!lens || !delay_key || !key || !value || lens->as_string() != manifest_.id)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "CausalDelay Act is incomplete"));
  auto emitted = beam.emit("optical.delay-committed", "tokmon.optical.delay.v1",
      cbor::object({{"lens_id", manifest_.id}, {"delay_key", *delay_key},
          {"key", *key}, {"value", *value},
          {"priority", cbor::find(act.parameters, "priority")
              ? *cbor::find(act.parameters, "priority")
              : cbor::Value(std::int64_t{0})}}));
  if (!emitted) return tl::unexpected(emitted.error());
  return RefractionResult{.status = RefractionStatus::completed,
                           .emitted = {emitted->id}, .detail = "delay committed"};
}
void CausalDelayLens::request_stop() noexcept { stopping_.store(true); }

Result<std::shared_ptr<OpticalAssemblyLens>> OpticalAssemblyLens::create(
    LensManifest boundary, std::vector<MountedLens> internal_lenses,
    OpticalAssemblySpec assembly) {
  if (boundary.id.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "OpticalAssemblyLens id is required"));
  for (const auto& binding : assembly.inputs) {
    const auto* outer = port_named(boundary.inputs, binding.assembly_port);
    const auto target = std::ranges::find_if(internal_lenses,
        [&binding](const MountedLens& mounted) {
          return mounted.lens && mounted.lens->manifest().id == binding.to.lens;
        });
    const auto* inner = target == internal_lenses.end() ? nullptr
        : port_named(target->lens->manifest().inputs, binding.to.port);
    if (!outer || !inner || !compatible(*outer, *inner))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "OpticalAssemblyLens input boundary mismatch"));
  }
  for (const auto& binding : assembly.outputs) {
    const auto* outer = port_named(boundary.outputs, binding.assembly_port);
    const auto source = std::ranges::find_if(internal_lenses,
        [&binding](const MountedLens& mounted) {
          return mounted.lens && mounted.lens->manifest().id == binding.from.lens;
        });
    const auto* inner = source == internal_lenses.end() ? nullptr
        : port_named(source->lens->manifest().outputs, binding.from.port);
    if (!outer || !inner || !compatible(*outer, *inner))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "OpticalAssemblyLens output boundary mismatch"));
  }
  for (const auto& input : boundary.inputs)
    if (!std::ranges::any_of(assembly.inputs, [&input](const auto& binding) {
          return binding.assembly_port == input.name;
        }))
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "OpticalAssemblyLens input is not bound"));
  for (const auto& output : boundary.outputs)
    if (!std::ranges::any_of(assembly.outputs, [&output](const auto& binding) {
          return binding.assembly_port == output.name;
        }))
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "OpticalAssemblyLens output is not bound"));
  auto compiled = compile_optical_assembly(0, internal_lenses, assembly);
  if (!compiled) return tl::unexpected(compiled.error());
  std::set<std::pair<std::string, std::string>> patterns;
  for (const auto& pattern : boundary.refracts)
    patterns.emplace(pattern.kind, pattern.schema);
  for (const auto& mounted : internal_lenses)
    for (const auto& pattern : mounted.lens->manifest().refracts)
      if (patterns.emplace(pattern.kind, pattern.schema).second)
        boundary.refracts.push_back(pattern);
  boundary.abi_major = 2;
  boundary.stateless = std::ranges::all_of(internal_lenses,
      [](const MountedLens& mounted) { return mounted.lens->manifest().stateless; });
  return std::shared_ptr<OpticalAssemblyLens>(new OpticalAssemblyLens(
      std::move(boundary), std::move(internal_lenses), std::move(assembly),
      std::move(*compiled)));
}

OpticalAssemblyLens::OpticalAssemblyLens(
    LensManifest boundary, std::vector<MountedLens> internal_lenses,
    OpticalAssemblySpec assembly,
    std::shared_ptr<const OpticalAssemblySnapshot> compiled)
    : manifest_(std::move(boundary)), lenses_(std::move(internal_lenses)),
      spec_(std::move(assembly)), assembly_(std::move(compiled)) {}
OpticalAssemblyLens::~OpticalAssemblyLens() { request_stop(); }
const LensManifest& OpticalAssemblyLens::manifest() const noexcept { return manifest_; }

Result<void> OpticalAssemblyLens::view(const OpticalInput& input,
                                      WavefrontBuilder& outgoing) {
  if (auto status = active(stopping_, input.beat(), manifest_.id); !status) return status;
  OpticalPropagator propagator;
  auto result = propagator.propagate(input.beat().key.ray, input.photon_window(),
                                     lenses_, *assembly_, &input.incident());
  if (!result) return tl::unexpected(result.error());
  std::unordered_map<FieldCellId, const FieldCell*> inner_cells;
  for (const auto& [band, cells] : result->wavefront.bands()) {
    (void)band;
    for (const auto& cell : cells) inner_cells.emplace(cell.id, &cell);
  }
  const auto outer_ids = input.incident().cell_ids();
  const std::set<FieldCellId> outer(outer_ids.begin(), outer_ids.end());
  const auto roots_for = [&](const FieldCellId& id) {
    std::set<FieldCellId> roots;
    std::set<FieldCellId> visiting;
    std::function<void(const FieldCellId&)> visit = [&](const FieldCellId& current) {
      if (outer.contains(current)) { roots.insert(current); return; }
      if (!visiting.insert(current).second) return;
      const auto found = inner_cells.find(current);
      if (found != inner_cells.end())
        for (const auto& cause : found->second->provenance.input_cells) visit(cause);
      visiting.erase(current);
    };
    visit(id);
    return std::vector<FieldCellId>(roots.begin(), roots.end());
  };
  for (const auto& binding : assembly_->outputs) {
    auto cells = result->wavefront.select(
        lenses_[binding.from_lens].lens->manifest().id, binding.from_port);
    for (const auto& cell : cells) {
      auto roots = roots_for(cell.id);
      auto emitted = outgoing.emit(binding.assembly_port, cell.key, cell.value,
                                   roots, cell.priority);
      if (!emitted) return tl::unexpected(emitted.error());
    }
  }
  for (auto act : result->surface.proposals) {
    auto roots = roots_for(act.proposal_cell);
    const auto internal_target = act.target;
    auto parameters = act.parameters.as_map();
    if (!parameters) act.parameters = cbor::Value::Map{};
    (*act.parameters.as_map())["_assembly_target"] = internal_target;
    act.target = manifest_.id;
    if (auto proposed = outgoing.propose(std::move(act), roots); !proposed)
      return proposed;
  }
  return {};
}

Result<RefractionResult> OpticalAssemblyLens::refract(
    const PhotonWindow& photons, const Act& act, RefractionBeam& beam) {
  if (stopping_.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     manifest_.id + " is stopping"));
  Act routed = act;
  std::string target;
  if (const auto* field = cbor::find(routed.parameters, "_assembly_target"))
    target = std::string(field->as_string());
  if (auto* parameters = routed.parameters.as_map()) parameters->erase("_assembly_target");
  const MountedLens* selected = nullptr;
  for (const auto& mounted : lenses_) {
    if (!target.empty() && mounted.lens->manifest().id != target) continue;
    if (!std::ranges::any_of(mounted.lens->manifest().refracts,
        [&routed](const ActPattern& pattern) { return pattern.matches(routed); }))
      continue;
    if (selected)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "composite Act routing is ambiguous"));
    selected = &mounted;
  }
  if (!selected)
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "composite Act target was not found"));
  routed.target = selected->lens->manifest().id;
  routed.generation = selected->generation;
  return selected->lens->refract(photons, routed, beam);
}

void OpticalAssemblyLens::request_stop() noexcept {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  for (const auto& mounted : lenses_) mounted.lens->request_stop();
}

}  // namespace tokmon
