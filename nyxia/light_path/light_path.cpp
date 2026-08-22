#include "tokmon/light_path.hpp"

#include <set>

#include "tokmon/hash.hpp"

namespace tokmon {

LightPath::LightPath() {
  active_.store(std::make_shared<const LightPathSnapshot>(), std::memory_order_release);
}

std::shared_ptr<const LightPathSnapshot> LightPath::snapshot() const noexcept {
  return active_.load(std::memory_order_acquire);
}

Result<void> LightPath::publish(std::shared_ptr<const LightPathSnapshot> candidate) {
  if (!candidate)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "LightPath candidate is null"));
  const auto current = snapshot();
  if (candidate->epoch <= current->epoch)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "LightPath epoch must increase"));
  std::set<LensId> ids;
  std::set<std::pair<std::string, std::string>> act_patterns;
  for (const auto& mounted : candidate->lenses) {
    if (!mounted.lens)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "LightPath contains a null Lens"));
    const auto& manifest = mounted.lens->manifest();
    if (!ids.insert(manifest.id).second)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "duplicate Lens id in LightPath: " + manifest.id));
    for (const auto& pattern : manifest.refracts) {
      if (pattern.kind == "*" || pattern.kind.ends_with('*')) continue;
      const auto key = std::pair(pattern.kind, pattern.schema);
      if (!act_patterns.insert(key).second)
        return tl::unexpected(make_error(ErrorCode::invalid_argument,
            "ambiguous ActPattern in LightPath: " + pattern.kind));
    }
  }
  active_.store(std::move(candidate), std::memory_order_release);
  return {};
}

std::shared_ptr<ILens> LightPath::find_target(const Act& act) const {
  const auto current = snapshot();
  for (const auto& mounted : current->lenses) {
    const auto& manifest = mounted.lens->manifest();
    if (!act.target.empty() && manifest.id != act.target) continue;
    for (const auto& pattern : manifest.refracts) {
      if (pattern.matches(act)) return mounted.lens;
    }
  }
  return {};
}

std::shared_ptr<ILens> LightPath::find(const LensId& id) const {
  const auto current = snapshot();
  for (const auto& mounted : current->lenses)
    if (mounted.lens->manifest().id == id) return mounted.lens;
  return {};
}

}  // namespace tokmon

