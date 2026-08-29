#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;

namespace tokmon::desk {

struct ShapedGlyph {
  std::uint32_t glyph_id{0};
  std::uint32_t cluster{0};
  float x_advance{0};
  float y_advance{0};
  float x_offset{0};
  float y_offset{0};
};

class FontManager final {
 public:
  FontManager();
  ~FontManager();
  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;

  [[nodiscard]] bool load_ui_font(const std::filesystem::path& path, std::string& error);
  [[nodiscard]] std::vector<ShapedGlyph> shape_utf8(const std::string& text,
                                                    float size_px) const;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] const std::filesystem::path& ui_font_path() const noexcept;

 private:
  FT_LibraryRec_* library_{nullptr};
  FT_FaceRec_* face_{nullptr};
  hb_font_t* harfbuzz_font_{nullptr};
  std::filesystem::path ui_font_path_;
};

} // namespace tokmon::desk
