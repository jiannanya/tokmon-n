#include "platform/desk_app_paths.hpp"

#include <cstdlib>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace tokmon::desk {
namespace {

std::filesystem::path environment_path(const char* name) {
  const auto* value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

bool is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
  if (parent.empty())
    return false;
  const auto normalized_child = std::filesystem::weakly_canonical(child);
  const auto normalized_parent = std::filesystem::weakly_canonical(parent);
  auto child_it = normalized_child.begin();
  for (auto parent_it = normalized_parent.begin(); parent_it != normalized_parent.end();
       ++parent_it, ++child_it) {
    if (child_it == normalized_child.end() || *child_it != *parent_it)
      return false;
  }
  return true;
}

} // namespace

DeskAppPaths DeskAppPaths::resolve() {
  DeskAppPaths paths;
#if defined(_WIN32)
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)))
    throw std::runtime_error("cannot resolve LocalAppData for tokmon-desk");
  const std::filesystem::path base = std::filesystem::path(raw) / L"Tokmon" / L"tokmon-desk";
  CoTaskMemFree(raw);
  paths.config = base / L"config";
  paths.data = base / L"data";
  paths.state = base / L"state";
  paths.cache = base / L"cache";
  paths.logs = base / L"logs";
#elif defined(__APPLE__)
  const auto home = environment_path("HOME");
  if (home.empty())
    throw std::runtime_error("cannot resolve HOME for tokmon-desk");
  const auto support = home / "Library" / "Application Support" / "Tokmon" / "tokmon-desk";
  paths.config = support / "config";
  paths.data = support / "data";
  paths.state = support / "state";
  paths.cache = home / "Library" / "Caches" / "Tokmon" / "tokmon-desk";
  paths.logs = home / "Library" / "Logs" / "Tokmon" / "tokmon-desk";
#else
  const auto home = environment_path("HOME");
  if (home.empty())
    throw std::runtime_error("cannot resolve HOME for tokmon-desk");
  const auto config_home = environment_path("XDG_CONFIG_HOME");
  const auto data_home = environment_path("XDG_DATA_HOME");
  const auto state_home = environment_path("XDG_STATE_HOME");
  const auto cache_home = environment_path("XDG_CACHE_HOME");
  paths.config = (config_home.empty() ? home / ".config" : config_home) / "tokmon-desk";
  paths.data = (data_home.empty() ? home / ".local" / "share" : data_home) / "tokmon-desk";
  paths.state = (state_home.empty() ? home / ".local" / "state" : state_home) / "tokmon-desk";
  paths.cache = (cache_home.empty() ? home / ".cache" : cache_home) / "tokmon-desk";
  paths.logs = paths.state / "logs";
#endif
  return paths;
}

bool DeskAppPaths::ensure(std::string& error) const {
  for (const auto& path : {config, data, state, cache, logs}) {
    if (path.empty() || !path.is_absolute()) {
      error = "tokmon-desk app path is empty or relative";
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
      error = "cannot create tokmon-desk app path: " + ec.message();
      return false;
    }
  }
  return true;
}

bool DeskAppPaths::isolated_from(const std::filesystem::path& workspace) const {
  const auto project_config = workspace / ".tokmon";
  for (const auto& path : {config, data, state, cache, logs}) {
    if (is_within(path, workspace) || is_within(path, project_config))
      return false;
  }
  return true;
}

} // namespace tokmon::desk
