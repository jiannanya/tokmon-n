#pragma once

#include <memory>
#include <string_view>

#include "tokmon/tokmon.hpp"

namespace tokmon::tests {

inline Result<OpticalBeatResult> view_lens_once(
    const std::shared_ptr<ILens>& lens, const PhotonWindow& photons = {},
    const GenerationId generation = 1, const MountEpoch epoch = 1) {
  if (!lens)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "test Lens is null"));
  std::vector<MountedLens> lenses{{lens, generation, "test-artifact"}};
  auto assembly = compile_optical_assembly(epoch, lenses);
  if (!assembly) return tl::unexpected(assembly.error());
  const auto* latest = photons.latest();
  OpticalPropagator propagator;
  return propagator.propagate(latest ? latest->ray : "ray-optical-test",
                              photons, lenses, **assembly);
}

inline const SurfaceContribution* find_surface(
    const SurfaceSnapshot& surface, const std::string_view channel,
    const std::string_view key = {}) {
  const auto found = std::ranges::find_if(surface.contributions,
      [channel, key](const SurfaceContribution& contribution) {
        return contribution.channel == channel &&
            (key.empty() || contribution.key == key);
      });
  return found == surface.contributions.end() ? nullptr : &*found;
}

}  // namespace tokmon::tests
