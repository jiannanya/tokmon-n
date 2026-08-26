#pragma once

#include <string>
#include <vector>

#include "tokmon/act.hpp"
#include "tokmon/cbor.hpp"
#include "tokmon/ids.hpp"

namespace tokmon {

struct SurfaceContribution {
  LensId lens;
  GenerationId generation{0};
  std::string channel;
  std::string key;
  cbor::Value value;
  std::int32_t priority{0};
  FieldCellId field_cell;
  std::vector<FieldCellId> input_cells;
  std::string assembly_hash;
};

struct SurfaceSnapshot {
  MountEpoch epoch{0};
  std::string assembly_hash;
  std::string wavefront_hash;
  std::size_t wavefront_cells{0};
  std::size_t propagation_rounds{0};
  std::vector<SurfaceContribution> contributions;
  std::vector<Act> proposals;
};

[[nodiscard]] cbor::Value to_cbor(const SurfaceSnapshot& surface);
[[nodiscard]] Result<SurfaceSnapshot> surface_from_cbor(const cbor::Value& value);

}  // namespace tokmon
