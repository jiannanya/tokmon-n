#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class StyxLens final : public LensBase { public: StyxLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
