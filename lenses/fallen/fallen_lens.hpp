#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class FallenLens final : public LensBase { public: FallenLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
