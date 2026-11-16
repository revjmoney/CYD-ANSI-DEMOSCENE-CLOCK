// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
#include "gfx.h"
#include <Arduino.h>
#include "rotatocam_data.h"

uint8_t*       fb;
uint8_t*       fbBase;
uint16_t       pal[256];
uint8_t        sinT[256];
int8_t         sinS[256];
const uint8_t* font16 = vga_font8x16;

const uint8_t cga[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

// coverage of each 4x8 cell pixel (0..4 lit source pixels), packed 2 cells... kept simple: 1 byte each
static uint8_t  glyph4x8[256][32];
static uint16_t attrLUT[256][5];

uint16_t hsv(int h, int s, int v) {
    h &= 255;
    int region = h / 43, rem = (h - region * 43) * 6;
    int p = (v * (255 - s)) >> 8, q = (v * (255 - ((s * rem) >> 8))) >> 8,
        t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0: return rgb(v, t, p);
        case 1: return rgb(q, v, p);
        case 2: return rgb(p, v, t);
        case 3: return rgb(p, q, v);
        case 4: return rgb(t, p, v);
        default: return rgb(v, p, q);
    }
}

void palRamp(int i0, int i1, int r0, int g0, int b0, int r1, int g1, int b1) {
    int n = i1 - i0;
    for (int i = 0; i <= n; i++) {
        int k = n ? (i * 256) / n : 0;
        pal[i0 + i] = rgb(r0 + (((r1 - r0) * k) >> 8), g0 + (((g1 - g0) * k) >> 8), b0 + (((b1 - b0) * k) >> 8));
    }
}

void gfxInit() {
    for (int i = 0; i < 256; i++) {
        float s = sinf(i * 2 * PI / 256);
        sinT[i] = (uint8_t)(127.5f + 127.5f * s);
        sinS[i] = (int8_t)(127 * s);
    }
    for (int g = 0; g < 256; g++)
        for (int r = 0; r < 8; r++) {
            uint8_t a = font16[g * 16 + r * 2], b = font16[g * 16 + r * 2 + 1];
            for (int c = 0; c < 4; c++) {
                int m = 0xC0 >> (c * 2);
                glyph4x8[g][r * 4 + c] = __builtin_popcount(a & m) + __builtin_popcount(b & m);
            }
        }
    for (int at = 0; at < 256; at++) {
        const uint8_t *f = cga[at & 15], *b = cga[at >> 4];
        for (int l = 0; l < 5; l++)
            attrLUT[at][l] = rgb(b[0] + (f[0] - b[0]) * l / 4, b[1] + (f[1] - b[1]) * l / 4, b[2] + (f[2] - b[2]) * l / 4);
    }
}

uint16_t attrBlend(uint8_t attr, int level) { return attrLUT[attr][level]; }

static uint8_t* arena     = nullptr;
static size_t   arenaSize = 0, arenaUsed = 0;
size_t          arenaInit(size_t want) {
    // take the biggest block we can get, up to want, before anything fragments the heap
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    arenaSize  = want < big - 4096 ? want : big - 4096;
    arena      = (uint8_t*)heap_caps_malloc(arenaSize, MALLOC_CAP_8BIT);
    if (!arena) arenaSize = 0;
    return arenaSize;
}
void* arenaAlloc(size_t bytes) {
    bytes = (bytes + 3) & ~3u;
    if (!arena || arenaUsed + bytes > arenaSize) return nullptr;
    void* p = arena + arenaUsed;
    arenaUsed += bytes;
    return p;
}
void arenaReset() { arenaUsed = 0; }
bool inArena(const void* p) { return arena && p >= arena && p < arena + arenaSize; }

void textLine4x8(uint16_t* out, const Cell* row, int glyphRow, int cols) {
    int x = 0;
    for (; x < cols && x < 80; x++) {
        const uint8_t*  g   = &glyph4x8[row[x].ch][glyphRow * 4];
        const uint16_t* lut = attrLUT[row[x].attr];
        out[0]              = lut[g[0]];
        out[1]              = lut[g[1]];
        out[2]              = lut[g[2]];
        out[3]              = lut[g[3]];
        out += 4;
    }
    for (; x < 80; x++, out += 4) out[0] = out[1] = out[2] = out[3] = 0;
}

void textLine8x16(uint16_t* out, const Cell* row, int glyphRow) {
    for (int x = 0; x < 40; x++, out += 8) {
        uint8_t  bits = font16[row[x].ch * 16 + glyphRow];
        uint16_t fg = attrLUT[row[x].attr][4], bg = attrLUT[row[x].attr][0];
        for (int b = 0; b < 8; b++) out[b] = (bits & (0x80 >> b)) ? fg : bg;
    }
}

void fbText(uint8_t* dst, int stride, int h, int x, int y, const char* s, uint8_t color, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const uint8_t* g = &font16[(uint8_t)*s * 16];
        for (int r = 0; r < 16 * scale; r++) {
            int yy = y + r;
            if (yy < 0 || yy >= h) continue;
            uint8_t bits = g[r / scale];
            for (int c = 0; c < 8 * scale; c++) {
                int xx = x + c;
                if (xx < 0 || xx >= stride) continue;
                if (bits & (0x80 >> (c / scale))) dst[yy * stride + xx] = color;
            }
        }
    }
}
