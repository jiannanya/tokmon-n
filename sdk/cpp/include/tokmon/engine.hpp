#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

#include "tokmon/light_path.hpp"
#include "tokmon/photon_store.hpp"

namespace tokmon {

class ActPipeline;
enum class AdmissionDecision : std::uint8_t { allow, ask, deny };

class RayTracingEngine final : public OpticalHost {
 public:
  RayTracingEngine(PhotonStore& store, LightPath& path, BeamRegistry& beams);

  Result<Photon> emit(PhotonDraft draft) override;
  void log(std::string_view level, std::string_view message,
           const LensId& lens) override;

  Result<SurfaceSnapshot> view(const RayId& ray);
  Result<std::size_t> advance(const RayId& ray, std::size_t max_beats = 32);
  Result<RayId> begin(std::string input, MountEpoch epoch = 0);
  Result<Photon> continue_ray(const RayId& ray, std::string input,
                              MountEpoch epoch = 0);
  void cancel_ray(const RayId& ray) noexcept;
  void request_stop() noexcept;
  void set_admission(std::function<AdmissionDecision(const Act&)> admission);

 private:
  Result<RefractionResult> execute(const PhotonWindow& window, Act act,
                                   const MountedLens& mounted);
  Result<void> audit_act(const Act& act, std::string kind,
                         cbor::Value payload = cbor::Value::Map{});

  PhotonStore& store_;
  LightPath& path_;
  BeamRegistry& beams_;
  std::atomic_bool stopping_{false};
  mutable std::mutex cancelled_mutex_;
  std::set<RayId, std::less<>> cancelled_rays_;
  std::function<AdmissionDecision(const Act&)> admission_;
};

class ActPipeline {
 public:
  using Admission = std::function<AdmissionDecision(const Act&)>;
  explicit ActPipeline(Admission admission = {});
  Result<Act> admit(Act act, const LightPathSnapshot& path) const;

 private:
  Admission admission_;
};

}  // namespace tokmon
