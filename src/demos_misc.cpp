// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Simulations and fractals: water ripples, Game of Life, Mandelbrot zoom.
#include <Arduino.h>
#include "demo.h"

// ------------------------------------------------------------------ Water
// Two-buffer wave equation, used as a refraction offset over a plasma image.
struct Water : Demo {
    int8_t * cur, *prev;
    uint8_t* img;
    const char* name() const override { return "Water Ripples (splash)"; }
    bool        interactive() const override { return true; }
    void        begin() override {
        cur  = (int8_t*)scratch(1);
        prev = (int8_t*)scratch(2);
        img  = scratch(3);
        memset(cur, 0, LSIZE);
        memset(prev, 0, LSIZE);
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++) {
                bool tile = ((x >> 4) ^ (y >> 4)) & 1;
                img[y * LW + x] = (tile ? 128 : 0) + ((sinT[(x * 3) & 255] + sinT[(y * 4) & 255]) >> 3);
            }
        fbText(img, LW, LH, 32, 52, "CYD 2432", 127);
        for (int i = 0; i < 128; i++) {
            pal[i]       = rgb(0, 40 + i, 90 + i);
            pal[128 + i] = rgb(10, 20 + i / 2, 60 + i);
        }
        pal[127] = rgb(255, 255, 200);
    }
    void drop(int x, int y, int v) {
        for (int k = -2; k <= 2; k++)
            for (int j = -2; j <= 2; j++) {
                int xx = x + j, yy = y + k;
                if (xx > 0 && xx < LW - 1 && yy > 0 && yy < LH - 1) cur[yy * LW + xx] = v;
            }
    }
    void update(uint32_t t) override {
        if (touch.down) {
            // the finger: a big splash on contact, then a wake while it drags
            int strength = touch.pressed ? 127 : 40 + 4 * (abs(touch.vx) + abs(touch.vy));
            touch.pressed = false;
            drop(touch.x / 2, touch.y / 2, strength > 127 ? 127 : strength);
        } else if (millis() - touch.lastMs > 4000) {
            // nobody playing: rain, and a "boat" tracing a figure eight
            if ((esp_random() & 7) == 0) drop(random(4, LW - 4), random(4, LH - 4), 100);
            drop(LW / 2 + (sinS[(t / 20) & 255] * 60 >> 7), LH / 2 + (sinS[(t / 10) & 255] * 35 >> 7), 60);
        }
        for (int y = 1; y < LH - 1; y++)
            for (int x = 1; x < LW - 1; x++) {
                int i = y * LW + x;
                int v = ((cur[i - 1] + cur[i + 1] + cur[i - LW] + cur[i + LW]) >> 1) - prev[i];
                v -= v >> 5;
                prev[i] = v < -127 ? -127 : v > 127 ? 127 : v;
            }
        std::swap(cur, prev);
        for (int y = 1; y < LH - 1; y++)
            for (int x = 1; x < LW - 1; x++) {
                int i = y * LW + x, dx = cur[i - 1] - cur[i + 1], dy = cur[i - LW] - cur[i + LW];
                int sx = x + (dx >> 3), sy = y + (dy >> 3);
                if (sx < 0) sx = 0;
                if (sx >= LW) sx = LW - 1;
                if (sy < 0) sy = 0;
                if (sy >= LH) sy = LH - 1;
                int c = img[sy * LW + sx], base = c & 128, v = (c & 127) + (dx >> 2);
                fb[i] = base | (v < 0 ? 0 : v > 126 ? 126 : v);
            }
    }
};
Demo* makeWater() { return new Water(); }

// ------------------------------------------------------------------ Game of Life
struct Life : Demo {
    uint8_t *a, *b;  // cell age, 0 = dead
    uint32_t lastStep = 0, seeded = 0;
    const char* name() const override { return "Game of Life"; }
    void        seed() {
        for (int i = 0; i < LSIZE; i++) a[i] = (esp_random() % 5 == 0) ? 1 : 0;
    }
    void begin() override {
        a = scratch(1);
        b = scratch(2);
        seed();
        pal[0] = rgb(0, 0, 10);
        for (int i = 1; i < 256; i++) pal[i] = hsv(i * 3, 220, i < 4 ? 255 : 200);
    }
    void update(uint32_t t) override {
        if (t - lastStep < 40) return;
        lastStep = t;
        if (t - seeded > 12000) {
            seed();
            seeded = t;
        }
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++) {
                int n = 0;
                for (int k = -1; k <= 1; k++)
                    for (int j = -1; j <= 1; j++)
                        if ((k || j) && a[((y + k + LH) % LH) * LW + (x + j + LW) % LW]) n++;
                uint8_t v = a[y * LW + x];
                b[y * LW + x] = v ? ((n == 2 || n == 3) ? (v < 255 ? v + 1 : 255) : 0) : (n == 3 ? 1 : 0);
            }
        std::swap(a, b);
        memcpy(fb, a, LSIZE);
    }
};
Demo* makeLife() { return new Life(); }

// ------------------------------------------------------------------ Mandelbrot zoom
struct Mandelbrot : Demo {
    const char* name() const override { return "Mandelbrot Zoom"; }
    uint32_t    duration() const override { return 30000; }
    void        begin() override {
        for (int i = 0; i < 255; i++) pal[i] = hsv(i * 5, 230, 60 + (i * 7 % 190));
        pal[255] = 0;
    }
    void update(uint32_t t) override {
        // zoom into seahorse valley and loop
        const float cx = -0.743643887f, cy = 0.131825904f;
        float       z = expf(-(float)(t % 28000) / 3000.0f) * 3.0f, step = z / LW;
        int         maxIt = 40 + (int)(-logf(z) * 14);
        if (maxIt > 254) maxIt = 254;
        int rot = t / 40;
        for (int y = 0; y < LH; y++) {
            float ci = cy + (y - LH / 2) * step;
            for (int x = 0; x < LW; x++) {
                float cr = cx + (x - LW / 2) * step, zr = 0, zi = 0;
                int   i  = 0;
                for (; i < maxIt; i++) {
                    float r2 = zr * zr, i2 = zi * zi;
                    if (r2 + i2 > 4) break;
                    zi = 2 * zr * zi + ci;
                    zr = r2 - i2 + cr;
                }
                fb[y * LW + x] = i >= maxIt ? 255 : (i + rot) % 255;
            }
        }
    }
};
Demo* makeMandelbrot() { return new Mandelbrot(); }

// ------------------------------------------------------------------ Touch finale
// A galaxy of glowing particles orbiting a gravity well. The finger is the
// well; a tap blasts everything outward. Idle, the well wanders on its own and
// the screen asks to be touched. Hi-res, with fading trails.
struct TouchFinale : Demo {
    struct Pt {
        float x, y, vx, vy, r, spd;  // r, spd: this particle's own orbit radius and speed
    };
    static constexpr int N = 1000;
    Pt*                  p    = nullptr;
    float                wx   = SW / 2, wy = SH / 2;
    uint32_t             last = 0;
    const char*          name() const override { return "Touch Finale"; }
    Res                  res() const override { return Res::High; }
    bool                 interactive() const override { return true; }
    uint32_t             duration() const override { return 40000; }
    float                pulse() const override { return 0.08f; }
    void                 begin() override {
        p = (Pt*)arenaAlloc(N * sizeof(Pt));
        for (int i = 0; i < N; i++) {
            float a = (esp_random() % 6283) / 1000.0f;
            // denser near the middle, like a galaxy
            float u = (esp_random() % 1000) / 1000.0f, r = 12 + u * u * 125;
            p[i]    = {SW / 2 + cosf(a) * r, SH / 2 + sinf(a) * r, 0, 0, r, 1.2f + 260 / (r + 40)};
        }
    }
    void update(uint32_t t) override {
        float dt = (t - last) * 0.06f;  // 1.0 at 60 fps
        last     = t;
        if (dt > 3) dt = 3;
        // glow palette: dark hue -> bright hue -> white, hue drifting with time
        int h = t / 60;
        for (int i = 0; i < 256; i++) {
            int v = i < 160 ? i * 255 / 160 : 255, sat = i < 160 ? 255 : 255 - (i - 160) * 255 / 96;
            pal[i] = hsv(h + i / 4, sat, v);
        }
        // trails
        for (int i = 0; i < SW * SH; i++) {
            uint8_t v = fb[i];
            fb[i]     = v > 10 ? v - 10 : 0;
        }
        bool  idle = millis() - touch.lastMs > 3000;
        float tx, ty;
        if (touch.down) {
            tx = touch.x;
            ty = touch.y;
        } else {
            tx = SW / 2 + sinf(t / 1300.0f) * 100;
            ty = SH / 2 + sinf(t / 1700.0f) * 60;
        }
        wx += (tx - wx) * 0.3f;
        wy += (ty - wy) * 0.3f;
        bool  burst = touch.pressed;
        touch.pressed = false;
        float kick  = syncNow().kick;
        for (int i = 0; i < N; i++) {
            Pt&   q  = p[i];
            float rx = q.x - wx, ry = q.y - wy, d = sqrtf(rx * rx + ry * ry) + 0.01f;
            float ux = rx / d, uy = ry / d;  // outward, and the tangent is (-uy, ux)
            if (burst && d < 140) {
                q.vx += ux * 14;
                q.vy += uy * 14;
            }
            // steer towards "on my orbit, moving along it": a spinning disk that
            // trails behind the finger and springs back after a burst
            float want = q.r * (1 + 0.18f * kick);
            float tvx  = ux * (want - d) * 0.06f - uy * q.spd, tvy = uy * (want - d) * 0.06f + ux * q.spd;
            float k    = 0.07f * dt;
            q.vx += (tvx - q.vx) * k;
            q.vy += (tvy - q.vy) * k;
            float sp = q.vx * q.vx + q.vy * q.vy;
            q.x += q.vx * dt;
            q.y += q.vy * dt;
            // wrap round the edges so nothing is lost for good
            if (q.x < 0) q.x += SW;
            if (q.x >= SW) q.x -= SW;
            if (q.y < 0) q.y += SH;
            if (q.y >= SH) q.y -= SH;
            int xi = (int)q.x, yi = (int)q.y;
            int b  = 150 + (int)(sp * 13);
            fb[yi * SW + xi] = b > 255 ? 255 : b;
        }
        if (idle && (t / 600) & 1) fbText(fb, SW, SH, SW / 2 - 64, SH - 40, "TOUCH ME", 255, 2);
    }
};
Demo* makeTouchFinale() { return new TouchFinale(); }
