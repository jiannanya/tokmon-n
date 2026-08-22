#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>

#include "tokmon/light_path.hpp"
#include "tokmon/photon_store.hpp"

namespace tokmon {

class ActPipeline;

class RayTracingEngine final : public OpticalHost {
 public:
  RayTracingEngine(PhotonStore& store, LightPath& path, BeamRegistry& beams);

  Result<Photon> emit(PhotonDraft draft) override;
  void log(std::string_view level, std::string_view message,
           const LensId& lens) override;

  Result<SurfaceSnapshot> view(const RayId& ray);
  Result<std::size_t> advance(const RayId& ray, std::size_t max_beats = 32);
  Result<RayId> begin(std::string input, MountEpoch epoch = 0);
  void request_stop() noexcept;
  void set_approval(std::function<bool(const Act&)> approval);

 private:
  Result<RefractionResult> execute(const PhotonWindow& window, Act act,
                                   const MountedLens& mounted);
  Result<void> audit_act(const Act& act, std::string kind,
                         cbor::Value payload = cbor::Value::Map{});

  PhotonStore& store_;
  LightPath& path_;
  BeamRegistry& beams_;
  std::atomic_bool stopping_{false};
  std::function<bool(const Act&)> approval_;
};

class ActPipeline {
 public:
  using Approval = std::function<bool(const Act&)>;
  explicit ActPipeline(Approval approval = {});
  Result<Act> admit(Act act, const LightPathSnapshot& path) const;

 private:
  Approval approval_;
};

}  // namespace tokmon

