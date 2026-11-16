// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// A tribute to Future Crew's Second Reality (Assembly '93): the dot tunnel,
// then the glenz vectors (a translucent checkered solid with a smaller one
// spinning inside it) bouncing on a checkerboard. Written from scratch after
// watching the original; its source is public domain (Unlicense).
#include <Arduino.h>
#include "demo.h"

namespace {

struct P3 {
    float x, y, z;
};
static P3 rot(P3 p, float ax, float ay, float az) {
    float c = cosf(ax), s = sinf(ax), y = p.y * c - p.z * s, z = p.y * s + p.z * c;
    p.y = y, p.z = z;
    c = cosf(ay), s = sinf(ay);
    float x = p.x * c + p.z * s;
    z       = -p.x * s + p.z * c;
    p.x = x, p.z = z;
    c = cosf(az), s = sinf(az);
    x = p.x * c - p.y * s;
    y = p.x * s + p.y * c;
    p.x = x, p.y = y;
    return p;
}

// filled triangle into the hi-res fb, OR-ing bits in (glenz transparency)
static void triOr(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t mask) {
    if (y1 < y0) std::swap(x0, x1), std::swap(y0, y1);
    if (y2 < y0) std::swap(x0, x2), std::swap(y0, y2);
    if (y2 < y1) std::swap(x1, x2), std::swap(y1, y2);
    if (y2 == y0) return;
    for (int y = y0 < 0 ? 0 : y0; y <= y2 && y < SH; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb = y < y1 ? (y1 == y0 ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0))
                        : (y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1));
        if (xa > xb) std::swap(xa, xb);
        if (xa < 0) xa = 0;
        if (xb >= SW) xb = SW - 1;
        uint8_t* row = fb + y * SW;
        for (int x = xa; x <= xb; x++) row[x] |= mask;
    }
}

constexpr uint32_t TUNNEL_MS = 16000;

struct SecondReality : Demo {
    const char* name() const override { return "Second Reality tribute"; }
    Res         res() const override { return Res::High; }
    uint32_t    duration() const override { return 34000; }
    uint32_t    last = 0;
    float       travel = 0;
    bool        glenzPal = false;

    void begin() override { tunnelPalette(); }

    // ------------------------------------------------ part 1: dot tunnel
    void tunnelPalette() {
        // 1..63: dots from dim blue-grey to white; 64..: trail fade
        palRamp(0, 63, 0, 0, 0, 230, 235, 255);
        glenzPal = false;
    }
    // the tunnel's centre line wanders on two sines
    static void path(float z, float& x, float& y) {
        x = sinf(z * 0.021f) * 60 + sinf(z * 0.0137f) * 40;
        y = cosf(z * 0.017f) * 45 + sinf(z * 0.009f) * 30;
    }
    void tunnel(float dt, uint32_t t) {
        // trails: fade the whole frame a little each time
        for (int i = 0; i < SW * SH; i++) {
            uint8_t v = fb[i];
            fb[i]     = v > 9 ? v - 9 : 0;
        }
        travel += dt * (160 + syncNow().kick * 80);
        float camx, camy;
        path(travel, camx, camy);
        constexpr int RINGS = 44, DOTS = 32;
        constexpr float GAP = 16;
        float first = floorf(travel / GAP) * GAP;
        for (int k = 1; k <= RINGS; k++) {
            float zw = first + k * GAP, z = zw - travel;
            if (z < 4) continue;
            float cx, cy;
            path(zw, cx, cy);
            cx -= camx;
            cy -= camy;
            float k2  = 190 / z;
            int   bri = 63 - (int)(z * 63 / (RINGS * GAP));
            if (bri < 3) continue;
            float spin = zw * 0.004f;
            for (int d = 0; d < DOTS; d++) {
                float a  = d * 2 * PI / DOTS + spin;
                int   sx = SW / 2 + (int)((cx + cosf(a) * 70) * k2), sy = SH / 2 + (int)((cy + sinf(a) * 70) * k2);
                if (sx < 0 || sx >= SW - 1 || sy < 0 || sy >= SH - 1) continue;
                uint8_t* p = fb + sy * SW + sx;
                p[0]       = bri;
                if (z < 260) p[1] = p[SW] = p[SW + 1] = bri;  // near dots are bigger
            }
        }
    }

    // ------------------------------------------------ part 2: glenz vectors
    void setGlenzPalette() {
        // bits 0-1 outer back/front faces A, 2-3 outer B, 4-5 inner, 6-7 floor
        for (int i = 0; i < 256; i++) {
            int r, g, b;
            int fl = i >> 6;  // floor: 0 sky, 1 dark tile, 2 light tile
            if (fl == 0) r = 0, g = 0, b = 24;
            else if (fl == 1) r = 50, g = 20, b = 70;
            else r = 120, g = 60, b = 150;
            if (i & 1) r = r / 2 + 30, g = g / 2 + 60, b = b / 2 + 140;    // outer back, blue
            if (i & 2) r = r / 2 + 100, g = g / 2 + 110, b = b / 2 + 140;  // outer back, white-ish
            if (i & 16) r = r / 2 + 150, g = g / 3, b = b / 3;             // inner, red
            if (i & 32) r = r / 2 + 200, g = g / 2 + 160, b = b / 2;       // inner, yellow
            if (i & 4) r = r / 3 + 40, g = g / 3 + 90, b = b / 3 + 230;    // outer front, bright blue
            if (i & 8) r = r / 3 + 190, g = g / 3 + 200, b = b / 3 + 230;  // outer front, white
            pal[i] = rgb(r, g, b);
        }
        glenzPal = true;
    }
    // a "glenz" solid: every cube face split into 4 triangles around a raised
    // centre (a tetrakis hexahedron), checkered by alternating triangles
    void glenz(float sc, float ax, float ay, float az, float ox, float oy, uint8_t back0, uint8_t back1, uint8_t front0,
               uint8_t front1) {
        static const P3  cv[8]   = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
        static const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}};
        int px[8], py[8];
        P3  rv[8];
        for (int i = 0; i < 8; i++) {
            rv[i]   = rot(cv[i], ax, ay, az);
            float z = rv[i].z + 4.5f;
            px[i]   = (int)(ox + rv[i].x * sc * 4 / z);
            py[i]   = (int)(oy + rv[i].y * sc * 4 / z);
        }
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < 6; k++) {
                P3 n = {0, 0, 0};
                for (int j = 0; j < 4; j++) n.x += rv[f[k][j]].x, n.y += rv[f[k][j]].y, n.z += rv[f[k][j]].z;
                n.x *= 0.42f, n.y *= 0.42f, n.z *= 0.42f;
                float z  = n.z + 4.5f;
                int   cx = (int)(ox + n.x * sc * 4 / z), cy = (int)(oy + n.y * sc * 4 / z);
                for (int j = 0; j < 4; j++) {
                    int  a = f[k][j], b = f[k][(j + 1) & 3];
                    long cr = (long)(px[b] - px[a]) * (cy - py[a]) - (long)(py[b] - py[a]) * (cx - px[a]);
                    bool front = cr < 0;
                    if (front != (pass == 1)) continue;
                    uint8_t m = front ? ((j + k) & 1 ? front0 : front1) : ((j + k) & 1 ? back0 : back1);
                    triOr(px[a], py[a], px[b], py[b], cx, cy, m);
                }
            }
    }
    void glenzPart(uint32_t t) {
        if (!glenzPal) setGlenzPalette();
        float tt = t / 1000.0f;
        // checkerboard floor in perspective below the horizon
        constexpr int HOR = 140;
        for (int y = 0; y < SH; y++) {
            uint8_t* row = fb + y * SW;
            if (y < HOR) {
                memset(row, 0, SW);
                continue;
            }
            float z  = 3000.0f / (y - HOR + 4);
            float sz = z + tt * 180;
            for (int x = 0; x < SW; x++) {
                float wx = (x - SW / 2) * z / 160 + sinf(tt * 0.7f) * 60;
                bool  ch = ((int)floorf(wx / 40) + (int)floorf(sz / 40)) & 1;
                row[x]   = ch ? 0x80 : 0x40;
            }
        }
        // the bounce: a parabola per hop, with a squash on landing
        float ph  = fmodf(tt * 1.1f, 1.0f);
        float hop = 4 * ph * (1 - ph);
        float sq  = ph < 0.08f || ph > 0.92f ? 0.85f : 1.0f;
        float oy  = 175 - hop * 100;
        glenz(70 * sq, tt * 0.8f, tt * 1.2f, tt * 0.35f, SW / 2, oy, 1, 2, 4, 8);
        glenz(30, -tt * 1.6f, tt * 0.9f, -tt * 1.1f, SW / 2, oy, 16, 32, 16, 32);
        fbText(fb, SW, SH, 6, 6, "SECOND REALITY TRIBUTE", 0xC8);
        fbText(fb, SW, SH, 6, 22, "FUTURE CREW 1993", 0xC4);
    }

    void update(uint32_t t) override {
        float dt = (t - last) / 1000.0f;
        last     = t;
        if (dt > 0.05f) dt = 0.05f;
        if (t < TUNNEL_MS) tunnel(dt, t);
        else glenzPart(t - TUNNEL_MS);
    }
};

}  // namespace

Demo* makeSecondReality() { return new SecondReality(); }
