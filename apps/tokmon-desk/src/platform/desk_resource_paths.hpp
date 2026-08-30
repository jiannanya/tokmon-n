#pragma once

#include <filesystem>
#include <vector>

namespace tokmon::desk {

// Runtime resources are resolved once at the application boundary. UI and
// feature modules receive these paths instead of embedding machine-specific
// paths or assumptions about the current working directory.
struct DeskResourcePaths {
  std::filesystem::path root;
  std::filesystem::path assets;
  std::filesystem::path main_document;
  std::filesystem::path ui_font;

  [[nodiscard]] static DeskResourcePaths resolve(
      const std::filesystem::path& executable_directory = {});
  [[nodiscard]] static std::vector<std::filesystem::path>
  platform_font_candidates();
};

} // namespace tokmon::desk
