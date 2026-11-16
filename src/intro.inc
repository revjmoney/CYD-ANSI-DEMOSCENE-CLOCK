// ------------------------------------------------------------------ ANSI explosion intro
// (included at the end of demos_text.cpp so it can share the BBS terminal code)
//
// The RotatoCAM splash dials in and types out like a BBS. On the drop, every
// character tears loose into 3D, swirls in a vortex, and slams back together
// as a giant "CYD DEMOSCENE" logo with a ring of leftover characters orbiting
// it. All timing is in music beats.
struct AnsiExplosion : Demo {
    static constexpr int   PRE = 3, NR = PRE + ROTATO_H, MAXP = NR * COLS;
    static constexpr float Z0  = 200;  // depth where 1 world unit = 1 pixel
    struct P {
        int16_t hx, hy, tx, ty;  // home (text mode) and logo target, top-left corner
        uint8_t ch, attr, logoCol;
        int8_t  dx, dy, dz;      // explosion direction
        uint8_t ang, rad;        // vortex orbit
        int8_t  oy;
        bool    logo;            // false = stays in the orbit ring
    };
    Cell      rows[NR * COLS];
    Terminal  term;
    P*        ps    = nullptr;
    uint16_t* order = nullptr;
    int       np    = 0;
    int       li;
    float     ci, E = -1, tb = -100, doneBeat = -1;
    uint32_t  tms = 0, doneMs = 0;
    bool      typing = true;
    // per-frame projected glyphs
    struct G {
        int16_t x, y;
        uint8_t depth, size, ch, fg, bg;
    };
    G*        gl = nullptr;

    static uint8_t tag(int artRow, int col) { return rotato_grid[(artRow * COLS + col) * 3 + 2]; }

    const char* name() const override { return "ANSI Explosion (intro)"; }
    Res         res() const override { return tb >= 0 ? Res::High : Res::Custom; }
    uint32_t    duration() const override { return 120000; }
    bool        finished() const override { return tb >= 34; }
    float       pulse() const override { return tb >= 0 && tb < 1 ? 0.6f : 0.15f; }

    void begin() override {
        lineOf(rows + 0 * COLS, "ATDT 1-800-ROTATOCAM", 7);
        lineOf(rows + 1 * COLS, "CONNECT 9600/ARQ/V.32bis  ::  RotatoCAM BBS node 1", 15);
        lineOf(rows + 2 * COLS, "", 7);
        for (int i = 0; i < ROTATO_H * COLS; i++) {
            rows[PRE * COLS + i] = {rotato_grid[i * 3], rotato_grid[i * 3 + 1]};
        }
        term.rows  = rows;
        term.nrows = NR;
        term.cps   = 960;
        term.layout();

        // particles: every visible cell (the marquee row is animated, skip it)
        auto visible = [&](int r, int c) {
            Cell ce = rows[r * COLS + c];
            if (r >= PRE && tag(r - PRE, c) == RTAG_MARQ) return false;
            return !((ce.ch == 32 || ce.ch == 0 || ce.ch == 255) && (ce.attr >> 4) == 0);
        };
        int count = 0;
        for (int r = 0; r < NR; r++)
            for (int c = 0; c < COLS; c++) count += visible(r, c);
        ps    = (P*)arenaAlloc(count * sizeof(P));
        order = (uint16_t*)arenaAlloc(count * 2);
        gl    = (G*)arenaAlloc(count * sizeof(G));
        np    = 0;
        if (!ps || !order || !gl) {
            Serial.println("intro: out of memory");
            return;
        }
        for (int r = 0; r < NR; r++)
            for (int c = 0; c < COLS; c++) {
                if (!visible(r, c)) continue;
                Cell ce = rows[r * COLS + c];
                P& p   = ps[np++];
                p.hx   = c * 4 - SW / 2;
                p.hy   = r * 8 - SH / 2;
                p.ch   = ce.ch;
                p.attr = ce.attr;
                // outward from the screen centre, with some random depth
                float ox = p.hx + 2 + (int)(esp_random() % 41) - 20, oy = p.hy + 4 + (int)(esp_random() % 41) - 20,
                      oz = (int)(esp_random() % 201) - 100, len = sqrtf(ox * ox + oy * oy + oz * oz) + 1;
                p.dx  = (int8_t)(ox / len * 127);
                p.dy  = (int8_t)(oy / len * 127);
                p.dz  = (int8_t)(oz / len * 127);
                p.ang = esp_random();
                p.rad = 50 + esp_random() % 100;
                p.oy  = (int)(esp_random() % 120) - 60;
            }
        // logo targets: each font pixel of "CYD" is one cell; "DEMOSCENE" pairs rows up
        struct T {
            int16_t x, y;
            uint8_t col;
        };
        T*  tg = (T*)scratch(1);  // only needed here, fb is free until the drop
        int nt = 0;
        const char* w1 = "CYD";
        for (int i = 0; i < 3; i++)
            for (int gy = 0; gy < 16; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if ((font16[(uint8_t)w1[i] * 16 + gy] & (0x80 >> gx)) && nt < 1200)
                        tg[nt++] = {(int16_t)(-48 + (i * 8 + gx) * 4), (int16_t)(-100 + gy * 8), (uint8_t)(16 + gy * 2)};
        const char* w2 = "DEMOSCENE";
        for (int i = 0; i < 9; i++)
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++) {
                    const uint8_t* g = &font16[(uint8_t)w2[i] * 16];
                    if (((g[gy * 2] | g[gy * 2 + 1]) & (0x80 >> gx)) && nt < 1200)
                        tg[nt++] = {(int16_t)(-144 + (i * 8 + gx) * 4), (int16_t)(36 + gy * 8), (uint8_t)(48 + gy * 4)};
                }
        // shuffle so the logo gets pulled together from all over the screen
        for (int i = np - 1; i > 0; i--) std::swap(ps[i], ps[esp_random() % (i + 1)]);
        for (int i = 0; i < np; i++) {
            ps[i].logo = i < nt;
            if (i < nt) {
                ps[i].tx      = tg[i].x;
                ps[i].ty      = tg[i].y;
                ps[i].logoCol = tg[i].col;
            }
        }
        // palette: 0..15 CGA, 16..47 gold cycle (CYD), 48..79 cyan cycle (DEMOSCENE)
        for (int i = 0; i < 16; i++) pal[i] = cgaColor(i);
        for (int i = 0; i < 32; i++) {
            float k     = (sinf(i * 2 * PI / 32) + 1) / 2;
            pal[16 + i] = rgb(255, 140 + (int)(115 * k), (int)(60 * k));
            pal[48 + i] = rgb((int)(120 * k), 160 + (int)(95 * k), 255);
        }
        Serial.printf("intro: %d particles, %d logo targets\n", np, nt);
    }

    void update(uint32_t ms) override {
        tms    = ms;
        Sync s = syncNow();
        typing = term.cursor(ms, li, ci);
        if (!typing && doneBeat < 0) {
            doneBeat = s.beats;
            doneMs   = ms;
            // explode on a bar line at least 2 beats out; at boot, wait for the drop (bar 4)
            E = ceilf((doneBeat + 2) / 4) * 4;
            if (E < 16) E = 16;
        }
        tb = E < 0 ? -100 : s.beats - E;
        fmtStatus(term.status, 9600, !typing, ms, "");
        if (tb >= 0) drawParticles(s);
    }

    static float smooth(float x) {
        x = x < 0 ? 0 : x > 1 ? 1 : x;
        return x * x * (3 - 2 * x);
    }

    void drawParticles(const Sync& s) {
        memset(fb, 0, SW * SH);
        // twinkling stars
        for (int i = 0; i < 90; i++) {
            uint32_t h = i * 2654435761u;
            int      x = h % SW, y = (h >> 9) % SH;
            fb[y * SW + x] = ((h >> 3) + (int)(tb * 4)) % 5 == 0 ? 15 : 8;
        }
        float t = tb, camEnv = t < 14 ? 1 : 1 - smooth((t - 14) / 5);
        float yaw = (0.5f * sinf(t * 0.35f) + (t < 14 ? t * 0.12f : 14 * 0.12f)) * camEnv;
        float cyw = cosf(yaw), syw = sinf(yaw);
        float pitch = 0.25f * sinf(t * 0.21f) * camEnv, cp = cosf(pitch), sp = sinf(pitch);
        float explodeK = 1 - expf(-t * 1.1f);
        int   bucket[34] = {0};
        for (int i = 0; i < np; i++) {
            P&    p  = ps[i];
            float st = (i & 31) * 0.04f;  // stagger
            // phase 1: explosion
            float R  = 140 + (p.rad & 63) * 2;
            float x1 = p.hx + p.dx / 127.0f * R * explodeK, y1 = p.hy + p.dy / 127.0f * R * explodeK,
                  z1 = Z0 + p.dz / 127.0f * R * explodeK;
            // phase 2: vortex; ring particles orbit wider and flatter
            float rr = (p.logo ? p.rad : 150 + p.rad / 3) * (1 + 0.2f * s.kick);
            float a  = p.ang * (2 * PI / 256) + t * (p.logo ? 0.9f : 0.5f);
            // the ring is tilted so it reads as an orbit, passing in front of and behind the logo
            float x2 = rr * cosf(a), z2 = Z0 + rr * sinf(a);
            float y2 = p.logo ? p.oy + 18 * sinf(a * 2 + t) : rr * sinf(a) * 0.28f + p.oy / 8.0f + 10;
            float m1 = smooth((t - 4.5f - st) / 3);
            float x = x1 + (x2 - x1) * m1, y = y1 + (y2 - y1) * m1, z = z1 + (z2 - z1) * m1;
            uint8_t ch = p.ch, fg = p.attr & 15, bg = p.attr >> 4;
            if (p.logo) {
                // phase 3: home in on the logo
                float m2 = smooth((t - 13.5f - st * 2) / 4);
                float wy = p.ty + sinf(p.tx * 0.05f + t * 3) * s.kick * 6;
                x += (p.tx - x) * m2;
                y += (wy - y) * m2;
                z += (Z0 - z) * m2;
                if (m2 > 0.8f) {
                    ch          = 219;
                    int shimmer = (int)(t * 8);
                    fg = p.logoCol < 48 ? 16 + ((p.logoCol - 16 + shimmer) & 31) : 48 + ((p.logoCol - 48 + shimmer) & 31);
                    bg = 0;
                }
            }
            // camera: yaw then pitch about the screen-plane centre
            float rx = x * cyw - (z - Z0) * syw, rz = x * syw + (z - Z0) * cyw;
            float ry = y * cp - rz * sp;
            rz       = y * sp + rz * cp + Z0;
            G& g = gl[i];
            if (rz < 30) {
                g.depth = 0;
                continue;
            }
            float k = Z0 / rz;
            g.x     = (int16_t)(SW / 2 + rx * k);
            g.y     = (int16_t)(SH / 2 + ry * k);
            int w   = (int)(k * 4 + 0.5f);
            g.size  = w < 1 ? 1 : w > 40 ? 40 : w;
            int b   = (int)((rz - 30) / 20);
            b       = b < 0 ? 0 : b > 31 ? 31 : b;
            g.depth = 32 - b;  // 1 = far, 32 = near
            bucket[g.depth]++;
            g.ch = ch;
            g.fg = fg;
            g.bg = bg;
        }
        // counting sort far to near, then paint
        int start[34];
        start[1] = 0;
        for (int b = 1; b <= 32; b++) start[b + 1] = start[b] + bucket[b];
        int n = start[33];
        for (int i = 0; i < np; i++)
            if (gl[i].depth) order[start[gl[i].depth]++] = i;
        for (int o = 0; o < n; o++) {
            const G& g = gl[order[o]];
            glyph(g.x, g.y, g.size, g.ch, g.fg, g.bg);
        }
    }

    // CP437 glyph with its top-left at (x,y), w pixels wide (4 = text size).
    // Downscaled glyphs OR the source pixels together so thin strokes survive.
    void glyph(int x, int y, int w, uint8_t ch, uint8_t fg, uint8_t bg) {
        if (x >= SW || y >= SH || x + w <= 0 || y + 2 * w <= 0) return;
        int     h = w * 2;
        uint8_t colMask[40];
        for (int px = 0; px < w; px++) {
            int     u0 = px * 8 / w, u1 = (px + 1) * 8 / w;
            uint8_t m  = 0x80 >> u0;
            for (int u = u0 + 1; u < u1; u++) m |= 0x80 >> u;
            colMask[px] = m;
        }
        int pxa = x < 0 ? -x : 0, pxb = x + w > SW ? SW - x : w;
        const uint8_t* g = &font16[ch * 16];
        for (int py = 0; py < h; py++) {
            int yy = y + py;
            if (yy < 0) continue;
            if (yy >= SH) break;
            int     v0 = py * 16 / h, v1 = (py + 1) * 16 / h;
            uint8_t bits = g[v0];
            for (int v = v0 + 1; v < v1; v++) bits |= g[v];
            uint8_t* row = fb + yy * SW + x;
            if (bg) {
                for (int px = pxa; px < pxb; px++) row[px] = (bits & colMask[px]) ? fg : bg;
            } else if (bits) {
                for (int px = pxa; px < pxb; px++)
                    if (bits & colMask[px]) row[px] = fg;
            }
        }
    }

    // text phase only (Res::Custom)
    void strip(int y0, int h, uint16_t* out) override {
        float tt = doneBeat < 0 ? 0 : (tms - doneMs) / 1000.0f;
        for (int y = y0; y < y0 + h; y++, out += SW) {
            if (y >= VIS * 8) {
                term.statusLine(out, y - VIS * 8);
                continue;
            }
            int L = y >> 3, gr = y & 7;
            if (L >= NR || L > li) {
                memset(out, 0, SW * 2);
                continue;
            }
            Cell r[COLS];
            memcpy(r, rows + L * COLS, sizeof(r));
            int lim = (typing && L == li) ? (int)ci : COLS;
            if (!typing && L >= PRE)
                for (int x = 0; x < COLS; x++) rotatoFx(r[x], tag(L - PRE, x), x, L - PRE, tt);
            textLine4x8(out, r, gr, lim);
            if (typing && L == li && ((millis() / 267) & 1) == 0 && gr == 7) {
                int cx = lim < COLS ? lim : COLS - 1;
                for (int k = 0; k < 4; k++) out[cx * 4 + k] = cgaColor(7);
            }
        }
    }
};
Demo* makeAnsiExplosion() { return new AnsiExplosion(); }
