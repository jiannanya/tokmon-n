#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class AyaLens final : public LensBase { public: AyaLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
