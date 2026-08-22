#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class CoveLens final : public LensBase { public: CoveLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
