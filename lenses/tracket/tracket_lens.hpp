#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class TracketLens final : public LensBase { public: TracketLens(); Result<void> view(const OpticalInput&, WavefrontBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
