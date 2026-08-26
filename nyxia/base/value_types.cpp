#include "tokmon/act.hpp"
#include "tokmon/lens.hpp"
#include "tokmon/photon.hpp"
#include "tokmon/surface.hpp"
#include "tokmon/hash.hpp"

#include <algorithm>
#include <limits>

namespace tokmon {
namespace {

std::string required_string(const cbor::Value& value, const std::string_view key) {
  if (const auto* field = cbor::find(value, key)) return std::string(field->as_string());
  return {};
}

std::int64_t required_integer(const cbor::Value& value, const std::string_view key) {
  if (const auto* field = cbor::find(value, key)) return field->as_integer();
  return 0;
}

}  // namespace

bool PhotonPattern::matches(const Photon& photon) const noexcept {
  const bool kind_matches = kind.empty() || kind == "*" || photon.kind == kind ||
      (kind.back() == '*' && photon.kind.starts_with(kind.substr(0, kind.size() - 1u)));
  const bool schema_matches = schema.empty() || schema == "*" || photon.schema == schema;
  return kind_matches && schema_matches;
}

PhotonWindow::PhotonWindow(std::vector<Photon> photons) : photons_(std::move(photons)) {}
const std::vector<Photon>& PhotonWindow::photons() const noexcept { return photons_; }
const Photon* PhotonWindow::latest() const noexcept {
  return photons_.empty() ? nullptr : &photons_.back();
}
const Photon* PhotonWindow::latest(const std::string_view kind) const noexcept {
  const auto iterator = std::find_if(photons_.rbegin(), photons_.rend(),
      [kind](const Photon& photon) { return photon.kind == kind; });
  return iterator == photons_.rend() ? nullptr : &*iterator;
}
bool PhotonWindow::contains_after(const std::string_view kind,
                                  const std::uint64_t sequence) const noexcept {
  return std::any_of(photons_.begin(), photons_.end(), [=](const Photon& photon) {
    return photon.sequence > sequence && photon.kind == kind;
  });
}

cbor::Value to_cbor(const Photon& photon) {
  return cbor::object({
      {"sequence", static_cast<std::int64_t>(photon.sequence)},
      {"id", photon.id}, {"ray", photon.ray},
      {"parent", photon.parent ? cbor::Value(*photon.parent) : cbor::Value(nullptr)},
      {"kind", photon.kind}, {"schema", photon.schema}, {"payload", photon.payload},
      {"epoch", static_cast<std::int64_t>(photon.epoch)},
      {"committed_at_ms", photon.committed_at_ms}, {"previous_hash", photon.previous_hash},
      {"hash", photon.hash}, {"caused_by_act", photon.caused_by_act}});
}

Result<Photon> photon_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch, "Photon must be a map"));
  Photon photon;
  photon.sequence = static_cast<std::uint64_t>(required_integer(value, "sequence"));
  photon.id = required_string(value, "id");
  photon.ray = required_string(value, "ray");
  if (const auto* parent = cbor::find(value, "parent"); parent && !parent->is_null())
    photon.parent = std::string(parent->as_string());
  photon.kind = required_string(value, "kind");
  photon.schema = required_string(value, "schema");
  if (const auto* payload = cbor::find(value, "payload")) photon.payload = *payload;
  photon.epoch = static_cast<MountEpoch>(required_integer(value, "epoch"));
  photon.committed_at_ms = required_integer(value, "committed_at_ms");
  photon.previous_hash = required_string(value, "previous_hash");
  photon.hash = required_string(value, "hash");
  photon.caused_by_act = required_string(value, "caused_by_act");
  if (photon.id.empty() || photon.ray.empty() || photon.kind.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Photon id, ray and kind are required"));
  return photon;
}

cbor::Value to_cbor(const PhotonWindow& window) {
  cbor::Value::Array photons;
  photons.reserve(window.photons().size());
  for (const auto& photon : window.photons()) photons.push_back(to_cbor(photon));
  return cbor::object({{"photons", std::move(photons)}});
}

Result<PhotonWindow> photon_window_from_cbor(const cbor::Value& value) {
  const auto* field = cbor::find(value, "photons");
  if (field == nullptr || field->as_array() == nullptr)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "PhotonWindow.photons must be an array"));
  std::vector<Photon> photons;
  photons.reserve(field->as_array()->size());
  for (const auto& item : *field->as_array()) {
    auto photon = photon_from_cbor(item);
    if (!photon) return tl::unexpected(photon.error());
    photons.push_back(std::move(*photon));
  }
  return PhotonWindow(std::move(photons));
}

bool ActPattern::matches(const Act& act) const noexcept {
  const bool kind_matches = kind.empty() || kind == "*" || kind == act.kind ||
      (kind.back() == '*' && act.kind.starts_with(kind.substr(0, kind.size() - 1u)));
  return kind_matches && (schema.empty() || schema == "*" || schema == act.schema);
}

std::string_view to_string(const RiskClass risk) noexcept {
  switch (risk) {
    case RiskClass::observe: return "observe";
    case RiskClass::reversible: return "reversible";
    case RiskClass::external: return "external";
    case RiskClass::external_irreversible: return "external_irreversible";
  }
  return "observe";
}

cbor::Value to_cbor(const Act& act) {
  cbor::Value::Array optical_inputs;
  optical_inputs.reserve(act.optical_inputs.size());
  for (const auto& id : act.optical_inputs) optical_inputs.emplace_back(id);
  return cbor::object({
      {"id", act.id}, {"ray", act.ray}, {"kind", act.kind}, {"schema", act.schema},
      {"parameters", act.parameters}, {"target", act.target},
      {"epoch", static_cast<std::int64_t>(act.epoch)},
      {"generation", static_cast<std::int64_t>(act.generation)},
      {"risk", std::string(to_string(act.risk))}, {"approved", act.approved},
      {"idempotency_key", act.idempotency_key},
      {"timeout_ms", static_cast<std::int64_t>(act.timeout.count())},
      {"assembly_hash", act.assembly_hash}, {"proposal_cell", act.proposal_cell},
      {"optical_inputs", std::move(optical_inputs)}});
}

Result<Act> act_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch, "Act must be a map"));
  Act act;
  act.id = required_string(value, "id"); act.ray = required_string(value, "ray");
  act.kind = required_string(value, "kind"); act.schema = required_string(value, "schema");
  if (const auto* parameters = cbor::find(value, "parameters")) act.parameters = *parameters;
  act.target = required_string(value, "target");
  act.epoch = static_cast<MountEpoch>(required_integer(value, "epoch"));
  act.generation = static_cast<GenerationId>(required_integer(value, "generation"));
  const auto risk = required_string(value, "risk");
  if (risk == "reversible") act.risk = RiskClass::reversible;
  else if (risk == "external") act.risk = RiskClass::external;
  else if (risk == "external_irreversible") act.risk = RiskClass::external_irreversible;
  if (const auto* approved = cbor::find(value, "approved")) act.approved = approved->as_bool();
  act.idempotency_key = required_string(value, "idempotency_key");
  act.timeout = std::chrono::milliseconds(required_integer(value, "timeout_ms"));
  act.assembly_hash = required_string(value, "assembly_hash");
  act.proposal_cell = required_string(value, "proposal_cell");
  if (const auto* inputs = cbor::find(value, "optical_inputs"); inputs && inputs->as_array())
    for (const auto& input : *inputs->as_array())
      act.optical_inputs.emplace_back(input.as_string());
  if (act.id.empty() || act.ray.empty() || act.kind.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Act id, ray and kind are required"));
  return act;
}

std::string act_binding_hash(const Act& act) {
  cbor::Value::Array optical_inputs;
  optical_inputs.reserve(act.optical_inputs.size());
  for (const auto& id : act.optical_inputs) optical_inputs.emplace_back(id);
  return sha256_hex(cbor::encode(cbor::object({
      {"id", act.id}, {"ray", act.ray}, {"kind", act.kind}, {"schema", act.schema},
      {"parameters", act.parameters}, {"target", act.target},
      {"epoch", static_cast<std::int64_t>(act.epoch)},
      {"generation", static_cast<std::int64_t>(act.generation)},
      {"risk", std::string(to_string(act.risk))},
      {"idempotency_key", act.idempotency_key},
      {"timeout_ms", static_cast<std::int64_t>(act.timeout.count())},
      {"assembly_hash", act.assembly_hash}, {"proposal_cell", act.proposal_cell},
      {"optical_inputs", std::move(optical_inputs)}})));
}

std::string act_secret_scope_hash(const Act& act) {
  auto parameters = act.parameters;
  if (auto* map = parameters.as_map()) {
    map->erase("secret_binding");
    // The opaque one-shot binding identifier is a transport carrier rather than
    // part of the requested effect.  Keep every semantic field (notably the
    // environment target and purpose) in the scope so that a binding issued for
    // one secret placement cannot be replayed for a different placement.
    if (auto found = map->find("secret_bindings"); found != map->end()) {
      if (const auto* bindings = found->second.as_array()) {
        auto sanitized = *bindings;
        for (auto& binding : sanitized)
          if (auto* entry = binding.as_map()) entry->erase("binding_id");
        found->second = cbor::Value(std::move(sanitized));
      }
    }
  }
  cbor::Value::Array optical_inputs;
  optical_inputs.reserve(act.optical_inputs.size());
  for (const auto& id : act.optical_inputs) optical_inputs.emplace_back(id);
  return sha256_hex(cbor::encode(cbor::object({
      {"id", act.id}, {"ray", act.ray}, {"kind", act.kind}, {"schema", act.schema},
      {"parameters", std::move(parameters)}, {"target", act.target},
      {"epoch", static_cast<std::int64_t>(act.epoch)},
      {"generation", static_cast<std::int64_t>(act.generation)},
      {"risk", std::string(to_string(act.risk))},
      {"idempotency_key", act.idempotency_key},
      {"timeout_ms", static_cast<std::int64_t>(act.timeout.count())},
      {"assembly_hash", act.assembly_hash}, {"proposal_cell", act.proposal_cell},
      {"optical_inputs", std::move(optical_inputs)}})));
}

cbor::Value to_cbor(const SurfaceSnapshot& surface) {
  cbor::Value::Array contributions;
  for (const auto& item : surface.contributions) {
    cbor::Value::Array inputs;
    for (const auto& id : item.input_cells) inputs.emplace_back(id);
    contributions.push_back(cbor::object({{"lens", item.lens},
        {"generation", static_cast<std::int64_t>(item.generation)},
        {"channel", item.channel}, {"key", item.key}, {"value", item.value},
        {"priority", item.priority}, {"field_cell", item.field_cell},
        {"input_cells", std::move(inputs)}, {"assembly_hash", item.assembly_hash}}));
  }
  cbor::Value::Array proposals;
  for (const auto& act : surface.proposals) proposals.push_back(to_cbor(act));
  return cbor::object({{"epoch", static_cast<std::int64_t>(surface.epoch)},
                       {"assembly_hash", surface.assembly_hash},
                       {"wavefront_hash", surface.wavefront_hash},
                       {"wavefront_cells", static_cast<std::int64_t>(surface.wavefront_cells)},
                       {"propagation_rounds", static_cast<std::int64_t>(surface.propagation_rounds)},
                       {"contributions", std::move(contributions)},
                       {"proposals", std::move(proposals)}});
}

Result<SurfaceSnapshot> surface_from_cbor(const cbor::Value& value) {
  if (!value.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "SurfaceSnapshot must be a map"));
  SurfaceSnapshot surface;
  surface.epoch = static_cast<MountEpoch>(required_integer(value, "epoch"));
  surface.assembly_hash = required_string(value, "assembly_hash");
  surface.wavefront_hash = required_string(value, "wavefront_hash");
  surface.wavefront_cells = static_cast<std::size_t>(required_integer(value, "wavefront_cells"));
  surface.propagation_rounds = static_cast<std::size_t>(
      required_integer(value, "propagation_rounds"));
  if (const auto* items = cbor::find(value, "contributions"); items && items->as_array()) {
    for (const auto& item : *items->as_array()) {
      SurfaceContribution contribution;
      contribution.lens = required_string(item, "lens");
      contribution.generation = static_cast<GenerationId>(required_integer(item, "generation"));
      contribution.channel = required_string(item, "channel");
      contribution.key = required_string(item, "key");
      if (const auto* field = cbor::find(item, "value")) contribution.value = *field;
      contribution.priority = static_cast<std::int32_t>(required_integer(item, "priority"));
      contribution.field_cell = required_string(item, "field_cell");
      contribution.assembly_hash = required_string(item, "assembly_hash");
      if (const auto* inputs = cbor::find(item, "input_cells"); inputs && inputs->as_array())
        for (const auto& input : *inputs->as_array())
          contribution.input_cells.emplace_back(input.as_string());
      surface.contributions.push_back(std::move(contribution));
    }
  }
  if (const auto* items = cbor::find(value, "proposals"); items && items->as_array()) {
    for (const auto& item : *items->as_array()) {
      auto act = act_from_cbor(item);
      if (!act) return tl::unexpected(act.error());
      surface.proposals.push_back(std::move(*act));
    }
  }
  return surface;
}

RefractionBeam::RefractionBeam(OpticalHost& host, Act act, std::stop_token stop,
                               const std::chrono::steady_clock::time_point deadline)
    : host_(host), act_(std::move(act)), stop_(stop), deadline_(deadline) {}
const Act& RefractionBeam::act() const noexcept { return act_; }
std::stop_token RefractionBeam::stop_token() const noexcept { return stop_; }
bool RefractionBeam::stop_requested() const noexcept { return stop_.stop_requested(); }
bool RefractionBeam::expired() const noexcept {
  return std::chrono::steady_clock::now() >= deadline_;
}
Result<Photon> RefractionBeam::emit(std::string kind, std::string schema,
                                    cbor::Value payload) {
  if (stop_requested())
    return tl::unexpected(make_error(ErrorCode::cancelled, "beam cancelled"));
  if (expired())
    return tl::unexpected(make_error(ErrorCode::timeout, "beam deadline exceeded"));
  return host_.emit(PhotonDraft{.ray = act_.ray, .kind = std::move(kind),
      .schema = std::move(schema), .payload = std::move(payload), .epoch = act_.epoch,
      .caused_by_act = act_.id});
}
Result<Photon> RefractionBeam::emit_to(const RayId& ray, std::string kind,
                                       std::string schema, cbor::Value payload,
                                       PhotonId parent) {
  if (ray.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "cross-ray emission requires a ray"));
  if (stop_requested())
    return tl::unexpected(make_error(ErrorCode::cancelled, "beam cancelled"));
  if (expired())
    return tl::unexpected(make_error(ErrorCode::timeout, "beam deadline exceeded"));
  return host_.emit(PhotonDraft{.ray = ray, .parent = std::move(parent),
      .kind = std::move(kind), .schema = std::move(schema), .payload = std::move(payload),
      .epoch = act_.epoch, .caused_by_act = act_.id});
}
void RefractionBeam::log(const std::string_view level, const std::string_view message) const {
  host_.log(level, message, act_.target);
}

std::string_view to_string(const RuntimeKind kind) noexcept {
  switch (kind) {
    case RuntimeKind::in_process: return "in_process";
    case RuntimeKind::native_worker: return "native_worker";
    case RuntimeKind::node: return "node";
    case RuntimeKind::cpython: return "cpython";
    case RuntimeKind::wasm: return "wasm";
    case RuntimeKind::desktop: return "desktop";
  }
  return "in_process";
}
std::string_view to_string(const RefractionStatus status) noexcept {
  switch (status) {
    case RefractionStatus::passed: return "passed";
    case RefractionStatus::completed: return "completed";
    case RefractionStatus::rejected: return "rejected";
    case RefractionStatus::failed: return "failed";
  }
  return "failed";
}

}  // namespace tokmon
