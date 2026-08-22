#pragma once
#include "lenses/common/lens_base.hpp"
namespace tokmon::builtin { class NotaLens final : public LensBase { public: NotaLens(); Result<void> view(const PhotonWindow&, SurfaceBuilder&) override; Result<RefractionResult> refract(const PhotonWindow&, const Act&, RefractionBeam&) override; }; }
