#include "tokmon/config.hpp"

#include <cstdlib>
#include <set>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "tokmon/builtin_lens.hpp"

namespace tokmon {
namespace {

std::filesystem::path home_directory() {
#if defined(_WIN32)
  if (const auto* profile = std::getenv("USERPROFILE")) return profile;
  const auto* drive = std::getenv("HOMEDRIVE");
  const auto* path = std::getenv("HOMEPATH");
  if (drive && path) return std::string(drive) + path;
#else
  if (const auto* home = std::getenv("HOME")) return home;
#endif
  return std::filesystem::current_path();
}

Result<void> reject_unknown(const YAML::Node& map, const std::set<std::string>& allowed,
                            const std::filesystem::path& source) {
  if (!map || !map.IsMap()) return {};
  for (const auto& entry : map) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "unknown YAML field '" + key + "' in " + source.string()));
  }
  return {};
}

Result<RuntimeKind> parse_runtime(const std::string& text) {
  if (text == "in_process") return RuntimeKind::in_process;
  if (text == "native_worker") return RuntimeKind::native_worker;
  if (text == "node") return RuntimeKind::node;
  if (text == "cpython") return RuntimeKind::cpython;
  if (text == "wasm") return RuntimeKind::wasm;
  if (text == "desktop") return RuntimeKind::desktop;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown Lens runtime: " + text));
}

Result<void> merge_config_file(RuntimeConfig& config, const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  try {
    const auto root = YAML::LoadFile(source.string());
    if (!root.IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + " must contain a YAML map"));
    if (auto result = reject_unknown(root, {"logging", "engine", "security", "ui"}, source);
        !result) return result;
    if (const auto logging = root["logging"]) {
      if (auto result = reject_unknown(logging, {"level"}, source); !result) return result;
      if (logging["level"]) config.log_level = logging["level"].as<std::string>();
    }
    if (const auto engine = root["engine"]) {
      if (auto result = reject_unknown(engine, {"photon_window", "max_beats"}, source); !result)
        return result;
      if (engine["photon_window"]) config.photon_window = engine["photon_window"].as<std::size_t>();
      if (engine["max_beats"]) config.max_beats = engine["max_beats"].as<std::size_t>();
    }
    if (const auto security = root["security"]) {
      if (auto result = reject_unknown(security, {"require_signatures"}, source); !result)
        return result;
      if (security["require_signatures"])
        config.require_signatures = security["require_signatures"].as<bool>();
    }
    if (config.photon_window == 0 || config.photon_window > 100'000 ||
        config.max_beats == 0 || config.max_beats > 1024)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "engine limits are outside the allowed range"));
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot parse " + source.string() + ": " + exception.what()));
  }
}

Result<void> merge_light_path(std::vector<DesiredLens>& lenses,
                              const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  try {
    const auto root = YAML::LoadFile(source.string());
    if (auto result = reject_unknown(root, {"version", "lenses"}, source); !result) return result;
    const auto entries = root["lenses"];
    if (!entries || !entries.IsSequence())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": lenses must be a sequence"));
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < lenses.size(); ++i) index[lenses[i].id] = i;
    for (const auto& entry : entries) {
      if (auto result = reject_unknown(entry, {"id", "artifact", "enabled", "runtime"}, source);
          !result) return result;
      DesiredLens lens;
      lens.id = entry["id"].as<std::string>();
      lens.artifact = entry["artifact"] ? entry["artifact"].as<std::string>() : "builtin:" + lens.id;
      if (!lens.artifact.starts_with("builtin:")) {
        auto artifact = std::filesystem::path(lens.artifact);
        if (artifact.is_relative())
          lens.artifact = (source.parent_path() / artifact).lexically_normal().string();
      }
      lens.enabled = !entry["enabled"] || entry["enabled"].as<bool>();
      auto runtime = parse_runtime(entry["runtime"] ?
          entry["runtime"].as<std::string>() : "in_process");
      if (!runtime) return tl::unexpected(runtime.error());
      lens.runtime = *runtime;
      if (const auto found = index.find(lens.id); found != index.end()) lenses[found->second] = lens;
      else { index[lens.id] = lenses.size(); lenses.push_back(std::move(lens)); }
    }
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot parse " + source.string() + ": " + exception.what()));
  }
}

}  // namespace

Result<TokmonPaths> resolve_paths(const std::optional<std::filesystem::path>& workspace) {
  TokmonPaths paths;
  paths.user = home_directory() / ".tokmon";
  paths.project = (workspace ? std::filesystem::absolute(*workspace) :
                               std::filesystem::current_path()) / ".tokmon";
  paths.data = paths.user / "data";
  paths.database = paths.data / "photons.sqlite3";
  paths.logs = paths.user / "logs";
  paths.run = paths.user / "run";
  paths.runtimes = paths.user / "runtimes";
  return paths;
}

Result<void> ensure_directory_layout(const TokmonPaths& paths) {
  std::error_code error;
  const std::vector<std::filesystem::path> directories = {
      paths.user, paths.data, paths.data / "blobs", paths.data / "segments",
      paths.data / "checkpoints", paths.data / "artifacts", paths.user / "lenses" / "installed",
      paths.user / "lenses" / "quarantine", paths.runtimes / "node",
      paths.runtimes / "cpython", paths.runtimes / "environments", paths.logs,
      paths.run, paths.user / "cache" / "projection", paths.user / "cache" / "ui"};
  for (const auto& directory : directories) {
    std::filesystem::create_directories(directory, error);
    if (error)
      return tl::unexpected(make_error(ErrorCode::io_error,
          "cannot create " + directory.string() + ": " + error.message()));
  }
  return {};
}

Result<RuntimeConfig> load_config(const std::optional<std::filesystem::path>& workspace) {
  auto paths = resolve_paths(workspace);
  if (!paths) return tl::unexpected(paths.error());
  RuntimeConfig config;
  config.paths = *paths;
  for (const auto& short_id : official_lens_order()) {
    config.light_path.push_back(DesiredLens{
        .id = "org.tokmon.lens." + short_id,
        .artifact = "builtin:" + short_id,
        .enabled = true,
        .runtime = short_id == "termon" ? RuntimeKind::desktop : RuntimeKind::in_process});
  }
  if (auto result = merge_config_file(config, config.paths.user / "config.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_config_file(config, config.paths.project / "config.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_light_path(config.light_path, config.paths.user / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_light_path(config.light_path, config.paths.project / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  return config;
}

}  // namespace tokmon
