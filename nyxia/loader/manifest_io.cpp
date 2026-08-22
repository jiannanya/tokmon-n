#include "tokmon/manifest_io.hpp"

#include <set>

#include <yaml-cpp/yaml.h>

namespace tokmon {
namespace {

Result<void> reject_unknown(const YAML::Node& map,
                            const std::set<std::string>& allowed,
                            const std::filesystem::path& source) {
  if (!map || !map.IsMap())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + " must contain a YAML map"));
  for (const auto& field : map) {
    const auto key = field.first.as<std::string>();
    if (!allowed.contains(key))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "unknown Lens manifest field '" + key + "' in " + source.string()));
  }
  return {};
}

RuntimeKind runtime_kind(const std::string_view text) {
  if (text == "native_worker") return RuntimeKind::native_worker;
  if (text == "node") return RuntimeKind::node;
  if (text == "cpython") return RuntimeKind::cpython;
  if (text == "wasm") return RuntimeKind::wasm;
  if (text == "desktop") return RuntimeKind::desktop;
  return RuntimeKind::in_process;
}

Result<std::vector<PhotonPattern>> photon_patterns(const YAML::Node& sequence,
                                                   const std::filesystem::path& source) {
  if (!sequence || !sequence.IsSequence())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": observes must be a sequence"));
  std::vector<PhotonPattern> patterns;
  for (const auto& entry : sequence) {
    if (auto checked = reject_unknown(entry, {"kind", "schema"}, source); !checked)
      return tl::unexpected(checked.error());
    if (!entry["kind"] || !entry["schema"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "observe pattern requires kind and schema"));
    patterns.push_back({entry["kind"].as<std::string>(),
                        entry["schema"].as<std::string>()});
  }
  return patterns;
}

Result<std::vector<ActPattern>> act_patterns(const YAML::Node& sequence,
                                             const std::filesystem::path& source) {
  if (!sequence || !sequence.IsSequence())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": refracts must be a sequence"));
  std::vector<ActPattern> patterns;
  for (const auto& entry : sequence) {
    if (auto checked = reject_unknown(entry, {"kind", "schema"}, source); !checked)
      return tl::unexpected(checked.error());
    if (!entry["kind"] || !entry["schema"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "refract pattern requires kind and schema"));
    patterns.push_back({entry["kind"].as<std::string>(),
                        entry["schema"].as<std::string>()});
  }
  return patterns;
}

std::vector<std::string> strings(const YAML::Node& sequence) {
  std::vector<std::string> result;
  if (sequence && sequence.IsSequence())
    for (const auto& value : sequence) result.push_back(value.as<std::string>());
  return result;
}

Result<std::vector<LensDependency>> dependencies(const YAML::Node& sequence,
                                                  const std::filesystem::path& source) {
  std::vector<LensDependency> result;
  if (!sequence) return result;
  if (!sequence.IsSequence())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": dependencies must be a sequence"));
  std::set<std::string> seen;
  for (const auto& item : sequence) {
    if (auto checked = reject_unknown(item, {"id", "version"}, source); !checked)
      return tl::unexpected(checked.error());
    if (!item["id"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens dependency requires id"));
    LensDependency dependency{item["id"].as<std::string>(),
        item["version"] ? item["version"].as<std::string>() : "*"};
    if (dependency.id.empty() || !seen.insert(dependency.id).second)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens dependencies must have unique non-empty ids"));
    result.push_back(std::move(dependency));
  }
  return result;
}

}  // namespace

Result<LensManifest> load_lens_manifest(const std::filesystem::path& path) {
  try {
    const auto root = YAML::LoadFile(path.string());
    if (auto checked = reject_unknown(root,
        {"api", "id", "display_name", "version", "abi", "runtime", "trust", "stateless",
         "observes", "view_channels", "refracts", "light_permissions", "dependencies",
         "conflicts", "optical_order", "resources", "replacement", "schema_bundle", "sbom"}, path);
        !checked) return tl::unexpected(checked.error());
    LensManifest manifest;
    if (!root["id"] || !root["display_name"] || !root["version"] || !root["abi"] ||
        !root["runtime"] || !root["observes"] || !root["view_channels"] ||
        !root["refracts"] || !root["light_permissions"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       path.string() + ": required manifest field is missing"));
    manifest.id = root["id"].as<std::string>();
    manifest.display_name = root["display_name"].as<std::string>();
    manifest.version = root["version"].as<std::string>();
    const auto abi = root["abi"];
    if (!abi.IsMap() || !abi["major"] || !abi["minor"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "manifest abi requires major and minor"));
    manifest.abi_major = abi["major"].as<std::uint32_t>();
    manifest.abi_minor = abi["minor"].as<std::uint32_t>();
    const auto runtime = root["runtime"];
    if (!runtime.IsMap() || !runtime["kind"])
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "manifest runtime.kind is required"));
    if (auto checked = reject_unknown(runtime, {"kind", "version", "entry", "module"}, path);
        !checked) return tl::unexpected(checked.error());
    const auto runtime_text = runtime["kind"].as<std::string>();
    manifest.runtime = runtime_kind(runtime_text);
    if (to_string(manifest.runtime) != runtime_text)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "unknown Lens runtime kind: " + runtime_text));
    if (runtime["version"])
      manifest.runtime_version = runtime["version"].as<std::string>();
    if (runtime["entry"])
      manifest.runtime_entry = runtime["entry"].as<std::string>();
    if ((manifest.runtime == RuntimeKind::node ||
         manifest.runtime == RuntimeKind::cpython) && manifest.runtime_entry.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "language Lens manifest runtime.entry is required"));
    const auto trust = root["trust"] ? root["trust"].as<std::string>() : "t1";
    if (trust == "t0") manifest.trust = TrustLevel::t0;
    else if (trust == "t1") manifest.trust = TrustLevel::t1;
    else if (trust == "t2") manifest.trust = TrustLevel::t2;
    else if (trust == "t3") manifest.trust = TrustLevel::t3;
    else return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                          "unknown Lens trust level: " + trust));
    manifest.stateless = !root["stateless"] || root["stateless"].as<bool>();
    if (root["api"] && root["api"].as<std::string>() != "tokmon.lens/v1")
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "unsupported Lens manifest api"));
    auto observes = photon_patterns(root["observes"], path);
    auto refracts = act_patterns(root["refracts"], path);
    if (!observes) return tl::unexpected(observes.error());
    if (!refracts) return tl::unexpected(refracts.error());
    manifest.observes = std::move(*observes);
    manifest.refracts = std::move(*refracts);
    manifest.view_channels = strings(root["view_channels"]);
    manifest.light_permissions = strings(root["light_permissions"]);
    auto required_lenses = dependencies(root["dependencies"], path);
    if (!required_lenses) return tl::unexpected(required_lenses.error());
    manifest.dependencies = std::move(*required_lenses);
    manifest.conflicts = strings(root["conflicts"]);
    if (const auto order = root["optical_order"]) {
      if (auto checked = reject_unknown(order, {"before", "after"}, path); !checked)
        return tl::unexpected(checked.error());
      manifest.optical_before = strings(order["before"]);
      manifest.optical_after = strings(order["after"]);
    }
    if (const auto resources = root["resources"]) {
      if (auto checked = reject_unknown(resources,
          {"memory_mb", "output_bytes", "deadline_ms"}, path); !checked)
        return tl::unexpected(checked.error());
      if (resources["memory_mb"])
        manifest.resources.memory_mb = resources["memory_mb"].as<std::size_t>();
      if (resources["output_bytes"])
        manifest.resources.output_bytes = resources["output_bytes"].as<std::size_t>();
      if (resources["deadline_ms"])
        manifest.resources.deadline = std::chrono::milliseconds(
            resources["deadline_ms"].as<std::int64_t>());
      if (manifest.resources.memory_mb == 0 || manifest.resources.memory_mb > 65'536u ||
          manifest.resources.output_bytes == 0 ||
          manifest.resources.output_bytes > 1024u * 1024u * 1024u ||
          manifest.resources.deadline <= std::chrono::milliseconds::zero() ||
          manifest.resources.deadline > std::chrono::hours(24))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "Lens resource limits are outside safe bounds"));
    }
    if (root["replacement"]) manifest.replacement = root["replacement"].as<std::string>();
    if (manifest.replacement != "R1" && manifest.replacement != "R2")
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens replacement must be R1 or R2"));
    if (root["schema_bundle"])
      manifest.schema_bundle = root["schema_bundle"].as<std::string>();
    if (root["sbom"]) manifest.sbom = root["sbom"].as<std::string>();
    if (manifest.id.empty() || manifest.display_name.empty() ||
        manifest.abi_major != 1u || manifest.view_channels.empty() ||
        manifest.refracts.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens manifest identity or contract is invalid"));
    return manifest;
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot parse Lens manifest " + path.string() + ": " + exception.what()));
  }
}

}  // namespace tokmon
