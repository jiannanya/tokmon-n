#pragma once

#include <cstdint>

namespace tokmon::desk::legacy_theme {

// Native Skia/RmlUi elements cannot inherit RCSS colours. Keep the native
// surfaces in lockstep with the Forest Sage RCSS theme so Terminal, Diff, file
// tree and editor surfaces remain one visual system.
struct Color {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha{255};
};

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
inline constexpr Color accent_background{234, 241, 232};
inline constexpr Color accent_border{205, 220, 208};

inline constexpr Color green{22, 163, 74};
inline constexpr Color green_ink{22, 101, 52};
inline constexpr Color red{220, 38, 38};
inline constexpr Color red_ink{153, 27, 27};
inline constexpr Color blue{74, 120, 96};

inline constexpr Color diff_add_background{220, 252, 231, 102};
inline constexpr Color diff_add_gutter{187, 247, 208, 102};
inline constexpr Color diff_delete_background{254, 226, 226, 102};
inline constexpr Color diff_delete_gutter{254, 202, 202, 102};
inline constexpr Color diff_empty{247, 245, 239, 128};
inline constexpr Color diff_banner{249, 251, 248};
inline constexpr Color diff_banner_border{226, 237, 224};

} // namespace tokmon::desk::legacy_theme
