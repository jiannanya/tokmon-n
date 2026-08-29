#include "fonts/font_manager.hpp"

#include <hb-ft.h>
#include <hb.h>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace tokmon::desk {

FontManager::FontManager() {
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) == 0)
    library_ = library;
}

FontManager::~FontManager() {
  if (harfbuzz_font_)
    hb_font_destroy(harfbuzz_font_);
  if (face_)
    FT_Done_Face(face_);
  if (library_)
    FT_Done_FreeType(library_);
}

bool FontManager::load_ui_font(const std::filesystem::path& path, std::string& error) {
  if (!library_) {
    error = "FreeType initialization failed";
    return false;
  }
  if (harfbuzz_font_) {
    hb_font_destroy(harfbuzz_font_);
    harfbuzz_font_ = nullptr;
  }
  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
  const auto utf8 = path.u8string();
  FT_Face face = nullptr;
  if (FT_New_Face(library_, reinterpret_cast<const char*>(utf8.c_str()), 0, &face) != 0) {
    error = "cannot load MiSans VF";
    return false;
  }
  face_ = face;
  harfbuzz_font_ = hb_ft_font_create_referenced(face_);
  ui_font_path_ = path;
  return true;
}

std::vector<ShapedGlyph> FontManager::shape_utf8(const std::string& text,
                                                 float size_px) const {
  std::vector<ShapedGlyph> result;
  if (!harfbuzz_font_ || !face_ || text.empty())
    return result;
  const auto size_26_6 = static_cast<FT_F26Dot6>(size_px * 64.0F);
  FT_Set_Char_Size(face_, 0, size_26_6, 96, 96);
  hb_ft_font_changed(harfbuzz_font_);

  auto* buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, text.c_str(), static_cast<int>(text.size()), 0,
                     static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(harfbuzz_font_, buffer, nullptr, 0);
  unsigned count = 0;
  const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
  const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
  result.reserve(count);
  for (unsigned index = 0; index < count; ++index) {
    result.push_back({infos[index].codepoint, infos[index].cluster,
                      positions[index].x_advance / 64.0F,
                      positions[index].y_advance / 64.0F,
                      positions[index].x_offset / 64.0F,
                      positions[index].y_offset / 64.0F});
  }
  hb_buffer_destroy(buffer);
  return result;
}

bool FontManager::ready() const noexcept { return harfbuzz_font_ != nullptr; }

const std::filesystem::path& FontManager::ui_font_path() const noexcept {
  return ui_font_path_;
}

} // namespace tokmon::desk
