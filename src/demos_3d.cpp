// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// "3D" parts: voxel landscape, glenz vectors, vector bobs and a raycaster.
// Original implementations (MIT) of the classic techniques.
#include <Arduino.h>
#include "demo.h"

// ------------------------------------------------------------------ Voxel landscape
// Comanche style column renderer over a procedural 128x128 wrapping map.
struct Voxel : Demo {
    uint8_t *hm, *cm;
    const char* name() const override { return "Voxel Landscape"; }
    void        begin() override {
        hm = scratch(1);
        cm = scratch(2);
        // sum of wrapping sines + a few hashed bumps = rolling hills
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) {
                int h = sinT[(x * 2) & 255] / 3 + sinT[(y * 4 + 30) & 255] / 4 + sinT[((x + y) * 6) & 255] / 6 +
                        sinT[((x * 3 - y * 2) * 2 + 100) & 255] / 5;
                h += (int)((x * 73856093u ^ y * 19349663u) % 9);
                hm[y * 128 + x] = h > 255 ? 255 : h;
            }
        // smooth twice so the hash noise becomes texture, not spikes
        for (int pass = 0; pass < 2; pass++)
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    int s = 0;
                    for (int k = -1; k <= 1; k++)
                        for (int j = -1; j <= 1; j++) s += hm[((y + k) & 127) * 128 + ((x + j) & 127)];
                    cm[y * 128 + x] = s / 9;
                }
        memcpy(hm, cm, 128 * 128);
        // colour index = height band (water, sand, grass, rock, snow) + slope light
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) {
                int h = hm[y * 128 + x], sl = h - hm[y * 128 + ((x + 1) & 127)];
                int band = h < 70 ? 0 : h < 80 ? 1 : h < 140 ? 2 : h < 175 ? 3 : 4;
                int lit  = 16 + sl * 3;
                cm[y * 128 + x] = band * 32 + (lit < 0 ? 0 : lit > 31 ? 31 : lit);
            }
        static const uint8_t c[5][3] = {{20, 60, 160}, {200, 180, 110}, {40, 150, 40}, {120, 100, 80}, {240, 240, 250}};
        for (int b = 0; b < 5; b++)
            for (int l = 0; l < 32; l++) {
                int k              = 96 + l * 5;
                pal[b * 32 + l]    = rgb(c[b][0] * k >> 8, c[b][1] * k >> 8, c[b][2] * k >> 8);
            }
        for (int i = 0; i < 64; i++) pal[192 + i] = rgb(90 + i, 120 + i, 200 + i / 2);  // sky
    }
    void update(uint32_t t) override {
        float px = t / 60.0f, py = t / 25.0f, ang = sinf(t / 7000.0f) * 1.2f;
        float sa = sinf(ang), ca = cosf(ang);
        int   ph = hm[((int)py & 127) * 128 + ((int)px & 127)];
        float camH = ph / 2 + 70 + 20 * sinf(t / 2300.0f), horizon = 25;
        for (int y = 0; y < LH; y++) memset(fb + y * LW, 192 + (y < 63 ? y : 63), LW);
        int ybuf[LW];
        for (int x = 0; x < LW; x++) ybuf[x] = LH;
        float dz = 1;
        for (float z = 1; z < 160; z += dz, dz += 0.04f) {
            // left/right ends of this depth slice
            float lx = (-ca - sa) * z + px, ly = (sa - ca) * z + py;
            float rx = (ca - sa) * z + px, ry = (-sa - ca) * z + py;
            float stepx = (rx - lx) / LW, stepy = (ry - ly) / LW, inv = 70 / z;
            for (int x = 0; x < LW; x++) {
                int     i  = ((int)ly & 127) * 128 + ((int)lx & 127);
                int     sy = (int)((camH - hm[i] / 2) * inv + horizon);
                if (sy < 0) sy = 0;
                if (sy < ybuf[x]) {
                    uint8_t c = cm[i];
                    for (int y = sy; y < ybuf[x]; y++) fb[y * LW + x] = c;
                    ybuf[x] = sy;
                }
                lx += stepx;
                ly += stepy;
            }
        }
    }
};
Demo* makeVoxel() { return new Voxel(); }

// ---- tiny 3D helpers ----
struct V3 {
    float x, y, z;
};
static V3 rot(V3 p, float ax, float ay, float az) {
    float c = cosf(ax), s = sinf(ax), y = p.y * c - p.z * s, z = p.y * s + p.z * c;
    p.y = y;
    p.z = z;
    c = cosf(ay), s = sinf(ay);
    float x = p.x * c + p.z * s;
    z       = -p.x * s + p.z * c;
    p.x = x;
    p.z = z;
    c = cosf(az), s = sinf(az);
    x   = p.x * c - p.y * s;
    y   = p.x * s + p.y * c;
    p.x = x;
    p.y = y;
    return p;
}

// Flat triangle fill into the hi-res fb, OR-ing a mask in (glenz transparency).
static void triOr(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t mask) {
    if (y1 < y0) std::swap(x0, x1), std::swap(y0, y1);
    if (y2 < y0) std::swap(x0, x2), std::swap(y0, y2);
    if (y2 < y1) std::swap(x1, x2), std::swap(y1, y2);
    if (y2 == y0) return;
    for (int y = y0 < 0 ? 0 : y0; y <= y2 && y < SH; y++) {
        float xa = x0 + (float)(x2 - x0) * (y - y0) / (y2 - y0);
        float xb = y < y1 ? (y1 == y0 ? x0 : x0 + (float)(x1 - x0) * (y - y0) / (y1 - y0))
                          : (y2 == y1 ? x1 : x1 + (float)(x2 - x1) * (y - y1) / (y2 - y1));
        int a = (int)xa, b = (int)xb;
        if (a > b) std::swap(a, b);
        if (a < 0) a = 0;
        if (b >= SW) b = SW - 1;
        uint8_t* row = fb + y * SW;
        for (int x = a; x <= b; x++) row[x] |= mask;
    }
}

// ------------------------------------------------------------------ Glenz vectors
// Translucent checkered octahedron-cube ("glenz"): back faces then front faces
// are OR-ed into the buffer and the palette mixes the colours.
struct Glenz : Demo {
    const char* name() const override { return "Glenz Vectors"; }
    Res         res() const override { return Res::High; }
    void        begin() override {
        // bits: 1,2 = back face colours, 4,8 = front face colours, 16..: background
        for (int i = 0; i < 256; i++) {
            int bg = i >> 4;
            int r = bg * 3, g = bg * 2, b = 20 + bg * 6;
            if (i & 1) r += 40, g += 40, b += 120;
            if (i & 2) r += 120, g += 20, b += 60;
            if (i & 4) r = r / 3 + 160, g = g / 3 + 170, b = b / 3 + 255;
            if (i & 8) r = r / 3 + 230, g = g / 3 + 60, b = b / 3 + 120;
            pal[i] = rgb(r, g, b);
        }
    }
    void update(uint32_t t) override {
        // background: soft vertical gradient in the high nibble
        for (int y = 0; y < SH; y++) memset(fb + y * SW, ((y * 15) / SH) << 4, SW);
        // cube faces split into 4 triangles each around a raised centre (classic glenz shape)
        static const V3  cv[8]   = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
        static const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}};
        float            tt = t / 1000.0f, sc = 62 + 12 * sinf(tt * 1.7f) + syncNow().kick * 12;
        int              px[8], py[8];
        V3               rv[8];
        for (int i = 0; i < 8; i++) {
            rv[i] = rot(cv[i], tt * 0.9f, tt * 1.3f, tt * 0.4f);
            float z = rv[i].z + 4;
            px[i]   = SW / 2 + (int)(rv[i].x * sc * 4 / z);
            py[i]   = SH / 2 + (int)(rv[i].y * sc * 4 / z);
        }
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < 6; k++) {
                // face centre pushed outwards
                V3 n = {0, 0, 0};
                for (int j = 0; j < 4; j++) n.x += rv[f[k][j]].x, n.y += rv[f[k][j]].y, n.z += rv[f[k][j]].z;
                n.x *= 0.25f * 1.6f, n.y *= 0.25f * 1.6f, n.z *= 0.25f * 1.6f;
                float z  = n.z + 4;
                int   cx = SW / 2 + (int)(n.x * sc * 4 / z), cy = SH / 2 + (int)(n.y * sc * 4 / z);
                for (int j = 0; j < 4; j++) {
                    int a = f[k][j], b = f[k][(j + 1) & 3];
                    // winding gives facing
                    long cross = (long)(px[b] - px[a]) * (cy - py[a]) - (long)(py[b] - py[a]) * (cx - px[a]);
                    bool front = cross < 0;
                    if (front != (pass == 1)) continue;
                    uint8_t m = front ? ((j & 1) ? 4 : 8) : ((j & 1) ? 1 : 2);
                    triOr(px[a], py[a], px[b], py[b], cx, cy, m);
                }
            }
    }
};
Demo* makeGlenz() { return new Glenz(); }

// ------------------------------------------------------------------ Vector bobs
// Shaded balls on a morphing 3D shape, depth sorted, hi-res.
struct VectorBobs : Demo {
    static constexpr int N = 96, R = 7;
    uint8_t              ball[2 * R + 1][2 * R + 1];
    const char*          name() const override { return "Vector Bobs"; }
    Res                  res() const override { return Res::High; }
    void                 begin() override {
        for (int y = -R; y <= R; y++)
            for (int x = -R; x <= R; x++) {
                float d = sqrtf(x * x + y * y) / R;
                // highlight up-left
                float hl         = sqrtf((x + 2.5f) * (x + 2.5f) + (y + 2.5f) * (y + 2.5f)) / (R * 1.4f);
                ball[y + R][x + R] = d > 1 ? 0 : (uint8_t)(1 + (1 - hl) * 62);
            }
        // 4 colour sets x 64 shades
        static const uint8_t c[4][3] = {{255, 80, 40}, {60, 180, 255}, {255, 230, 60}, {200, 80, 255}};
        for (int s = 0; s < 4; s++)
            for (int i = 1; i < 64; i++) {
                int k               = i * 4;
                pal[s * 64 + i]     = rgb(c[s][0] * k >> 8 | (i > 54 ? 200 : 0), c[s][1] * k >> 8 | (i > 54 ? 200 : 0),
                                          c[s][2] * k >> 8 | (i > 54 ? 200 : 0));
            }
        pal[0] = rgb(0, 0, 18);
    }
    void update(uint32_t t) override {
        memset(fb, 0, SW * SH);
        float tt = t / 1000.0f, m = (sinf(tt * 0.5f) + 1) / 2;  // morph sphere <-> torus
        float zoom = 330 * (1 + 0.15f * syncNow().kick);
        struct P {
            int   x, y;
            float z;
            int   c;
        } p[N];
        for (int i = 0; i < N; i++) {
            float a = i * 2 * PI / 16, b = (i / 16) * 2 * PI / 6 + tt * 0.3f;
            V3    s = {cosf(a) * sinf(b / 2 + 0.3f), sinf(a) * sinf(b / 2 + 0.3f), cosf(b / 2 + 0.3f)};  // sphere-ish
            V3    o = {(1 + 0.45f * cosf(b)) * cosf(a), (1 + 0.45f * cosf(b)) * sinf(a), 0.45f * sinf(b)};  // torus
            V3    v = {s.x + (o.x - s.x) * m, s.y + (o.y - s.y) * m, s.z + (o.z - s.z) * m};
            v       = rot(v, tt * 0.7f, tt * 1.1f, tt * 0.3f);
            float z = v.z + 3.2f;
            p[i]    = {SW / 2 + (int)(v.x * zoom / z), SH / 2 + (int)(v.y * zoom / z), z, (i / 16) & 3};
        }
        // painter's algorithm: far first
        for (int i = 1; i < N; i++)
            for (int j = i; j > 0 && p[j].z > p[j - 1].z; j--) std::swap(p[j], p[j - 1]);
        for (int i = 0; i < N; i++)
            for (int y = -R; y <= R; y++) {
                int yy = p[i].y + y;
                if (yy < 0 || yy >= SH) continue;
                for (int x = -R; x <= R; x++) {
                    int xx = p[i].x + x;
                    uint8_t v = ball[y + R][x + R];
                    if (v && xx >= 0 && xx < SW) fb[yy * SW + xx] = p[i].c * 64 + v;
                }
            }
    }
};
Demo* makeVectorBobs() { return new VectorBobs(); }

// ------------------------------------------------------------------ Raycaster
// Wolf3D style DDA raycaster (after Lode Vandevenne's tutorial, BSD-2) with
// procedural textures, auto-piloted through a small maze.
struct Raycaster : Demo {
    static constexpr int MW = 16;
    const char*          map =
        "################"
        "#......#.......#"
        "#.##...#..###..#"
        "#.#....#....#..#"
        "#.#..###.##.#..#"
        "#....#......#..#"
        "####.#.####.##.#"
        "#....#....#....#"
        "#.######..#.##.#"
        "#......#..#..#.#"
        "#.####.#.###.#.#"
        "#.#....#.....#.#"
        "#.#.########.#.#"
        "#.#..........#.#"
        "#...########...#"
        "################";
    float       x = 1.5f, y = 1.5f, a = 0;
    uint32_t    last = 0;
    const char* name() const override { return "Raycaster"; }
    bool        wall(float px, float py) { return map[(int)py * MW + (int)px] == '#'; }
    void        begin() override {
        // 0..63 brick shades, 64..127 alt side, 128..191 floor, 192..255 ceiling
        for (int i = 0; i < 64; i++) {
            pal[i]       = rgb(150 * i >> 6, 60 * i >> 6, 40 * i >> 6);
            pal[64 + i]  = rgb(110 * i >> 6, 45 * i >> 6, 30 * i >> 6);
            pal[128 + i] = rgb(i, i, i + 10);
            pal[192 + i] = rgb(10, 10, 30 + i);
        }
    }
    void update(uint32_t t) override {
        float dt = (t - last) / 1000.0f;
        last     = t;
        if (dt > 0.1f) dt = 0.1f;
        // autopilot: follow the left wall
        float dx = cosf(a), dy = sinf(a), step = 1.4f * dt;
        bool  frontBlocked = wall(x + dx * 0.6f, y + dy * 0.6f);
        bool  leftOpen     = !wall(x + cosf(a - PI / 2) * 0.9f, y + sinf(a - PI / 2) * 0.9f);
        static float turn = 0;
        if (turn != 0) {
            float s = (turn > 0 ? 1 : -1) * 2.2f * dt;
            if (fabsf(s) > fabsf(turn)) s = turn;
            a += s;
            turn -= s;
        } else if (frontBlocked) {
            turn = PI / 2;
        } else {
            x += dx * step;
            y += dy * step;
            // re-centre in the corridor so turns stay clean
            float cx = floorf(x) + 0.5f, cy = floorf(y) + 0.5f;
            if (fabsf(dx) > 0.7f) y += (cy - y) * 0.1f;
            else x += (cx - x) * 0.1f;
            static int lastCell = -1;
            int        cell     = (int)y * MW + (int)x;
            if (leftOpen && cell != lastCell && fabsf(x - cx) < 0.15f && fabsf(y - cy) < 0.15f) {
                turn     = -PI / 2;
                lastCell = cell;
            }
        }
        float px = -sinf(a) * 0.66f, py = cosf(a) * 0.66f;
        dx = cosf(a), dy = sinf(a);
        for (int col = 0; col < LW; col++) {
            float cam = 2.0f * col / LW - 1, rx = dx + px * cam, ry = dy + py * cam;
            int   mx = (int)x, my = (int)y, sx, sy, side = 0;
            float ddx = fabsf(1 / rx), ddy = fabsf(1 / ry), sdx, sdy;
            if (rx < 0) sx = -1, sdx = (x - mx) * ddx;
            else sx = 1, sdx = (mx + 1 - x) * ddx;
            if (ry < 0) sy = -1, sdy = (y - my) * ddy;
            else sy = 1, sdy = (my + 1 - y) * ddy;
            while (map[my * MW + mx] != '#') {
                if (sdx < sdy) sdx += ddx, mx += sx, side = 0;
                else sdy += ddy, my += sy, side = 1;
            }
            float d  = side ? sdy - ddy : sdx - ddx;
            int   lh = (int)(LH / (d + 0.0001f));
            int   y0 = LH / 2 - lh / 2, y1 = LH / 2 + lh / 2;
            float wx = side ? x + d * rx : y + d * ry;
            wx -= floorf(wx);
            int tx    = (int)(wx * 32);
            int shade = (int)(63 / (1 + d * 0.35f));
            for (int yy = 0; yy < LH; yy++) {
                uint8_t c;
                if (yy < y0) c = 192 + yy * 63 / (LH / 2);
                else if (yy >= y1) c = 128 + (yy - LH / 2) * 63 / (LH / 2);
                else {
                    int ty = (yy - y0) * 32 / (lh ? lh : 1);
                    // brick texture: mortar lines every 8 rows, offset every other course
                    bool mortar = (ty & 7) == 0 || ((tx + ((ty >> 3) & 1) * 8) & 15) == 0;
                    int  s      = mortar ? shade / 3 : shade;
                    c           = (side ? 64 : 0) + s;
                }
                fb[yy * LW + col] = c;
            }
        }
    }
};
Demo* makeRaycaster() { return new Raycaster(); }
