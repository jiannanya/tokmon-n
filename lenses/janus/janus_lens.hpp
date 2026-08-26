#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class JanusLens final : public LensBase, public IOpticalLensExtension { public: JanusLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; [[nodiscard]] bool supports_coordinate() const noexcept override; Result<void> coordinate(const PhotonWindow&, const OpticalContext&, SurfaceBuilder&) override; }; }
