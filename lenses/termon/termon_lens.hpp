#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class TermonLens final : public LensBase { public: TermonLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
