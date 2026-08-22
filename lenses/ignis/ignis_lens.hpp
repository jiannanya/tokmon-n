#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class IgnisLens final : public LensBase { public: IgnisLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
