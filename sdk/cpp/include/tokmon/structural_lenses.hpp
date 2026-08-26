#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "tokmon/optical_assembly.hpp"

namespace tokmon {

struct PrismBranch {
  OpticalPortSpec output;
  std::string key_prefix;
  std::string value_field;
  cbor::Value equals;
};

class IdentityLens final : public ILens {
 public:
  IdentityLens(LensId id, OpticalPortSpec input, OpticalPortSpec output);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

class SplitterLens final : public ILens {
 public:
  SplitterLens(LensId id, OpticalPortSpec input,
               std::vector<OpticalPortSpec> outputs);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

class PrismLens final : public ILens {
 public:
  PrismLens(LensId id, OpticalPortSpec input, std::vector<PrismBranch> branches);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::vector<PrismBranch> branches_;
  std::atomic_bool stopping_{false};
};

class MergeLens final : public ILens {
 public:
  MergeLens(LensId id, std::vector<OpticalPortSpec> inputs,
            OpticalPortSpec output);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

class ApertureLens final : public ILens {
 public:
  ApertureLens(LensId id, OpticalPortSpec input, OpticalPortSpec output,
               std::size_t limit);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::size_t limit_{0};
  std::atomic_bool stopping_{false};
};

class ProjectionLens final : public ILens {
 public:
  ProjectionLens(LensId id, OpticalPortSpec input, OpticalPortSpec surface_output);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

class CausalDelayLens final : public ILens {
 public:
  CausalDelayLens(LensId id, OpticalPortSpec input, OpticalPortSpec output);
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;
 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

class OpticalAssemblyLens final : public ILens {
 public:
  static Result<std::shared_ptr<OpticalAssemblyLens>> create(
      LensManifest boundary, std::vector<MountedLens> internal_lenses,
      OpticalAssemblySpec assembly);
  ~OpticalAssemblyLens() override;
  [[nodiscard]] const LensManifest& manifest() const noexcept override;
  Result<void> view(const OpticalInput&, WavefrontBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&,
                                   RefractionBeam&) override;
  void request_stop() noexcept override;

 private:
  OpticalAssemblyLens(LensManifest boundary,
                      std::vector<MountedLens> internal_lenses,
                      OpticalAssemblySpec assembly,
                      std::shared_ptr<const OpticalAssemblySnapshot> compiled);
  LensManifest manifest_;
  std::vector<MountedLens> lenses_;
  OpticalAssemblySpec spec_;
  std::shared_ptr<const OpticalAssemblySnapshot> assembly_;
  std::atomic_bool stopping_{false};
};

}  // namespace tokmon
