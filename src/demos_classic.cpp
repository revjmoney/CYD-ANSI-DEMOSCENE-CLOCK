// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Classic 2D demoscene effects. All original implementations (MIT), written
// from the well known algorithms; low-res ones draw 160x120 into fb.
#include <Arduino.h>
#include "demo.h"

// ------------------------------------------------------------------ Plasma
struct Plasma : Demo {
    const char* name() const override { return "Plasma"; }
    void        update(uint32_t t) override {
        int a = t / 9, b = t / 13, c = t / 21, d = t / 7;
        // slowly drifting palette
        for (int i = 0; i < 256; i++)
            pal[i] = rgb(sinT[(i + a) & 255], sinT[(i * 2 + 85 + b) & 255], sinT[(i + 170 - c) & 255]);
        uint8_t* p = fb;
        for (int y = 0; y < LH; y++) {
            uint8_t ry = sinT[(y * 3 + b) & 255], ry2 = sinT[(y * 2 - d) & 255];
            for (int x = 0; x < LW; x++) {
                int v = sinT[(x * 2 + a) & 255] + ry + sinT[(x + y + c) & 255] + sinT[((sinT[(x + d) & 255] >> 1) + ry2) & 255];
                *p++  = v >> 2;
            }
        }
    }
};
Demo* makePlasma() { return new Plasma(); }

// ------------------------------------------------------------------ Doom fire
struct Fire : Demo {
    const char* name() const override { return "Doom Fire (stir it)"; }
    bool        interactive() const override { return true; }
    float       wind = 0;
    uint32_t    last = 0;
    void        begin() override {
        // the PSX Doom fire palette: black, red, orange, yellow, white
        static const uint8_t p[37][3] = {
            {7, 7, 7},       {31, 7, 7},      {47, 15, 7},     {71, 15, 7},     {87, 23, 7},     {103, 31, 7},
            {119, 31, 7},    {143, 39, 7},    {159, 47, 7},    {175, 63, 7},    {191, 71, 7},    {199, 71, 7},
            {223, 79, 7},    {223, 87, 7},    {223, 87, 7},    {215, 95, 7},    {215, 95, 7},    {215, 103, 15},
            {207, 111, 15},  {207, 119, 15},  {207, 127, 15},  {207, 135, 23},  {199, 135, 23},  {199, 143, 23},
            {199, 151, 31},  {191, 159, 31},  {191, 159, 31},  {191, 167, 39},  {191, 167, 39},  {191, 175, 47},
            {183, 175, 47},  {183, 183, 47},  {183, 183, 55},  {207, 207, 111}, {223, 223, 159}, {239, 239, 199},
            {255, 255, 255}};
        for (int i = 0; i < 37; i++) pal[i] = rgb(p[i][0], p[i][1], p[i][2]);
        memset(fb + (LH - 1) * LW, 36, LW);
    }
    void update(uint32_t t) override {
        // the words burn for a while, then the base goes out and comes back
        bool lit = (t % 20000) < 16000;
        memset(fb + (LH - 1) * LW, lit ? 36 : 0, LW);
        if (lit && (t % 20000) > 3000 && (t % 20000) < 13000) {
            fbText(fb, LW, LH, 8, 52, "CHEAP YELLOW", 36);
            fbText(fb, LW, LH, 24, 70, "DISPLAY", 36, 1);
        }
        // the finger is a torch: heat where it touches, and sideways motion blows the flames
        if (touch.down) {
            int fx = touch.x / 2, fy = touch.y / 2;
            for (int k = -4; k <= 4; k++)
                for (int j = -4; j <= 4; j++)
                    if (j * j + k * k <= 16 && fx + j >= 0 && fx + j < LW && fy + k >= 1 && fy + k < LH)
                        fb[(fy + k) * LW + fx + j] = 36;
            wind += touch.vx * 0.08f;
        }
        wind *= 0.95f;
        int windI = (int)wind;
        windI     = windI < -3 ? -3 : windI > 3 ? 3 : windI;
        for (int x = 0; x < LW; x++)
            for (int y = 1; y < LH; y++) {
                int src = y * LW + x, v = fb[src];
                if (!v) {
                    fb[src - LW] = 0;
                    continue;
                }
                int r   = esp_random() & 3;
                int dst = src - r + 1 + (windI && (esp_random() & 1) ? windI : 0);
                if (dst < LW) dst = LW;
                fb[dst - LW] = v - (r & 1);
            }
    }
};
Demo* makeFire() { return new Fire(); }

// ------------------------------------------------------------------ Tunnel (touch)
// Texture mapped tunnel from distance/angle lookup tables, shaded by depth.
// The tables cover a margin around the screen; each pixel looks up its depth,
// then re-reads the tables shifted in proportion to that depth, so the far end
// bends towards the finger while the near walls barely move.
// Drag sideways to twist it, flick up/down to change speed.
struct Tunnel : Demo {
    static constexpr int TW = 192, TH = 144, MX = (TW - LW) / 2, MY = (TH - LH) / 2;
    // how far the far end can swing. Bigger than the table margin is fine: the
    // shift scales with depth, so only the distant middle moves that far.
    static constexpr float BX = 46, BY = 33;
    uint8_t *dist, *ang, *tex;
    float    ox = 0, oy = 0, twist = 0, twistV = 0, speed = 1, pos = 0;
    uint32_t last = 0;
    const char* name() const override { return "Tunnel (touch me)"; }
    bool        interactive() const override { return true; }
    void        begin() override {
        dist = scratch(1);           // 27.6 KB
        ang  = scratch(1) + TW * TH; // 27.6 KB, still inside the three scratch slots
        tex  = (uint8_t*)arenaAlloc(64 * 64);
        for (int y = 0; y < TH; y++)
            for (int x = 0; x < TW; x++) {
                float dx = x - TW / 2 + 0.5f, dy = (y - TH / 2 + 0.5f) * 1.1f, r = sqrtf(dx * dx + dy * dy);
                dist[y * TW + x] = (uint8_t)fminf(255, 2048 / (r + 1));
                ang[y * TW + x]  = (uint8_t)(int)(256 * atan2f(dy, dx) / (2 * PI) + 256);
            }
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                int  v          = (x ^ y) & 63;
                bool edge       = (x & 15) == 0 || (y & 15) == 0;
                tex[y * 64 + x] = edge ? 63 : v / 2 + 8;
            }
        // 4 shade bands of 64 colours: index = shade*64 + texel
        for (int sh = 0; sh < 4; sh++)
            for (int i = 0; i < 64; i++) {
                int k           = (sh + 1) * 64;
                pal[sh * 64 + i] = rgb((i * 3 * k) >> 8, (i * 2 * k) >> 8, ((64 + i * 3) * k) >> 9);
            }
    }
    void update(uint32_t t) override {
        float dt = (t - last) / 1000.0f;
        last     = t;
        if (dt > 0.1f) dt = 0.1f;
        // where the far end should point: the finger, or a lazy Lissajous when idle
        float tx, ty;
        if (touch.down) {
            tx = (touch.x - SW / 2) * BX / (SW / 2.0f);
            ty = (touch.y - SH / 2) * BY / (SH / 2.0f);
            twistV += touch.vx * 0.15f;
            speed -= touch.vy * 0.05f;
        } else {
            // two sines each way, so the path never quite repeats
            tx = (sinf(t / 1100.0f) * 0.7f + sinf(t / 2900.0f) * 0.3f) * BX;
            ty = (sinf(t / 1500.0f) * 0.7f + sinf(t / 3700.0f + 1) * 0.3f) * BY;
            twistV += sinf(t / 2300.0f) * 0.25f;
        }
        ox += (tx - ox) * (touch.down ? 0.15f : 0.06f);
        oy += (ty - oy) * (touch.down ? 0.15f : 0.06f);
        twistV *= 0.94f;
        twist += twistV * dt * 60;
        speed += (1 - speed) * 0.02f;  // drifts back to cruising speed
        speed = speed < -4 ? -4 : speed > 8 ? 8 : speed;
        pos += speed * dt * (160 + syncNow().kick * 160);
        int du = (int)pos, dv = (int)twist + (sinS[(t / 30) & 255] >> 2);
        int oxi = (int)(ox * 256), oyi = (int)(oy * 256);
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++) {
                int base = (y + MY) * TW + x + MX;
                int d0   = dist[base];
                // bend: shift grows with depth
                int sx = x + MX + ((oxi * d0) >> 16), sy = y + MY + ((oyi * d0) >> 16);
                sx = sx < 0 ? 0 : sx >= TW ? TW - 1 : sx;
                sy = sy < 0 ? 0 : sy >= TH ? TH - 1 : sy;
                int i = sy * TW + sx, d = dist[i];
                int shade = d > 160 ? 0 : d > 90 ? 1 : d > 50 ? 2 : 3;
                if (d > 250) shade = 0;
                fb[y * LW + x] = shade * 64 + tex[(((d + du) & 63) << 6) | ((ang[i] + dv) & 63)];
            }
    }
};
Demo* makeTunnel() { return new Tunnel(); }

// ------------------------------------------------------------------ Rotozoomer
struct Rotozoom : Demo {
    uint8_t*    tex;  // 128x128
    const char* name() const override { return "Rotozoomer"; }
    void        begin() override {
        tex = scratch(3);  // 16 KB fits in the 19.2 KB slot
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) tex[y * 128 + x] = (((x >> 4) ^ (y >> 4)) & 1) ? 1 : 2;
        // stamp "CYD" big into the middle of the tile
        fbText(tex, 128, 128, 40, 48, "CYD", 3, 2);
        for (int i = 0; i < 256; i++) pal[i] = 0;
        pal[1] = rgb(30, 20, 90);
        pal[2] = rgb(60, 40, 160);
        pal[3] = rgb(255, 220, 40);
    }
    void update(uint32_t t) override {
        float a = t / 1400.0f, z = 0.6f + 0.45f * sinf(t / 1900.0f);
        int   ca = (int)(cosf(a) * z * 65536), sa = (int)(sinf(a) * z * 65536);
        int   u0 = (int)(t * 40) - (LW / 2) * ca + (LH / 2) * sa, v0 = (int)(t * 25) - (LW / 2) * sa - (LH / 2) * ca;
        uint8_t* p = fb;
        for (int y = 0; y < LH; y++) {
            int u = u0, v = v0;
            for (int x = 0; x < LW; x++) {
                *p++ = tex[((v >> 16) & 127) * 128 + ((u >> 16) & 127)];
                u += ca;
                v += sa;
            }
            u0 -= sa;
            v0 += ca;
        }
    }
};
Demo* makeRotozoom() { return new Rotozoom(); }

// ------------------------------------------------------------------ Starfield (hi-res)
struct Starfield : Demo {
    static constexpr int N = 700;
    float                sx[N], sy[N], sz[N];
    uint32_t             last = 0;
    const char*          name() const override { return "Starfield"; }
    Res                  res() const override { return Res::High; }
    void                 begin() override {
        for (int i = 0; i < N; i++) reset(i, random(1, 1000) / 1000.0f * 8);
        // 0..63 grey ramp for trails, 64..127 bright blue-white
        palRamp(0, 63, 0, 0, 0, 255, 255, 255);
        palRamp(64, 127, 0, 0, 30, 160, 200, 255);
    }
    void reset(int i, float z) {
        // round spawn disc, wide enough to fill the screen wherever the vanishing point is
        float a = random(0, 6283) / 1000.0f, r = sqrtf(random(0, 1000) / 1000.0f) * 28;
        sx[i]   = cosf(a) * r;
        sy[i]   = sinf(a) * r;
        sz[i] = z;
    }
    void update(uint32_t t) override {
        float dt = (t - last) / 1000.0f;
        last     = t;
        // fade: trails
        for (int i = 0; i < SW * SH; i++) {
            uint8_t v = fb[i];
            if (v) fb[i] = (v & 63) > 6 ? v - 6 : 0;
        }
        float speed = 3 + 2.5f * sinf(t / 3000.0f) + syncNow().kick * 9;
        // the ship banks and steers: the roll swings back and forth and the
        // vanishing point roams around the screen
        float rot = t / 4000.0f + 1.2f * sinf(t / 2700.0f), cr = cosf(rot), sr = sinf(rot);
        int   cx  = SW / 2 + (int)((sinf(t / 1900.0f) * 0.7f + sinf(t / 4300.0f) * 0.3f) * 95);
        int   cy  = SH / 2 + (int)((sinf(t / 2600.0f) * 0.7f + sinf(t / 5100.0f) * 0.3f) * 65);
        for (int i = 0; i < N; i++) {
            sz[i] -= speed * dt;
            if (sz[i] < 0.05f) reset(i, 8);
            float x = sx[i] * cr - sy[i] * sr, y = sx[i] * sr + sy[i] * cr;
            int   px = cx + (int)(x / sz[i] * 30), py = cy + (int)(y / sz[i] * 30);
            if (px < 0 || px >= SW || py < 0 || py >= SH) {
                reset(i, 8);
                continue;
            }
            int b = 63 - (int)(sz[i] * 5);
            if (b < 18) b = 18;
            fb[py * SW + px] = b;
            if (sz[i] < 1.5f && px + 1 < SW && py + 1 < SH) fb[py * SW + px + 1] = fb[(py + 1) * SW + px] = 64 + b;
        }
    }
};
Demo* makeStarfield() { return new Starfield(); }

// ------------------------------------------------------------------ Copper bars + sine scroller
struct CopperScroller : Demo {
    const char* msg =
        "        CYD DEMOSCENE  ***  greetings from Rev J Money and the Simian crew  ***  "
        "a twelve dollar ESP32 doing what Amigas did in 1989  ***  plasma, fire, tunnels, rotozoom, voxels, "
        "glenz vectors and real ANSI art from ACiD, iCE, Fire and Blocktronics  ***  tap for the next part  ***  "
        "mm's are for communists  ***        ";
    int         scroll = 0, len;
    int16_t     wave[SW];
    uint16_t    bar[SH], rainbow[32];
    uint32_t    t = 0;
    const char* name() const override { return "Copper + Sine Scroller"; }
    Res         res() const override { return Res::Custom; }
    void        begin() override { len = strlen(msg); }
    void        update(uint32_t ms) override {
        t      = ms;
        scroll = ms / 9;
        for (int x = 0; x < SW; x++) wave[x] = 104 + (sinS[((x + scroll) / 2 + ms / 10) & 255] * 50 >> 7);
        // copper: dark blue gradient background with 5 bars bobbing on sines
        for (int y = 0; y < SH; y++) bar[y] = rgb(0, 0, y / 6);
        static const uint8_t col[5][3] = {{255, 40, 40}, {255, 200, 0}, {40, 255, 80}, {40, 160, 255}, {220, 60, 255}};
        for (int b = 0; b < 5; b++) {
            int cy = 120 + (sinS[(ms / 12 + b * 24) & 255] * 100 >> 7);
            int hw = 10 + (int)(syncNow().kick * 6);
            for (int k = -hw; k <= hw; k++) {
                int y = cy + k;
                if (y < 0 || y >= SH) continue;
                int f  = 255 - abs(k) * 240 / hw;
                bar[y] = rgb(col[b][0] * f >> 8, col[b][1] * f >> 8, col[b][2] * f >> 8);
            }
        }
        for (int i = 0; i < 32; i++) rainbow[i] = hsv(i * 8 + ms / 10, 200, 255);
    }
    void strip(int y0, int h, uint16_t* out) override {
        for (int y = y0; y < y0 + h; y++, out += SW) {
            uint16_t bg = bar[y];
            for (int x = 0; x < SW; x++) {
                int gy = y - wave[x];
                uint16_t c = bg;
                if (gy >= 0 && gy < 32) {
                    int     sxp = x + scroll;
                    char    ch  = msg[(sxp / 16) % len];
                    uint8_t bits = font16[(uint8_t)ch * 16 + gy / 2];
                    if (bits & (0x80 >> ((sxp & 15) >> 1))) c = rainbow[gy];
                } else if (gy >= 34 && gy < 34 + 20 && y > 150) {
                    // squashed, darker reflection
                    int     ry  = 31 - (gy - 34) * 3 / 2;
                    int     sxp = x + scroll + (sinS[(y * 8 + t / 4) & 255] >> 5);
                    char    ch  = msg[(sxp / 16) % len];
                    if (ry >= 0) {
                        uint8_t bits = font16[(uint8_t)ch * 16 + ry / 2];
                        if (bits & (0x80 >> ((sxp & 15) >> 1))) c = rgb(40, 60, 140);
                    }
                }
                out[x] = c;
            }
        }
    }
};
Demo* makeCopperScroller() { return new CopperScroller(); }

// ------------------------------------------------------------------ Metaballs
struct Metaballs : Demo {
    const char* name() const override { return "Metaballs"; }
    void        begin() override {
        for (int i = 0; i < 256; i++) {
            int v = i < 128 ? 0 : (i - 128) * 2;
            pal[i] = i < 100 ? rgb(i / 4, 0, i / 2) : i < 128 ? rgb(i, i / 4, 120) : rgb(255, 128 + v / 2, v);
        }
    }
    void update(uint32_t t) override {
        int bx[6], by[6], strength = 150000 + (int)(syncNow().kick * 70000);
        for (int b = 0; b < 6; b++) {
            bx[b] = LW / 2 + (sinS[(t / (17 + b * 3) + b * 40) & 255] * (40 + b * 4) >> 7);
            by[b] = LH / 2 + (sinS[(t / (23 + b * 2) + b * 90 + 64) & 255] * (30 + b * 3) >> 7);
        }
        uint8_t* p = fb;
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++) {
                int s = 0;
                for (int b = 0; b < 6; b++) {
                    int dx = x - bx[b], dy = y - by[b];
                    s += strength / (dx * dx + dy * dy + 40);
                }
                *p++ = s > 255 * 16 ? 255 : s >> 4;  // edge of a ball sits around 128
            }
    }
};
Demo* makeMetaballs() { return new Metaballs(); }

// ------------------------------------------------------------------ Twister
struct Twister : Demo {
    const char* name() const override { return "Twister"; }
    void        begin() override {
        // 4 faces x 64 shades
        static const uint8_t c[4][3] = {{255, 60, 60}, {60, 200, 255}, {255, 220, 60}, {120, 255, 120}};
        for (int f = 0; f < 4; f++)
            for (int s = 0; s < 64; s++) pal[f * 64 + s] = rgb(c[f][0] * s >> 6, c[f][1] * s >> 6, c[f][2] * s >> 6);
        pal[0] = rgb(10, 0, 20);
    }
    void update(uint32_t t) override {
        float tt = t / 1000.0f;
        for (int y = 0; y < LH; y++) {
            uint8_t* row = fb + y * LW;
            for (int x = 0; x < LW; x++) row[x] = 0;
            float a  = sinf(tt * 0.9f + y * 0.012f * sinf(tt * 0.3f) * 3) * 3.2f + tt;
            float cx = LW / 2 + sinf(tt * 1.3f + y * 0.03f) * 20, w = 34;
            float xs[5];
            for (int i = 0; i < 5; i++) xs[i] = cx + sinf(a + i * PI / 2) * w;
            for (int i = 0; i < 4; i++) {
                float x0 = xs[i], x1 = xs[i + 1];
                if (x1 <= x0) continue;  // back face
                int shade = (int)((x1 - x0) / (w * 1.42f) * 63);
                if (shade > 63) shade = 63;
                for (int x = (int)x0; x < (int)x1; x++)
                    if (x >= 0 && x < LW) row[x] = i * 64 + (x == (int)x0 ? 63 : shade);
            }
        }
    }
};
Demo* makeTwister() { return new Twister(); }

// ------------------------------------------------------------------ Kefrens bars
struct Kefrens : Demo {
    const char* name() const override { return "Kefrens Bars"; }
    void        begin() override {
        for (int i = 0; i < 16; i++) {
            int k        = i < 8 ? i : 15 - i;
            pal[1 + i]   = rgb(60 + k * 24, 20 + k * 28, 255 - k * 10);
            pal[17 + i]  = rgb(255, 60 + k * 24, k * 20);
        }
    }
    void update(uint32_t t) override {
        uint8_t line[LW + 16];
        memset(line, 0, sizeof(line));
        for (int y = 0; y < LH; y++) {
            int x = LW / 2 - 8 + ((sinS[(y * 2 + t / 7) & 255] + sinS[(y * 3 - t / 11) & 255]) * 70 >> 8);
            int c = (y / 30) & 1 ? 17 : 1;
            for (int i = 0; i < 16; i++)
                if (x + i >= 0 && x + i < LW) line[x + i] = c + i;
            memcpy(fb + y * LW, line, LW);
        }
    }
};
Demo* makeKefrens() { return new Kefrens(); }
