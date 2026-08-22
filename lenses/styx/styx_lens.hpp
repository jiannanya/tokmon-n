#pragma once
#include <memory>

#include "lenses/common/lens_base.hpp"

namespace tokmon::builtin {

class StyxLens final : public LensBase {
 public:
  StyxLens();
  ~StyxLens() override;
  Result<void> view(const PhotonWindow&, SurfaceBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon::builtin
