// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// RotatoCAM 4th axis: a desktop router with a rotary unit. The stock spins
// between a chuck and a tailstock while the router rides along it on the
// gantry, roughing it down in layers and then finishing a turned table leg,
// a barley-sugar twist or a rope twist. Flat-shaded 3D, painter's sort,
// chips, amber DRO in inches. Drag to orbit.
#include <Arduino.h>
#include <algorithm>
#include "demo.h"

namespace {

struct V3 {
    float x, y, z;
};
static inline V3    add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3    sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3    mul(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3    cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline V3    norm(V3 a) {
    float l = sqrtf(dot(a, a));
    return l > 1e-6f ? mul(a, 1 / l) : a;
}

enum Mat { BASE, AMBERM, STEEL, WOOD, SPINDLE, TOOL };
constexpr int BG = 192, CHIP = 224, AMBER = 240;

struct Tri {  // 16 bytes
    int16_t  x[3], y[3];
    uint16_t z;
    uint8_t  col;
};

constexpr int   NX = 40, NA = 24;          // stock: rings along X x angles around it
constexpr float LEN = 150, R0 = 16, AXIS_Y = 46;
constexpr float X0 = -LEN / 2;
constexpr int   MAXTRI = 1150, MAXCHIP = 150;

struct Chip {
    V3    p, v;
    float life;
};

struct Rotary : Demo {
    Tri*     tris;
    int      ntri = 0;
    Chip*    chips;
    int      nchip = 0;
    uint16_t rad[NX][NA], target[NX][NA];  // radius * 256
    int      shape = 0, pass = 0, dir = 1;
    float    A = 0, toolX = X0, yaw = 0.25f, pitch = 0.42f, yawV = 0, pitchV = 0, dist = 255;
    V3       camPos, camF, camR, camU, light, tip{X0, AXIS_Y + R0, 0};
    uint32_t last = 0, doneAt = 0;
    bool     wasDown = false;

    const char* name() const override { return "RotatoCAM 4th Axis"; }
    Res         res() const override { return Res::High; }
    bool        interactive() const override { return true; }
    uint32_t    duration() const override { return 45000; }
    float       pulse() const override { return 0.05f; }

    void begin() override {
        tris  = (Tri*)arenaAlloc(MAXTRI * sizeof(Tri));
        chips = (Chip*)arenaAlloc(MAXCHIP * sizeof(Chip));
        static const uint8_t lo[6][3] = {{22, 28, 40}, {70, 40, 0}, {30, 32, 36}, {70, 40, 18}, {25, 25, 30}, {70, 50, 10}};
        static const uint8_t hi[6][3] = {{120, 140, 175}, {255, 190, 50}, {170, 172, 182}, {235, 180, 120}, {150, 155, 170}, {255, 225, 130}};
        for (int m = 0; m < 6; m++) palRamp(m * 32, m * 32 + 31, lo[m][0], lo[m][1], lo[m][2], hi[m][0], hi[m][1], hi[m][2]);
        palRamp(BG, BG + 31, 4, 6, 12, 18, 26, 44);
        palRamp(CHIP, CHIP + 7, 255, 225, 160, 200, 140, 80);   // fresh wood chips
        palRamp(CHIP + 8, CHIP + 15, 190, 140, 90, 110, 80, 50);
        pal[AMBER]     = rgb(245, 168, 0);
        pal[AMBER + 1] = rgb(120, 80, 0);
        pal[AMBER + 2] = rgb(0, 0, 0);
        light          = norm({0.4f, 0.85f, -0.4f});
        newStock();
    }

    // finished shapes: radius as a function of position along the part (0..1) and angle
    float shapeR(float u, float th) const {
        switch (shape % 3) {
            case 0: {  // turned table leg: square-ish top block, beads, a cove, a long taper, a foot
                if (u < 0.12f) return 15.5f;
                if (u < 0.16f) return 11.5f;
                if (u < 0.22f) return 13.5f - 6 * fabsf(u - 0.19f) / 0.03f * 0.5f;  // bead
                if (u < 0.30f) return 9.5f + 2.5f * sinf((u - 0.22f) / 0.08f * PI);  // cove
                if (u < 0.86f) return 12 - 6 * (u - 0.30f) / 0.56f;                     // taper
                if (u < 0.92f) return 8.5f;                                             // foot bead
                return 6.5f;
            }
            case 1: {  // barley-sugar: two helical strands around a core
                if (u < 0.08f || u > 0.92f) return 14;
                float v = cosf(2 * th + u * 24);
                return 8 + 7 * (v > 0 ? v : 0);
            }
            default: {  // four-strand rope twist
                if (u < 0.08f || u > 0.92f) return 14;
                float v = fabsf(sinf(2 * th + u * 30));
                return 9 + 5.5f * v;
            }
        }
    }
    void newStock() {
        for (int i = 0; i < NX; i++)
            for (int a = 0; a < NA; a++) {
                float u = (float)i / (NX - 1), th = a * 2 * PI / NA;
                rad[i][a]    = (uint16_t)(R0 * 256);
                target[i][a] = (uint16_t)(fminf(R0, shapeR(u, th)) * 256);
            }
        pass   = 0;
        toolX  = X0 + 4;
        dir    = 1;
        doneAt = 0;
    }

    // ---- geometry ----
    void quad(V3 p0, V3 p1, V3 p2, V3 p3, int mat) {
        V3 n = norm(cross(sub(p1, p0), sub(p3, p0)));
        if (dot(n, sub(camPos, p0)) <= 0) return;
        float   l = dot(n, light);
        int     shade = (int)(6 + (l > 0 ? l : 0) * 25);
        V3      q[4]  = {p0, p1, p2, p3};
        int16_t sx[4], sy[4];
        float   zs = 0;
        for (int i = 0; i < 4; i++) {
            V3    d = sub(q[i], camPos);
            float z = dot(d, camF);
            if (z < 5) return;
            float k = 230 / z;
            sx[i]   = (int16_t)(SW / 2 + dot(d, camR) * k);
            sy[i]   = (int16_t)(SH / 2 - dot(d, camU) * k);
            zs += z;
        }
        uint8_t col = mat * 32 + (shade > 31 ? 31 : shade);
        for (int t = 0; t < 2 && ntri < MAXTRI; t++) {
            Tri& tr = tris[ntri++];
            int  a = 0, b = t ? 2 : 1, c = t ? 3 : 2;
            tr.x[0] = sx[a], tr.y[0] = sy[a], tr.x[1] = sx[b], tr.y[1] = sy[b], tr.x[2] = sx[c], tr.y[2] = sy[c];
            tr.z   = (uint16_t)(zs * 4);
            tr.col = col;
        }
    }
    void box(V3 c, V3 s, int mat) {
        V3 p[8];
        for (int i = 0; i < 8; i++)
            p[i] = {c.x + (i & 1 ? s.x : -s.x) / 2, c.y + (i & 2 ? s.y : -s.y) / 2, c.z + (i & 4 ? s.z : -s.z) / 2};
        quad(p[0], p[2], p[3], p[1], mat);
        quad(p[4], p[5], p[7], p[6], mat);
        quad(p[0], p[4], p[6], p[2], mat);
        quad(p[1], p[3], p[7], p[5], mat);
        quad(p[0], p[1], p[5], p[4], mat);
        quad(p[2], p[6], p[7], p[3], mat);
    }
    // cylinder along X from x0 to x1, radii r0 -> r1 (a cone when they differ), spun by rot
    void cylX(float x0, float x1, float r0, float r1, int segs, int mat, float rot = 0) {
        for (int i = 0; i < segs; i++) {
            float a0 = rot + i * 2 * PI / segs, a1 = rot + (i + 1) * 2 * PI / segs;
            V3    p00 = {x0, AXIS_Y + r0 * sinf(a0), r0 * cosf(a0)}, p01 = {x0, AXIS_Y + r0 * sinf(a1), r0 * cosf(a1)};
            V3    p10 = {x1, AXIS_Y + r1 * sinf(a0), r1 * cosf(a0)}, p11 = {x1, AXIS_Y + r1 * sinf(a1), r1 * cosf(a1)};
            quad(p00, p10, p11, p01, mat);
            V3 c0 = {x0, AXIS_Y, 0}, c1 = {x1, AXIS_Y, 0};
            quad(p01, c0, c0, p00, mat);
            quad(p10, c1, c1, p11, mat);
        }
    }

    // ---- the cut ----
    void cut(float dt) {
        if (doneAt) {
            if (millis() - doneAt > 2500) {
                shape++;
                newStock();
            }
            return;
        }
        // roughing passes take 3.5 mm a layer, the last pass goes to the finished shape
        float floorR = R0 - 3.5f * (pass + 1);
        bool  finish = floorR <= 6;
        toolX += dir * 26 * dt;
        if (toolX > X0 + LEN - 4 || toolX < X0 + 4) {
            dir = -dir;
            toolX = fminf(fmaxf(toolX, X0 + 4), X0 + LEN - 4);
            if (finish) doneAt = millis();
            else pass++;
        }
        // the angle at the top of the stock is the one under the cutter
        int   a  = ((int)lroundf((PI / 2 - A) / (2 * PI) * NA) % NA + NA) % NA;
        int   ri = (int)lroundf((toolX - X0) / LEN * (NX - 1));
        bool  removed = false;
        float topR = 0;
        for (int i = ri - 1; i <= ri + 1; i++) {
            if (i < 0 || i >= NX) continue;
            for (int k = -1; k <= 1; k++) {  // the cutter is wider than one angle step
                int      aa   = (a + k + NA) % NA;
                uint16_t goal = (uint16_t)(fmaxf(target[i][aa] / 256.0f, finish ? 0 : floorR) * 256);
                if (rad[i][aa] > goal) {
                    rad[i][aa] = rad[i][aa] - goal > 300 ? rad[i][aa] - 300 : goal;
                    removed    = true;
                }
            }
            if (i == ri) topR = rad[i][a] / 256.0f;
        }
        V3 want = {toolX, AXIS_Y + topR + 1, 0};
        tip     = add(tip, mul(sub(want, tip), fminf(1, dt * 12)));
        if (removed)
            for (int k = 0; k < 3 && nchip < MAXCHIP; k++)
                chips[nchip++] = {tip, {(float)((int)(esp_random() % 60) - 30), 25 + (float)(esp_random() % 35), 30 + (float)(esp_random() % 30)}, 0};
    }
    void chipsStep(float dt) {
        for (int i = 0; i < nchip;) {
            Chip& c = chips[i];
            c.v.y -= 260 * dt;
            c.p = add(c.p, mul(c.v, dt));
            c.life += dt;
            if (c.p.y < 10) {
                c.p.y = 10;
                c.v   = {c.v.x * 0.3f, -c.v.y * 0.25f, c.v.z * 0.3f};
            }
            if (c.life > 2.5f) c = chips[--nchip];
            else i++;
        }
    }

    void fill(const Tri& t) {
        int x0 = t.x[0], y0 = t.y[0], x1 = t.x[1], y1 = t.y[1], x2 = t.x[2], y2 = t.y[2];
        if (y1 < y0) std::swap(x0, x1), std::swap(y0, y1);
        if (y2 < y0) std::swap(x0, x2), std::swap(y0, y2);
        if (y2 < y1) std::swap(x1, x2), std::swap(y1, y2);
        if (y2 == y0) return;
        int ystart = y0 < 0 ? 0 : y0, yend = y2 >= SH ? SH - 1 : y2;
        for (int y = ystart; y <= yend; y++) {
            int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
            int xb = y < y1 ? (y1 == y0 ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0))
                            : (y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1));
            if (xa > xb) std::swap(xa, xb);
            if (xa < 0) xa = 0;
            if (xb >= SW) xb = SW - 1;
            if (xa <= xb) memset(fb + y * SW + xa, t.col, xb - xa + 1);
        }
    }

    void update(uint32_t ms) override {
        float dt = (ms - last) / 1000.0f;
        last     = ms;
        if (dt > 0.05f) dt = 0.05f;
        if (touch.down) {
            if (wasDown) {
                yawV   = -touch.vx * 0.012f;
                pitchV = touch.vy * 0.008f;
            }
        } else {
            yawV += (0.6f * sinf(ms / 9000.0f) * dt - yawV) * 0.05f;  // sway, don't spin round
            pitchV *= 0.9f;
        }
        wasDown = touch.down;
        yaw += yawV;
        pitch += pitchV;
        pitch = pitch < 0.08f ? 0.08f : pitch > 1.2f ? 1.2f : pitch;

        A += 3.2f * dt;
        cut(dt);
        chipsStep(dt);

        V3 target = {0, 50, 0};
        camPos    = add(target, {dist * cosf(pitch) * sinf(yaw), dist * sinf(pitch), -dist * cosf(pitch) * cosf(yaw)});
        camF      = norm(sub(target, camPos));
        camR      = norm(cross({0, 1, 0}, camF));
        camU      = cross(camF, camR);
        for (int y = 0; y < SH; y++) memset(fb + y * SW, BG + y * 31 / SH, SW);

        ntri = 0;
        // bed, rails, headstock + chuck (amber), tailstock + live centre
        box({0, 5, 0}, {250, 10, 130}, BASE);
        box({0, 14, -52}, {240, 8, 10}, STEEL);
        box({0, 14, 52}, {240, 8, 10}, STEEL);
        box({X0 - 26, 36, 0}, {34, 52, 48}, AMBERM);
        cylX(X0 - 9, X0 + 1, 22, 22, 12, STEEL, A);  // chuck body turns with the part
        for (int j = 0; j < 3; j++) {                 // three jaws
            float ja = A + j * 2 * PI / 3;
            V3    c  = {X0 + 4, AXIS_Y + 19 * sinf(ja), 19 * cosf(ja)};
            box(c, {10, 6, 6}, AMBERM);
        }
        box({X0 + LEN + 26, 32, 0}, {26, 44, 40}, AMBERM);
        cylX(X0 + LEN + 1, X0 + LEN + 13, 3, 8, 8, STEEL);

        // the stock, rebuilt from the live radii and turned by A
        for (int i = 0; i + 1 < NX; i++) {
            float x0 = X0 + LEN * i / (NX - 1), x1 = X0 + LEN * (i + 1) / (NX - 1);
            for (int a = 0; a < NA; a++) {
                int   b = (a + 1) % NA;
                float t0 = A + a * 2 * PI / NA, t1 = A + b * 2 * PI / NA;
                float r00 = rad[i][a] / 256.0f, r01 = rad[i][b] / 256.0f, r10 = rad[i + 1][a] / 256.0f, r11 = rad[i + 1][b] / 256.0f;
                quad({x0, AXIS_Y + r00 * sinf(t0), r00 * cosf(t0)}, {x1, AXIS_Y + r10 * sinf(t0), r10 * cosf(t0)},
                     {x1, AXIS_Y + r11 * sinf(t1), r11 * cosf(t1)}, {x0, AXIS_Y + r01 * sinf(t1), r01 * cosf(t1)}, WOOD);
            }
        }
        // like a desktop router with a rotary on the bed: a bridge along X at the
        // back, a carriage sliding on it, the router hanging forward over the part
        float gx = tip.x, ty = tip.y;
        box({-138, 62, 56}, {14, 104, 16}, BASE);
        box({138, 62, 56}, {14, 104, 16}, BASE);
        box({0, 118, 56}, {290, 14, 16}, BASE);
        box({gx, 104, 44}, {30, 34, 8}, AMBERM);         // carriage
        box({gx, ty + 34, 22}, {16, 10, 40}, AMBERM);    // Z plate reaching forward
        router(gx, ty);

        std::sort(tris, tris + ntri, [](const Tri& a, const Tri& b) { return a.z > b.z; });
        for (int i = 0; i < ntri; i++) fill(tris[i]);

        for (int i = 0; i < nchip; i++) {
            V3    d = sub(chips[i].p, camPos);
            float z = dot(d, camF);
            if (z < 5) continue;
            int x = SW / 2 + (int)(dot(d, camR) * 230 / z), y = SH / 2 - (int)(dot(d, camU) * 230 / z);
            if (x < 0 || x >= SW - 1 || y < 0 || y >= SH - 1) continue;
            uint8_t c = CHIP + (int)(chips[i].life * 6) % 16;
            fb[y * SW + x] = fb[y * SW + x + 1] = fb[(y + 1) * SW + x] = c;
        }

        char        line[32];
        float       in = 1 / 25.4f;
        const char* lab[4] = {"X", "Y", "Z", "A"};
        float       val[4] = {tip.x * in, tip.z * in, (tip.y - AXIS_Y) * in, fmodf(A * 57.2958f, 360)};
        for (int i = 0; i < 4; i++) {
            snprintf(line, sizeof line, i < 3 ? "%s%+7.3f\"" : "%s%7.2f\xF8", lab[i], val[i]);
            fbText(fb, SW, SH, 7, 7 + i * 16, line, AMBER + 2);
            fbText(fb, SW, SH, 6, 6 + i * 16, line, AMBER);
        }
        fbText(fb, SW, SH, SW - 9 * 16 - 6, 8, "ROTATOCAM", AMBER, 2);
        fbText(fb, SW, SH, SW - 15 * 8 - 6, 42, "4TH AXIS ROTARY", AMBER + 1);
        static const char* names[3] = {"TABLE LEG", "BARLEY TWIST", "ROPE TWIST"};
        fbText(fb, SW, SH, 6, SH - 22, doneAt ? "PART DONE" : names[shape % 3], AMBER);
    }

    // the router, as stacked boxes: cutter, collet, motor body, cap
    void router(float gx, float ty) {
        box({gx, ty + 4, 0}, {4, 8, 4}, TOOL);
        box({gx, ty + 12, 0}, {9, 8, 9}, STEEL);
        box({gx, ty + 30, 0}, {20, 28, 20}, SPINDLE);
        box({gx, ty + 48, 0}, {14, 8, 14}, SPINDLE);
    }
};

}  // namespace

Demo* makeRotatoRotary() { return new Rotary(); }
