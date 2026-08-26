#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class ClothoLens final : public LensBase { public: ClothoLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
