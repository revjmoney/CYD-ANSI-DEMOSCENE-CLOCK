// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
//
// Port of ACID WARP by Noah Spurrier, with Mark Bilk (c) 1992, 1993, from
// the Linux port by Steven Wills (distributed under the GNU GPL). The picture
// functions, the four palette types (plus their "lightning" variants) and
// the per-channel palette rolling with its random direction flips and fades
// follow acidwarp.c, palinit.c and rolnfade.c. The trig look-up tables are
// replaced by the same functions computed directly (same units: a circle is
// 256, a sine peaks at 511).
//
// The picture is evaluated at full screen coordinates (320x240) and sampled
// every other pixel into a 160x120 buffer, so it has the original's scale.
#include "acidwarp.h"
#include <Arduino.h>
#include <math.h>
#include "gfx.h"

namespace acidwarp {
namespace {

constexpr int ANGLE_UNIT = 256, ANGLE_UNIT_HALF = 128, ANGLE_UNIT_QUART = 64;
constexpr int NUM_IMAGE_FUNCTIONS = 40, NUM_PALETTE_TYPES = 8;
enum { RGBW_PAL, W_PAL, W_HALF_PAL, PASTEL_PAL, RGBW_LIGHTNING_PAL, W_LIGHTNING_PAL, W_HALF_LIGHTNING_PAL, PASTEL_LIGHTNING_PAL };

uint8_t* buf = nullptr;
int16_t  sinTab[ANGLE_UNIT];
uint8_t  mainPal[256 * 3], targetPal[256 * 3];  // 6-bit VGA DAC values
uint16_t out565[256];
int      redDir = 0, grnDir = 0, bluDir = 0;
int      funcList[NUM_IMAGE_FUNCTIONS], funcIdx = NUM_IMAGE_FUNCTIONS;
enum { FADE_IN, HOLD, FADE_OUT } phase = FADE_IN;
float    tickAcc = 0, holdT = 0;

long RANDOM(long a) { return (long)(esp_random() % (uint32_t)a); }

long lut_sin(long angle) {
    if (angle < 0) angle = -angle + ANGLE_UNIT_HALF;
    angle %= ANGLE_UNIT;
    return sinTab[angle];
}
long lut_cos(long angle) { return lut_sin(angle + ANGLE_UNIT_QUART); }
long lut_angle(long x, long y) {
    if (!x && !y) return 0;
    int a = (int)floorf(atan2f((float)y, (float)x) * (ANGLE_UNIT / (2 * PI)));
    return a < 0 ? a + ANGLE_UNIT : a;
}
long lut_dist(long x, long y) { return (long)sqrtf((float)(x * x + y * y)); }

void makeShuffledList(int* list, int n) {
    for (int i = 0; i < n; i++) list[i] = -1;
    for (int i = 0; i < n; i++) {
        int r;
        do r = RANDOM(n);
        while (list[r] != -1);
        list[r] = i;
    }
}

// generate_image() from acidwarp.c; buf_graf is our low-res buffer, the
// rain functions read their neighbours from it
void generateImage(int imageFuncNum) {
    const long xmax = 320, ymax = 240, xcenter = 160, ycenter = 120, colormax = 255;
    long x1 = RANDOM(40) - 20, x2 = RANDOM(40) - 20, x3 = RANDOM(40) - 20, x4 = RANDOM(40) - 20;
    long y1 = RANDOM(40) - 20, y2 = RANDOM(40) - 20, y3 = RANDOM(40) - 20, y4 = RANDOM(40) - 20;
    for (int by = 0; by < H; by++)
        for (int bx = 0; bx < W; bx++) {
            long x = bx * 2, y = by * 2;
            long dx = x - xcenter, dy = y - ycenter;
            long dist = lut_dist(dx, dy), angle = lut_angle(dx, dy), color;
            auto left = [&] { return (long)buf[by * W + bx - 1]; };
            auto up   = [&] { return (long)buf[(by - 1) * W + bx]; };
            switch (imageFuncNum) {
                case 0:  // Rays plus 2D Waves
                    color = angle + lut_sin(dist * 10) / 64 + lut_cos(x * ANGLE_UNIT / xmax * 2) / 32 + lut_cos(y * ANGLE_UNIT / ymax * 2) / 32;
                    break;
                case 1:  // Rays plus 2D Waves
                    color = angle + lut_sin(dist * 10) / 16 + lut_cos(x * ANGLE_UNIT / xmax * 2) / 8 + lut_cos(y * ANGLE_UNIT / ymax * 2) / 8;
                    break;
                case 2:
                    color = lut_sin(lut_dist(dx + x1, dy + y1) * 4) / 32 + lut_sin(lut_dist(dx + x2, dy + y2) * 8) / 32 +
                            lut_sin(lut_dist(dx + x3, dy + y3) * 16) / 32 + lut_sin(lut_dist(dx + x4, dy + y4) * 32) / 32;
                    break;
                case 3:  // Peacock
                    color = angle + lut_sin(lut_dist(dx + 20, dy) * 10) / 32 + angle + lut_sin(lut_dist(dx - 20, dy) * 10) / 32;
                    break;
                case 4: color = lut_sin(dist) / 16; break;
                case 5:  // 2D Wave + Spiral
                    color = lut_cos(x * ANGLE_UNIT / xmax) / 8 + lut_cos(y * ANGLE_UNIT / ymax) / 8 + angle + lut_sin(dist) / 32;
                    break;
                case 6:  // Peacock, three centers
                    color = lut_sin(lut_dist(dx, dy - 20) * 4) / 32 + lut_sin(lut_dist(dx + 20, dy + 20) * 4) / 32 +
                            lut_sin(lut_dist(dx - 20, dy + 20) * 4) / 32;
                    break;
                case 7:  // Peacock, three centers
                    color = angle + lut_sin(lut_dist(dx, dy - 20) * 8) / 32 + lut_sin(lut_dist(dx + 20, dy + 20) * 8) / 32 +
                            lut_sin(lut_dist(dx - 20, dy + 20) * 8) / 32;
                    break;
                case 8:  // Peacock, three centers
                    color = lut_sin(lut_dist(dx, dy - 20) * 12) / 32 + lut_sin(lut_dist(dx + 20, dy + 20) * 12) / 32 +
                            lut_sin(lut_dist(dx - 20, dy + 20) * 12) / 32;
                    break;
                case 9: color = dist + lut_sin(5 * angle) / 64; break;  // Five Arm Star
                case 10: color = lut_cos(x * ANGLE_UNIT / xmax * 2) / 4 + lut_cos(y * ANGLE_UNIT / ymax * 2) / 4; break;  // 2D Wave
                case 11: color = lut_cos(x * ANGLE_UNIT / xmax) / 8 + lut_cos(y * ANGLE_UNIT / ymax) / 8; break;          // 2D Wave
                case 12: color = dist; break;                                       // Simple Concentric Rings
                case 13: color = angle; break;                                      // Simple Rays
                case 14: color = angle + lut_sin(dist * 8) / 32; break;             // Toothed Spiral Sharp
                case 15: color = lut_sin(dist * 4) / 32; break;                     // Rings with sine
                case 16: color = dist + lut_sin(dist * 4) / 32; break;              // Rings with sine with sliding inner Rings
                case 17:
                    color = lut_sin(lut_cos(2 * x * ANGLE_UNIT / xmax)) / (20 + dist) + lut_sin(lut_cos(2 * y * ANGLE_UNIT / ymax)) / (20 + dist);
                    break;
                case 18: color = lut_cos(7 * x * ANGLE_UNIT / xmax) / (20 + dist) + lut_cos(7 * y * ANGLE_UNIT / ymax) / (20 + dist); break;
                case 19: color = lut_cos(17 * x * ANGLE_UNIT / xmax) / (20 + dist) + lut_cos(17 * y * ANGLE_UNIT / ymax) / (20 + dist); break;
                case 20:  // 2D Wave Interference
                    color = lut_cos(17 * x * ANGLE_UNIT / xmax) / 32 + lut_cos(17 * y * ANGLE_UNIT / ymax) / 32 + dist + angle;
                    break;
                case 21: color = lut_cos(7 * x * ANGLE_UNIT / xmax) / 32 + lut_cos(7 * y * ANGLE_UNIT / ymax) / 32 + dist; break;
                case 22:
                    color = lut_cos(7 * x * ANGLE_UNIT / xmax) / 32 + lut_cos(7 * y * ANGLE_UNIT / ymax) / 32 +
                            lut_cos(11 * x * ANGLE_UNIT / xmax) / 32 + lut_cos(11 * y * ANGLE_UNIT / ymax) / 32;
                    break;
                case 23: color = lut_sin(angle * 7) / 32; break;
                case 24:
                    color = lut_sin(lut_dist(dx + x1, dy + y1) * 2) / 12 + lut_sin(lut_dist(dx + x2, dy + y2) * 4) / 12 +
                            lut_sin(lut_dist(dx + x3, dy + y3) * 6) / 12 + lut_sin(lut_dist(dx + x4, dy + y4) * 8) / 12;
                    break;
                case 25:
                    color = angle + lut_sin(lut_dist(dx + x1, dy + y1) * 2) / 16 + angle + lut_sin(lut_dist(dx + x2, dy + y2) * 4) / 16 +
                            lut_sin(lut_dist(dx + x3, dy + y3) * 6) / 8 + lut_sin(lut_dist(dx + x4, dy + y4) * 8) / 8;
                    break;
                case 26:
                    color = angle + lut_sin(lut_dist(dx + x1, dy + y1) * 2) / 12 + angle + lut_sin(lut_dist(dx + x2, dy + y2) * 4) / 12 +
                            angle + lut_sin(lut_dist(dx + x3, dy + y3) * 6) / 12 + angle + lut_sin(lut_dist(dx + x4, dy + y4) * 8) / 12;
                    break;
                case 27:
                    color = lut_sin(lut_dist(dx + x1, dy + y1) * 2) / 32 + lut_sin(lut_dist(dx + x2, dy + y2) * 4) / 32 +
                            lut_sin(lut_dist(dx + x3, dy + y3) * 6) / 32 + lut_sin(lut_dist(dx + x4, dy + y4) * 8) / 32;
                    break;
                case 28:  // Random Curtain of Rain (in strong wind)
                    if (by == 0 || bx == 0) color = RANDOM(16);
                    else color = (left() + up()) / 2 + RANDOM(16) - 8;
                    break;
                case 29:
                    if (by == 0 || bx == 0) color = RANDOM(1024);
                    else color = dist / 6 + (left() + up()) / 2 + RANDOM(16) - 8;
                    break;
                case 30:
                    color = lut_sin(lut_dist(dx, dy - 20) * 4) / 32 ^ lut_sin(lut_dist(dx + 20, dy + 20) * 4) / 32 ^
                            lut_sin(lut_dist(dx - 20, dy + 20) * 4) / 32;
                    break;
                case 31: color = (angle % (ANGLE_UNIT / 4)) ^ dist; break;
                case 32: color = dy ^ dx; break;
                case 33:  // Variation on Rain
                    if (by == 0 || bx == 0) color = RANDOM(16);
                    else color = (left() + up()) / 2;
                    color += RANDOM(2) - 1;
                    if (color < 64) color += RANDOM(16) - 8;
                    break;
                case 34:  // Variation on Rain
                    if (by == 0 || bx == 0) color = RANDOM(16);
                    else color = (left() + up()) / 2;
                    if (color < 100) color += RANDOM(16) - 8;
                    break;
                case 35:
                    color = angle + lut_sin(dist * 8) / 32;
                    dy    = (y - ycenter) * 2;
                    dist  = lut_dist(dx, dy);
                    angle = lut_angle(dx, dy);
                    color = (color + angle + lut_sin(dist * 8) / 32) / 2;
                    break;
                case 36:
                    color = angle + lut_sin(dist * 10) / 16 + lut_cos(x * ANGLE_UNIT / xmax * 2) / 8 + lut_cos(y * ANGLE_UNIT / ymax * 2) / 8;
                    dy    = (y - ycenter) * 2;
                    dist  = lut_dist(dx, dy);
                    angle = lut_angle(dx, dy);
                    color = (color + angle + lut_sin(dist * 8) / 32) / 2;
                    break;
                case 37:
                    color = angle + lut_sin(dist * 10) / 16 + lut_cos(x * ANGLE_UNIT / xmax * 2) / 8 + lut_cos(y * ANGLE_UNIT / ymax * 2) / 8;
                    dy    = (y - ycenter) * 2;
                    dist  = lut_dist(dx, dy);
                    angle = lut_angle(dx, dy);
                    color = (color + angle + lut_sin(dist * 10) / 16 + lut_cos(x * ANGLE_UNIT / xmax * 2) / 8 +
                             lut_cos(y * ANGLE_UNIT / ymax * 2) / 8) / 2;
                    break;
                case 38:
                    if (dy % 2) {
                        dy *= 2;
                        dist  = lut_dist(dx, dy);
                        angle = lut_angle(dx, dy);
                    }
                    color = angle + lut_sin(dist * 8) / 32;
                    break;
                case 39:
                    color = (angle % (ANGLE_UNIT / 4)) ^ dist;
                    dy    = (y - ycenter) * 2;
                    dist  = lut_dist(dx, dy);
                    angle = lut_angle(dx, dy);
                    color = (color + ((angle % (ANGLE_UNIT / 4)) ^ dist)) / 2;
                    break;
                default: color = RANDOM(colormax - 1) + 1; break;
            }
            // fit into 1..254, with a modulo that is never negative
            color %= (colormax - 1);
            if (color < 0) color += colormax - 1;
            buf[by * W + bx] = (uint8_t)(color + 1);
        }
}

// ---- palinit.c ----
void addSparkles(uint8_t* p, int amount) {
    for (int r = 1; r < 256; r += 4)
        for (int c = 0; c < 3; c++) p[r * 3 + c] = (uint8_t)min(63, p[r * 3 + c] + amount);
}
void initRgbw(uint8_t* p) {
    for (int r = 0; r < 64; r++) {
        uint8_t v = r < 32 ? r * 2 : (63 - r) * 2;
        const uint8_t sets[4][3] = {{v, 0, 0}, {0, v, 0}, {0, 0, v}, {v, v, v}};
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 3; c++) p[(r + 64 * s) * 3 + c] = sets[s][c];
    }
}
void initW(uint8_t* p) {
    for (int r = 0; r < 256; r++) p[r * 3] = p[r * 3 + 1] = p[r * 3 + 2] = (r < 128 ? r : 255 - r) / 2;
}
void initWHalf(uint8_t* p) {
    for (int r = 0; r < 256; r++) p[r * 3] = p[r * 3 + 1] = p[r * 3 + 2] = r < 64 ? r : r < 128 ? 127 - r : 0;
}
void initPastel(uint8_t* p) {
    for (int r = 0; r < 128; r++) {
        for (int c = 0; c < 3; c++) {
            p[r * 3 + c]         = 31 + r / 4;
            p[(r + 128) * 3 + c] = 31 + (127 - r) / 4;
        }
    }
}
void initPalArray(uint8_t* p, int type) {
    switch (type) {
        case RGBW_PAL: initRgbw(p); break;
        case W_PAL: initW(p); break;
        case W_HALF_PAL: initWHalf(p); break;
        case PASTEL_PAL: initPastel(p); break;
        case RGBW_LIGHTNING_PAL: initRgbw(p), addSparkles(p, 9); break;
        case W_LIGHTNING_PAL: initW(p), addSparkles(p, 9); break;
        case W_HALF_LIGHTNING_PAL: initWHalf(p), addSparkles(p, 9); break;
        case PASTEL_LIGHTNING_PAL: initPastel(p), addSparkles(p, 9); break;
        default: initW(p); break;
    }
}

// ---- rolnfade.c ----
void rotateBackward(int c, uint8_t* p) {
    uint8_t t = p[254 * 3 + 3 + c];
    for (int x = 254; x >= 1; x--) p[x * 3 + 3 + c] = p[x * 3 + c];
    p[3 + c] = t;
}
void rotateForward(int c, uint8_t* p) {
    uint8_t t = p[3 + c];
    for (int x = 1; x < 255; x++) p[x * 3 + c] = p[x * 3 + 3 + c];
    p[255 * 3 + c] = t;
}
void rollRgb(uint8_t* p) {
    redDir ? rotateBackward(0, p) : rotateForward(0, p);
    grnDir ? rotateBackward(1, p) : rotateForward(1, p);
    bluDir ? rotateBackward(2, p) : rotateForward(2, p);
}
void maybeInvertDirection() {
    switch (RANDOM(256)) {
        case 0: redDir = !redDir; break;
        case 1: grnDir = !grnDir; break;
        case 2: bluDir = !bluDir; break;
    }
}
// one step towards target (or black when target is null); true when there
bool fadeTowards(uint8_t* p, const uint8_t* target) {
    int same = 0;
    for (int i = 3; i < 768; i++) {
        uint8_t want = target ? target[i] : 0;
        if (p[i] < want) p[i]++;
        else if (p[i] > want) p[i]--;
        else same++;
    }
    return same >= 765;
}

void nextImage() {
    if (++funcIdx >= NUM_IMAGE_FUNCTIONS) {
        funcIdx = 0;
        makeShuffledList(funcList, NUM_IMAGE_FUNCTIONS);
    }
    generateImage(funcList[funcIdx]);
    initPalArray(targetPal, RANDOM(NUM_PALETTE_TYPES + 1));
    phase = FADE_IN;
}

}  // namespace

void begin(uint8_t* b) {
    buf = b;
    for (int i = 0; i < ANGLE_UNIT; i++) sinTab[i] = (int16_t)lroundf(511 * sinf(i * 2 * PI / ANGLE_UNIT));
    memset(mainPal, 0, sizeof mainPal);
    funcIdx = NUM_IMAGE_FUNCTIONS;
    holdT   = 0;
    nextImage();
}

void setBuffer(uint8_t* b) { buf = b; }
void skip() {
    if (buf) nextImage();
}

void step(float dt, int holdSec) {
    if (!buf) return;
    // the original rolls the palette once per 30 ms tick
    tickAcc += dt;
    int ticks = 0;
    while (tickAcc >= 0.03f && ticks < 4) {
        tickAcc -= 0.03f;
        ticks++;
        maybeInvertDirection();
        switch (phase) {
            case FADE_IN:
                rollRgb(mainPal);
                rollRgb(targetPal);
                if (fadeTowards(mainPal, targetPal)) {
                    phase = HOLD;
                    holdT = 0;
                }
                break;
            case HOLD:
                rollRgb(mainPal);
                holdT += 0.03f;
                if (holdT >= holdSec) phase = FADE_OUT;
                break;
            case FADE_OUT:
                rollRgb(mainPal);
                if (fadeTowards(mainPal, nullptr)) nextImage();
                break;
        }
    }
    if (tickAcc > 0.03f) tickAcc = 0;
}

const uint16_t* lut(int bright, bool dark) {
    for (int i = 0; i < 256; i++) {
        int ch[3];
        for (int k = 0; k < 3; k++) {
            int v = mainPal[i * 3 + k];  // 0..63
            // dark mode: cube it (only the peaks stay lit, even the pale pastel
            // palette goes deep) and cap at about a third of full brightness
            ch[k] = dark ? (v * v * v * 90 / (63 * 63 * 63)) * bright >> 8 : v * 4 * bright >> 8;
        }
        out565[i] = rgb(ch[0], ch[1], ch[2]);
    }
    return out565;
}

void line(uint16_t* out, int y, const uint16_t* l) {
    if (!buf) return (void)memset(out, 0, 320 * 2);
    const uint8_t* row = buf + (y >> 1) * W;
    for (int x = 0; x < W; x++) out[x * 2] = out[x * 2 + 1] = l[row[x]];
}

}  // namespace acidwarp

// ---- Acid Warp as a demo of its own: the whole screen, tap for the next picture ----
#include "demo.h"
namespace {
struct AcidWarpDemo : Demo {
    uint32_t    lastMs = 0;
    const char* name() const override { return "Acid Warp"; }
    Res         res() const override { return Res::Low; }  // 160x120, exactly its buffer
    bool        interactive() const override { return true; }
    float       pulse() const override { return 0; }  // no beat flash: the palette is the show
    uint32_t    duration() const override { return 45000; }
    void        begin() override { acidwarp::begin(fb); }
    void        update(uint32_t ms) override {
        float dt = (ms - lastMs) / 1000.0f;
        lastMs   = ms;
        if (dt > 0.1f) dt = 0.1f;
        acidwarp::setBuffer(fb);  // the engine flips low-res buffers (each starts as a copy)
        if (touch.pressed) {
            touch.pressed = false;
            acidwarp::skip();
        }
        acidwarp::step(dt);
        memcpy(pal, acidwarp::lut(), sizeof pal);
    }
};
}  // namespace
Demo* makeAcidWarp() { return new AcidWarpDemo(); }
