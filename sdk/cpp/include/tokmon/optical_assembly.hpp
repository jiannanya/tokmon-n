#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tokmon/lens.hpp"
#include "tokmon/surface.hpp"
#include "tokmon/wavefront.hpp"

namespace tokmon {

struct OpticalEndpoint {
  LensId lens;
  PortName port;
};

struct OpticalConnectionSpec {
  OpticalEndpoint from;
  OpticalEndpoint to;
};

struct OpticalInputBindingSpec {
  PortName assembly_port;
  OpticalEndpoint to;
};

struct OpticalOutputBindingSpec {
  OpticalEndpoint from;
  PortName assembly_port;
};

struct ResonatorSpec {
  AssemblyId id;
  std::vector<LensId> lenses;
  OpticalBudget budget;
};

struct OpticalAssemblySpec {
  AssemblyId id{"org.tokmon.assembly.active-light-path"};
  std::vector<OpticalConnectionSpec> connections;
  std::vector<OpticalInputBindingSpec> inputs;
  std::vector<OpticalOutputBindingSpec> outputs;
  std::vector<ResonatorSpec> resonators;
  OpticalBudget budget;
  bool autowire_unique{true};
};

struct CompiledConnection {
  std::size_t from_lens{0};
  PortName from_port;
  std::size_t to_lens{0};
  PortName to_port;
};

struct CompiledInputBinding {
  PortName assembly_port;
  std::size_t to_lens{0};
  PortName to_port;
};

struct CompiledOutputBinding {
  std::size_t from_lens{0};
  PortName from_port;
  PortName assembly_port;
};

struct PropagationStep {
  std::vector<std::size_t> lenses;
  bool resonator{false};
  ResonatorSpec resonance;
};

struct OpticalAssemblySnapshot {
  AssemblyId id;
  MountEpoch epoch{0};
  std::string hash;
  OpticalBudget budget;
  std::vector<CompiledConnection> connections;
  std::vector<CompiledInputBinding> inputs;
  std::vector<CompiledOutputBinding> outputs;
  std::vector<std::vector<PropagationStep>> layers;
};

struct OpticalTraceEntry {
  LensId lens;
  GenerationId generation{0};
  std::size_t path_index{0};
  std::uint32_t round{0};
  std::size_t input_cells{0};
  std::size_t output_cells{0};
  std::size_t output_bytes{0};
  bool cache_hit{false};
  std::chrono::microseconds duration{0};
  std::string status{"completed"};
  std::string detail;
};

struct OpticalBeatResult {
  Wavefront wavefront;
  SurfaceSnapshot surface;
  std::vector<OpticalTraceEntry> trace;
};

[[nodiscard]] Result<std::shared_ptr<const OpticalAssemblySnapshot>>
compile_optical_assembly(MountEpoch epoch,
                         const std::vector<MountedLens>& lenses,
                         const OpticalAssemblySpec& spec = {});

class OpticalPropagator {
 public:
  [[nodiscard]] Result<OpticalBeatResult> propagate(
      const RayId& ray, const PhotonWindow& photons,
      const std::vector<MountedLens>& lenses,
      const OpticalAssemblySnapshot& assembly,
      const IncidentWave* external_incident = nullptr) const;
};

[[nodiscard]] cbor::Value to_cbor(const OpticalAssemblySnapshot& assembly);

}  // namespace tokmon
