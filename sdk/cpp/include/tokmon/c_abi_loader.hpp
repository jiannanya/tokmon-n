#pragma once

#include <filesystem>
#include <memory>

#include "tokmon/lens.hpp"

namespace tokmon {

class DynamicLibrary;

class CAbiLens final : public ILens, public IOpticalLensExtension {
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
  [[nodiscard]] bool supports_derive() const noexcept override;
  [[nodiscard]] bool supports_coordinate() const noexcept override;
  [[nodiscard]] bool supports_query() const noexcept override;
  Result<cbor::Value> derive(const PhotonWindow& photons) override;
  Result<void> coordinate(const PhotonWindow& photons,
                          const OpticalContext& optical,
                          SurfaceBuilder& surface) override;
  Result<cbor::Value> optical_query(const FrozenLensState& state,
      std::string_view capability, const cbor::Value& parameters,
      const QueryBudget& budget) const override;

 private:
  CAbiLens();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon

