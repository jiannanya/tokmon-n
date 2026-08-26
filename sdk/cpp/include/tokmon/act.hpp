#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "tokmon/cbor.hpp"
#include "tokmon/ids.hpp"

namespace tokmon {

enum class RiskClass : std::uint8_t {
  observe,
  reversible,
  external,
  external_irreversible,
};

struct Act {
  ActId id;
  RayId ray;
  std::string kind;
  std::string schema;
  cbor::Value parameters{cbor::Value::Map{}};
  LensId target;
  MountEpoch epoch{0};
  GenerationId generation{0};
  RiskClass risk{RiskClass::observe};
  bool approved{false};
  std::string idempotency_key;
  std::chrono::milliseconds timeout{30'000};
  std::string assembly_hash;
  FieldCellId proposal_cell;
  std::vector<FieldCellId> optical_inputs;
};

struct ActPattern {
  std::string kind;
  std::string schema;
  [[nodiscard]] bool matches(const Act& act) const noexcept;
};

[[nodiscard]] std::string_view to_string(RiskClass risk) noexcept;
[[nodiscard]] cbor::Value to_cbor(const Act& act);
[[nodiscard]] Result<Act> act_from_cbor(const cbor::Value& value);
[[nodiscard]] std::string act_binding_hash(const Act& act);
// Stable consumer identity used before an opaque one-shot binding id exists.
// Only the binding carrier fields are omitted; every effect-bearing field stays bound.
[[nodiscard]] std::string act_secret_scope_hash(const Act& act);

}  // namespace tokmon
