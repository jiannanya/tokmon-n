#include "tokmon/light_path.hpp"

#include <array>
#include <algorithm>
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

bool side_effect_query_name(const std::string_view capability) {
  static constexpr std::array<std::string_view, 14> forbidden{
      "fs.", "file.", "filesystem.", "process.", "model.call", "secret.",
      "keyring.", "mount.", "lens.mount", "lens.unmount", "http.", "network.",
      "child.spawn", "git."};
  return std::ranges::any_of(forbidden, [&](const std::string_view prefix) {
    return capability.starts_with(prefix);
  });
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
  std::unordered_map<std::string, std::vector<const OpticalQueryCapability*>> providers;
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
    std::set<std::string> query_names;
    const auto* optical = dynamic_cast<const IOpticalLensExtension*>(mounted.lens.get());
    for (const auto& capability : manifest.provides_queries) {
      if (capability.capability.empty() || capability.request_schema.empty() ||
          capability.response_schema.empty() ||
          !query_names.insert(capability.capability).second)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "Lens query capabilities must have unique names and schemas: " + manifest.id));
      if (!capability.deterministic || side_effect_query_name(capability.capability))
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            "synchronous query capability must be deterministic and side-effect free: " +
            capability.capability));
      if (!optical || !optical->supports_query())
        return tl::unexpected(make_error(ErrorCode::unsupported,
            manifest.id + " declares queries without an optical query extension"));
      providers[capability.capability].push_back(&capability);
    }
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
  for (const auto& [capability, declarations] : providers) {
    if (declarations.size() < 2u) continue;
    const auto* first = declarations.front();
    for (const auto* provider : declarations)
      if (provider->request_schema != first->request_schema ||
          provider->response_schema != first->response_schema)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "providers disagree on optical query schemas for " + capability));
  }
  for (const auto& mounted : candidate->lenses) {
    const auto& manifest = mounted.lens->manifest();
    const auto* optical = dynamic_cast<const IOpticalLensExtension*>(mounted.lens.get());
    if (!manifest.consumes_queries.empty() &&
        (!optical || !optical->supports_coordinate()))
      return tl::unexpected(make_error(ErrorCode::unsupported,
          manifest.id + " declares query consumption without a coordinate extension"));
    std::set<std::string> consumed;
    for (const auto& consumption : manifest.consumes_queries) {
      if (consumption.capability.empty() || !consumed.insert(consumption.capability).second)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "Lens consumed query capabilities must be unique and non-empty: " + manifest.id));
      const auto found = providers.find(consumption.capability);
      const auto count = found == providers.end() ? 0u : found->second.size();
      if (consumption.cardinality == OpticalQueryCardinality::single && count != 1u)
        return tl::unexpected(make_error(count == 0u ? ErrorCode::provider_not_found :
                                                      ErrorCode::ambiguous_provider,
            manifest.id + " requires exactly one provider for " + consumption.capability));
      if (consumption.cardinality == OpticalQueryCardinality::optional_single && count > 1u)
        return tl::unexpected(make_error(ErrorCode::ambiguous_provider,
            manifest.id + " allows at most one provider for " + consumption.capability));
      if (consumption.required && count == 0u)
        return tl::unexpected(make_error(ErrorCode::provider_not_found,
            manifest.id + " requires provider for " + consumption.capability));
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
