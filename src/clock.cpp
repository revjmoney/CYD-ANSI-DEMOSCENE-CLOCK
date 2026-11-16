// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// ANSI desk clock: the time drawn in TheDraw (.TDF) fonts from the SD card,
// the way ACiD and iCE drew their logos. Swipe up/down to change font, tap
// for settings. Time comes from NTP at boot (see net.cpp).
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include "demo.h"
#include "net.h"
#include "acidwarp.h"

static SPIClass sdSpi(VSPI);
static bool     sdTried = false, sdOk = false;
bool clockSdMount() {
    if (!sdTried) {
        sdTried = true;
        sdSpi.begin(18, 19, 23, 5);
        sdOk = SD.begin(5, sdSpi, 20000000, "/sd", 2);  // two open files is plenty
        Serial.printf("clock: sd %s, heap %u (max block %u)\n", sdOk ? "ok" : "FAILED", ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    return sdOk;
}

extern int clockStyleForce, clockBgForce, clockFxForce;
extern bool clockSentinelNow;
namespace {

constexpr int BIG_W = 40, BIG_H = 14;    // 8x16 cells above the status line
constexpr int SMALL_W = 80, SMALL_H = 29;  // 4x8 cells
const char*   IDX = "/fonts/clock.idx";

const char* const autoNames[] = {"OFF", "EVERY MINUTE", "EVERY 10 MIN", "EVERY HOUR"};
const int         autoMins[]  = {0, 1, 10, 60};

struct Clock : Demo {
    // settings (NVS "clock")
    int  fontIdx = 0, autoMode = 0, tz = 0;
    bool h24 = false, showDate = true, stay = false;  // stay: the clock is the desk mode (stayMode 1)
    uint8_t stayTo = 0;                               // 0 auto-play, 1 stay on the clock, 2 Cell Pet (restarts into it)
    // backdrop behind the ANSI-font clock
    enum { BG_BLANK, BG_ANSI, BG_MATRIX, BG_STARS, BG_ACID, BG_ACID_DARK, NBG };
    int bgMode = BG_ANSI;
    int  secMode = 0;  // 0 off, 1 HH:MM:SS, 2 giant seconds

    // font
    int      fontCount = 0;
    uint8_t* font = nullptr;  // whole .tdf file, in a scratch slot
    size_t   fontLen = 0;
    char     fontName[16] = "";
    bool     fontOk = false;

    // screen
    Cell*    grid;   // SMALL_W * (SMALL_H) cells, also used as BIG_W * BIG_H
    Cell*    shown;  // the finished copy strip() draws (strip runs on the other core)
    bool     shownBig = false;
    char     dateStr[24] = "", secStr[4] = "";
    int      infoX = 0, infoY = 200;
    Cell*    art;   // backdrop: SMALL_H rows of an ANSI gallery piece
    bool     artOk = false;
    uint32_t artMs = 0;  // when the current ANSI backdrop was loaded (a new one every 10 s)
    bool     big = false, menu = false;
    char     status[81];
    int      lastMin = -1, lastSec = -1;
    uint32_t touchMs = 0;
    int      tx0, ty0, tlx, tly;
    bool     wasDown = false;

    File pak;  // /fonts/clock.pak: every clock font in one file, so loading is one seek
    void end() override {
        if (pak) pak.close();
    }

    const char* name() const override { return "ANSI Clock"; }
    // 0 ANSI (TheDraw fonts), 1 MATRIX (digits fill with code rain), 2 STARFIELD
    int  style   = 0;
    bool fbDirty = false;  // the starfield drew over the font/grid scratch memory
    Res  res() const override { return style == 2 && !menu ? Res::High : Res::Custom; }
    bool        interactive() const override { return true; }
    uint32_t    duration() const override { return stay ? 0xFFFFFFFF : clockBetween ? 10000 : 30000; }

    void load() {
        Preferences p;
        p.begin("clock", true);
        fontIdx  = p.getInt("font", -1);
        autoMode = p.getUChar("auto", 0);
        h24      = p.getBool("24h", false);
        showDate = p.getBool("date", true);
        stayTo   = p.getUChar("stayto", p.getBool("stay", false) ? 1 : 0) % 3;
        stay     = stayTo == 1;
        stayMode = stayTo;
        bgMode   = p.getUChar("bgm", p.getBool("bg", true) ? BG_ANSI : BG_BLANK) % NBG;
        secMode  = p.getUChar("sec", 1);
        style    = p.getUChar("style", 0) % 3;
        mfx      = p.getUChar("mfx", FX_CYCLE) % NFX;
        if (p.getUChar("ver", 0) < 2) secMode = 1;  // seconds became the default in v2
        p.end();
        tz = netTz();
    }
    void save() {
        Preferences p;
        p.begin("clock");
        p.putInt("font", fontIdx);
        p.putUChar("auto", autoMode);
        p.putBool("24h", h24);
        p.putBool("date", showDate);
        p.putBool("stay", stay);
        p.putUChar("stayto", stayTo);
        p.putUChar("bgm", bgMode);
        p.putUChar("sec", secMode);
        p.putUChar("ver", 2);
        p.putUChar("style", style);
        p.putUChar("mfx", mfx);
        p.putBool("between", clockBetween);
        p.end();
    }

    void begin() override {
        load();
        grid = (Cell*)scratch(3);  // custom-res demo: the frame buffer slots are free
        art   = grid + SMALL_W * SMALL_H;
        shown = art + SMALL_W * SMALL_H;
        loadArt();
        // the grid lives where the last demo was drawing: clear it before the first strip
        for (int i = 0; i < SMALL_W * SMALL_H; i++) grid[i] = shown[i] = {32, 0x07};
        font = scratch(1);         // slots 1+2 are contiguous: 38 KB, fonts are under 26 KB
        if (clockSdMount()) {
            // in the root: looking a name up inside the 4,600-file /fonts folder takes seconds
            pak = SD.open("/clock.pak");
            if (!pak) pak = SD.open("/fonts/clock.pak");
            char magic[8];
            if (pak && pak.read((uint8_t*)magic, 8) == 8 && !memcmp(magic, "TDFPACK1", 8)) {
                uint32_t n = 0;
                pak.read((uint8_t*)&n, 4);
                fontCount = n;
            } else {
                if (pak) pak.close();
                indexFonts();
            }
        }
        if (fontCount) {
            // first run: start on a drippy red graffiti font
            if (fontIdx < 0) fontIdx = fontIndexOf("Font37DripR.tdf");
            fontIdx = (fontIdx % fontCount + fontCount) % fontCount;
        }
        loadFont();
        // popping up between demos: a surprise style and font each time (not
        // saved, so a desk clock on "stay" keeps its own pick)
        if (clockBetween && !stay) {
            style  = esp_random() % 3;
            bgMode = 1 + esp_random() % (NBG - 1);  // any backdrop but blank
        }
        if (clockStyleForce >= 0) style = clockStyleForce;  // bench testing (serial 'c')
        if (clockFxForce >= 0) mfx = clockFxForce;  // bench testing (serial 'x')
        if (clockBgForce >= 0) bgMode = clockBgForce;      // bench testing (serial 'b')
        matrixInit();
        starsInit();
        shipInit();
        acid = scratch(0);  // custom-res demo: nobody draws in the frame buffer
        if (bgMode >= BG_ACID) acidwarp::begin(acid);
        fxCur = -1;
        if (clockBetween && !stay && fontCount) {
            int keep = fontIdx;
            for (int tries = 0; tries < 5; tries++) {
                fontIdx = esp_random() % fontCount;
                loadFont();
                if (fontOk) break;
            }
            if (!fontOk) {
                fontIdx = keep;
                loadFont();
            }
        }
    }

    // a random slice of a random ANSI gallery piece, dimmed, behind the time
    void loadArt() {
        artOk = false;
        if (!ansiCount()) return;
        AnsiFile f = ansiOpen(esp_random());
        uint8_t hdr[84];
        if (!f || f.read(hdr, 84) != 84 || memcmp(hdr, "CG1", 3)) return;
        int h = hdr[4] | hdr[5] << 8, start = h > SMALL_H ? esp_random() % (h - SMALL_H + 1) : 0;
        memset(art, 0, SMALL_W * SMALL_H * sizeof(Cell));
        f.seek(84 + start * SMALL_W * 2);
        f.read((uint8_t*)art, min(h - start, SMALL_H) * SMALL_W * 2);
        // ghost it: every colour becomes dark grey on black, so any font stays readable
        for (int i = 0; i < SMALL_W * SMALL_H; i++) {
            bool ink = (art[i].attr & 15) || (art[i].attr >> 4);
            if ((art[i].attr >> 4) && art[i].ch == 32) art[i].ch = 177;  // solid background -> shade
            art[i].attr = ink ? 0x08 : 0x00;
        }
        artOk = true;
        artMs = millis();
    }

    // name of font number i from the index (one line per font)
    // where each line of clock.idx starts, so any font name is one seek away
    uint32_t* lineAt = nullptr;
    void      indexFonts() {
        constexpr int MAXFONTS = 4096;
        File          f = SD.open(IDX);
        if (!f) return;
        lineAt = (uint32_t*)arenaAlloc(MAXFONTS * 4);
        if (!lineAt) return;
        uint8_t  buf[512];
        uint32_t pos = 0;
        bool     atStart = true;
        int      got;
        while ((got = f.read(buf, sizeof buf)) > 0) {
            for (int k = 0; k < got; k++, pos++) {
                if (atStart && buf[k] != '\n' && fontCount < MAXFONTS) lineAt[fontCount++] = pos;
                atStart = buf[k] == '\n';
            }
        }
        f.close();
    }
    String fontFile(int i) {
        if (!lineAt || i < 0 || i >= fontCount) return "";
        File f = SD.open(IDX);
        if (!f) return "";
        f.seek(lineAt[i]);
        String s = f.readStringUntil('\n');
        f.close();
        s.trim();
        return s;
    }

    int fontIndexOf(const char* name) {
        if (pak) {
            char want[16] = {};
            strncpy(want, name, 15);
            char* dot = strrchr(want, '.');
            if (dot) *dot = 0;
            for (int i = 0; i < fontCount; i++) {
                char nm[17] = {};
                pak.seek(12 + i * 24 + 8);
                pak.read((uint8_t*)nm, 16);
                if (!strcmp(nm, want)) return i;
            }
            return 0;
        }
        File f = SD.open(IDX);
        for (int i = 0; f && f.available(); i++) {
            String s = f.readStringUntil('\n');
            s.trim();
            if (s == name) return i;
        }
        return 0;
    }

    void loadFont() {
        fontOk  = false;
        fontLen = 0;
        strcpy(fontName, "BUILT-IN");
        if (!fontCount) return;
        if (pak) {
            uint32_t ent[2];
            pak.seek(12 + fontIdx * 24);
            if (pak.read((uint8_t*)ent, 8) != 8 || ent[1] > LSIZE * 2 || ent[1] < 233) return;
            pak.seek(ent[0]);
            fontLen = pak.read(font, ent[1]);
        } else {
            File f = SD.open("/fonts/" + fontFile(fontIdx));
            if (!f || f.size() > LSIZE * 2 || f.size() < 233) return;
            fontLen = f.read(font, f.size());
            f.close();
        }
        int nl = font[24] < 12 ? font[24] : 12;
        memcpy(fontName, font + 25, nl);
        fontName[nl] = 0;
        fontOk       = font[41] == 1 || font[41] == 2;
        lastMin      = -1;  // force a redraw
    }
    void stepFont(int d) {
        if (!fontCount) return;
        fontIdx = ((fontIdx + d) % fontCount + fontCount) % fontCount;
        loadFont();
        if (bgMode == BG_ANSI) loadArt();
        save();
    }

    // ---- glyphs ----
    // glyph data for an ASCII char, or nullptr
    const uint8_t* glyph(char c) {
        if (!fontOk || c < 33 || c > 126) return nullptr;
        int off = font[45 + (c - 33) * 2] | font[46 + (c - 33) * 2] << 8;
        if (off == 0xFFFF || 233 + off + 2 > (int)fontLen) return nullptr;
        return font + 233 + off;
    }
    int glyphW(char c) {
        const uint8_t* g = glyph(c);
        if (g) return g[0];
        return c == ':' ? 2 : c == ' ' ? 3 : 8;  // built-in fallbacks
    }
    int colonH = 16;
    int glyphH(char c) {
        const uint8_t* g = glyph(c);
        if (g) return g[1];
        return c == ':' ? 1 : 16;  // the fallback colon fits any height (see drawBuiltin)
    }
    // draw char c with its top-left at (x,y) in a gw x gh grid; returns its width
    int draw(char c, int x, int y, int gw, int gh, bool dimColon) {
        const uint8_t* g = glyph(c);
        if (!g) return drawBuiltin(c, x, y, gw, gh, dimColon);
        int w = g[0], row = 0, col = 0;
        const uint8_t* p = g + 2;
        bool colorFont   = font[41] == 2;
        static const uint8_t ramp[] = {15, 11, 11, 3, 3, 9, 9, 1, 1, 1, 1, 1};  // block fonts: a blue fade
        while (*p && p < font + fontLen) {
            uint8_t ch = *p++;
            if (ch == '\r') {
                row++;
                col = 0;
                continue;
            }
            uint8_t at = colorFont ? *p++ : ramp[row < 12 ? row : 11];
            if (ch < 32) ch = 32;
            int xx = x + col, yy = y + row;
            bool clear = ch == 32 && (at >> 4) == 0 && bgMode == BG_ANSI && artOk;  // let the art show through
            if (xx >= 0 && xx < gw && yy >= 0 && yy < gh && !(dimColon && c == ':') && !clear)
                grid[yy * gw + xx] = {ch, at};
            col++;
        }
        return w;
    }
    // no font: the VGA ROM glyph blown up, one full block per pixel
    int drawBuiltin(char c, int x, int y, int gw, int gh, bool dimColon) {
        const uint8_t* g = &font16[(uint8_t)c * 16];
        int            w = glyphW(c);
        if (c == ':') {  // two dots at a third and two thirds of the digit height
            if (!dimColon)
                for (int r : {colonH / 3, colonH * 2 / 3})
                    if (y + r >= 0 && y + r < gh) grid[(y + r) * gw + x] = grid[(y + r) * gw + x + 1] = {219, 0x0F};
            return w;
        }
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if ((g[r] & (0x80 >> b)) && x + b < gw && y + r < gh) grid[(y + r) * gw + x + b] = {219, (uint8_t)(r < 8 ? 0x0F : 0x0B)};
        return w;
    }

    void text(const char* s, int y, int gw, uint8_t attr) {
        if (y < 0 || y >= (big ? BIG_H : SMALL_H)) return;
        int n = strlen(s), x = (gw - n) / 2;
        for (int i = 0; i < n; i++)
            if (x + i >= 0 && x + i < gw) grid[y * gw + x + i] = {(uint8_t)s[i], attr};
    }

    // ---- compose the clock face ----
    void compose(const tm& t, bool colonOff) {
        char s[12];
        int  hr = h24 ? t.tm_hour : (t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12);
        if (secMode == 2) snprintf(s, sizeof s, "%02d", t.tm_sec);
        else snprintf(s, sizeof s, h24 ? "%02d:%02d" : "%d:%02d", hr, t.tm_min);
        int n = strlen(s), sp = fontOk ? font[42] : 1, w = 0, h = 0;
        for (int i = 0; i < n; i++) {
            w += glyphW(s[i]) + (i ? sp : 0);
            h = max(h, glyphH(s[i]));
        }
        // room under the time for the info line (32 px) or giant-seconds time text
        bool info     = showDate || secMode == 1;
        int  dateRows = info ? 3 : 0;
        // tighten the letter spacing before giving up on the big 8x16 cells
        while (w > BIG_W - 2 && sp > 0) {
            w -= n - 1;
            sp--;
        }
        big    = w <= BIG_W - 2 && h + dateRows <= BIG_H;
        if (!big && info) dateRows = 5;  // same 32+ px in 8 px rows
        int gw = big ? BIG_W : SMALL_W, gh = big ? BIG_H : SMALL_H;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                grid[y * gw + x] = bgMode == BG_ANSI && artOk && y < SMALL_H ? art[y * SMALL_W + x] : Cell{32, 0x07};
        int x = (gw - w) / 2, y = (gh - h - dateRows) / 2;
        colonH = h;  // the fallback colon scales to the digits
        for (int i = 0; i < n && !boomHide; i++) {  // (left out while the pieces are flying)
            // short glyphs (most fonts' colons) sit centred on the digits, not on top
            int gy = glyph(s[i]) ? y + (h - glyphH(s[i])) / 2 : y;
            x += draw(s[i], x, gy, gw, gh, colonOff) + sp;
        }
        // info line under the time: date at 1.5x and seconds at 2x, drawn over the grid
        char ds[24] = "", ss[4] = "";
        if (showDate) {
            strftime(ds, sizeof ds, "%a %b %d %Y", &t);
            for (char* c = ds; *c; c++) *c = toupper(*c);
        }
        if (secMode == 1) snprintf(ss, sizeof ss, "%02d", t.tm_sec);
        if (secMode == 2)  // giant seconds: the time itself goes in the info line
            snprintf(ds, sizeof ds, h24 ? "%02d:%02d" : "%d:%02d %s", hr, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
        else weatherLine(t, ds, sizeof ds);
        int dw = strlen(ds) * 12, sw = strlen(ss) * 16, gap = dw && sw ? 16 : 0;
        publishInfo(ds, ss, (SW - dw - gap - sw) / 2, (y + h) * (big ? 16 : 8) + 6);
        publish();
    }
    // hand finished text to strip() in one go, so it never sees half a frame
    void publish() {
        memcpy(shown, grid, SMALL_W * SMALL_H * sizeof(Cell));
        shownBig = big;
    }
    void publishInfo(const char* ds, const char* ss, int x, int y) {
        memcpy(dateStr, ds, sizeof dateStr);
        memcpy(secStr, ss, sizeof secStr);
        infoX = x;
        infoY = y;
    }

    // ---- settings ----
    void menuLine(int row, const char* s, uint8_t attr = 0x0A) {
        int n = strlen(s);
        for (int i = 0; i < BIG_W; i++) grid[row * BIG_W + i] = {(uint8_t)(i < n ? s[i] : ' '), attr};
    }
    void composeMenu() {
        static const char* secNames[] = {"OFF", "ON", "BIG"};
        char b[48];
        for (int i = 0; i < BIG_W * BIG_H; i++) grid[i] = {32, 0x07};
        big = true;
        static const char* styleNames[] = {"ANSI FONTS", "MATRIX", "STARFIELD"};
        snprintf(b, sizeof b, " CLOCK SETTINGS    STYLE: %-10s >", styleNames[style]);
        menuLine(0, b, 0x1F);
        snprintf(b, sizeof b, " < FONT: %-12s %4d/%-4d >", fontName, fontIdx + 1, fontCount);
        menuLine(1, b, 0x0E);
        snprintf(b, sizeof b, "   AUTO FONT: %s", autoNames[autoMode]);
        menuLine(2, b);
        snprintf(b, sizeof b, "   TIME: %s", h24 ? "24 HOUR" : "12 HOUR");
        menuLine(3, b);
        snprintf(b, sizeof b, "   SECONDS: %s", secNames[secMode]);
        menuLine(4, b);
        snprintf(b, sizeof b, "   DATE: %s", showDate ? "ON" : "OFF");
        menuLine(5, b);
        snprintf(b, sizeof b, "   ZONE: %s", netTzName(tz));
        menuLine(6, b);
        static const char* bgNames[] = {"BLANK", "ANSI ART (NEW EVERY 10 S)", "MATRIX", "STARFIELD", "ACID WARP", "ACID WARP DARK"};
        static const char* fxNames[] = {"OFF", "CYCLE ALL", "PLASMA", "WIREFRAME", "TUNNEL", "RIPPLES"};
        if (style == 1) snprintf(b, sizeof b, "   MATRIX FX: %s", fxNames[mfx]);
        else snprintf(b, sizeof b, "   BACKDROP: %s", bgNames[bgMode]);
        menuLine(7, b);
        snprintf(b, sizeof b, "   BETWEEN DEMOS: %s", clockBetween ? "ON" : "OFF");
        menuLine(8, b);
        static const char* stayNames[] = {"OFF", "CLOCK (boots here)", "PET (boots there)"};
        snprintf(b, sizeof b, "   STAY ON: %s", stayNames[stayTo]);
        menuLine(9, b);
        menuLine(10, "   WIFI: < TYPE PASSWORD | ROUTER WPS >", 0x0B);
        menuLine(11, "   SYNC TIME NOW (restarts)", 0x0B);
        menuLine(12, !netConfigured() ? "   (or: tools/wifi_set.py over USB)" : forgetArmed ? "   TAP AGAIN TO FORGET WIFI" : "   FORGET SAVED WIFI", forgetArmed ? 0x0C : 0x03);
        menuLine(13, "   CLOSE", 0x0F);
        publish();
    }
    bool forgetArmed = false;
    void menuTap(int x, int y) {
        if (y / 16 != 12) forgetArmed = false;
        switch (y / 16) {
            case 0:
                style = (style + 1) % 3;
                matrixInit();
                break;
            case 1: stepFont(x < SW / 2 ? -1 : 1); break;
            case 2: autoMode = (autoMode + 1) % 4; break;
            case 3: h24 = !h24; break;
            case 4: secMode = (secMode + 1) % 3; break;
            case 5: showDate = !showDate; break;
            case 6:
                tz = (tz + 1) % netTzCount();
                netSetTz(tz);
                break;
            case 7:
                if (style == 1) {
                    mfx   = (mfx + 1) % NFX;
                    fxCur = -1;
                    break;
                }
                bgMode = (bgMode + 1) % NBG;
                if (bgMode == BG_ANSI) loadArt();
                if (bgMode >= BG_ACID) acidwarp::begin(acid);
                break;
            case 8: clockBetween = !clockBetween; break;
            case 9:
                stayTo   = (stayTo + 1) % 3;
                stay     = stayTo == 1;
                stayMode = stayTo;
                if (stayTo == 2) {  // Cell Pet takes the whole board: restart into it (its settings have the way back)
                    save();
                    delay(50);
                    ESP.restart();
                }
                break;
            case 10: x < SW / 2 ? netRequestKeyboard() : netRequestWps(); break;  // both restart into setup
            case 11: netRequestSync(); break;
            case 12:  // needs a second tap, so a stray touch can't wipe the WiFi
                if (netConfigured() && forgetArmed) {
                    netForget();
                    forgetArmed = false;
                } else {
                    forgetArmed = netConfigured();
                }
                save();
                lastMin = -1;
                return;
            case 13: menu = false; break;
            default: return;
        }
        save();
        lastMin = -1;
    }

    // ---- shared: the date / seconds line ----
    void infoStrings(const tm& t) {
        int hr     = h24 ? t.tm_hour : (t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12);
        char ds[24] = "", ss[4] = "";
        if (showDate) {
            strftime(ds, sizeof ds, "%a %b %d %Y", &t);
            for (char* c = ds; *c; c++) *c = toupper(*c);
        }
        if (secMode) snprintf(ss, sizeof ss, "%02d", t.tm_sec);
        (void)hr;
        weatherLine(t, ds, sizeof ds);
        int dw = strlen(ds) * 12, sw = strlen(ss) * 16, gap = dw && sw ? 16 : 0;
        publishInfo(ds, ss, (SW - dw - gap - sw) / 2, infoY);
    }
    // the info line takes turns: the date for 6 seconds, then the weather for 6
    void weatherLine(const tm& t, char* ds, size_t n) {
        int tf, code, hi, lo;
        if ((t.tm_sec / 6) % 2 == 0 || !netWeather(time(nullptr), tf, code, hi, lo)) return;
        if (hi > -999) snprintf(ds, n, "%d\xF8" "F %s H%d L%d", tf, netWeatherName(code), hi, lo);
        else snprintf(ds, n, "%d\xF8" "F %s", tf, netWeatherName(code));
    }
    // the time as a pixel mask from the VGA ROM font: rows 2..13 of each glyph
    // (colons are 4 columns wide); returns the width in pixels-of-the-font
    int timeMask(const tm& t, uint8_t* mask, int mw, int mh, int ox, int oy) {
        char s[8];
        int  hr = h24 ? t.tm_hour : (t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12);
        snprintf(s, sizeof s, h24 ? "%02d:%02d" : "%d:%02d", hr, t.tm_min);
        int w = 0;
        for (char* c = s; *c; c++) w += *c == ':' ? 4 : 8;
        int x = ox >= 0 ? ox : (mw - w) / 2;
        memset(mask, 0, mw * mh);
        for (char* c = s; *c; c++) {
            int cols = *c == ':' ? 4 : 8, first = *c == ':' ? 2 : 0;
            for (int r = 0; r < 12; r++)
                for (int k = 0; k < cols; k++)
                    if ((font16[(uint8_t)*c * 16 + r + 2] & (0x80 >> (k + first))) && x + k >= 0 && x + k < mw && oy + r < mh)
                        mask[(oy + r) * mw + x + k] = 1;
            x += cols;
        }
        return w;
    }

    // ---- MATRIX: code rain behind clean, solid green digits ----
    static constexpr int MCOLS = 40, MROWS = 15;
    uint8_t mch[MCOLS * MROWS], mage[MCOLS * MROWS], mmask[MCOLS * MROWS];
    float   mhead[MCOLS], mspeed[MCOLS];
    int     mlastRow[MCOLS], mmin = -1;
    uint16_t mramp[64];
    void matrixInit() {
        for (int i = 0; i < MCOLS * MROWS; i++) {
            mch[i]  = 33 + esp_random() % 222;
            mage[i] = 63;
        }
        for (int c = 0; c < MCOLS; c++) {
            mhead[c]    = -(float)(esp_random() % 30);
            mspeed[c]   = 6 + esp_random() % 14;
            mlastRow[c] = -99;
        }
        for (int i = 0; i < 64; i++) mramp[i] = i == 0 ? rgb(220, 255, 220) : rgb(0, (63 - i) * 4, (63 - i));
        mmin = -1;
    }
    void matrixStep(const tm& t, float dt) {
        if (t.tm_min != mmin) {
            timeMask(t, mmask, MCOLS, MROWS, -1, 0);
            mmin = t.tm_min;
        }
        for (int i = 0; i < MCOLS * MROWS; i++) {
            if (mage[i] < 63) mage[i]++;
            if (esp_random() % 60 == 0) mch[i] = 33 + esp_random() % 222;
        }
        for (int c = 0; c < MCOLS; c++) {
            int before = (int)mhead[c];
            mhead[c] += mspeed[c] * dt;
            for (int r = before + 1; r <= (int)mhead[c]; r++) {
                if (r < 0 || r >= MROWS) continue;
                mage[r * MCOLS + c] = 0;
                mch[r * MCOLS + c]  = 33 + esp_random() % 222;
            }
            if (mhead[c] > MROWS + 18) {
                mhead[c]  = -(float)(esp_random() % 10);
                mspeed[c] = 6 + esp_random() % 14;
            }
        }
    }
    // the rain alone, as the font clock's backdrop (dimmer, no white heads)
    void matrixLine(uint16_t* out, int y) {
        int row = y / 16, gr = y % 16;
        for (int c = 0; c < MCOLS; c++) {
            int      i    = row * MCOLS + c;
            uint16_t fg   = mramp[mage[i] > 20 ? mage[i] : 20];
            uint8_t  bits = font16[mch[i] * 16 + gr];
            for (int b = 0; b < 8; b++) out[c * 8 + b] = (bits & (0x80 >> b)) ? fg : 0;
        }
    }

    // the matrix clock, one scanline: everything is drawn as phosphor brightness
    // (0..63) and turned into colour once, by the CRT
    void shipLine(uint16_t* out, int y) {
        static uint8_t lum[SW];
        int            row = y / 16, gr = y % 16;
        for (int c = 0; c < MCOLS; c++) {  // the rain
            int     i = row * MCOLS + c;
            uint8_t v = 63 - mage[i], bits = font16[mch[i] * 16 + gr];
            for (int b = 0; b < 8; b++) lum[c * 8 + b] = bits & (0x80 >> b) ? v : 0;
        }
        if (fxOn) {  // the effect layer, between the rain and the digits
            const uint8_t* fx = acid + (y >> 1) * acidwarp::W;
            for (int x = 0; x < SW; x++) {
                int v  = lum[x] + fxLum[fx[x >> 1]];
                lum[x] = v > 63 ? 63 : v;
            }
        }
        glitchDigits(lum, y);
        if (y >= infoY - 1 && y < infoY + 33) {  // the date line tears along with the digits
            int dw = strlen(dateStr) * 12, tear = gShift[y] / 2;
            if (dateStr[0]) bigText<uint8_t>(lum, y, infoX + tear, infoY + 4, dateStr, 3, 2, 50);
            if (secStr[0]) bigText<uint8_t>(lum, y, infoX + tear + dw + (dw ? 16 : 0), infoY, secStr, 2, 1, 63);
        }
        // scanlines and darker corners: one colour table per line, the side
        // falloff only where it bites (the outer 72 px each side)
        int      vy = vigY[y] * (y & 1 ? 150 : 256) >> 8;
        uint16_t tone[64];
        for (int l = 0; l < 64; l++) tone[l] = crtLut[l * vy >> 8];
        for (int x = 72; x < SW - 72; x++) out[x] = tone[lum[x]];
        for (int x = 0; x < 72; x++) {
            out[x]          = crtLut[lum[x] * vigX[x] * vy >> 16];
            out[SW - 1 - x] = crtLut[lum[SW - 1 - x] * vigX[SW - 1 - x] * vy >> 16];
        }
        crtExtras(out, y);
    }

    // ---- the hovercraft: this is the dash clock in the bowels of a ship, running
    // dark from the Sentinels. Green phosphor CRT; now and then a Sentinel sweep:
    // red alert, power down, a searchlight crawls past, power flickers back.
    uint16_t crtLut[64], beamLut[48];
    uint8_t  vigX[SW], vigY[SH];
    float    sentT = 0, sentWait = 0, power = 1, sweepY = -999;  // -999: no searchlight
    int      sentCount = 0, alarm = 0;
    void     shipInit() {
        for (int x = 0; x < SW; x++) {
            float d = (x - SW / 2) / (float)(SW / 2);
            vigX[x] = (uint8_t)(255 * (1 - 0.35f * d * d));
        }
        for (int y = 0; y < SH; y++) {
            float d = (y - SH / 2) / (float)(SH / 2);
            vigY[y] = (uint8_t)(255 * (1 - 0.45f * d * d));
        }
        for (int i = 0; i < 48; i++) beamLut[i] = rgb(i * 5, i * 5, i * 4);  // the searchlight: cold white
        sentT    = 0;
        sentWait = esp_random() % 3 == 0 ? 3 + esp_random() % 4 : 25 + esp_random() % 40;  // sometimes right away
        power    = 1;
    }
    bool sentinelDark() const { return sentT > 1.5f && sentT < 7.5f; }
    void shipStep(float dt) {
        auto rnd = [](int n) { return (int)(esp_random() % n); };
        float flick = 0.92f + rnd(9) / 100.0f;
        sweepY      = -999;
        alarm       = 0;
        if (sentT > 0) {
            sentT += dt;
            if (sentT < 1.5f) alarm = fmodf(sentT, 0.5f) < 0.25f ? 2 : 0;  // red alert
            else if (sentT < 2.5f) power = 1 - 0.78f * (sentT - 1.5f);    // powering down
            else if (sentT < 7.5f) {                                      // dark, a searchlight passes
                power  = 0.22f;
                sweepY = -30 + (sentT - 2.8f) * 70;
                alarm  = fmodf(sentT, 1.2f) < 0.15f;
            } else if (sentT < 9) {  // power back, stuttering
                power = 0.22f + 0.78f * (sentT - 7.5f) / 1.5f;
                flick = rnd(4) ? 0.4f + rnd(60) / 100.0f : 1;
            } else {
                sentT    = 0;
                power    = 1;
                sentWait = 40 + rnd(50);
                sentCount++;
            }
        } else if ((sentWait -= dt) <= 0 || clockSentinelNow) sentT = 0.001f;
        clockSentinelNow = false;
        float lv = power * flick;
        for (int i = 0; i < 64; i++) {
            int v    = (int)(i * 4 * lv);
            crtLut[i] = rgb(v * v >> 9, v, v * 9 >> 4);  // phosphor green, blooming white at the top
        }
    }
    // after everything is drawn: monochrome phosphor, scanlines, dark corners,
    // the searchlight and the red alert lamps
    void crtLine(uint16_t* out, int y) {
        int vy   = vigY[y] * (y & 1 ? 150 : 256) >> 8;
        int beam = sweepY > -100 ? 47 - abs(y - (int)sweepY) : 0;
        for (int x = 0; x < SW; x++) {
            uint16_t c = __builtin_bswap16(out[x]);
            int      r = (c >> 11) << 1, g = (c >> 5) & 63, b = (c & 31) << 1;
            int      l = r > g ? (r > b ? r : b) : (g > b ? g : b);
            out[x]     = crtLut[l * vigX[x] * vy >> 16];
        }
        (void)beam;
        crtExtras(out, y);
    }
    void crtExtras(uint16_t* out, int y) {
        int beam = sweepY > -100 ? 47 - abs(y - (int)sweepY) : 0;
        if (beam > 0)
            for (int x = 0; x < SW; x++) out[x] = add565(out[x], beamLut[beam * vigX[x] >> 8]);
        if (alarm)
            for (int x = 0; x < 28; x++) {
                int a = (28 - x) * (alarm == 2 ? 9 : 5);
                uint16_t red = rgb(a, 0, a / 8);
                out[x]          = add565(out[x], red);
                out[SW - 1 - x] = add565(out[SW - 1 - x], red);
            }
    }

    // ---- glitching digits: torn scanlines, RGB split, cells decaying into code ----
    int8_t  gShift[SH];             // horizontal tear per scanline
    uint8_t gLine[SH];              // 0 normal, 1 dropped out, 2 overexposed
    uint8_t gCorrupt[MCOLS * MROWS];  // a code glyph showing through a digit cell (0: solid)
    int     gSplit = 0;             // red / blue channel offset in pixels
    float   gBurst = 0, gWait = 2;
    void    glitchStep(float dt) {
        auto rnd = [](int n) { return (int)(esp_random() % n); };
        if ((gWait -= dt) <= 0) {  // a big burst every few seconds
            gBurst = 0.12f + rnd(35) / 100.0f;
            gWait  = 1.2f + rnd(400) / 100.0f;
        }
        if (sentinelDark() && rnd(6) == 0) gBurst = 0.08f + rnd(20) / 100.0f;  // the Sentinel's interference
        bool burst = gBurst > 0;
        if (burst) gBurst -= dt;
        gSplit = burst ? 2 + rnd(6) : rnd(12) == 0 ? 1 + rnd(2) : 0;
        memset(gShift, 0, sizeof gShift);
        memset(gLine, 0, sizeof gLine);
        int bands = burst ? 3 + rnd(7) : rnd(5) == 0;
        for (int b = 0; b < bands; b++) {
            int y0 = rnd(SH), h = 1 + rnd(burst ? 18 : 4), sh = burst ? rnd(61) - 30 : rnd(9) - 4;
            for (int y = y0; y < y0 + h && y < SH; y++) gShift[y] = sh;
        }
        for (int y = 0; y < SH; y++)
            if (rnd(burst ? 14 : 500) == 0) gLine[y] = 1 + rnd(2);
        for (int i = 0; i < MCOLS * MROWS; i++) {
            if (!mmask[i]) gCorrupt[i] = 0;
            else if (rnd(1000) < (burst ? 220 : 12)) gCorrupt[i] = 33 + rnd(222);
            else if (gCorrupt[i] && rnd(100) < 12) gCorrupt[i] = 0;
        }
    }
    void glitchDigits(uint8_t* out, int y) {
        int row = y / 16, gr = y % 16;
        if (row >= MROWS || gLine[y] == 1) return;
        static uint8_t pad[64 + SW + 64];  // margins: tears and splits read off the edges without checks
        uint8_t*       cov = pad + 64;        // 0 empty, 1 solid digit, 2 glyph pixel in a decaying cell
        for (int c = 0; c < MCOLS; c++) {
            int i = row * MCOLS + c;
            if (!mmask[i]) memset(cov + c * 8, 0, 8);
            else if (!gCorrupt[i]) memset(cov + c * 8, 1, 8);
            else {
                uint8_t bits = font16[gCorrupt[i] * 16 + gr];
                for (int b = 0; b < 8; b++) cov[c * 8 + b] = bits & (0x80 >> b) ? 2 : 0;
            }
        }
        // digits: full brightness, a touch dimmer down each cell row; an
        // overexposed line blows out; the split leaves a misconverged ghost
        uint8_t digit = gLine[y] == 2 ? 63 : 58 - gr / 3;
        int     sh = gShift[y], sp = gSplit;
        const uint8_t* src = cov - sh;  // gShift is within +-30, gSplit under 8
        for (int x = 0; x < SW; x++) {
            int g = src[x];
            if (g) out[x] = g == 2 ? 63 : digit;
            else if (sp && (src[x + sp] | src[x - sp]) && out[x] < 30) out[x] = 30;
        }
        if (gLine[y] == 2)
            for (int x = 0; x < SW; x++) out[x] = out[x] + 14 > 63 ? 63 : out[x] + 14;
    }
    // saturating add of two panel (byte-swapped RGB565) colours
    static inline uint16_t add565(uint16_t a, uint16_t b) {
        if (!b) return a;
        a = __builtin_bswap16(a);
        b = __builtin_bswap16(b);
        int r = (a >> 11) + (b >> 11), g = ((a >> 5) & 63) + ((b >> 5) & 63), bl = (a & 31) + (b & 31);
        return __builtin_bswap16((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (bl > 31 ? 31 : bl));
    }

    // ---- MATRIX FX: demo effects drawn into a 160x120 layer over the rain ----
    enum { FX_OFF, FX_CYCLE, FX_PLASMA, FX_WIRE, FX_TUNNEL, FX_RIPPLE, NFX };
    int             mfx = FX_CYCLE, fxCur = -1, fxNext = FX_PLASMA;
    float           fxClock = 0, fxLevel = 0, fxHold = 0;
    uint8_t         fxLum[256];      // brightness of each layer value, this frame
    bool            fxOn = false;
    struct Ring {
        float x, y, r;
    };
    Ring  rings[8];
    int   nrings = 0;
    float ringWait = 0;

    void fxStart(int fx) {
        fxCur = fx;
        memset(acid, 0, acidwarp::W * acidwarp::H);
        nrings = 0;
    }
    int fxRandom() {
        int f;
        do f = FX_PLASMA + esp_random() % (NFX - FX_PLASMA);
        while (f == fxCur && NFX - FX_PLASMA > 1);
        return f;
    }
    void fxLine(int x0, int y0, int x1, int y1, uint8_t v) {
        constexpr int W = acidwarp::W, H = acidwarp::H;
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (int n = 0; n < 400; n++) {
            if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H && acid[y0 * W + x0] < v) acid[y0 * W + x0] = v;
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) err += dy, x0 += sx;
            if (e2 <= dx) err += dx, y0 += sy;
        }
    }
    void fxDecay(int keep) {  // trails: every pixel keeps keep/16 of its brightness
        for (int i = 0; i < acidwarp::W * acidwarp::H; i++) acid[i] = acid[i] * keep >> 4;
    }
    void fxStep(float dt) {
        constexpr int W = acidwarp::W, H = acidwarp::H;
        if (mfx == FX_OFF || !acid) {
            fxOn = false;
            return;
        }
        fxClock += dt;
        // pick what to show; "cycle" changes effect every 20 seconds, fading through black
        if (fxCur < 0) {
            fxStart(mfx == FX_CYCLE ? fxRandom() : mfx);
            fxNext  = fxCur;
            fxLevel = 0;
            fxHold  = 0;
        }
        if (mfx == FX_CYCLE && (fxHold += dt) > 20 && fxNext == fxCur) fxNext = fxRandom();
        if (fxNext != fxCur) {
            if ((fxLevel -= dt * 1.5f) <= 0) {
                fxStart(fxNext);
                fxLevel = 0;
                fxHold  = 0;
            }
        } else if (fxLevel < 1) fxLevel = min(1.0f, fxLevel + dt * 1.5f);
        int lvl = (int)(fxLevel * 256);

        uint8_t t1 = (uint8_t)(int)(fxClock * 50), t2 = (uint8_t)(int)(fxClock * -37), t3 = (uint8_t)(int)(fxClock * 23);
        switch (fxCur) {
            case FX_PLASMA:
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        acid[y * W + x] = (sinT[(x * 3 + t1) & 255] + sinT[(y * 4 + t2) & 255] + sinT[((x + y) * 2 + t3) & 255] +
                                           sinT[((x - y) + (t1 >> 1)) & 255]) >> 2;
                break;
            case FX_TUNNEL: {  // a square corridor rushing at you, its vanishing point wandering
                float cx = W / 2 + sinf(fxClock * 0.7f) * 30, cy = H / 2 + sinf(fxClock * 0.9f) * 18;
                int   z0 = (int)(fxClock * 90);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        int d = max(abs(x - (int)cx), abs((y - (int)cy) * 4 / 3)) + 1, z = (4096 / d + z0) & 63;
                        int v = z < 6 ? 255 : z < 12 ? 70 : 0;
                        acid[y * W + x] = v * min(d, 48) / 48;  // fades out down the corridor
                    }
            } break;
            case FX_WIRE: {  // a hypercube: two cubes spinning against each other, joined at the corners
                fxDecay(11);
                float a = fxClock * 0.9f, b = fxClock * 0.6f, c = fxClock * 0.4f;
                int   px[16], py[16];
                for (int i = 0; i < 16; i++) {
                    float s = i < 8 ? 1.0f : 0.5f, sgn = i < 8 ? 1 : -1;
                    float x = (i & 1 ? 1 : -1) * s, y = (i & 2 ? 1 : -1) * s, z = (i & 4 ? 1 : -1) * s;
                    float ca = cosf(a * sgn), sa = sinf(a * sgn), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
                    float x1 = x * ca - z * sa, z1 = x * sa + z * ca;
                    float y1 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
                    float x2 = x1 * cc - y1 * sc, y2 = x1 * sc + y1 * cc;
                    float k = 70 / (z2 + 3.2f);
                    px[i] = W / 2 + (int)(x2 * k);
                    py[i] = H / 2 + (int)(y2 * k);
                }
                for (int i = 0; i < 8; i++)
                    for (int bit : {1, 2, 4})
                        if (!(i & bit)) {
                            fxLine(px[i], py[i], px[i | bit], py[i | bit], 255);
                            fxLine(px[i + 8], py[i + 8], px[(i | bit) + 8], py[(i | bit) + 8], 200);
                        }
                for (int i = 0; i < 8; i++) fxLine(px[i], py[i], px[i + 8], py[i + 8], 130);
            } break;
            case FX_RIPPLE: {  // shockwaves rolling out from random points
                fxDecay(12);
                if ((ringWait -= dt) <= 0 && nrings < 8) {
                    rings[nrings++] = {(float)(esp_random() % W), (float)(esp_random() % H), 1};
                    ringWait = 0.3f + (esp_random() % 90) / 100.0f;
                }
                for (int k = 0; k < nrings; k++) {
                    Ring& r = rings[k];
                    r.r += dt * 45;
                    if (r.r > 110) {
                        r = rings[--nrings];
                        k--;
                        continue;
                    }
                    uint8_t v = (uint8_t)(255 * (1 - r.r / 110));
                    int     n = (int)(r.r * 6.3f) + 8;
                    for (int j = 0; j < n; j++) {
                        float an = j * 6.2832f / n;
                        int   x = (int)(r.x + cosf(an) * r.r), y = (int)(r.y + sinf(an) * r.r * 0.8f);
                        if (x >= 0 && x < W && y >= 0 && y < H && acid[y * W + x] < v) acid[y * W + x] = v;
                    }
                }
            } break;
        }
        // brightness 0..40 (the rain and digits stay on top); plasma gets banded
        for (int i = 0; i < 256; i++) {
            int v    = fxCur == FX_PLASMA ? (sinT[(i * 6) & 255] * sinT[(i * 6) & 255]) >> 9 : i;
            fxLum[i] = (v * lvl >> 8) * 40 >> 8;
        }
        fxLum[0] = 0;
        fxOn     = true;
    }

    // ---- STARFIELD: warp stars; a swarm flies in and becomes the time ----
    struct Star {
        float x, y, z;
    };
    struct Part {
        float x, y, sx, sy, tx, ty, d;  // position, start, target, delay
    };
    static constexpr int NSTARS = 300, NPARTS = 320;
    Star* stars = nullptr;
    Part* parts = nullptr;
    int   nparts = 0, smin = -1;
    float sclock = 0;
    void starsInit() {
        stars = (Star*)arenaAlloc(NSTARS * sizeof(Star));
        parts = (Part*)arenaAlloc(NPARTS * sizeof(Part));
        if (!stars || !parts) return;
        for (int i = 0; i < NSTARS; i++) stars[i] = {(float)((int)(esp_random() % 2000) - 1000) / 100, (float)((int)(esp_random() % 2000) - 1000) / 100, 0.2f + (esp_random() % 800) / 100.0f};
        nparts = 0;
        smin   = -1;
    }
    void starsPalette() {
        palRamp(0, 63, 0, 0, 0, 255, 255, 255);
        palRamp(64, 127, 0, 30, 60, 120, 240, 255);
        pal[128] = rgb(170, 255, 255);
        pal[129] = rgb(255, 230, 80);
        pal[130] = rgb(0, 0, 0);
    }
    void starsStep(const tm& t, float dt) {
        if (!stars) return;
        starsPalette();
        sclock += dt;
        if (t.tm_min != smin) {  // new minute: burst out, then gather into the new time
            static uint8_t mask[80 * 14];
            timeMask(t, mask, 80, 14, -1, 1);
            int n = 0;
            for (int i = 0; i < 80 * 14 && n < NPARTS; i++)
                if (mask[i]) {
                    Part& p = parts[n];
                    float sx = n < nparts ? p.x : (float)(esp_random() % SW), sy = n < nparts ? p.y : (float)(esp_random() % SH);
                    // explode away from the centre before flying in
                    float ax = sx - SW / 2, ay = sy - SH / 2;
                    p      = {sx, sy, sx + ax * 1.5f, sy + ay * 1.5f, (float)(SW / 2 + ((i % 80) - 40) * 6), (float)((i / 80) * 11 + 4), (esp_random() % 100) / 100.0f};
                    n++;
                }
            nparts = n;
            smin   = t.tm_min;
            sclock = 0;
        }
        for (int i = 0; i < SW * SH; i++) {  // trails
            uint8_t v = fb[i];
            fb[i]     = v && v < 64 ? (v > 8 ? v - 8 : 0) : 0;
        }
        for (int i = 0; i < NSTARS; i++) {
            Star& st = stars[i];
            st.z -= dt * 3;
            if (st.z < 0.1f) st = {(float)((int)(esp_random() % 2000) - 1000) / 100, (float)((int)(esp_random() % 2000) - 1000) / 100, 8};
            int x = SW / 2 + (int)(st.x / st.z * 30), y = SH / 2 + (int)(st.y / st.z * 30);
            if (x < 0 || x >= SW || y < 0 || y >= SH) {
                st.z = 0;
                continue;
            }
            int b = 63 - (int)(st.z * 6);
            fb[y * SW + x] = b < 10 ? 10 : b;
        }
        for (int i = 0; i < nparts; i++) {
            Part& p = parts[i];
            // 0..0.6 s: burst outward; then ease into place (staggered)
            float k = (sclock - p.d * 0.5f);
            float ex, ey;
            if (k < 0.6f) {
                float e = k < 0 ? 0 : k / 0.6f;
                ex = p.sx + (p.sx - SW / 2) * 0.5f * e;
                ey = p.sy + (p.sy - SH / 2) * 0.5f * e;
            } else {
                float e = (k - 0.6f) / 1.2f;
                e       = e > 1 ? 1 : e * e * (3 - 2 * e);
                float bx = p.sx + (p.sx - SW / 2) * 0.5f, by = p.sy + (p.sy - SH / 2) * 0.5f;
                ex = bx + (p.tx - bx) * e;
                ey = by + (p.ty - by) * e;
            }
            p.x = ex, p.y = ey;
            int x = (int)ex, y = (int)ey;
            for (int dy = 0; dy < 5; dy++)
                for (int dx = 0; dx < 5; dx++) {
                    if (x + dx < 0 || x + dx >= SW || y + dy < 0 || y + dy >= SH || (dx % 4 == 0 && dy % 4 == 0)) continue;
                    bool core = dx >= 1 && dx <= 3 && dy >= 1 && dy <= 3;  // a round-ish glowing dot
                    fb[(y + dy) * SW + x + dx] = core ? 128 : 110;
                }
        }
        infoStrings(t);
        int dw = strlen(dateStr) * 8;
        if (dateStr[0]) fbText(fb, SW, SH, infoX, 192, dateStr, 128);
        if (secStr[0]) fbText(fb, SW, SH, infoX + dw + (dw ? 16 : 0), 180, secStr, 129, 2);
        fbDirty = true;
    }

    // ---- backdrops for the ANSI-font clock (drawn under the text, per scanline) ----
    // starfield: the warp stars, projected here and plotted in strip()
    int16_t spx[NSTARS], spy[NSTARS];
    uint8_t spb[NSTARS];
    void    starsBgStep(float dt) {
        if (!stars) return;
        for (int i = 0; i < NSTARS; i++) {
            Star& st = stars[i];
            st.z -= dt * 2;
            if (st.z < 0.1f) st = {(float)((int)(esp_random() % 2000) - 1000) / 100, (float)((int)(esp_random() % 2000) - 1000) / 100, 8};
            int x = SW / 2 + (int)(st.x / st.z * 30), y = SH / 2 + (int)(st.y / st.z * 30);
            if (x < 0 || x >= SW - 1 || y < 0 || y >= SH - 1) {
                st.z   = 0;
                spy[i] = -9;
                continue;
            }
            int b  = 63 - (int)(st.z * 7);
            spx[i] = x;
            spy[i] = y;
            spb[i] = b < 6 ? 6 : b;
        }
    }
    void starsLine(uint16_t* out, int y) {
        memset(out, 0, SW * 2);
        if (!stars) return;
        for (int i = 0; i < NSTARS; i++) {
            int b = spb[i], dy = y - spy[i];
            if (dy < 0 || dy > (b > 40 ? 1 : 0)) continue;  // close stars are 2x2
            uint16_t c = rgb(b * 3, b * 3 + 40 > 255 ? 255 : b * 3 + 40, 255 * b / 63 + 40 > 255 ? 255 : 255 * b / 63 + 40);
            out[spx[i]] = c;
            if (b > 40) out[spx[i] + 1] = c;
        }
    }

    // acid warp backdrop: the real thing (acidwarp.cpp), in the shared low-res buffer
    uint8_t*        acid  = nullptr;
    const uint16_t* acidL = nullptr;

    void update(uint32_t ms) override {
        // gestures: tap = settings (or a menu choice), vertical swipe = font
        if (touch.down && !wasDown) {
            tx0 = tlx = touch.x;
            ty0 = tly = touch.y;
            touchMs = millis();
        }
        if (touch.down) {
            tlx = touch.x;
            tly = touch.y;
        }
        if (!touch.down && wasDown) {
            int dx = tlx - tx0, dy = tly - ty0;
            if (!menu && abs(dy) > 40 && abs(dy) > abs(dx)) stepFont(dy < 0 ? 1 : -1);
            else if (abs(dx) < 20 && abs(dy) < 20 && millis() - touchMs < 700) {
                if (menu) menuTap(tx0, ty0);
                else menu = true;
                lastMin = -1;
            }
        }
        wasDown = touch.down;

        time_t now = time(nullptr);
        tm     t;
        localtime_r(&now, &t);
        if (!menu && autoMode && t.tm_min != lastMin && lastMin >= 0 && t.tm_min % autoMins[autoMode] == 0 && t.tm_sec == 0)
            if (fontCount) {
                fontIdx = esp_random() % fontCount;
                loadFont();
            }
        // nightly resync when it lives on the desk
        if (stay && t.tm_hour == 4 && t.tm_min == 0 && t.tm_sec < 3 && millis() > 3600000UL) netRequestSync();
        if (!menu && style == 0 && bgMode == BG_ANSI && millis() - artMs >= 10000) {
            loadArt();
            lastMin = -1;
        }
        static uint32_t lastMs = 0;
        float           dt     = (ms - lastMs) / 1000.0f;
        lastMs                 = ms;
        if (dt > 0.1f) dt = 0.1f;
        if (fbDirty && (menu || style != 2)) {  // the starfield drew over the font and grid
            loadFont();
            if (bgMode == BG_ANSI) loadArt();
            if (bgMode >= BG_ACID) acidwarp::begin(acid);  // its picture lives in the frame buffer too
            fxCur = -1;
            fbDirty = false;
            lastMin = -1;
        }
        if (menu && boomT >= 0) {  // the menu cancels a flight in progress
            boomT    = -1;
            boomHide = false;
        }
        if (menu) composeMenu();
        else if (style == 1) {
            shipStep(dt);
            matrixStep(t, dt * (0.35f + 0.65f * power));  // the rain crawls while the ship hides
            fxStep(dt);
            glitchStep(dt);
            infoY = 196;
            infoStrings(t);
        } else if (style == 2) starsStep(t, dt);
        else {
            // every 2 minutes (or serial 'z'), the digits explode into the new time
            bool boomNow = (t.tm_min != lastMin && lastMin >= 0 && t.tm_min % 2 == 0) || clockSentinelNow;
            clockSentinelNow = false;
            if (boomNow && boomT < 0 && secMode != 2) startBoom(t);
            boomStep(dt);
            if (t.tm_sec != lastSec || t.tm_min != lastMin) compose(t, (t.tm_sec & 1) && netHasTime());
            if (bgMode == BG_MATRIX) matrixStep(t, dt);
            if (bgMode == BG_STARS) starsBgStep(dt);
            if (bgMode >= BG_ACID) {
                acidwarp::step(dt);
                acidL = acidwarp::lut(bgMode == BG_ACID_DARK ? 256 : 210, bgMode == BG_ACID_DARK);
            }
        }
        lastSec = t.tm_sec;
        lastMin = t.tm_min;

        char sync[24];
        time_t ls = netLastSync();
        if (!netHasTime()) strcpy(sync, "NO TIME: TAP > WIFI");
        else if (!ls) strcpy(sync, "TIME SET");
        else snprintf(sync, sizeof sync, "NTP %ldh AGO", (long)((now - ls) / 3600));
        if (style == 1 && !menu) {
            if (sentT > 0 && sentT < 9)
                snprintf(status, sizeof status, " HOVERCRAFT \xBA %s \xBA EMP ARMED \xBA %s ",
                         sentT < 1.5f ? "!! SENTINEL CONTACT !!" : sentT < 7.5f ? "POWER DOWN: HOLD STILL" : "RESTORING POWER...  ",
                         sentT < 7.5f ? "SCANNER ON THE HULL" : "SWEEP PASSED");
            else
                snprintf(status, sizeof status, " HOVERCRAFT \xBA DECK 3 \xBA BROADCAST DEPTH \xBA SIGNAL \xDB\xDB\xDB\xB0 \xBA EMP ARMED \xBA SWEEPS %d \xBA %s ",
                         sentCount, sync);
        } else
            snprintf(status, sizeof status, " ANSI CLOCK \xB3 %-12s %4d/%-4d \xB3 %-19s \xB3 %s %s ", fontName, fontIdx + 1,
                     fontCount, sync, netTzName(tz), fontCount ? "\xB3 SWIPE:FONT TAP:MENU" : "\xB3 NO /fonts ON SD");
    }

    // ---- every 2 minutes the digits blow apart and fly back together as the new time ----
    struct Bit {
        float   x, y, vx, vy;  // flying; on the way home vx,vy hold where the trip began
        int16_t tx, ty;        // home in the new time
        uint8_t ch, at, nch, nat;
    };
    Bit*  bits     = nullptr;  // lives in the starfield particle memory (free in this style)
    int   nbits    = 0;
    float boomT    = -1;  // seconds into it, <0: not happening
    bool  boomHide = false;
    bool  boomBig  = true;
    // what compose() put under cell i before the digits went on
    Cell backCell(int i, int gw) const {
        int y = i / gw, x = i % gw;
        return bgMode == BG_ANSI && artOk && y < SMALL_H ? art[y * SMALL_W + x] : Cell{32, 0x07};
    }
    bool digitCell(int i, int gw) const {
        Cell c = grid[i], b = backCell(i, gw);
        bool visible = (c.ch != 32 && c.ch != 0 && c.ch != 255) || (c.attr >> 4);
        return visible && (c.ch != b.ch || c.attr != b.attr);
    }
    void startBoom(const tm& t) {
        if (!parts) return;
        bits      = (Bit*)parts;
        int maxb  = NPARTS * sizeof(Part) / sizeof(Bit);
        auto rnd  = [](int n) { return (int)(esp_random() % n); };
        // the old time, from the frame on screen now
        int ogw = big ? BIG_W : SMALL_W, ocells = ogw * (big ? BIG_H : SMALL_H), ocw = big ? 8 : 4, och = big ? 16 : 8;
        int count = 0;
        for (int i = 0; i < ocells; i++) count += digitCell(i, ogw);
        int stride = count / maxb + 1;
        nbits      = 0;
        for (int i = 0, k = 0; i < ocells && nbits < maxb; i++) {
            if (!digitCell(i, ogw) || k++ % stride) continue;
            Bit& b = bits[nbits++];
            b.x = (i % ogw) * ocw, b.y = (i / ogw) * och;
            float dx = b.x - SW / 2 + rnd(41) - 20, dy = b.y - 90 + rnd(41) - 20, d = sqrtf(dx * dx + dy * dy) + 1;
            float sp = 160 + rnd(260);
            b.vx = dx / d * sp, b.vy = dy / d * sp - 120;  // out, and a little up
            b.ch = grid[i].ch, b.at = grid[i].attr;
        }
        // the new time: where everything lands
        compose(t, false);
        int ngw = big ? BIG_W : SMALL_W, ncells = ngw * (big ? BIG_H : SMALL_H), ncw = big ? 8 : 4, nch = big ? 16 : 8;
        static int16_t home[NPARTS * sizeof(Part) / sizeof(Bit)];
        int            nhome = 0;
        count                = 0;
        for (int i = 0; i < ncells; i++) count += digitCell(i, ngw);
        stride = count / maxb + 1;
        for (int i = 0, k = 0; i < ncells && nhome < maxb; i++)
            if (digitCell(i, ngw) && k++ % stride == 0) home[nhome++] = i;
        if (!nbits || !nhome) {
            nbits = 0;
            return;
        }
        // more cells in the new time: extra pieces split off old ones
        for (int k = nbits; k < nhome && k < maxb; k++) {
            bits[k] = bits[rnd(nbits)];
            bits[k].vx += rnd(121) - 60;
            bits[k].vy += rnd(121) - 60;
        }
        nbits = max(nbits, nhome);
        for (int k = 0; k < nbits; k++) {  // shuffle who goes where, so pieces cross the screen
            int j = rnd(nhome), i = home[j];
            if (k < nhome) i = home[(k * 7919) % nhome];
            Bit& b = bits[k];
            b.tx = (i % ngw) * ncw, b.ty = (i / ngw) * nch;
            b.nch = grid[i].ch, b.nat = grid[i].attr;
        }
        boomBig  = big;
        boomHide = true;
        compose(t, false);  // the face without digits, while they fly
        boomT = 0;
    }
    void boomStep(float dt) {
        if (boomT < 0) return;
        float prev = boomT;
        boomT += dt;
        for (int k = 0; k < nbits; k++) {
            Bit& b = bits[k];
            if (boomT < 0.9f) {  // blown out, gravity pulling
                b.x += b.vx * dt;
                b.y += b.vy * dt;
                b.vy += 260 * dt;
                b.vx *= 0.985f;
            } else {
                if (prev < 0.9f) b.vx = b.x, b.vy = b.y;  // remember where the trip home starts
                float d = (k * 37 % 30) / 100.0f, e = (boomT - 0.9f - d) / 1.1f;
                e       = e < 0 ? 0 : e > 1 ? 1 : e * e * (3 - 2 * e);
                b.x     = b.vx + (b.tx - b.vx) * e;
                b.y     = b.vy + (b.ty - b.vy) * e;
                if (e > 0.6f) b.ch = b.nch, b.at = b.nat;  // turns into its piece of the new time
            }
        }
        if (boomT > 2.5f) {  // everyone's home: back to the plain face
            boomT    = -1;
            boomHide = false;
            lastMin  = -1;
        }
    }
    void drawBits(uint16_t* out, int y) {
        int cw = boomBig ? 8 : 4, chh = boomBig ? 16 : 8;
        for (int k = 0; k < nbits; k++) {
            const Bit& b = bits[k];
            int        r = y - (int)b.y;
            if (r < 0 || r >= chh) continue;
            uint8_t  g  = font16[b.ch * 16 + (boomBig ? r : r * 2)];
            uint16_t fg = attrBlend(b.at, 4), bg = attrBlend(b.at, 0);
            bool     solidBg = b.at >> 4;
            for (int px = 0; px < cw; px++) {
                int x = (int)b.x + px;
                if (x < 0 || x >= SW) continue;
                bool on = boomBig ? g & (0x80 >> px) : g & (0xC0 >> (px * 2));
                if (on) out[x] = fg;
                else if (solidBg) out[x] = bg;
            }
        }
    }

    // one scanline of text scaled by num/den (3/2 or 2/1), with a dark drop shadow
    template <typename T>
    static void bigText(T* out, int y, int x0, int y0, const char* s, int num, int den, T col) {
        int     cw = 8 * num / den, ch = 16 * num / den;
        const T shade = 0;
        for (int pass = 0; pass < 2; pass++) {
            int ry = y - y0 - (pass ? 0 : 1);  // shadow: one pixel down and right
            if (ry < 0 || ry >= ch) continue;
            int gr = ry * den / num;
            for (int i = 0; s[i]; i++) {
                uint8_t bits = font16[(uint8_t)s[i] * 16 + gr];
                if (!bits) continue;
                for (int px = 0; px < cw; px++) {
                    int x = x0 + i * cw + px + (pass ? 0 : 1);
                    if (x >= 0 && x < SW && (bits & (0x80 >> (px * den / num)))) out[x] = pass ? col : shade;
                }
            }
        }
    }

    void strip(int y0, int h, uint16_t* out) override {
        for (int y = y0; y < y0 + h; y++, out += SW) {
            if (y >= 232) {
                Cell r[80];
                int  n = strlen(status);
                for (int i = 0; i < 80; i++) {
                    char c = i < n ? status[i] : ' ';
                    r[i]   = {(uint8_t)c, (uint8_t)(style == 1 && !menu ? 0x0A : c == '\xB3' ? 0x31 : 0x30)};
                }
                textLine4x8(out, r, y - 232);
            } else if (style == 1 && !menu) {
                shipLine(out, y);
                continue;
            } else if (!menu && style == 0 && bgMode >= BG_MATRIX) {
                if (bgMode == BG_MATRIX) matrixLine(out, y);
                else if (bgMode == BG_STARS) starsLine(out, y);
                else if (acidL) acidwarp::line(out, y, acidL);
                else memset(out, 0, SW * 2);
                static uint16_t txt[SW];
                if (shownBig) {
                    if (y / 16 < BIG_H) textLine8x16(txt, shown + (y / 16) * BIG_W, y % 16);
                    else memset(txt, 0, sizeof txt);
                } else {
                    textLine4x8(txt, shown + (y / 8) * SMALL_W, y % 8);
                }
                for (int x = 0; x < SW; x++)
                    if (txt[x]) out[x] = txt[x];
            } else if (shownBig) {
                int row = y / 16;
                if (row < BIG_H) textLine8x16(out, shown + row * BIG_W, y % 16);
                else memset(out, 0, SW * 2);
            } else {
                textLine4x8(out, shown + (y / 8) * SMALL_W, y % 8);
            }
            if (boomT >= 0 && style == 0 && !menu && y < 232) drawBits(out, y);
            if (!menu && y < 232 && y >= infoY - 1 && y < infoY + 33) {
                static const uint16_t cyan = rgb(85, 255, 255), yellow = rgb(255, 230, 80);
                int dw = strlen(dateStr) * 12;
                if (dateStr[0]) bigText(out, y, infoX, infoY + 4, dateStr, 3, 2, cyan);
                if (secStr[0]) bigText(out, y, infoX + dw + (dw ? 16 : 0), infoY, secStr, 2, 1, yellow);
            }
            if (style == 1 && !menu) crtLine(out, y);  // the ship's status bar
        }
    }
};

}  // namespace

int   clockStyleForce = -1, clockBgForce = -1, clockFxForce = -1;
bool  clockSentinelNow = false;
Demo* makeAnsiClock() { return new Clock(); }

int clockStaysOn() {
    Preferences p;
    p.begin("clock", true);
    int s = p.getUChar("stayto", p.getBool("stay", false) ? 1 : 0) % 3;
    p.end();
    return s;
}
