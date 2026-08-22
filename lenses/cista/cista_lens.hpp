#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class CistaLens final : public LensBase { public: CistaLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
