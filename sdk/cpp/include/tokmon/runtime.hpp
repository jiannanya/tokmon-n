#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include "tokmon/config.hpp"
#include "tokmon/engine.hpp"

namespace tokmon {

class TokmonRuntime {
 public:
  TokmonRuntime();
  ~TokmonRuntime();
  TokmonRuntime(const TokmonRuntime&) = delete;
  TokmonRuntime& operator=(const TokmonRuntime&) = delete;

  Result<void> open(const std::optional<std::filesystem::path>& workspace = std::nullopt,
                    std::string_view process_name = "tokmon");
  Result<void> reload_configuration();
  Result<void> reconcile();
  Result<RayId> submit(std::string input);
  Result<RayId> submit(std::string input, cbor::Value context);
  Result<RayId> submit_to(const RayId& ray, std::string input);
  Result<RayId> submit_to(const RayId& ray, std::string input, cbor::Value context);
  Result<RefractionResult> refract(Act act);
  Result<std::size_t> advance(const RayId& ray);
  void cancel(const RayId& ray) noexcept;
  Result<std::vector<Photon>> history(const RayId& ray) const;
  Result<std::vector<Photon>> history_all(std::uint64_t after = 0) const;
  Result<SurfaceSnapshot> surface(const RayId& ray);
  Result<void> verify() const;
  void stop() noexcept;

  [[nodiscard]] const RuntimeConfig& config() const noexcept;
  [[nodiscard]] std::shared_ptr<const LightPathSnapshot> light_path() const noexcept;
  [[nodiscard]] PhotonStore& store() noexcept;

 private:
  std::optional<std::filesystem::path> workspace_;
  RuntimeConfig config_;
  PhotonStore store_;
  LightPath path_;
  BeamRegistry beams_;
  std::unique_ptr<RayTracingEngine> engine_;
  std::string reconciled_configuration_hash_;
  std::mutex reconcile_mutex_;
  bool open_{false};
};

}  // namespace tokmon
