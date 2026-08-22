#pragma once

#include <filesystem>
#include <memory>

#include "tokmon/lens.hpp"

namespace tokmon {

class DynamicLibrary;

class CAbiLens final : public ILens {
 public:
  ~CAbiLens() override;
  CAbiLens(const CAbiLens&) = delete;
  CAbiLens& operator=(const CAbiLens&) = delete;

  static Result<std::shared_ptr<CAbiLens>> load(const std::filesystem::path& path);

  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const PhotonWindow& photons, SurfaceBuilder& surface) override;
  Result<RefractionResult> refract(const PhotonWindow& photons, const Act& act,
                                   RefractionBeam& beam) override;
  void request_stop() noexcept override;

 private:
  CAbiLens();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon

