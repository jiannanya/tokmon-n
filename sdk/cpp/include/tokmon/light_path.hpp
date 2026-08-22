#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "tokmon/error.hpp"
#include "tokmon/ids.hpp"
#include "tokmon/lens.hpp"

namespace tokmon {

struct MountedLens {
  std::shared_ptr<ILens> lens;
  GenerationId generation{0};
  std::string artifact_hash;
};

struct LightPathSnapshot {
  MountEpoch epoch{0};
  std::string hash;
  std::vector<MountedLens> lenses;
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
    GenerationId generation{0};
    std::stop_source stop;
    std::chrono::steady_clock::time_point deadline;
  };

  [[nodiscard]] std::shared_ptr<Ticket> acquire(const LensId& lens,
                                                GenerationId generation,
                                                std::chrono::milliseconds timeout);
  void release(const std::string& ticket_id);
  std::size_t stop_generation(const LensId& lens, GenerationId generation);
  [[nodiscard]] std::size_t active(const LensId& lens,
                                   GenerationId generation) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Ticket>> tickets_;
};

}  // namespace tokmon

