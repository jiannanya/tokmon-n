#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "tokmon/error.hpp"
#include "tokmon/lens.hpp"

namespace tokmon {

struct TokmonPaths {
  std::filesystem::path user;
  std::filesystem::path project;
  std::filesystem::path data;
  std::filesystem::path database;
  std::filesystem::path logs;
  std::filesystem::path run;
  std::filesystem::path runtimes;
};

struct DesiredLens {
  LensId id;
  std::string artifact;
  bool enabled{true};
  RuntimeKind runtime{RuntimeKind::in_process};
};

struct RuntimeConfig {
  TokmonPaths paths;
  std::vector<DesiredLens> light_path;
  std::string log_level{"info"};
  std::size_t photon_window{4096};
  std::size_t max_beats{32};
  bool require_signatures{false};
};

[[nodiscard]] Result<TokmonPaths> resolve_paths(
    const std::optional<std::filesystem::path>& workspace = std::nullopt);
[[nodiscard]] Result<RuntimeConfig> load_config(
    const std::optional<std::filesystem::path>& workspace = std::nullopt);
Result<void> ensure_directory_layout(const TokmonPaths& paths);

}  // namespace tokmon

