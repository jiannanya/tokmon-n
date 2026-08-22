#include "tokmon/light_path.hpp"

#include <set>
#include <unordered_map>

#include "tokmon/hash.hpp"

namespace tokmon {
namespace {

bool version_matches(const std::string_view constraint, const std::string_view actual) {
  if (constraint.empty() || constraint == "*") return true;
  if (constraint.starts_with("==")) return actual == constraint.substr(2);
  if (constraint.starts_with('^')) {
    const auto requested = constraint.substr(1);
    const auto requested_major = requested.substr(0, requested.find('.'));
    const auto actual_major = actual.substr(0, actual.find('.'));
    return !requested_major.empty() && requested_major == actual_major && actual >= requested;
  }
  return actual == constraint;
}

}  // namespace

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
  std::unordered_map<LensId, std::size_t> positions;
  for (std::size_t index = 0; index < candidate->lenses.size(); ++index) {
    if (candidate->lenses[index].lens)
      positions[candidate->lenses[index].lens->manifest().id] = index;
  }
  for (const auto& mounted : candidate->lenses) {
    if (!mounted.lens)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "LightPath contains a null Lens"));
    const auto& manifest = mounted.lens->manifest();
    if (!ids.insert(manifest.id).second)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "duplicate Lens id in LightPath: " + manifest.id));
    std::set<std::string> permissions;
    for (const auto& permission : manifest.light_permissions)
      if (permission.empty() || !permissions.insert(permission).second)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "Lens permissions must be unique and non-empty: " + manifest.id));
    for (const auto& dependency : manifest.dependencies) {
      const auto found = positions.find(dependency.id);
      if (found == positions.end())
        return tl::unexpected(make_error(ErrorCode::not_found,
            manifest.id + " requires missing Lens " + dependency.id));
      const auto& actual = candidate->lenses[found->second].lens->manifest().version;
      if (!version_matches(dependency.version, actual))
        return tl::unexpected(make_error(ErrorCode::integrity_error,
            manifest.id + " requires " + dependency.id + " " + dependency.version +
            ", mounted version is " + actual));
    }
    for (const auto& conflict : manifest.conflicts)
      if (positions.contains(conflict))
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            manifest.id + " conflicts with mounted Lens " + conflict));
    for (const auto& before : manifest.optical_before)
      if (const auto found = positions.find(before);
          found != positions.end() && positions.at(manifest.id) >= found->second)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            manifest.id + " must appear before " + before));
    for (const auto& after : manifest.optical_after)
      if (const auto found = positions.find(after);
          found != positions.end() && positions.at(manifest.id) <= found->second)
        return tl::unexpected(make_error(ErrorCode::invalid_state,
            manifest.id + " must appear after " + after));
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
