// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Extras: a separate boot mode with the whole board to itself.
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_partition.h>
#include <dirent.h>
#include <algorithm>
#include <vector>
#include "board.h"
#include "gfx.h"
#include "ext/nes/nescore.h"
#include "ext/gbcore.h"

void extrasArm() {
    Preferences p;
    p.begin("demoscene");
    p.putUChar("x", 1);
    p.end();
    delay(50);
    ESP.restart();
}

extern unsigned short FrameSkip;  // the NES core's own frame skip setting

namespace {

constexpr int PW = 240, PH = 320;  // portrait
constexpr int PAD_Y = 228;         // controls live below this line
constexpr int ROWS  = 16;          // lines per DMA strip

SPIClass                   sdSpi(VSPI);
const esp_partition_t*     romPart;
const uint8_t*             rom = nullptr;
size_t                     romSize = 0;
spi_flash_mmap_handle_t    romMap;
uint16_t*                  strip[2];
int                        stripIdx = 0, stripY0 = 0, stripRows = 0;
bool                       inFrame = false;
uint8_t                    portrait;
Preferences                prefs;
String                     romPath;

enum Kind { NONE, GB, NES };
struct Entry {
    String name, path;
    Kind   kind;
    bool   inFlash;
};

Kind kindOf(const String& n) {
    String l = n;
    l.toLowerCase();
    if (l.endsWith(".gb") || l.endsWith(".gbc")) return GB;
    if (l.endsWith(".nes")) return NES;
    return NONE;
}

uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return rgb(r, g, b); }

// boardTouch() already corrects the panel's mirroring for this orientation
bool touchAt(int& x, int& y) { return boardTouch(x, y); }

// ---------------------------------------------------------------- strips
void stripEnd();
void stripBegin() {
    if (inFrame) stripEnd();  // the game switched the LCD off mid-frame
    inFrame   = true;
    stripY0   = 0;
    stripRows = 0;
    tft.startWrite();
}
uint16_t* stripRow() { return strip[stripIdx] + stripRows * PW; }
void      stripFlush() {
    if (!stripRows) return;
    tft.pushImageDMA(0, stripY0, PW, stripRows, (const lgfx::swap565_t*)strip[stripIdx]);
    stripIdx ^= 1;
    stripY0 += stripRows;
    stripRows = 0;
}
void stripNext() {
    if (++stripRows == ROWS) stripFlush();
}
void stripEnd() {
    stripFlush();
    tft.endWrite();
    inFrame = false;
}

// ---------------------------------------------------------------- UI bits
void text(int x, int y, const char* s, uint16_t fg, uint16_t bg = TFT_BLACK, int size = 1) {
    tft.setTextColor(fg, bg);
    tft.setTextSize(size);
    tft.drawString(s, x, y);
}
void bar(const char* title) {
    tft.fillRect(0, 0, PW, 20, TFT_DARKGREEN);
    text(6, 6, title, TFT_WHITE, TFT_DARKGREEN);
}
// ---------------------------------------------------------------- USB file copy
// Line commands from the PC (tools live outside this repo):
//   @BAUD n | @MKDIR path | @LS dir | @PUT path size (then 4 KB chunks, each
//   acked with @K) | @RM path | @REFRESH | 1 (taps the first list row)
bool sdReady = false;

void mkdirs(const String& path) {
    for (int i = 1; i < (int)path.length(); i++)
        if (path[i] == '/') SD.mkdir(path.substring(0, i));
}

// returns true when the game list should be rebuilt
bool serialCommand(const String& line) {
    if (line.startsWith("@BAUD ")) {
        Serial.println("@OK");
        Serial.flush();
        Serial.updateBaudRate(line.substring(6).toInt());
    } else if (line.startsWith("@MKDIR ")) {
        mkdirs(line.substring(7) + "/");
        Serial.println("@OK");
    } else if (line.startsWith("@MV ")) {  // "@MV /from /to"
        int sp = line.indexOf(' ', 4);
        Serial.println(sp > 0 && SD.rename(line.substring(4, sp), line.substring(sp + 1)) ? "@OK" : "@ERR");
    } else if (line.startsWith("@RM ")) {
        Serial.println(SD.remove(line.substring(4)) ? "@OK" : "@ERR");
    } else if (line.startsWith("@LS ")) {
        File d = SD.open(line.substring(4));
        if (d)
            for (File f = d.openNextFile(); f; f = d.openNextFile()) Serial.printf("%s %u\n", f.name(), (unsigned)f.size());
        Serial.println("@END");
    } else if (line.startsWith("@PUT ")) {
        int    sp   = line.lastIndexOf(' ');
        String path = line.substring(5, sp);
        size_t size = line.substring(sp + 1).toInt(), done = 0;
        uint32_t sum = 0;
        mkdirs(path);
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.println("@ERR open");
            return false;
        }
        uint8_t* buf = (uint8_t*)malloc(4096);
        Serial.println("@READY");
        Serial.setTimeout(3000);
        while (done < size) {
            size_t want = size - done > 4096 ? 4096 : size - done;
            size_t got  = Serial.readBytes(buf, want);
            if (got != want) break;
            f.write(buf, got);
            for (size_t i = 0; i < got; i++) sum += buf[i];
            done += got;
            Serial.println("@K");
        }
        f.close();
        free(buf);
        Serial.printf(done == size ? "@DONE %lu\n" : "@ERR short %lu\n", (unsigned long)(done == size ? sum : done));
        tft.fillRect(0, 22, PW, 2, done == size ? TFT_GREEN : TFT_RED);  // a blink per file
    } else if (line == "@REFRESH") {
        Serial.println("@OK");
        return true;
    }
    return false;
}

// polls serial; sets x,y to the first row for '1', or to -1,-1 for a list refresh
bool serialService(int& x, int& y) {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (line.length() == 0 && c == '1') {
            x = 100;
            y = 30;
            return true;
        }
        if (c == '\n') {
            String l = line;
            line     = "";
            l.trim();
            if (sdReady && serialCommand(l)) {
                x = y = -1;
                return true;
            }
        } else if (c != '\r' && line.length() < 200) {
            line += c;
        }
    }
    return false;
}

// wait for a full tap, return its position (-1,-1: the PC asked for a refresh)
void waitTap(int& x, int& y) {
    while (touchAt(x, y)) delay(10);
    while (!touchAt(x, y)) {
        if (serialService(x, y)) return;
        delay(5);
    }
    int lx = x, ly = y;
    while (touchAt(x, y)) {
        lx = x;
        ly = y;
        delay(10);
    }
    x = lx;
    y = ly;
}
void message(const char* l1, const char* l2 = nullptr) {
    tft.fillScreen(TFT_BLACK);
    bar("EXTRAS");
    text(10, 120, l1, TFT_YELLOW);
    if (l2) text(10, 140, l2, TFT_LIGHTGREY);
    text(10, 290, "tap to continue", TFT_DARKGREY);
    int x, y;
    waitTap(x, y);
}

// ---------------------------------------------------------------- ROM storage
// The ROM is copied once into the "rom" flash partition and memory mapped, so
// the cores read it at flash-cache speed. The copy is skipped if it is already there.
bool flashHasRom(Kind& k, char* title) {
    const uint8_t* p;
    spi_flash_mmap_handle_t h;
    if (esp_partition_mmap(romPart, 0, 0x10000, SPI_FLASH_MMAP_DATA, (const void**)&p, &h) != ESP_OK) return false;
    k = NONE;
    static const uint8_t logo[4] = {0xCE, 0xED, 0x66, 0x66};
    if (!memcmp(p, "NES\x1a", 4)) {
        k = NES;
        strcpy(title, "NES game");
    } else if (!memcmp(p + 0x104, logo, 4)) {
        k = GB;
        memcpy(title, p + 0x134, 16);
        title[16] = 0;
    }
    spi_flash_munmap(h);
    String n = prefs.getString("rn", "");
    if (n.length()) strncpy(title, n.c_str() + (n.lastIndexOf('/') + 1), 30), title[30] = 0;
    return k != NONE;
}

bool loadToFlash(const Entry& e) {
    if (e.inFlash) {
        romSize = prefs.getUInt("rs", romPart->size);
    } else {
        File f = SD.open(e.path);
        if (!f) return false;
        size_t n = f.size();
        if (n > romPart->size) {
            message("Too big for this board:", "max 1 MB");
            return false;
        }
        if (prefs.getString("rn", "") != e.path || prefs.getUInt("rs", 0) != n) {
            tft.fillScreen(TFT_BLACK);
            bar("LOADING");
            text(10, 100, e.name.c_str(), TFT_WHITE);
            esp_partition_erase_range(romPart, 0, (n + 4095) & ~4095u);
            uint8_t* buf = (uint8_t*)malloc(4096);
            for (size_t off = 0; off < n; off += 4096) {
                int got = f.read(buf, 4096);
                if (got <= 0) break;
                esp_partition_write(romPart, off, buf, got);
                tft.fillRect(10, 140, (PW - 20) * (off + got) / n, 12, TFT_GREEN);
            }
            free(buf);
            prefs.putString("rn", e.path);
            prefs.putUInt("rs", n);
        }
        romSize = n;
        f.close();
    }
    romPath = prefs.getString("rn", "");
    return esp_partition_mmap(romPart, 0, (romSize + 0xFFFF) & ~0xFFFFu, SPI_FLASH_MMAP_DATA, (const void**)&rom,
                              &romMap) == ESP_OK;
}

// ---------------------------------------------------------------- touch pad
struct Btn {
    int     x, y, w, h;
    uint8_t bit;
    const char* label;
};
// portrait layout, controls under the picture like a handheld
constexpr int DX = 58, DY = 272, DR = 46;  // d-pad centre and reach
const Btn     btns[] = {
    {196, 252, 40, 40, GBK_A, "A"},     {148, 276, 40, 40, GBK_B, "B"},
    {104, 232, 36, 16, GBK_SELECT, "SEL"}, {104, 300, 36, 16, GBK_START, "STA"},
};

void drawPad() {
    tft.fillRect(0, PAD_Y, PW, PH - PAD_Y, c565(40, 40, 48));
    uint16_t dark = c565(20, 20, 24), light = c565(150, 150, 160);
    tft.fillRect(DX - 14, DY - 40, 28, 80, dark);
    tft.fillRect(DX - 40, DY - 14, 80, 28, dark);
    tft.fillTriangle(DX, DY - 34, DX - 6, DY - 24, DX + 6, DY - 24, light);
    tft.fillTriangle(DX, DY + 34, DX - 6, DY + 24, DX + 6, DY + 24, light);
    tft.fillTriangle(DX - 34, DY, DX - 24, DY - 6, DX - 24, DY + 6, light);
    tft.fillTriangle(DX + 34, DY, DX + 24, DY - 6, DX + 24, DY + 6, light);
    for (auto& b : btns) {
        bool round = b.bit == GBK_A || b.bit == GBK_B;
        if (round) tft.fillCircle(b.x + b.w / 2, b.y + b.h / 2, b.w / 2, c565(150, 30, 70));
        else tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, dark);
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(1);
        tft.drawString(b.label, b.x + b.w / 2 - (strlen(b.label) * 3), b.y + b.h / 2 - 4);
    }
    text(150, 232, "hold SEL: menu", c565(110, 110, 120), c565(40, 40, 48));
}

uint8_t readPad(bool& selHeld) {
    static uint32_t selSince = 0;
    int             x, y;
    uint8_t         k = 0;
    if (touchAt(x, y) && y >= PAD_Y - 8) {
        for (auto& b : btns)
            if (x >= b.x - 4 && x < b.x + b.w + 4 && y >= b.y - 4 && y < b.y + b.h + 4) k |= b.bit;
        int dx = x - DX, dy = y - DY;
        if (!k && dx * dx + dy * dy < (DR + 6) * (DR + 6) && dx * dx + dy * dy > 64) {
            // 8-way: a direction counts if it is at least half the other axis
            if (dx > 0 && dx * 2 > abs(dy)) k |= GBK_RIGHT;
            if (dx < 0 && -dx * 2 > abs(dy)) k |= GBK_LEFT;
            if (dy > 0 && dy * 2 > abs(dx)) k |= GBK_DOWN;
            if (dy < 0 && -dy * 2 > abs(dx)) k |= GBK_UP;
        }
    }
    if (!digitalRead(0)) {  // BOOT button opens the menu too
        selHeld = true;
        return 0;
    }
    if (k == GBK_SELECT) {
        if (!selSince) selSince = millis();
        if (millis() - selSince > 1200) {
            selSince = 0;
            selHeld  = true;
            return 0;
        }
    } else {
        selSince = 0;
    }
    selHeld = false;
    return k;
}

// returns 0 resume, 1 reset, 2 game list, 3 exit
enum MenuResult { M_RESUME, M_RESET, M_LIST, M_EXIT, M_SAVE, M_LOAD };
int pauseMenu() {
    static const char*     items[] = {"RESUME", "SAVE STATE", "LOAD STATE", "RESET", "GAME LIST", "FLIP SCREEN", "EXIT"};
    static const MenuResult res[]  = {M_RESUME, M_SAVE, M_LOAD, M_RESET, M_LIST, M_RESUME, M_EXIT};
    for (;;) {
        tft.fillRect(20, 24, 200, 222, c565(10, 30, 10));
        tft.drawRect(20, 24, 200, 222, TFT_GREEN);
        for (int i = 0; i < 7; i++) text(46, 34 + i * 30, items[i], i == 1 || i == 2 ? TFT_CYAN : TFT_GREEN, c565(10, 30, 10), 2);
        int x, y;
        waitTap(x, y);
        int i = (y - 28) / 30;
        if (x < 20 || x > 220 || y < 28 || i < 0 || i > 6) continue;
        if (i == 5) {
            portrait ^= 2;
            tft.setRotation(portrait);
            prefs.putUChar("p", portrait);
            tft.fillScreen(TFT_BLACK);
            drawPad();
            continue;
        }
        return res[i];
    }
}

// one save-state slot per game, next to the ROM on the SD card
String statePath() { return romPath.substring(0, romPath.lastIndexOf('.')) + ".st0"; }
bool   haveState() { return romPath.startsWith("/") && SD.exists(statePath()); }
void   toast(const char* s) {
    tft.fillRect(0, 100, PW, 24, TFT_DARKGREEN);
    text(10, 108, s, TFT_WHITE, TFT_DARKGREEN);
    delay(700);
}
// at launch: pick up where you left off, or start fresh
bool askContinue() {
    if (!haveState()) return false;
    tft.fillScreen(TFT_BLACK);
    bar("SAVED GAME FOUND");
    tft.drawRoundRect(10, 60, PW - 20, 60, 8, TFT_GREEN);
    text(40, 82, "CONTINUE", TFT_GREEN, TFT_BLACK, 2);
    tft.drawRoundRect(10, 150, PW - 20, 60, 8, TFT_DARKGREY);
    text(40, 172, "START OVER", TFT_LIGHTGREY, TFT_BLACK, 2);
    int x, y;
    for (;;) {
        waitTap(x, y);
        if (y >= 60 && y < 120) return true;
        if (y >= 150 && y < 210) return false;
    }
}

// recently played: newest first, kept in /roms/.recent
void addRecent(const String& path) {
    if (!path.startsWith("/")) return;
    std::vector<String> list{path};
    File f = SD.open("/roms/.recent");
    while (f && f.available() && list.size() < 20) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (l.length() && l != path) list.push_back(l);
    }
    if (f) f.close();
    f = SD.open("/roms/.recent", FILE_WRITE);
    if (!f) return;
    for (auto& l : list) f.println(l);
    f.close();
}

// ---------------------------------------------------------------- GB
uint16_t gbPal[12];
uint8_t  xmap[PW];
uint8_t* cartRam = nullptr;
size_t   cartRamSize = 0;

void gbLine(const uint8_t* px, int line) {
    if (line == 0) stripBegin();
    // 144 -> 216 rows: every other source line is doubled
    int y0 = line * 3 / 2, y1 = (line + 1) * 3 / 2;
    for (int y = y0; y < y1; y++) {
        uint16_t* o = stripRow();
        for (int x = 0; x < PW; x++) {
            uint8_t p = px[xmap[x]];
            o[x]      = gbPal[((p >> 4) & 3) * 4 + (p & 3)];
        }
        stripNext();
    }
    if (line == 143) stripEnd();
}

String savePath() { return romPath.substring(0, romPath.lastIndexOf('.')) + ".sav"; }
bool   gbSaveState() {
    if (!romPath.startsWith("/")) return false;
    File f = SD.open(statePath(), FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)"GBS1", 4);
    f.write((const uint8_t*)gbc_state_ptr(), gbc_state_size());
    if (cartRam) f.write(cartRam, cartRamSize);
    f.close();
    return true;
}
bool gbLoadState() {
    File f = SD.open(statePath());
    char m[4];
    if (!f || f.read((uint8_t*)m, 4) != 4 || memcmp(m, "GBS1", 4) || f.size() < 4 + gbc_state_size()) return false;
    gbc_keep keep;
    gbc_state_load_begin(&keep);
    f.read((uint8_t*)gbc_state_ptr(), gbc_state_size());
    gbc_state_load_end(&keep);
    if (cartRam) f.read(cartRam, cartRamSize);
    f.close();
    return true;
}
void   gbSave() {
    if (!cartRam || !romPath.startsWith("/")) return;
    File f = SD.open(savePath(), FILE_WRITE);
    if (f) {
        f.write(cartRam, cartRamSize);
        f.close();
    }
}

int runGB() {
    // classic pea green for every layer
    static const uint8_t g[4][3] = {{155, 188, 15}, {139, 172, 15}, {48, 98, 48}, {15, 56, 15}};
    for (int l = 0; l < 3; l++)
        for (int s = 0; s < 4; s++) gbPal[l * 4 + s] = c565(g[s][0], g[s][1], g[s][2]);
    for (int x = 0; x < PW; x++) xmap[x] = x * 2 / 3;
    if (!gbc_init(rom, gbLine)) {
        message("Not a Game Boy ROM", "(or a Color-only game)");
        return 2;
    }
    cartRamSize = gbc_save_size();
    if (cartRamSize) {
        cartRam = (uint8_t*)calloc(1, cartRamSize);
        File f  = SD.open(savePath());
        if (f) {
            f.read(cartRam, cartRamSize);
            f.close();
        }
        gbc_set_ram(cartRam);
    }
    bool resume = askContinue();
    tft.fillScreen(TFT_BLACK);
    drawPad();
    if (resume) gbLoadState();
    uint32_t next = micros(), lastSave = millis(), frames = 0, t0 = millis();
    int      behind = 0;
    for (;;) {
        bool    menu;
        uint8_t k = readPad(menu);
        if (menu) {
            gbSave();
            int r = pauseMenu();
            tft.fillScreen(TFT_BLACK);
            drawPad();
            if (r == M_SAVE) toast(gbSaveState() ? "STATE SAVED" : "SAVE FAILED");
            if (r == M_LOAD) toast(gbLoadState() ? "STATE LOADED" : "NO SAVED STATE");
            if (r == M_RESET) gbc_reset();
            if (r == M_LIST || r == M_EXIT) {
                gbSaveState();  // leaving: always resumable
                return r == M_LIST ? 2 : 3;
            }
            next = micros();
        }
        gbc_frame(k);
        frames++;
        // pace to 59.7 Hz; skip drawing alternate frames if we fall behind
        next += 16742;
        int32_t slack = (int32_t)(next - micros());
        if (slack > 0) delayMicroseconds(slack);
        else if (slack < -50000) next = micros();
        behind = slack < 0 ? behind + 1 : behind > 0 ? behind - 1 : 0;
        gbc_frame_skip(behind > 10);
        if (cartRam && millis() - lastSave > 30000 && gbc_ram_dirty()) {
            gbSave();
            lastSave = millis();
        }
        if (millis() - t0 > 5000) {
            Serial.printf("gb %.1f fps\n", frames * 1000.0f / (millis() - t0));
            frames = 0;
            t0     = millis();
        }
    }
}

// ---------------------------------------------------------------- NES
uint8_t  nx[PW];
int      nesPrevRow, nesResult, nesBehind;
uint32_t nesNext, nesFrames, nesT0;

void nesLine(int line, uint16_t* px) {
    // 240 -> 225 rows (drop every 16th), 256 -> 240 columns
    if (line == 0) {
        stripBegin();
        nesPrevRow = -1;
    }
    int row = line * 15 / 16;
    if (row != nesPrevRow && row < 225) {
        nesPrevRow  = row;
        uint16_t* o = stripRow();
        for (int x = 0; x < PW; x++) {
            uint16_t c = px[nx[x]];
            uint16_t v = ((c & 0x7C00) << 1) | ((c & 0x03E0) << 1) | ((c >> 4) & 0x20) | (c & 0x1F);
            o[x]       = (v >> 8) | (v << 8);
        }
        stripNext();
    }
    if (line == 239) stripEnd();
}

bool nesPendingLoad = false;
int  nesFrame() {
    if (nesPendingLoad) {  // first frame: the core is up, restore the saved game
        nesPendingLoad = false;
        String sp      = "/sd" + statePath();
        if (!nes_load_state(sp.c_str())) nes_reset();
    }
    bool    menu;
    uint8_t k = readPad(menu);
    if (menu) {
        if (inFrame) stripEnd();
        int r = pauseMenu();
        tft.fillScreen(TFT_BLACK);
        drawPad();
        String sp = "/sd" + statePath();
        if (r == M_SAVE) toast(romPath.startsWith("/") && nes_save_state(sp.c_str()) ? "STATE SAVED" : "SAVE FAILED");
        if (r == M_LOAD) {
            bool ok = haveState() && nes_load_state(sp.c_str());
            if (!ok && haveState()) nes_reset();  // a half-read state is worse than none
            toast(ok ? "STATE LOADED" : "NO SAVED STATE");
        }
        if (r == M_RESET) nes_reset();
        if (r == M_LIST || r == M_EXIT) {
            if (romPath.startsWith("/")) nes_save_state(sp.c_str());  // leaving: always resumable
            nesResult = r == M_LIST ? 2 : 3;
            return -1;
        }
        nesNext = micros();
    }
    nesFrames++;
    nesNext += 16639;  // 60.1 Hz
    int32_t slack = (int32_t)(nesNext - micros());
    if (slack > 0) delayMicroseconds(slack);
    else if (slack < -50000) nesNext = micros();
    nesBehind = slack < 0 ? nesBehind + 1 : nesBehind > 0 ? nesBehind - 1 : 0;
    FrameSkip = nesBehind > 10 ? 1 : 0;
    if (millis() - nesT0 > 5000) {
        Serial.printf("nes %.1f fps\n", nesFrames * 1000.0f / (millis() - nesT0));
        nesFrames = 0;
        nesT0     = millis();
    }
    return k;
}

int runNES() {
    for (int x = 0; x < PW; x++) nx[x] = x * 16 / 15;
    nesPendingLoad = askContinue();
    tft.fillScreen(TFT_BLACK);
    drawPad();
    nesResult = 2;
    nesBehind = 0;
    nesNext   = micros();
    nesT0     = millis();
    nesFrames = 0;
    const char* err = nes_run(rom, romSize, nesLine, nesFrame);
    if (inFrame) stripEnd();
    if (err) message("Can't run this game:", err);
    return nesResult;
}

// ---------------------------------------------------------------- game list
struct Cat {
    const char*         title;
    const char*         dir;
    std::vector<String> names;
};
Cat cats[] = {{"RECENTLY PLAYED", "", {}}, {"GB GAMES", "/roms/gb", {}}, {"GB DEMOS", "/roms/demos", {}},
              {"NES GAMES", "/roms/nes", {}}, {"OTHER", "/roms", {}}};
constexpr int NCATS = sizeof(cats) / sizeof(cats[0]);
// display name: file name without folder or extension
String shortName(const String& n) {
    String b = n.substring(n.lastIndexOf('/') + 1);
    return b.substring(0, b.lastIndexOf('.'));
}

bool lessNoCase(const String& a, const String& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

void scanCats() {
    // readdir straight from the VFS: SD.openNextFile() opens every file and
    // takes 10+ seconds on a big folder
    for (auto& c : cats) {
        c.names.clear();
        if (!*c.dir) {  // recently played: full paths, newest first, no sorting
            File f = SD.open("/roms/.recent");
            while (f && f.available()) {
                String l = f.readStringUntil('\n');
                l.trim();
                if (l.length() && SD.exists(l)) c.names.push_back(l);
            }
            continue;
        }
        DIR* d = opendir((String("/sd") + c.dir).c_str());
        if (!d) continue;
        while (dirent* e = readdir(d))
            if (e->d_type != DT_DIR && kindOf(e->d_name) != NONE) c.names.push_back(e->d_name);
        closedir(d);
        std::sort(c.names.begin(), c.names.end(), lessNoCase);
    }
}

// A-Z picker: returns the index of the first name at or after the tapped letter
int letterJump(const Cat& c) {
    tft.fillScreen(TFT_BLACK);
    bar("JUMP TO");
    const char* keys = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 0; i < 27; i++) {
        int  x = (i % 6) * 40, y = 28 + (i / 6) * 44;
        char s[2] = {keys[i], 0};
        tft.drawRect(x + 2, y + 2, 36, 40, TFT_DARKGREEN);
        text(x + 14, y + 16, s, TFT_GREEN, TFT_BLACK, 2);
    }
    int x, y;
    waitTap(x, y);
    int i = x / 40 + ((y - 28) / 44) * 6;
    if (y < 28 || i < 0 || i > 26) return -1;
    if (i == 0) return 0;
    for (int n = 0; n < (int)c.names.size(); n++)
        if (toupper(c.names[n][0]) >= keys[i]) return n;
    return c.names.size() - 1;
}

// list one category; returns true with out filled, false for "back"
bool pickFrom(const Cat& c, Entry& out) {
    constexpr int PER = 14;
    int           page = 0, pages = (c.names.size() + PER - 1) / PER;
    for (;;) {
        tft.fillScreen(TFT_BLACK);
        char t[40];
        snprintf(t, sizeof t, "%s  %d/%d", c.title, page + 1, pages);
        bar(t);
        for (int i = 0; i < PER && page * PER + i < (int)c.names.size(); i++) {
            String n = shortName(c.names[page * PER + i]).substring(0, 37);
            text(6, 27 + i * 19, n.c_str(), i & 1 ? TFT_WHITE : TFT_LIGHTGREY);
        }
        tft.fillRect(0, 296, PW, 24, TFT_DARKGREY);
        text(14, 304, "<", TFT_WHITE, TFT_DARKGREY, 1);
        text(72, 304, "A-Z", TFT_WHITE, TFT_DARKGREY, 1);
        text(140, 304, ">", TFT_WHITE, TFT_DARKGREY, 1);
        text(190, 304, "BACK", TFT_YELLOW, TFT_DARKGREY, 1);
        int x, y;
        waitTap(x, y);
        if (x < 0) return false;  // refresh from the PC: rebuild from the top
        if (y >= 296) {
            if (x < 50) page = (page + pages - 1) % pages;
            else if (x < 120) {
                int n = letterJump(c);
                if (n >= 0) page = n / PER;
            } else if (x < 175) page = (page + 1) % pages;
            else return false;
            continue;
        }
        int i = (y - 24) / 19;
        if (y >= 24 && i >= 0 && i < PER && page * PER + i < (int)c.names.size()) {
            const String& n = c.names[page * PER + i];
            out             = {shortName(n), *c.dir ? String(c.dir) + "/" + n : n, kindOf(n), false};
            return true;
        }
    }
}

bool pickGame(Entry& out, bool sdOk) {
    for (;;) {
        if (sdOk) scanCats();
        Kind fk;
        char title[32];
        bool inFlash = flashHasRom(fk, title);
        // home screen: categories, the game still in flash, exit
        struct Row {
            String label;
            int    cat;  // -1 flash, -2 exit
        };
        std::vector<Row> rows;
        for (int i = 0; i < NCATS; i++)
            if (cats[i].names.size()) rows.push_back({String(cats[i].title) + "  (" + cats[i].names.size() + ")", i});
        if (inFlash) rows.push_back({String("RESUME: ") + title, -1});
        rows.push_back({"EXIT", -2});
        tft.fillScreen(TFT_BLACK);
        bar(sdOk ? "SELECT" : "SELECT (no SD card)");
        for (int i = 0; i < (int)rows.size(); i++) {
            tft.drawRoundRect(8, 30 + i * 44, PW - 16, 38, 6, TFT_DARKGREEN);
            text(18, 42 + i * 44, rows[i].label.substring(0, 34).c_str(), rows[i].cat == -2 ? TFT_YELLOW : TFT_GREEN);
        }
        if (rows.size() == 1) text(10, 250, "Put games in /roms/gb, /roms/nes", TFT_LIGHTGREY);
        int x, y;
        waitTap(x, y);
        if (x < 0) continue;
        int i = (y - 30) / 44;
        if (y < 30 || i < 0 || i >= (int)rows.size()) continue;
        if (rows[i].cat == -2) return false;
        if (rows[i].cat == -1) {
            out = {title, "", fk, true};
            return true;
        }
        if (pickFrom(cats[rows[i].cat], out)) return true;
    }
}

}  // namespace

// Never returns: leaving restarts into the demos.
void extrasRun(uint8_t landscapeRotation) {
    prefs.begin("demoscene");
    portrait = prefs.getUChar("p", landscapeRotation == 1 ? 0 : 2);
    tft.setRotation(portrait);  // the panel is already up; just turn it upright
    for (auto& s : strip) s = (uint16_t*)heap_caps_malloc(PW * ROWS * 2, MALLOC_CAP_DMA);
    romPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "rom");
    if (!romPart) {
        message("No rom partition", "reflash with the current partitions.csv");
        ESP.restart();
    }
    // the cached-ROM bookkeeping is only valid for the partition it was written to
    if (prefs.getUInt("ro", 0) != romPart->address) {
        prefs.remove("rn");
        prefs.remove("rs");
        prefs.putUInt("ro", romPart->address);
    }
    sdSpi.begin(18, 19, 23, 5);
    bool sdOk = SD.begin(5, sdSpi, 20000000);
    sdReady   = sdOk;
    Serial.printf("extras: sd %d, rotation %d, heap %u (max block %u)\n", sdOk, portrait, ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    for (;;) {
        Entry e;
        if (!pickGame(e, sdOk)) ESP.restart();
        if (!loadToFlash(e)) continue;
        addRecent(romPath);
        int r = e.kind == GB ? runGB() : runNES();
        if (rom) {
            spi_flash_munmap(romMap);
            rom = nullptr;
        }
        free(cartRam);
        cartRam = nullptr;
        if (r == 3) ESP.restart();
    }
}
