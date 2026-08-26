#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class RheaLens final : public LensBase { public: RheaLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
