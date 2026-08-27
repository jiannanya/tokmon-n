#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "tokmon/error.hpp"
#include "tokmon/ids.hpp"
#include "tokmon/lens.hpp"
#include "tokmon/optical_assembly.hpp"

namespace tokmon {

struct LightPathSnapshot {
  MountEpoch epoch{0};
  std::string hash;
  std::vector<MountedLens> lenses;
  OpticalAssemblySpec optical;
  std::shared_ptr<const OpticalAssemblySnapshot> assembly;
};

class LightPath {
 public:
  LightPath();
  [[nodiscard]] std::shared_ptr<const LightPathSnapshot> snapshot() const noexcept;
  Result<void> publish(std::shared_ptr<const LightPathSnapshot> candidate);
  [[nodiscard]] std::shared_ptr<ILens> find_target(const Act& act) const;
  [[nodiscard]] std::shared_ptr<ILens> find(const LensId& id) const;

 private:
  std::atomic<std::shared_ptr<const LightPathSnapshot>> active_;
};

class BeamRegistry {
 public:
  struct Ticket {
    std::string id;
    LensId lens;
    RayId ray;
    GenerationId generation{0};
    std::stop_source stop;
    std::chrono::steady_clock::time_point deadline;
  };

  [[nodiscard]] Result<std::shared_ptr<Ticket>> acquire(
      const LensId& lens, GenerationId generation, RayId ray,
      std::chrono::milliseconds timeout);
  void release(const std::string& ticket_id);
  // Closing and admission share the same mutex, making the transition a hard
  // gate: once this returns, no new Beam can target the generation.
  std::size_t stop_generation(const LensId& lens, GenerationId generation);
  void reopen_generation(const LensId& lens, GenerationId generation);
  std::size_t stop_ray(const RayId& ray);
  [[nodiscard]] std::size_t active(const LensId& lens,
                                   GenerationId generation) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Ticket>> tickets_;
  std::set<std::pair<LensId, GenerationId>> closed_generations_;
};

}  // namespace tokmon
