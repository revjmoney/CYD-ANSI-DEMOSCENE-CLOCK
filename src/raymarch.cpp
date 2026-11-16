// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Chrome metaballs, sphere-traced in real time at 64x48 on both cores, reflecting a
// synthwave sky and grid floor, then blown up 5x with bilinear filtering and
// an ordered dither so it reads like a prerendered sequence.
#include <Arduino.h>
#include "demo.h"

namespace {

struct V {
    float x, y, z;
};
static inline V     operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V     operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V     operator*(V a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V     norm(V a) { return a * (1.0f / sqrtf(dot(a, a))); }
static inline V     cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

constexpr int   RW = 64, RH = 48, NB = 4;  // 5x upscale to 320x240
constexpr float K  = 0.55f;   // blend radius between blobs
constexpr float FLOOR_Y = -1.6f;
const uint8_t   bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

struct Raymarch : Demo {
    uint8_t* lo;  // RW*RH shades 0..255
    V        c[NB];
    float    r[NB];
    float    boundR = 2.6f;
    V        sun;

    const char* name() const override { return "Chrome Raymarch"; }
    Res         res() const override { return Res::High; }
    float       pulse() const override { return 0.08f; }

    void begin() override {
        lo  = (uint8_t*)arenaAlloc(RW * RH);
        sun = norm({0, 0.18f, 1});
        // synthwave ramp: black, navy, purple, hot pink, orange, yellow, white (64 steps)
        static const uint8_t k[7][3] = {{0, 0, 0}, {14, 10, 50}, {80, 20, 120}, {230, 40, 140}, {255, 120, 40}, {255, 220, 80}, {255, 255, 255}};
        for (int i = 0; i < 64; i++) {
            float f = i / 63.0f * 6;
            int   a = (int)f;
            if (a > 5) a = 5;
            float t = f - a;
            pal[i]  = rgb(k[a][0] + (k[a + 1][0] - k[a][0]) * t, k[a][1] + (k[a + 1][1] - k[a][1]) * t,
                          k[a][2] + (k[a + 1][2] - k[a][2]) * t);
        }
    }

    // ---- the scene ----
    inline float smin(float a, float b) const {
        float h = 0.5f + 0.5f * (b - a) / K;
        h       = h < 0 ? 0 : h > 1 ? 1 : h;
        return b + (a - b) * h - K * h * (1 - h);
    }
    inline float sdf(V p) const {
        float d = 1e9f;
        for (int i = 0; i < NB; i++) {
            V q = p - c[i];
            d   = smin(d, sqrtf(dot(q, q)) - r[i]);
        }
        return d;
    }
    // what the chrome sees: sunset sky with a striped sun, a glowing grid floor
    float env(V o, V d) const {
        if (d.y > -0.02f) {
            float h = 1 - d.y;
            float v = 18 + 150 * h * h * h * h;
            float s = dot(d, sun);
            if (s > 0.965f) {  // the sun, with the classic horizontal cut-outs
                float band = fmodf((d.y - sun.y + 0.3f) * 60, 2);
                if (!(d.y < sun.y && band < 0.7f)) v = 225 + 30 * (s - 0.965f) / 0.035f;
            }
            return v;
        }
        float t  = (FLOOR_Y - o.y) / d.y;
        float gx = o.x + d.x * t, gz = o.z + d.z * t + millis() * 0.002f;
        float fx = fabsf(gx - floorf(gx + 0.5f)), fz = fabsf(gz - floorf(gz + 0.5f));
        float line = fminf(fx, fz);
        float fade = 1 / (1 + t * 0.08f);
        float v    = line < 0.05f ? 160 * fade + 50 : 34;
        return v + 60 * (1 - fade);  // haze towards the horizon
    }

    // per-frame camera, shared with the core 0 helper
    V ro, fw, rt, up;
    SemaphoreHandle_t go = nullptr, done = nullptr;
    TaskHandle_t      helper = nullptr;
    volatile bool     quit = false;

    static void helperTask(void* arg) {
        auto* me = (Raymarch*)arg;
        for (;;) {
            xSemaphoreTake(me->go, portMAX_DELAY);
            if (me->quit) break;
            me->rows(1);  // odd rows
            xSemaphoreGive(me->done);
        }
        xSemaphoreGive(me->done);
        vTaskDelete(nullptr);
    }
    void end() override {
        if (!helper) return;
        quit = true;
        xSemaphoreGive(go);
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(go);
        vSemaphoreDelete(done);
    }

    void rows(int first) {
        for (int py = first; py < RH; py += 2)
            for (int px = 0; px < RW; px++) {
                float u = (px - RW / 2 + 0.5f) / RH * 1.1f, v = -(py - RH / 2 + 0.5f) / RH * 1.1f;
                V     rd = norm(fw + rt * u + up * v);
                float val;
                // only rays that meet the bounding sphere get marched
                float b = dot(ro, rd), cc = dot(ro, ro) - boundR * boundR, disc = b * b - cc;
                bool  hit = false;
                V     p;
                if (disc > 0) {
                    float tt = -b - sqrtf(disc), tmax = -b + sqrtf(disc);
                    if (tt < 0) tt = 0;
                    for (int s = 0; s < 18 && tt < tmax; s++) {
                        p       = ro + rd * tt;
                        float d = sdf(p);
                        if (d < 0.006f) {
                            hit = true;
                            break;
                        }
                        tt += d;
                    }
                }
                if (hit) {
                    // tetrahedral gradient for the normal (4 extra evaluations)
                    const float e = 0.01f;
                    V n = norm(V{1, -1, -1} * sdf(p + V{e, -e, -e}) + V{-1, -1, 1} * sdf(p + V{-e, -e, e}) +
                               V{-1, 1, -1} * sdf(p + V{-e, e, -e}) + V{1, 1, 1} * sdf(p + V{e, e, e}));
                    V     rf  = rd - n * (2 * dot(rd, n));
                    float fr  = 1 + dot(rd, n);
                    fr        = fr * fr * fr;
                    float sp  = dot(rf, sun);
                    sp        = sp > 0 ? sp * sp * sp * sp : 0;
                    sp *= sp * sp * sp;
                    // chrome: the reflection, brightened at grazing angles, plus sky light from above
                    float sky = n.y > 0 ? n.y : 0;
                    val       = env(p, rf) * (0.75f + 0.5f * fr) + 255 * sp + 55 * sky + 30;
                } else {
                    val = env(ro, rd);
                }
                lo[py * RW + px] = val > 255 ? 255 : val < 0 ? 0 : (uint8_t)val;
            }
    }

    void update(uint32_t ms) override {
        if (!helper) {
            go   = xSemaphoreCreateBinary();
            done = xSemaphoreCreateBinary();
            xTaskCreatePinnedToCore(helperTask, "march", 4096, this, 2, &helper, 0);
        }
        float t = ms / 1000.0f;
        for (int i = 0; i < NB; i++) {
            float ph = i * 1.7f;
            c[i]     = {1.1f * sinf(t * (0.7f + 0.13f * i) + ph), 0.75f * sinf(t * (0.9f + 0.11f * i) + ph * 2),
                        1.0f * cosf(t * (0.6f + 0.17f * i) + ph)};
            r[i]     = 0.55f + 0.12f * sinf(t * 1.3f + i) + 0.08f * syncNow().kick;
        }
        // camera slowly circling the blobs
        float a = t * 0.35f;
        ro      = {3.9f * sinf(a), 0.55f + 0.4f * sinf(t * 0.3f), -3.9f * cosf(a)};
        fw = norm(V{0, 0, 0} - ro), rt = norm(cross({0, 1, 0}, fw)), up = cross(fw, rt);
        // core 0 is idle while a full-res scene draws: it marches the odd rows
        xSemaphoreGive(go);
        rows(0);
        xSemaphoreTake(done, portMAX_DELAY);

        // 5x bilinear upscale + 4x4 ordered dither into 64 palette steps
        for (int y = 0; y < SH; y++) {
            int fy = y * RH * 64 / SH - 32;  // source y in 1/64 steps, sample centres
            int y0 = fy >> 6, wy = fy & 63;
            if (fy < 0) y0 = 0, wy = 0;
            int            y1 = y0 + 1 < RH ? y0 + 1 : RH - 1;
            const uint8_t *r0 = lo + y0 * RW, *r1 = lo + y1 * RW;
            uint8_t*       out = fb + y * SW;
            const uint8_t* bay = bayer[y & 3];
            for (int x = 0; x < SW; x++) {
                int fx = x * RW * 64 / SW - 32;
                int x0 = fx >> 6, wx = fx & 63;
                if (fx < 0) x0 = 0, wx = 0;
                int x1 = x0 + 1 < RW ? x0 + 1 : RW - 1;
                int top = r0[x0] * (64 - wx) + r0[x1] * wx, bot = r1[x0] * (64 - wx) + r1[x1] * wx;
                int v   = (top * (64 - wy) + bot * wy) >> 12;  // 0..255
                v       = (v + bay[x & 3]) >> 2;
                out[x]  = v > 63 ? 63 : v;
            }
        }
        fbText(fb, SW, SH, 6, SH - 20, "REALTIME RAYMARCH  64x48  2 CORES", 40);
    }
};

}  // namespace

Demo* makeRaymarch() { return new Raymarch(); }
