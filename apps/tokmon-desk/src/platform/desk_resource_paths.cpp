#include "platform/desk_resource_paths.hpp"

#include <stdexcept>

namespace tokmon::desk {
namespace {

bool has_main_document(const std::filesystem::path& root) {
  std::error_code error;
  return std::filesystem::is_regular_file(
      root / "rml" / "documents" / "main.rml", error);
}

} // namespace

DeskResourcePaths DeskResourcePaths::resolve(
    const std::filesystem::path& executable_directory) {
  std::filesystem::path root;
  if (!executable_directory.empty() && has_main_document(executable_directory))
    root = executable_directory;
#ifdef TOKMON_DESK_SOURCE_DIR
  if (root.empty()) {
    const std::filesystem::path source(TOKMON_DESK_SOURCE_DIR);
    if (has_main_document(source))
      root = source;
  }
#endif
  if (root.empty()) {
    const auto source = std::filesystem::current_path() / "apps" / "tokmon-desk";
    if (has_main_document(source))
      root = source;
  }
  if (root.empty())
    throw std::runtime_error("cannot locate tokmon-desk runtime resources");

  DeskResourcePaths result;
  result.root = std::filesystem::weakly_canonical(root);
  result.assets = result.root / "assets";
  result.main_document = result.root / "rml" / "documents" / "main.rml";
  result.ui_font = result.assets / "fonts" / "MiSansVF.ttf";
  return result;
}

std::vector<std::filesystem::path>
DeskResourcePaths::platform_font_candidates() {
#if defined(_WIN32)
  return {
      "C:/Windows/Fonts/consola.ttf",
      "C:/Windows/Fonts/seguisym.ttf",
      "C:/Windows/Fonts/seguiemj.ttf",
  };
#elif defined(__APPLE__)
  return {
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/Apple Symbols.ttf",
      "/System/Library/Fonts/Apple Color Emoji.ttc",
  };
#else
  return {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
  };
#endif
}

} // namespace tokmon::desk
