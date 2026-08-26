#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/act.hpp"
#include "tokmon/error.hpp"
#include "tokmon/photon.hpp"

namespace tokmon {

enum class PortCardinality : std::uint8_t { one, many };
enum class PortRequirement : std::uint8_t { optional, required };
enum class MergeLaw : std::uint8_t {
  set_union,
  map_union_unique,
  priority_then_path,
  top_k,
  product,
  optional_single,
  stable_concat,
};
enum class TriggerPolicy : std::uint8_t {
  once_when_ready,
  on_delta,
  on_seal,
  per_key_join,
};
enum class FieldSensitivity : std::uint8_t {
  public_data,
  normal,
  sensitive,
  secret_reference,
};

struct OpticalBudget {
  std::size_t max_cells{16'384};
  std::size_t max_bytes{16u * 1024u * 1024u};
  std::size_t max_cell_bytes{1024u * 1024u};
  std::size_t max_lens_executions{4'096};
  std::uint32_t max_rounds{8};
  std::chrono::milliseconds deadline{5'000};
};

struct OpticalPortSpec {
  PortName name;
  BandId band;
  std::string schema{"*"};
  PortCardinality cardinality{PortCardinality::many};
  PortRequirement requirement{PortRequirement::optional};
  MergeLaw merge{MergeLaw::set_union};
  FieldSensitivity sensitivity{FieldSensitivity::normal};
  std::uint8_t maximum_trust_tier{3};
  std::vector<LensId> allowed_audiences;
  std::string redaction_policy{"none"};
  bool exportable{true};
  bool transient_handle{false};
  std::size_t max_cells{1'024};
  std::size_t max_cell_bytes{1024u * 1024u};
  bool surface{false};

  bool operator==(const OpticalPortSpec&) const = default;
};

struct BeatKey {
  RayId ray;
  MountEpoch epoch{0};
  std::string input_prefix_hash;
  std::string assembly_hash;

  [[nodiscard]] std::string canonical() const;
};

struct FieldProvenance {
  LensId producer;
  GenerationId generation{0};
  MountEpoch epoch{0};
  std::size_t path_index{0};
  PortName output_port;
  std::vector<FieldCellId> input_cells;
  std::vector<PhotonId> input_photons;
  std::string assembly_hash;
};

struct FieldCell {
  FieldCellId id;
  BandId band;
  std::string schema;
  std::string key;
  cbor::Value value;
  std::int32_t priority{0};
  bool surface{false};
  FieldSensitivity sensitivity{FieldSensitivity::normal};
  std::uint8_t producer_trust{1};
  std::vector<LensId> allowed_audiences;
  std::string redaction_policy{"none"};
  bool exportable{true};
  bool transient_handle{false};
  FieldProvenance provenance;
};

class IncidentWave {
 public:
  IncidentWave() = default;

  [[nodiscard]] const std::vector<FieldCell>& cells(PortName port) const noexcept;
  [[nodiscard]] const FieldCell* one(PortName port) const noexcept;
  [[nodiscard]] bool connected(PortName port) const noexcept;
  [[nodiscard]] bool sealed(PortName port) const noexcept;
  [[nodiscard]] std::vector<FieldCellId> cell_ids() const;
  [[nodiscard]] std::string canonical_hash() const;

 private:
  friend class OpticalPropagator;
  friend cbor::Value to_cbor(const IncidentWave& wave);
  friend Result<IncidentWave> incident_wave_from_cbor(const cbor::Value& value);
  Result<void> connect(PortName port, std::vector<FieldCell> cells,
                       const OpticalPortSpec& spec, bool sealed = true);

  std::map<PortName, std::vector<FieldCell>, std::less<>> ports_;
  std::set<PortName, std::less<>> sealed_;
};

struct BeatContext {
  BeatKey key;
  OpticalBudget budget;
  std::uint32_t round{0};
  std::chrono::steady_clock::time_point deadline{};

  [[nodiscard]] bool expired() const noexcept;
};

class OpticalInput {
 public:
  OpticalInput(const PhotonWindow& photons, const IncidentWave& incident,
               const BeatContext& beat) noexcept;

  [[nodiscard]] const PhotonWindow& photon_window() const noexcept;
  [[nodiscard]] const IncidentWave& incident() const noexcept;
  [[nodiscard]] const BeatContext& beat() const noexcept;

  // PhotonWindow forwarding keeps Lens business code concise while preserving
  // the single new view(OpticalInput, WavefrontBuilder) interface.
  [[nodiscard]] const std::vector<Photon>& photons() const noexcept;
  [[nodiscard]] const Photon* latest() const noexcept;
  [[nodiscard]] const Photon* latest(std::string_view kind) const noexcept;
  [[nodiscard]] bool contains_after(std::string_view kind,
                                    std::uint64_t sequence) const noexcept;

 private:
  const PhotonWindow& photons_;
  const IncidentWave& incident_;
  const BeatContext& beat_;
};

class WavefrontBuilder {
 public:
  WavefrontBuilder(LensId source, GenerationId generation,
                   std::size_t path_index,
                   std::vector<OpticalPortSpec> outputs,
                   BeatContext context,
                   std::vector<FieldCellId> visible_inputs = {},
                   std::vector<PhotonId> visible_photons = {},
                   std::uint8_t source_trust = 1);

  Result<FieldCellId> emit(PortName output, std::string key,
                           cbor::Value value,
                           std::span<const FieldCellId> caused_by = {},
                           std::int32_t priority = 0);
  Result<void> add(std::string channel, std::string key, cbor::Value value,
                   std::int32_t priority = 0);
  Result<void> propose(Act act,
                       std::span<const FieldCellId> caused_by = {});

  [[nodiscard]] const std::vector<FieldCell>& cells() const noexcept;
  [[nodiscard]] std::size_t bytes() const noexcept;

 private:
  [[nodiscard]] const OpticalPortSpec* output_spec(
      std::string_view output) const noexcept;
  Result<FieldCellId> append_cell(const OpticalPortSpec& spec,
                                  std::string key, cbor::Value value,
                                  std::span<const FieldCellId> caused_by,
                                  std::int32_t priority);

  LensId source_;
  GenerationId generation_{0};
  std::size_t path_index_{0};
  std::vector<OpticalPortSpec> outputs_;
  BeatContext context_;
  std::set<FieldCellId, std::less<>> visible_inputs_;
  std::vector<PhotonId> visible_photons_;
  std::uint8_t source_trust_{1};
  std::vector<FieldCell> cells_;
  std::size_t bytes_{0};
};

class Wavefront {
 public:
  explicit Wavefront(BeatKey key = {});

  Result<bool> merge(FieldCell cell, MergeLaw law = MergeLaw::set_union,
                     std::size_t max_cells = 0);
  [[nodiscard]] const std::vector<FieldCell>& band(BandId band) const noexcept;
  [[nodiscard]] std::vector<FieldCell> select(const LensId& producer,
                                              PortName output) const;
  [[nodiscard]] const std::map<BandId, std::vector<FieldCell>, std::less<>>&
      bands() const noexcept;
  [[nodiscard]] bool contains(const FieldCellId& id) const noexcept;
  [[nodiscard]] std::size_t cell_count() const noexcept;
  [[nodiscard]] std::size_t bytes() const noexcept;
  [[nodiscard]] std::string canonical_hash() const;
  [[nodiscard]] const BeatKey& beat() const noexcept;

 private:
  BeatKey key_;
  std::map<BandId, std::vector<FieldCell>, std::less<>> bands_;
  std::set<FieldCellId, std::less<>> ids_;
  std::size_t bytes_{0};
};

[[nodiscard]] std::string_view to_string(PortCardinality value) noexcept;
[[nodiscard]] std::string_view to_string(PortRequirement value) noexcept;
[[nodiscard]] std::string_view to_string(MergeLaw value) noexcept;
[[nodiscard]] std::string_view to_string(TriggerPolicy value) noexcept;
[[nodiscard]] std::string_view to_string(FieldSensitivity value) noexcept;
[[nodiscard]] Result<PortCardinality> port_cardinality_from_string(std::string_view value);
[[nodiscard]] Result<PortRequirement> port_requirement_from_string(std::string_view value);
[[nodiscard]] Result<MergeLaw> merge_law_from_string(std::string_view value);
[[nodiscard]] Result<TriggerPolicy> trigger_policy_from_string(std::string_view value);
[[nodiscard]] Result<FieldSensitivity> field_sensitivity_from_string(std::string_view value);

[[nodiscard]] cbor::Value to_cbor(const OpticalPortSpec& port);
[[nodiscard]] Result<OpticalPortSpec> optical_port_spec_from_cbor(
    const cbor::Value& value);
[[nodiscard]] cbor::Value to_cbor(const FieldCell& cell);
[[nodiscard]] Result<FieldCell> field_cell_from_cbor(const cbor::Value& value);
[[nodiscard]] cbor::Value to_cbor(const IncidentWave& wave);
[[nodiscard]] Result<IncidentWave> incident_wave_from_cbor(
    const cbor::Value& value);
[[nodiscard]] cbor::Value to_cbor(const BeatContext& beat);
[[nodiscard]] Result<BeatContext> beat_context_from_cbor(
    const cbor::Value& value);
[[nodiscard]] cbor::Value to_cbor(const OpticalInput& input);
[[nodiscard]] cbor::Value to_cbor(const Wavefront& wavefront);

}  // namespace tokmon
