// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// RotatoCAM: a 5-axis trunnion machine carving a part in real time.
// Flat-shaded 3D (painter's sort), a stock cylinder that loses material where
// the cutter touches it, flying chips, and an amber RotatoCAM DRO in inches.
// Drag to orbit the camera; it orbits by itself when left alone.
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

// affine transform: rotation r (columns = local axes) plus translation t
struct M {
    V3 cx{1, 0, 0}, cy{0, 1, 0}, cz{0, 0, 1}, t{0, 0, 0};
    V3 apply(V3 p) const { return add(t, add(add(mul(cx, p.x), mul(cy, p.y)), mul(cz, p.z))); }
    V3 dir(V3 p) const { return add(add(mul(cx, p.x), mul(cy, p.y)), mul(cz, p.z)); }
};
static M operator*(const M& a, const M& b) {  // a after b
    M r;
    r.cx = a.dir(b.cx);
    r.cy = a.dir(b.cy);
    r.cz = a.dir(b.cz);
    r.t  = a.apply(b.t);
    return r;
}
static M rotX(float a) {
    M m;
    m.cy = {0, cosf(a), sinf(a)};
    m.cz = {0, -sinf(a), cosf(a)};
    return m;
}
static M rotY(float a) {
    M m;
    m.cx = {cosf(a), 0, -sinf(a)};
    m.cz = {sinf(a), 0, cosf(a)};
    return m;
}
static M trans(float x, float y, float z) {
    M m;
    m.t = {x, y, z};
    return m;
}

enum Mat { BASE, TRUNNION, STEEL, ALU, SPINDLE, TOOL };
constexpr int BG = 192, CHIP = 224, AMBER = 240;

struct Tri {  // 16 bytes
    int16_t  x[3], y[3];
    uint16_t z;  // depth * 16
    uint8_t  col;
};

constexpr int NA = 24, NH = 10;   // workpiece: angles x height rings (vertices)
constexpr float STOCK_R = 17, STOCK_H = 30;
constexpr int   MAXTRI = 760, MAXCHIP = 200;  // ~21 KB of the per-demo arena

struct Chip {
    V3    p, v;
    float life;
};

struct RotatoCAM : Demo {
    Tri*     tris;
    int      ntri = 0;
    Chip*    chips;
    int      nchip = 0;
    float    rad[NH][NA];   // current radius per vertex
    float    target[NH][NA];
    int      ring = NH - 1, shape = 0;
    float    C = 0, A = 0, yaw = 0.6f, pitch = 0.42f, yawV = 0, pitchV = 0, dist = 225;
    V3       camPos, camF, camR, camU, light, tip{0, 0, 0};
    uint32_t last = 0, doneAt = 0;
    int      tdx = 0, tdy = 0;
    bool     wasDown = false;

    const char* name() const override { return "RotatoCAM 5-Axis"; }
    Res         res() const override { return Res::High; }
    bool        interactive() const override { return true; }
    uint32_t    duration() const override { return 40000; }
    float       pulse() const override { return 0.05f; }

    void begin() override {
        tris  = (Tri*)arenaAlloc(MAXTRI * sizeof(Tri));
        chips = (Chip*)arenaAlloc(MAXCHIP * sizeof(Chip));
        // palettes: 6 materials x 32 shades, background, chips, amber text
        static const uint8_t lo[6][3] = {{22, 28, 40}, {70, 40, 0}, {30, 32, 36}, {55, 58, 64}, {60, 10, 10}, {70, 50, 10}};
        static const uint8_t hi[6][3] = {{120, 140, 175}, {255, 190, 50}, {170, 172, 182}, {240, 242, 250}, {230, 70, 55}, {255, 225, 130}};
        for (int m = 0; m < 6; m++) palRamp(m * 32, m * 32 + 31, lo[m][0], lo[m][1], lo[m][2], hi[m][0], hi[m][1], hi[m][2]);
        palRamp(BG, BG + 31, 4, 6, 12, 18, 26, 44);
        palRamp(CHIP, CHIP + 7, 255, 230, 140, 200, 90, 20);   // hot chips
        palRamp(CHIP + 8, CHIP + 15, 220, 220, 230, 90, 90, 100);  // cooled aluminum
        pal[AMBER]     = rgb(245, 168, 0);
        pal[AMBER + 1] = rgb(120, 80, 0);
        pal[AMBER + 2] = rgb(0, 0, 0);
        light          = norm({0.45f, 0.85f, -0.35f});
        newStock();
    }

    // the part being cut: fluted / twisted shapes, a new one each cycle
    void newStock() {
        for (int h = 0; h < NH; h++)
            for (int a = 0; a < NA; a++) {
                rad[h][a]  = STOCK_R;
                float th   = a * 2 * PI / NA, hv = (float)h / (NH - 1);
                float t;
                switch (shape % 3) {
                    case 0: t = 11 + 5 * fmaxf(0, cosf(6 * th + hv * 2.2f)); break;      // twisted 6-flute
                    case 1: t = 9 + 7 * (0.5f + 0.5f * cosf(4 * th)) * (0.4f + 0.6f * hv); break;  // tapered square
                    default: t = 8 + 8 * fabsf(sinf(3 * th + hv * 3.0f)); break;        // 3-lobe helix
                }
                target[h][a] = h == 0 ? STOCK_R : fminf(STOCK_R, t);  // keep a foot to clamp
            }
        ring   = NH - 1;
        doneAt = 0;
    }

    // ---- transforms ----
    // world <- cradle (tilted by A about the trunnion axis, 60 up)
    M cradle() const { return trans(0, 60, 0) * rotX(A); }
    // world <- workpiece local (on the platter, spun by C)
    M part() const { return cradle() * trans(0, -6, 0) * rotY(C); }

    // ---- geometry emit ----
    void quad(V3 p0, V3 p1, V3 p2, V3 p3, int mat) {
        V3 n = norm(cross(sub(p1, p0), sub(p3, p0)));
        if (dot(n, sub(camPos, p0)) <= 0) return;  // facing away
        float l     = dot(n, light);
        int   shade = (int)(6 + (l > 0 ? l : 0) * 25);
        shade       = shade > 31 ? 31 : shade;
        V3    q[4]  = {p0, p1, p2, p3};
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
        uint8_t col = mat * 32 + shade;
        for (int t = 0; t < 2 && ntri < MAXTRI; t++) {
            Tri& tr = tris[ntri++];
            int  a = 0, b = t ? 2 : 1, c = t ? 3 : 2;
            tr.x[0] = sx[a], tr.y[0] = sy[a], tr.x[1] = sx[b], tr.y[1] = sy[b], tr.x[2] = sx[c], tr.y[2] = sy[c];
            tr.z   = (uint16_t)(zs * 4);  // average depth * 16
            tr.col = col;
        }
    }
    void box(const M& m, V3 c, V3 s, int mat) {
        V3 p[8];
        for (int i = 0; i < 8; i++)
            p[i] = m.apply({c.x + (i & 1 ? s.x : -s.x) / 2, c.y + (i & 2 ? s.y : -s.y) / 2, c.z + (i & 4 ? s.z : -s.z) / 2});
        // faces wound outward
        quad(p[0], p[2], p[3], p[1], mat);  // -z
        quad(p[4], p[5], p[7], p[6], mat);  // +z
        quad(p[0], p[4], p[6], p[2], mat);  // -x
        quad(p[1], p[3], p[7], p[5], mat);  // +x
        quad(p[0], p[1], p[5], p[4], mat);  // -y
        quad(p[2], p[6], p[7], p[3], mat);  // +y
    }
    void cyl(const M& m, V3 c, float r, float h, int segs, int mat, bool top = true) {
        for (int i = 0; i < segs; i++) {
            float a0 = i * 2 * PI / segs, a1 = (i + 1) * 2 * PI / segs;
            V3    b0 = m.apply({c.x + r * cosf(a0), c.y, c.z + r * sinf(a0)}), b1 = m.apply({c.x + r * cosf(a1), c.y, c.z + r * sinf(a1)});
            V3    t0 = m.apply({c.x + r * cosf(a0), c.y + h, c.z + r * sinf(a0)}), t1 = m.apply({c.x + r * cosf(a1), c.y + h, c.z + r * sinf(a1)});
            quad(b0, t0, t1, b1, mat);
            V3 ct = m.apply({c.x, c.y + h, c.z}), cb = m.apply({c.x, c.y, c.z});
            if (top) quad(t0, ct, ct, t1, mat);
            quad(b1, cb, cb, b0, mat);
        }
    }

    // ---- the carving ----
    void cut(float dt) {
        if (doneAt) {
            if (millis() - doneAt > 2500) {
                shape++;
                newStock();
            }
            return;
        }
        // the angle facing the cutter (world +X) is -C; tilting A about X keeps it there
        int   a   = ((int)lroundf(-C / (2 * PI) * NA) % NA + NA) % NA;
        float bite = 22 * dt;
        bool  removed = false;
        for (int h : {ring, ring - 1})
            if (h >= 1) {
                float& r = rad[h][a];
                if (r > target[h][a]) {
                    r       = fmaxf(target[h][a], r - bite);
                    removed = true;
                }
            }
        // contact point on the part's surface, in world space
        float hy   = (float)ring / (NH - 1) * STOCK_H;
        float th   = a * 2 * PI / NA;
        float r    = rad[ring][a];
        V3    cp   = part().apply({r * cosf(th), hy, r * sinf(th)});
        V3    want = add(cp, {3.2f, 0, 0});  // tool radius off the surface
        tip        = add(tip, mul(sub(want, tip), fminf(1, dt * 10)));
        if (removed)
            for (int k = 0; k < 2 && nchip < MAXCHIP; k++) {
                float s = 30 + (esp_random() % 40);
                chips[nchip++] = {cp, {(float)((int)(esp_random() % 40) - 10), 20 + (float)(esp_random() % 30), -s}, 0};
            }
        // ring finished once a full turn removes nothing on it
        static float quiet = 0;
        quiet              = removed ? 0 : quiet + dt;
        if (quiet > 2 * PI / 2.2f) {
            quiet = 0;
            if (--ring < 1) doneAt = millis();
        }
    }

    void chipsStep(float dt) {
        for (int i = 0; i < nchip;) {
            Chip& c = chips[i];
            c.v.y -= 260 * dt;
            c.p = add(c.p, mul(c.v, dt));
            c.life += dt;
            if (c.p.y < 12) {  // bounce off the base, then settle
                c.p.y = 12;
                c.v   = {c.v.x * 0.4f, -c.v.y * 0.3f, c.v.z * 0.4f};
            }
            if (c.life > 2.5f) c = chips[--nchip];
            else i++;
        }
    }

    // ---- raster ----
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
        // camera: drag to orbit, otherwise drift
        if (touch.down) {
            if (wasDown) {
                yawV   = -touch.vx * 0.012f;
                pitchV = touch.vy * 0.008f;
            }
        } else {
            yawV += (0.25f * dt - yawV) * 0.05f;
            pitchV *= 0.9f;
        }
        wasDown = touch.down;
        yaw += yawV;
        pitch += pitchV;
        pitch = pitch < 0.05f ? 0.05f : pitch > 1.2f ? 1.2f : pitch;

        // machine motion: C spins, A rocks
        C += 2.2f * dt;
        A = 0.55f * sinf(ms / 4300.0f);
        cut(dt);
        chipsStep(dt);

        V3 target{0, 52, 0};
        camPos = add(target, {dist * cosf(pitch) * sinf(yaw), dist * sinf(pitch), -dist * cosf(pitch) * cosf(yaw)});
        camF   = norm(sub(target, camPos));
        camR   = norm(cross({0, 1, 0}, camF));
        camU   = cross(camF, camR);

        // background: dark gradient
        for (int y = 0; y < SH; y++) memset(fb + y * SW, BG + y * 31 / SH, SW);

        ntri = 0;
        M I;
        // base and trunnion towers (amber, it's RotatoCAM)
        box(I, {0, 6, 0}, {170, 12, 110}, BASE);
        box(I, {-62, 36, 0}, {16, 50, 34}, TRUNNION);
        box(I, {62, 36, 0}, {16, 50, 34}, TRUNNION);
        // cradle: floor + side cheeks, tilting on A
        M cr = cradle();
        box(cr, {0, -14, 0}, {100, 8, 56}, TRUNNION);
        box(cr, {-50, -4, 0}, {8, 28, 50}, TRUNNION);
        box(cr, {50, -4, 0}, {8, 28, 50}, TRUNNION);
        // rotary platter on C, with a slot so you can see it turn
        M pl = cradle() * trans(0, -10, 0) * rotY(C);
        cyl(pl, {0, 0, 0}, 30, 4, 16, STEEL);
        box(pl, {20, 4.5f, 0}, {14, 1.5f, 4}, BASE);
        // the workpiece, rebuilt from the live radii
        M pt = part();
        for (int h = 0; h + 1 < NH; h++)
            for (int a = 0; a < NA; a++) {
                int   b   = (a + 1) % NA;
                float th0 = a * 2 * PI / NA, th1 = b * 2 * PI / NA;
                float y0 = (float)h / (NH - 1) * STOCK_H, y1 = (float)(h + 1) / (NH - 1) * STOCK_H;
                V3    p00 = pt.apply({rad[h][a] * cosf(th0), y0, rad[h][a] * sinf(th0)});
                V3    p01 = pt.apply({rad[h][b] * cosf(th1), y0, rad[h][b] * sinf(th1)});
                V3    p10 = pt.apply({rad[h + 1][a] * cosf(th0), y1, rad[h + 1][a] * sinf(th0)});
                V3    p11 = pt.apply({rad[h + 1][b] * cosf(th1), y1, rad[h + 1][b] * sinf(th1)});
                quad(p00, p10, p11, p01, ALU);
            }
        V3 topc = pt.apply({0, STOCK_H, 0});
        for (int a = 0; a < NA; a++) {
            int   b = (a + 1) % NA;
            float th0 = a * 2 * PI / NA, th1 = b * 2 * PI / NA;
            quad(pt.apply({rad[NH - 1][a] * cosf(th0), STOCK_H, rad[NH - 1][a] * sinf(th0)}), topc, topc,
                 pt.apply({rad[NH - 1][b] * cosf(th1), STOCK_H, rad[NH - 1][b] * sinf(th1)}), ALU);
        }
        // spindle: cutter, collet, motor, head, column
        M sp = trans(tip.x, tip.y, tip.z);
        cyl(sp, {0, 0, 0}, 3, 22, 8, TOOL);
        cyl(sp, {0, 22, 0}, 7, 10, 10, STEEL);
        cyl(sp, {0, 32, 0}, 12, 30, 12, SPINDLE);
        box(sp, {0, 70, 0}, {34, 18, 34}, SPINDLE);
        // column stands behind the machine; a ram reaches forward to the head
        constexpr float COL_Z = 78;
        box(I, {0, 80, COL_Z}, {26, 148, 18}, BASE);
        float ramLen = COL_Z - tip.z;
        box(I, {tip.x, tip.y + 70, tip.z + ramLen / 2}, {18, 14, ramLen}, BASE);

        // painter's sort, far first
        std::sort(tris, tris + ntri, [](const Tri& a, const Tri& b) { return a.z > b.z; });
        for (int i = 0; i < ntri; i++) fill(tris[i]);

        // chips
        for (int i = 0; i < nchip; i++) {
            V3    d = sub(chips[i].p, camPos);
            float z = dot(d, camF);
            if (z < 5) continue;
            int x = SW / 2 + (int)(dot(d, camR) * 230 / z), y = SH / 2 - (int)(dot(d, camU) * 230 / z);
            if (x < 0 || x >= SW - 1 || y < 0 || y >= SH - 1) continue;
            uint8_t c = CHIP + (int)(chips[i].life * 6) % 16;
            fb[y * SW + x] = fb[y * SW + x + 1] = fb[(y + 1) * SW + x] = c;
        }

        // amber DRO in inches (mm's are for communists) + branding
        char line[32];
        float in = 1 / 25.4f;
        const char* lab[5] = {"X", "Y", "Z", "A", "C"};
        float val[5] = {tip.x * in, tip.z * in, (tip.y - 12) * in, A * 57.2958f, fmodf(C * 57.2958f, 360)};
        for (int i = 0; i < 5; i++) {
            snprintf(line, sizeof line, i < 3 ? "%s%+7.3f\"" : "%s%7.2f\xF8", lab[i], val[i]);
            fbText(fb, SW, SH, 7, 7 + i * 16, line, AMBER + 2);
            fbText(fb, SW, SH, 6, 6 + i * 16, line, AMBER);
        }
        fbText(fb, SW, SH, SW - 9 * 16 - 6, 8, "ROTATOCAM", AMBER, 2);
        fbText(fb, SW, SH, SW - 25 * 8 - 6, 42, "5-AXIS FOR THE REST OF US", AMBER + 1);
        if (doneAt) fbText(fb, SW, SH, SW / 2 - 52, SH - 26, "PART DONE", AMBER, 1);
    }
};

}  // namespace

Demo* makeRotatoCAM() { return new RotatoCAM(); }
