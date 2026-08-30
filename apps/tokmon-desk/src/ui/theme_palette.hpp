#pragma once

#include <cstdint>

namespace tokmon::desk::legacy_theme {

// Native Skia/RmlUi elements cannot inherit RCSS colours. Keep their palette
// beside the templates and mirror the established Slint Palette exactly so
// Terminal, Diff, file tree and editor surfaces remain one visual system.
struct Color {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha{255};
};

inline constexpr Color surface{250, 250, 249};
inline constexpr Color surface_warm{250, 249, 246};
inline constexpr Color white{255, 255, 255};
inline constexpr Color border{231, 229, 228};
inline constexpr Color hairline{240, 238, 232};
inline constexpr Color separator{245, 245, 244};

inline constexpr Color ink{28, 25, 23};
inline constexpr Color body{41, 37, 36};
inline constexpr Color strong{68, 64, 60};
inline constexpr Color mid{87, 83, 78};
inline constexpr Color dim{120, 113, 108};
inline constexpr Color faint{168, 162, 158};

inline constexpr Color accent{200, 106, 40};
inline constexpr Color accent_text{139, 82, 41};
inline constexpr Color accent_background{247, 239, 229};
inline constexpr Color accent_border{235, 220, 208};

inline constexpr Color green{22, 163, 74};
inline constexpr Color green_ink{22, 101, 52};
inline constexpr Color red{220, 38, 38};
inline constexpr Color red_ink{153, 27, 27};
inline constexpr Color blue{2, 132, 199};

inline constexpr Color diff_add_background{220, 252, 231, 102};
inline constexpr Color diff_add_gutter{187, 247, 208, 102};
inline constexpr Color diff_delete_background{254, 226, 226, 102};
inline constexpr Color diff_delete_gutter{254, 202, 202, 102};
inline constexpr Color diff_empty{245, 245, 244, 128};
inline constexpr Color diff_banner{244, 244, 244};
inline constexpr Color diff_banner_border{229, 229, 229};

} // namespace tokmon::desk::legacy_theme
