// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Shared drawing state: one 320x240 8-bit indexed framebuffer, a 256-entry
// palette, CP437 text helpers and the strip pusher that streams frames to
// the panel over DMA.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

constexpr int SW = 320, SH = 240;  // panel, landscape
constexpr int LW = 160, LH = 120;  // low-res mode, pixel doubled
constexpr int STRIP = 16;          // lines per DMA strip
constexpr int LSIZE = LW * LH;     // 19200: low-res frame, also the size of each scratch slot

// fb is the frame being drawn (SW*SH bytes; the engine double buffers, so it
// flips between two buffers). Low-res demos draw in fb[0..LSIZE) and may keep
// their own tables in scratch(1..3), which never move.
extern uint8_t* fb;
extern uint8_t* fbBase;
extern uint16_t pal[256];  // byte-swapped RGB565, ready for the panel
extern const uint8_t cga[16][3];
extern uint8_t sinT[256];  // 0..255 sine, period 256
extern int8_t  sinS[256];  // -127..127 sine, period 256

static inline uint8_t* scratch(int i) { return fbBase + LSIZE * i; }

// Per-demo working memory: a static arena, reset each time a demo starts, so
// big demos never fail on a fragmented heap. Returns nullptr when full.
size_t arenaInit(size_t want);  // call once at boot, returns bytes actually reserved
void*  arenaAlloc(size_t bytes);
void  arenaReset();
bool  inArena(const void* p);

static inline uint16_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}
static inline uint16_t cgaColor(int i) { return rgb(cga[i][0], cga[i][1], cga[i][2]); }

// HSV with h 0..255, s,v 0..255
uint16_t hsv(int h, int s, int v);
// linear ramp of palette entries [i0..i1] from colour a to colour b
void palRamp(int i0, int i1, int r0, int g0, int b0, int r1, int g1, int b1);

void gfxInit();

// ---- text ----
extern const uint8_t* font16;  // IBM VGA 8x16, 4 KB
// One CP437 cell: character + attribute (fg | bg << 4)
struct Cell {
    uint8_t ch, attr;
};
// Render one scanline of an 80 column row using 4x8 cells (VGA glyphs
// 2x2 averaged). cols = how many cells are drawn, the rest is black.
void textLine4x8(uint16_t* out, const Cell* row, int glyphRow, int cols = 80);
// One scanline of a 40 column row of full size 8x16 cells.
void textLine8x16(uint16_t* out, const Cell* row, int glyphRow);
// Same, but colours come from a caller-supplied 5-level ramp per cell.
uint16_t attrBlend(uint8_t attr, int level);  // level 0..4 = bg..fg
// Draw a string into the 8-bit framebuffer with the 8x16 font at scale s.
void fbText(uint8_t* dst, int stride, int h, int x, int y, const char* s, uint8_t color, int scale = 1);
// helpers used by text demos
uint8_t cp437(const char* utf8, int& advance);
