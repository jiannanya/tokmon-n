#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class ChoraLens final : public LensBase { public: ChoraLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
