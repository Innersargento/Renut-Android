// The aspect ratio option (renut_aspect_ratio, guest/widescreen.cpp): the game's screen is 720 rows high and
// 1280 columns wide at 16:9, wider for a wider ratio. Ported from reNut's widescreen mode for the PC.
#pragma once

#include <cstdint>

namespace renut::widescreen {

constexpr uint32_t kNativeWidth = 1280;
constexpr uint32_t kNativeHeight = 720;

// The game's screen width: 1280, or wider (a multiple of 16) for a wider ratio. Settled the first time it is
// asked and kept until the game restarts.
uint32_t ScreenWidth();

// A surface this wide is the screen's: its width, or its pitch (the width rounded up to the 128 texels of an
// EDRAM tile row at most).
inline bool IsScreenWidth(uint32_t width) {
  const uint32_t screen = ScreenWidth();
  return width >= screen && width <= ((screen + 127) & ~127u);
}

}  // namespace renut::widescreen
