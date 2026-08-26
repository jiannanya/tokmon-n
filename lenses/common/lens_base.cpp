#include "lenses/common/lens_base.hpp"

#include <algorithm>

#include "tokmon/hash.hpp"
#include "tokmon/ids.hpp"

namespace tokmon::builtin {

LensBase::LensBase(LensManifest manifest) : manifest_(std::move(manifest)) {}
const LensManifest& LensBase::manifest() const noexcept { return manifest_; }
void LensBase::request_stop() noexcept { stopping_.store(true, std::memory_order_release); }

Result<void> LensBase::ready() const {
  if (stopping_.load(std::memory_order_acquire))
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     manifest_.id + " is stopping"));
  return {};
}

bool LensBase::accepts(const Act& act) const noexcept {
  return std::any_of(manifest_.refracts.begin(), manifest_.refracts.end(),
      [&act](const ActPattern& pattern) { return pattern.matches(act); });
}

Result<void> LensBase::identify(WavefrontBuilder& outgoing, std::string channel,
                                cbor::Value detail) const {
  auto map = detail.as_map();
  if (!map) detail = cbor::Value::Map{};
  map = detail.as_map();
  (*map)["lens_id"] = manifest_.id;
  (*map)["version"] = manifest_.version;
  return outgoing.add(std::move(channel), manifest_.id, std::move(detail), -100);
}

Result<RefractionResult> LensBase::emit(RefractionBeam& beam, std::string kind,
                                        std::string schema, cbor::Value payload,
                                        std::string detail) const {
  if (auto status = ready(); !status) return tl::unexpected(status.error());
  if (beam.stop_requested())
    return tl::unexpected(make_error(ErrorCode::cancelled, "beam cancelled"));
  auto photon = beam.emit(std::move(kind), std::move(schema), std::move(payload));
  if (!photon) return tl::unexpected(photon.error());
  return RefractionResult{.status = RefractionStatus::completed,
                           .emitted = {photon->id}, .detail = std::move(detail)};
}

Act LensBase::propose(const Photon& source, std::string kind, std::string schema,
                      std::string target, cbor::Value parameters,
                      const RiskClass risk) {
  const auto identity = sha256_hex(cbor::encode(cbor::object({
      {"source", source.id}, {"ray", source.ray}, {"kind", kind},
      {"schema", schema}, {"target", target}, {"parameters", parameters},
      {"epoch", static_cast<std::int64_t>(source.epoch)}})));
  return Act{.id = "act-" + identity.substr(0, 32), .ray = source.ray,
      .kind = std::move(kind), .schema = std::move(schema),
      .parameters = std::move(parameters), .target = std::move(target),
      .epoch = source.epoch, .risk = risk,
      .idempotency_key = identity};
}

std::string LensBase::text(const Photon& photon, const std::string_view field) {
  const auto* value = cbor::find(photon.payload, field);
  return value ? std::string(value->as_string()) : std::string{};
}

LensManifest LensBase::make_manifest(std::string_view short_id,
    std::string display_name, std::vector<std::string> channels,
    std::vector<PhotonPattern> observes, std::vector<ActPattern> refracts,
    std::vector<std::string> permissions, const RuntimeKind runtime) {
  LensManifest manifest{.id = "org.tokmon.lens." + std::string(short_id),
      .display_name = std::move(display_name), .version = "0.1.0",
      .runtime = runtime, .trust = TrustLevel::t1,
      .observes = std::move(observes),
      .refracts = std::move(refracts), .light_permissions = std::move(permissions),
      .stateless = true};
  manifest.outputs.reserve(channels.size());
  for (auto& channel : channels) {
    manifest.outputs.push_back(OpticalPortSpec{
        .name = channel,
        .band = channel,
        .schema = "tokmon.surface.contribution.v1",
        .cardinality = PortCardinality::many,
        .requirement = PortRequirement::optional,
        .merge = MergeLaw::stable_concat,
        .surface = true});
  }
  return manifest;
}

void LensBase::set_optical_ports(std::vector<OpticalPortSpec> inputs,
                                 std::vector<OpticalPortSpec> outputs,
                                 const TriggerPolicy trigger,
                                 const bool monotone) {
  manifest_.inputs = std::move(inputs);
  manifest_.outputs = std::move(outputs);
  manifest_.trigger = trigger;
  manifest_.monotone = monotone;
}

}  // namespace tokmon::builtin
