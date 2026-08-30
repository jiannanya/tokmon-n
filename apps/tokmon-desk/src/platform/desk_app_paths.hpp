#pragma once

#include <filesystem>
#include <string>

namespace tokmon::desk {

struct DeskAppPaths {
  std::filesystem::path config;
  std::filesystem::path data;
  std::filesystem::path state;
  std::filesystem::path cache;
  std::filesystem::path logs;
  std::filesystem::path runtime;
  std::filesystem::path recovery;
  std::filesystem::path change_snapshots;

  [[nodiscard]] static DeskAppPaths resolve(
      const std::filesystem::path& override_root = {});
  [[nodiscard]] bool ensure(std::string& error) const;
  [[nodiscard]] bool isolated_from(const std::filesystem::path& workspace) const;
};

} // namespace tokmon::desk
