#pragma once

#include <cstdint>

namespace tokmon::desk::legacy_theme {

// Native Skia/RmlUi elements cannot inherit RCSS colours. Keep the native
// surfaces in lockstep with the Forest Sage RCSS theme (legacy-palette.rcss)
// so Terminal, Diff, file tree and editor surfaces remain one visual system
// in both the 浅色 and 深色 modes.
struct Color {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha{255};
};

// -- Light palette (浅色·暖白奶茶) -------------------------------------------
inline constexpr Color surface{255, 255, 255};
inline constexpr Color surface_warm{250, 249, 246};
inline constexpr Color white{255, 255, 255};
inline constexpr Color border{234, 230, 220};
inline constexpr Color hairline{243, 241, 234};
inline constexpr Color separator{247, 245, 239};

inline constexpr Color ink{26, 33, 28};
inline constexpr Color body{37, 45, 39};
inline constexpr Color strong{56, 64, 59};
inline constexpr Color mid{74, 83, 76};
inline constexpr Color dim{92, 103, 96};
inline constexpr Color faint{148, 158, 151};

inline constexpr Color accent{45, 90, 67};
inline constexpr Color accent_text{45, 90, 67};
inline constexpr Color accent_background{237, 244, 236};
inline constexpr Color accent_border{205, 220, 208};

inline constexpr Color green{22, 163, 74};
inline constexpr Color green_ink{22, 101, 52};
inline constexpr Color red{220, 38, 38};
inline constexpr Color red_ink{153, 27, 27};
inline constexpr Color blue{74, 120, 96};

inline constexpr Color diff_add_background{220, 252, 231, 120};
inline constexpr Color diff_add_gutter{187, 247, 208, 180};
inline constexpr Color diff_delete_background{254, 226, 226, 120};
inline constexpr Color diff_delete_gutter{254, 202, 202, 180};
inline constexpr Color diff_empty{247, 245, 239, 128};
inline constexpr Color diff_banner{250, 249, 246};
inline constexpr Color diff_banner_border{234, 230, 220};

// -- Dark palette (深色·森林夜色, mirrors body.dark in legacy-palette.rcss) ---
inline constexpr Color surface_dark{26, 34, 29};
inline constexpr Color surface_warm_dark{22, 28, 24};
inline constexpr Color white_dark{26, 34, 29};
inline constexpr Color border_dark{255, 255, 255, 20};
inline constexpr Color hairline_dark{255, 255, 255, 16};
inline constexpr Color separator_dark{255, 255, 255, 14};

inline constexpr Color ink_dark{230, 237, 231};
inline constexpr Color body_dark{214, 224, 215};
inline constexpr Color strong_dark{184, 199, 187};
inline constexpr Color mid_dark{184, 199, 187};
inline constexpr Color dim_dark{155, 176, 160};
inline constexpr Color faint_dark{98, 117, 104};

inline constexpr Color accent_dark{114, 184, 144};
inline constexpr Color accent_text_dark{114, 184, 144};
inline constexpr Color accent_background_dark{28, 44, 34};
inline constexpr Color accent_border_dark{114, 184, 144, 64};

inline constexpr Color green_dark{34, 197, 94};
inline constexpr Color green_ink_dark{134, 239, 172};
inline constexpr Color red_dark{248, 113, 113};
inline constexpr Color red_ink_dark{252, 165, 165};
inline constexpr Color blue_dark{114, 184, 144};

inline constexpr Color diff_add_background_dark{34, 197, 94, 36};
inline constexpr Color diff_add_gutter_dark{34, 197, 94, 56};
inline constexpr Color diff_delete_background_dark{239, 68, 68, 36};
inline constexpr Color diff_delete_gutter_dark{239, 68, 68, 56};
inline constexpr Color diff_empty_dark{255, 255, 255, 14};
inline constexpr Color diff_banner_dark{22, 28, 24};
inline constexpr Color diff_banner_border_dark{255, 255, 255, 20};

// -- Runtime mode switch ------------------------------------------------------
// The RML side toggles body.dark via data-class-dark; native surfaces query
// this flag instead. Bumping the revision lets cached geometry know when to
// rebuild with the new palette.

inline bool& dark_mode_flag() noexcept {
  static bool dark = false;
  return dark;
}

inline std::uint32_t& theme_revision_counter() noexcept {
  static std::uint32_t revision = 0;
  return revision;
}

inline void set_dark_mode(const bool dark) noexcept {
  if (dark_mode_flag() == dark)
    return;
  dark_mode_flag() = dark;
  ++theme_revision_counter();
}

inline bool is_dark_mode() noexcept { return dark_mode_flag(); }

inline std::uint32_t theme_revision() noexcept {
  return theme_revision_counter();
}

// Resolve a light palette colour to its dark counterpart when the dark mode
// is active. Unknown colours pass through unchanged.
inline Color themed(const Color& light) noexcept {
  if (!is_dark_mode())
    return light;
  // clang-format off
  if (light.red == surface.red && light.green == surface.green &&
      light.blue == surface.blue)
    return surface_dark;
  if (light.red == surface_warm.red && light.green == surface_warm.green &&
      light.blue == surface_warm.blue)
    return surface_warm_dark;
  if (light.red == white.red && light.green == white.green &&
      light.blue == white.blue)
    return white_dark;
  if (light.red == border.red && light.green == border.green &&
      light.blue == border.blue)
    return border_dark;
  if (light.red == hairline.red && light.green == hairline.green &&
      light.blue == hairline.blue)
    return hairline_dark;
  if (light.red == separator.red && light.green == separator.green &&
      light.blue == separator.blue)
    return separator_dark;
  if (light.red == ink.red && light.green == ink.green &&
      light.blue == ink.blue)
    return ink_dark;
  if (light.red == body.red && light.green == body.green &&
      light.blue == body.blue)
    return body_dark;
  if (light.red == strong.red && light.green == strong.green &&
      light.blue == strong.blue)
    return strong_dark;
  if (light.red == mid.red && light.green == mid.green &&
      light.blue == mid.blue)
    return mid_dark;
  if (light.red == dim.red && light.green == dim.green &&
      light.blue == dim.blue)
    return dim_dark;
  if (light.red == faint.red && light.green == faint.green &&
      light.blue == faint.blue)
    return faint_dark;
  if (light.red == accent.red && light.green == accent.green &&
      light.blue == accent.blue)
    return accent_dark;
  if (light.red == accent_background.red &&
      light.green == accent_background.green &&
      light.blue == accent_background.blue)
    return accent_background_dark;
  if (light.red == accent_border.red && light.green == accent_border.green &&
      light.blue == accent_border.blue)
    return accent_border_dark;
  if (light.red == green.red && light.green == green.green &&
      light.blue == green.blue)
    return green_dark;
  if (light.red == green_ink.red && light.green == green_ink.green &&
      light.blue == green_ink.blue)
    return green_ink_dark;
  if (light.red == red.red && light.green == red.green &&
      light.blue == red.blue)
    return red_dark;
  if (light.red == red_ink.red && light.green == red_ink.green &&
      light.blue == red_ink.blue)
    return red_ink_dark;
  if (light.red == blue.red && light.green == blue.green &&
      light.blue == blue.blue)
    return blue_dark;
  if (light.red == diff_add_background.red &&
      light.green == diff_add_background.green &&
      light.blue == diff_add_background.blue)
    return diff_add_background_dark;
  if (light.red == diff_add_gutter.red &&
      light.green == diff_add_gutter.green &&
      light.blue == diff_add_gutter.blue)
    return diff_add_gutter_dark;
  if (light.red == diff_delete_background.red &&
      light.green == diff_delete_background.green &&
      light.blue == diff_delete_background.blue)
    return diff_delete_background_dark;
  if (light.red == diff_delete_gutter.red &&
      light.green == diff_delete_gutter.green &&
      light.blue == diff_delete_gutter.blue)
    return diff_delete_gutter_dark;
  if (light.red == diff_empty.red && light.green == diff_empty.green &&
      light.blue == diff_empty.blue)
    return diff_empty_dark;
  if (light.red == diff_banner.red && light.green == diff_banner.green &&
      light.blue == diff_banner.blue)
    return diff_banner_dark;
  if (light.red == diff_banner_border.red &&
      light.green == diff_banner_border.green &&
      light.blue == diff_banner_border.blue)
    return diff_banner_border_dark;
  // clang-format on
  return light;
}

} // namespace tokmon::desk::legacy_theme
