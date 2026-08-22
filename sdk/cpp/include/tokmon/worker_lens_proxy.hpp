#pragma once

#include <chrono>
#include <filesystem>
#include <memory>

#include "tokmon/lens.hpp"

namespace tokmon {

struct WorkerLensOptions {
  LensManifest manifest;
  std::filesystem::path supervisor;
  std::filesystem::path runtime_executable;
  std::filesystem::path adapter;
  std::filesystem::path entry;
  std::chrono::milliseconds startup_timeout{10'000};
};

class WorkerLensProxy final : public ILens {
 public:
  ~WorkerLensProxy() override;
  WorkerLensProxy(const WorkerLensProxy&) = delete;
  WorkerLensProxy& operator=(const WorkerLensProxy&) = delete;

  [[nodiscard]] static Result<std::shared_ptr<WorkerLensProxy>> launch(
      WorkerLensOptions options);

  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const PhotonWindow& photons, SurfaceBuilder& surface) override;
  Result<RefractionResult> refract(const PhotonWindow& photons, const Act& act,
                                   RefractionBeam& beam) override;
  void request_stop() noexcept override;

 private:
  WorkerLensProxy();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon
