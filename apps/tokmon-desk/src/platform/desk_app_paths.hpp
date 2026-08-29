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

  [[nodiscard]] static DeskAppPaths resolve();
  [[nodiscard]] bool ensure(std::string& error) const;
  [[nodiscard]] bool isolated_from(const std::filesystem::path& workspace) const;
};

} // namespace tokmon::desk
