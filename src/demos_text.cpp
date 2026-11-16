// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Text-mode demos: the RotatoCAM BBS splash, the ANSI art gallery and matrix rain.
// Everything renders 80 columns of 4x8 cells (VGA glyphs, 2x2 averaged), 29
// rows of content plus a Telix style status line.
#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>
#include <dirent.h>
#include <algorithm>
#include <vector>
#include "demo.h"
#include "rotatocam_data.h"

static constexpr int COLS = 80, VIS = 29;  // visible content rows

static void lineOf(Cell* r, const char* s, uint8_t attr) {
    int x = 0;
    for (; *s && x < COLS; s++) r[x++] = {(uint8_t)*s, attr};
    for (; x < COLS; x++) r[x] = {32, attr};
}
static int visibleLen(const Cell* r) {
    int n = COLS;
    while (n > 0 && r[n - 1].ch == 32 && (r[n - 1].attr >> 4) == 0) n--;
    return n < 1 ? 1 : n;
}

// Shared BBS "terminal": a stack of rows typed out at a given cps.
struct Terminal {
    Cell*                 rows  = nullptr;
    int                   nrows = 0;
    std::vector<uint32_t> lineStart;
    uint32_t              total = 0;
    int                   cps   = 1920;
    char                  status[COLS + 1];

    void layout() {
        lineStart.resize(nrows);
        total = 0;
        for (int i = 0; i < nrows; i++) {
            lineStart[i] = total;
            total += visibleLen(rows + i * COLS) + 2;
        }
    }
    // where the typing cursor is at time t; returns false when finished
    bool cursor(uint32_t t, int& li, float& ci) const {
        uint64_t p = (uint64_t)t * cps / 1000;
        if (p >= total) {
            li = nrows - 1;
            ci = COLS;
            return false;
        }
        li = 0;
        while (li + 1 < nrows && lineStart[li + 1] <= p) li++;
        ci = p - lineStart[li];
        return true;
    }
    void statusLine(uint16_t* out, int glyphRow) {
        Cell r[COLS];
        lineOf(r, status, 0x30);
        for (auto& c : r)
            if (c.ch == 0xB3) c.attr = 0x31;
        textLine4x8(out, r, glyphRow);
    }
};

static void fmtStatus(char* dst, int baud, bool online, uint32_t ms, const char* tail) {
    uint32_t s = ms / 1000;
    snprintf(dst, COLS + 1, " ALT-Z for Help \xB3 ANSI-BBS \xB3 %-5d N81 \xB3 FDX \xB3 %s %02lu:%02lu:%02lu \xB3 %s", baud,
             online ? "Online" : "Rcv   ", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), tail);
}

// the HTML version's idle animations, per tagged cell
static void rotatoFx(Cell& c, uint8_t tag, int x, int y, float tt) {
    int fg = c.attr & 15;
    switch (tag) {
        case RTAG_ROT:
        case RTAG_CAM: {
            float band = fmodf(tt * 40, 130) - 25, d = fabsf(x + (y - 3) * 1.2f - band);
            if (d < 1.6f) fg = 15;
            else if (d < 3.5f) fg |= 8;
            break;
        }
        case RTAG_PATH: {
            float ph = fmodf(fmodf(x - tt * 30, 14) + 14, 14);
            fg       = ph < 4 ? 13 : 5;
            if (x > 52) fg = 13;
            break;
        }
        case RTAG_PIVOT: fg = sinf(tt * 5) > 0 ? 14 : 6; break;
        case RTAG_FLUTE: c.ch = ((int)floorf(tt * 14 + y) & 1) ? 178 : 177; break;
        case RTAG_CHIP: {
            static const uint8_t ch[3] = {249, 250, 32};
            c.ch                       = ch[(int)floorf(tt * 9 + x * 7 + y * 3) % 3];
            break;
        }
        case RTAG_TAG: {
            float d = fabsf(x - (fmodf(tt * 25, 110) - 15));
            fg      = d > 2 ? 15 : 11;
            break;
        }
        case RTAG_MARQ: c.ch = 32; break;
    }
    c.attr = (c.attr & 0xF0) | fg;
}

// ------------------------------------------------------------------ RotatoCAM BBS
struct RotatoBBS : Demo {
    static constexpr int PRE = 7, NR = PRE + ROTATO_H;
    Cell     rows[NR * COLS];
    uint8_t  tags[ROTATO_H * COLS];
    Terminal term;
    uint32_t t = 0, doneAt = 0;
    int      li;
    float    ci;
    bool     typing = true;
    float    topF   = 0;

    const char* name() const override { return "RotatoCAM BBS"; }
    Res         res() const override { return Res::Custom; }
    uint32_t    duration() const override { return 32000; }

    void begin() override {
        lineOf(rows + 0 * COLS, "ATDT 1-800-ROTATOCAM", 7);
        lineOf(rows + 1 * COLS, "", 7);
        lineOf(rows + 2 * COLS, "CONNECT 19200/ARQ/V.34/LAPM/V.42BIS", 15);
        lineOf(rows + 3 * COLS, "", 7);
        lineOf(rows + 4 * COLS, "RotatoCAM BBS :: node 1 :: ANSI graphics detected", 3);
        lineOf(rows + 5 * COLS, "Loading login screen ...", 8);
        lineOf(rows + 6 * COLS, "", 7);
        for (int i = 0; i < ROTATO_H * COLS; i++) {
            rows[PRE * COLS + i] = {rotato_grid[i * 3], rotato_grid[i * 3 + 1]};
            tags[i]              = rotato_grid[i * 3 + 2];
        }
        term.rows  = rows;
        term.nrows = NR;
        term.cps   = 1920;
        term.layout();
    }

    void fx(Cell& c, int x, int y, float tt) { rotatoFx(c, tags[y * COLS + x], x, y, tt); }

    void update(uint32_t ms) override {
        t      = ms;
        typing = term.cursor(t, li, ci);
        if (!typing && !doneAt) doneAt = t;
        float target = li - VIS + 1;
        if (target < 0) target = 0;
        topF += (target - topF) * 0.35f;
        if (fabsf(target - topF) < 0.02f) topF = target;
        fmtStatus(term.status, 19200, !typing, t, "R B M C H F");
    }

    void strip(int y0, int h, uint16_t* out) override {
        float tt = (t - doneAt) / 1000.0f;
        for (int y = y0; y < y0 + h; y++, out += SW) {
            if (y >= VIS * 8) {
                term.statusLine(out, y - VIS * 8);
                continue;
            }
            int src = (int)(topF * 8) + y, L = src >> 3, gr = src & 7;
            if (L >= NR || L > li) {
                memset(out, 0, SW * 2);
                continue;
            }
            Cell r[COLS];
            memcpy(r, rows + L * COLS, sizeof(r));
            int artY = L - PRE, lim = (typing && L == li) ? (int)ci : COLS;
            if (!typing && artY >= 0) {
                for (int x = 0; x < COLS; x++) fx(r[x], x, artY, tt);
                if (artY == ROTATO_H - 1) {
                    // smooth marquee: shift the whole row by a sub-cell pixel offset
                    int  px = (int)(tt * 24), start = px / 4, off = px % 4, sh = (int)(tt * 8);
                    int  n = sizeof(rotato_msg) - 1;
                    Cell m[COLS + 1];
                    for (int i = 0; i <= COLS; i++) {
                        int  j  = (start + i) % n;
                        char ch = rotato_msg[j];
                        m[i]    = {(uint8_t)ch, (uint8_t)(ch == ':' ? 8 : rotato_cyc[(j + sh) % sizeof(rotato_cyc)])};
                    }
                    uint16_t tmp[SW + 4];
                    textLine4x8(tmp, m, gr);
                    // textLine4x8 only draws 80 cells, add the 81st by hand
                    uint16_t tail[4 * 80];
                    textLine4x8(tail, m + 1, gr, 80);
                    memcpy(out, tmp + off, (SW - off) * 2);
                    memcpy(out + SW - off, tail + SW - 4, off * 2);
                    continue;
                }
            }
            textLine4x8(out, r, gr, lim);
            if (typing && L == li && ((millis() / 267) & 1) == 0 && gr == 7) {
                int cx = lim < COLS ? lim : COLS - 1;
                for (int k = 0; k < 4; k++) out[cx * 4 + k] = cgaColor(7);
            }
        }
    }
};
Demo* makeRotatoBBS() { return new RotatoBBS(); }

// ------------------------------------------------------------------ ANSI gallery
// Plays the .cg files from SD /ansi (or the small LittleFS set when there is no
// card) like a BBS: dial, type out, then pan.
// No list of names is kept: 200 Strings cut the heap into pieces too small for
// the display task. The big gallery is one file, /ansi.pak in the SD card's
// root (tools/make_ansi_pack.py): an index then the pieces. Without it, pieces
// are found by number in the /ansi folder (SD, else LittleFS). Either way they
// play in a shuffled order: a random start and a step coprime to the count.
static int     ansiN = 0, ansiStart = 0, ansiStep = 1, ansiNext = 0;
static fs::FS* ansiFs  = &LittleFS;
static bool    ansiPak = false;

static bool isCg(const char* n) { return strcasestr(n, ".cg"); }

// path of the k-th piece in folder order, or "" (k < 0 just counts them into ansiN)
static String ansiName(int k) {
    int i = 0;
    if (ansiFs == &SD) {  // readdir straight from the VFS: SD openNextFile() opens every file on the way
        String r;
        if (DIR* d = opendir("/sd/ansi")) {
            while (dirent* e = readdir(d))
                if (e->d_type != DT_DIR && isCg(e->d_name) && i++ == k) {
                    r = String("/ansi/") + e->d_name;
                    break;
                }
            closedir(d);
        }
        if (k < 0) ansiN = i;
        return r;
    }
    if (File dir = LittleFS.open("/ansi"))
        for (File f = dir.openNextFile(); f; f = dir.openNextFile())
            if (isCg(f.name()) && i++ == k) return String("/ansi/") + f.name();
    if (k < 0) ansiN = i;
    return String();
}

int      ansiCount() { return ansiN; }
AnsiFile ansiOpen(int i) {
    AnsiFile a;
    if (!ansiN) return a;
    int k = (ansiStart + (uint32_t)(i % ansiN + ansiN) % ansiN * ansiStep) % ansiN;
    if (ansiPak) {
        struct {
            uint32_t off, size;
            char     name[24];
        } e;
        a.f = SD.open("/ansi.pak");
        if (!a.f || !a.f.seek(12 + 32 * k) || a.f.read((uint8_t*)&e, 32) != 32 || !a.f.seek(e.off)) return AnsiFile();
        a.base = e.off;
        memcpy(a.name, e.name, 24);
        return a;
    }
    String p = ansiName(k);
    if (p.length()) a.f = ansiFs->open(p);
    if (a.f) strlcpy(a.name, a.f.name(), sizeof a.name);
    return a;
}

void scanAnsi() {
    if (ansiN) return;
    if (clockSdMount()) {
        ansiFs = &SD;
        File    f = SD.open("/ansi.pak");
        uint8_t h[12];
        if (f && f.read(h, 12) == 12 && !memcmp(h, "ANSIPAK1", 8)) {
            ansiPak = true;
            ansiN   = h[8] | h[9] << 8 | h[10] << 16 | h[11] << 24;
        } else {
            ansiName(-1);
        }
    }
    if (!ansiN) {
        ansiFs = &LittleFS;
        ansiName(-1);
    }
    // shuffle, so every boot is a different show
    if (ansiN) {
        ansiStart = esp_random() % ansiN;
        do ansiStep = 1 + esp_random() % ansiN;
        while (std::__gcd(ansiStep, ansiN) != 1);
    }
    Serial.printf("ANSI gallery: %d pieces (%s)\n", ansiN,
                  ansiPak ? "SD /ansi.pak" : ansiFs == &SD ? "SD /ansi" : "LittleFS");
}

struct AnsiGallery : Demo {
    static constexpr int PER_VISIT = 5;
    Cell*                rows = nullptr;
    int                  nrows = 0, shown = 0, li = 0;
    float                ci, topF = 0;
    Terminal             term;
    uint32_t             t0 = 0, t = 0, doneAt = 0;
    bool                 typing = true, over = false, manual = false;
    uint32_t             lastTouch = 0;
    int                  piece     = 0;
    char                 title[37], author[21], group[21];

    const char* name() const override { return "ANSI Gallery"; }
    Res         res() const override { return Res::Custom; }
    uint32_t    duration() const override { return 600000; }  // ends itself after PER_VISIT pieces
    bool        finished() const override { return over; }

    void begin() override {
        scanAnsi();
        load();
    }
    Cell* buf = nullptr;  // arena block big enough for the tallest piece

    bool load() {
        if (!buf) buf = (Cell*)arenaAlloc((150 + 3) * COLS * sizeof(Cell));
        rows  = buf;
        nrows = 0;
        if (!buf) return false;
        if (!ansiN) {
            static const char* msg[] = {"NO CARRIER", "", "No ANSI art on the SD card (/ansi).",
                                        "Run tools/fetch_ansi.py and tools/sd_ansi.py"};
            nrows = 4;
            for (int i = 0; i < 4; i++) lineOf(rows + i * COLS, msg[i], i ? 7 : 12);
            title[0] = author[0] = group[0] = 0;
        } else {
            int n = ansiN;
            piece = ((ansiNext % n) + n) % n;
            ansiNext = piece + 1;
            AnsiFile f = ansiOpen(piece);
            uint8_t hdr[84];
            if (!f || f.read(hdr, 84) != 84 || memcmp(hdr, "CG1", 3)) return false;
            int h = hdr[4] | hdr[5] << 8;
            memcpy(title, hdr + 8, 36);
            title[36] = 0;
            memcpy(author, hdr + 44, 20);
            author[20] = 0;
            memcpy(group, hdr + 64, 20);
            group[20] = 0;
            // 3 rows of "modem" preamble in front of the art
            if (h > 150) h = 150;
            nrows = h + 3;
            char l0[COLS + 1];
            snprintf(l0, sizeof(l0), "Downloading %s ...", f.name);
            lineOf(rows, l0, 8);
            if (title[0]) {
                snprintf(l0, sizeof(l0), "\"%s\" by %s%s%s", title, author[0] ? author : "?", group[0] ? " / " : "", group);
            } else {
                l0[0] = 0;
            }
            lineOf(rows + COLS, l0, 11);
            lineOf(rows + 2 * COLS, "", 7);
            f.read(rows + 3 * COLS, h * COLS * 2);
        }
        term.rows  = rows;
        term.nrows = nrows;
        term.cps   = 2880;
        term.layout();
        t0     = millis();
        doneAt = 0;
        topF   = 0;
        typing = true;
        manual = false;
        return true;
    }

    bool next() {
        if (++shown >= PER_VISIT) return false;
        load();
        return true;
    }
    void skipTyping() { t0 = millis() - (term.total * 1000ull / term.cps) - 1; }

    // horizontal swipes flip pieces (shown instantly, no dial-up)
    bool swipe(int dir) override {
        ansiNext = piece + dir;
        load();
        skipTyping();
        doneAt = millis() - t0;
        topF   = 0;
        manual = true;
        lastTouch = millis();
        return true;
    }
    bool drag(int dy) override {
        if (typing) {
            skipTyping();
            topF = nrows > VIS ? nrows - VIS : 0;
        }
        manual    = true;
        lastTouch = millis();
        topF -= dy / 8.0f;
        float maxTop = nrows > VIS ? nrows - VIS : 0;
        topF = topF < 0 ? 0 : topF > maxTop ? maxTop : topF;
        return true;
    }

    void update(uint32_t) override {
        t      = millis() - t0;
        typing = term.cursor(t, li, ci);
        float target;
        if (typing) {
            target = li - VIS + 1;
        } else if (manual || holdMode) {
            // the viewer is in charge: stay put; a forgotten manual piece resumes the show after 20 s
            target = topF;
            if (manual && !holdMode && millis() - lastTouch > 20000) {
                if (!next()) over = true;
                return;
            }
        } else {
            if (!doneAt) doneAt = t;
            // after typing: hold the bottom, pan back to the top, pan down again, next
            float hold = 2500, maxTop = nrows > VIS ? nrows - VIS : 0, speed = 7.0f;  // rows per second
            float e = t - doneAt, upT = maxTop / speed * 1000 / 3;
            if (e < hold) target = maxTop;
            else if (e < hold + upT) target = maxTop * (1 - (e - hold) / upT);
            else if (e < hold + upT + 1500) target = 0;
            else if (e < hold * 2 + upT + 1500 + maxTop / speed * 1000) target = fminf(maxTop, (e - hold - upT - 1500) / 1000 * speed);
            else {
                if (!next()) over = true;
                return;
            }
        }
        if (target < 0) target = 0;
        topF += (target - topF) * (typing ? 0.35f : 1.0f);
        char tail[40];
        snprintf(tail, sizeof(tail), "%d/%d%s", piece + 1, ansiN, holdMode ? " HOLD" : "");
        fmtStatus(term.status, 28800, !typing, millis(), tail);
    }

    void strip(int y0, int h, uint16_t* out) override {
        for (int y = y0; y < y0 + h; y++, out += SW) {
            if (y >= VIS * 8) {
                term.statusLine(out, y - VIS * 8);
                continue;
            }
            int src = (int)(topF * 8) + y, L = src >> 3, gr = src & 7;
            if (!rows || L >= nrows || L > li) {
                memset(out, 0, SW * 2);
                continue;
            }
            int lim = (typing && L == li) ? (int)ci : COLS;
            textLine4x8(out, rows + L * COLS, gr, lim);
            if (typing && L == li && ((millis() / 267) & 1) == 0 && gr == 7) {
                int cx = lim < COLS ? lim : COLS - 1;
                for (int k = 0; k < 4; k++) out[cx * 4 + k] = cgaColor(7);
            }
        }
    }
};
Demo* makeAnsiGallery() { return new AnsiGallery(); }

// ------------------------------------------------------------------ Matrix rain
// Full size 8x16 glyphs, 40x15 grid, each column a falling stream.
struct Matrix : Demo {
    static constexpr int C = 40, R = 15;
    uint8_t              ch[C * R], age[C * R];
    float                head[C], speed[C];
    uint16_t             ramp[64];
    uint32_t             last = 0;

    const char* name() const override { return "Matrix Rain"; }
    Res         res() const override { return Res::Custom; }

    void begin() override {
        for (int i = 0; i < C * R; i++) {
            ch[i]  = random(33, 255);
            age[i] = 63;
        }
        for (int x = 0; x < C; x++) {
            head[x]  = -random(0, 30);
            speed[x] = 6 + random(0, 14);
        }
        for (int i = 0; i < 64; i++) {
            int k   = 63 - i;
            ramp[i] = i == 0 ? rgb(220, 255, 220) : rgb(0, k * 4, k);
        }
    }
    void update(uint32_t t) override {
        float dt = (t - last) / 1000.0f;
        last     = t;
        for (int i = 0; i < C * R; i++) {
            if (age[i] < 63) age[i]++;
            if (random(0, 60) == 0) ch[i] = random(33, 255);
        }
        for (int x = 0; x < C; x++) {
            int before = (int)head[x];
            head[x] += speed[x] * dt;
            for (int y = before + 1; y <= (int)head[x]; y++)
                if (y >= 0 && y < R) {
                    age[y * C + x] = 0;
                    ch[y * C + x]  = random(33, 255);
                }
            if (head[x] > R + 20) {
                head[x]  = -random(0, 10);
                speed[x] = 6 + random(0, 14);
            }
        }
    }
    void strip(int y0, int h, uint16_t* out) override {
        for (int y = y0; y < y0 + h; y++, out += SW) {
            int row = y / 16, gr = y % 16;
            for (int x = 0; x < C; x++) {
                int      i    = row * C + x;
                uint8_t  bits = font16[ch[i] * 16 + gr];
                uint16_t fg = ramp[age[i]], bg = 0;
                for (int b = 0; b < 8; b++) out[x * 8 + b] = (bits & (0x80 >> b)) ? fg : bg;
            }
        }
    }
};
Demo* makeMatrix() { return new Matrix(); }

#include "intro.inc"
