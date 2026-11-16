// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Acid Warp (Noah Spurrier and Mark Bilk, 1992-93; Linux port by Steven Wills)
// on a 160x120 index buffer: the original picture functions, palettes and
// palette rolling, fades included.
#pragma once
#include <stdint.h>

namespace acidwarp {
constexpr int W = 160, H = 120;
// buf: W*H bytes. Draws the first picture and starts its fade in.
void begin(uint8_t* buf);
// the picture moved (a double-buffered frame); the next picture goes here
void setBuffer(uint8_t* buf);
// jump to the next picture now (the palette keeps rolling into its new colours)
void skip();
// run the palette (rolls, fades, a new picture every holdSec seconds)
void step(float dt, int holdSec = 20);
// the current palette as panel colours, scaled by bright (0..256). dark: a
// "dark mode" palette (cubed and capped: deep and dim) that keeps text readable
const uint16_t* lut(int bright = 256, bool dark = false);
// one 320 px scanline of the picture, pixel doubled
void line(uint16_t* out, int y, const uint16_t* lut);
}  // namespace acidwarp
