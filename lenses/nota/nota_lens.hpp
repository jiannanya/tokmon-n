#pragma once
#include <memory>

#include "lenses/common/lens_base.hpp"

namespace tokmon::builtin {

class NotaLens final : public LensBase {
 public:
  NotaLens();
  ~NotaLens() override;
  Result<void> view(const PhotonWindow&, SurfaceBuilder&) override;
  Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override;
  void request_stop() noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon::builtin
