// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// CYD Demoscene: a pile of classic demo effects and an ANSI art BBS player
// for the ESP32-2432S028R "Cheap Yellow Display".
//
// Engine: core 1 runs the demo's update() into one 8-bit buffer while core 0
// expands the previous frame through its palette and DMAs it to the panel.
// The built-in song (audio.cpp) is the master clock for beat sync.
//
// Touch: swipe left/right = next/previous (pieces inside the ANSI gallery),
// tap = hold on this one / resume auto-play, drag up/down = scroll long ANSI,
// press and hold 1 s = skip to the next demo. BOOT button = next demo.
// Serial (921600): n/p next/prev, h hold, r rotate 180, l list, g<N> go,
// i info, m mute, f SPI clock (40/55/80 MHz, reboots), R software screenshot.
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <dirent.h>
#include <sys/stat.h>
#include "board.h"
#include "cellpet/petmain.h"
#include "demo.h"
#include "net.h"

void extrasArm();
void extrasRun(uint8_t landscapeRotation);

static Demo* (*const factories[])() = {
    makeAnsiExplosion, makeRotatoCAM,   makePlasma,      makeAnsiGallery, makeFire,      makeTunnel,     makeCopperScroller,
    makeStarfield,     makeRotozoom,    makeAnsiGallery, makeMetaballs, makeVoxel,      makeGlenz,
    makeMatrix,        makeTwister,     makeAnsiGallery, makeKefrens,   makeRaycaster,  makeVectorBobs,
    makeWater,         makeMandelbrot,  makeLife,        makeRotatoBBS,   makeRaymarch,    makeSecondReality, makeAcidWarp,
    makeRotatoRotary,  makeTouchFinale, makeAnsiClock,
};
static constexpr int NDEMOS = sizeof(factories) / sizeof(factories[0]);
static const char*   demoNames[NDEMOS];  // captured once at boot (building demos later can run out of memory)

bool holdMode = false;
static bool touchLog = false;
bool clockBetween = true;
int  stayMode     = 0;

static uint8_t*          bufs[2];
static int               back = 0;
static uint16_t*         sbuf[2];
static Demo*             cur;
static int               curIdx  = 0;
static uint32_t          startMs = 0, flashUntil = 0;
static uint8_t           rotation;
static uint32_t          spiMHz;
static Preferences       prefs;
static float             fps;
static SemaphoreHandle_t semReady, semDone;
static volatile uint32_t pushUs = 0, updateUs = 0, frames = 0;
extern volatile uint32_t audioBusyUs;

// the frame core 0 is pushing
static Res            frameRes;
static const uint8_t* frameBuf;
static uint16_t       framePal[256];

// on-screen message, drawn by the push task over any demo
static char     osd[32];
static uint32_t osdUntil = 0;
static void     say(const char* s) {
    strncpy(osd, s, sizeof(osd) - 1);
    osdUntil = millis() + 1200;
    Serial.println(s);
}

static void startDemo(int idx) {
    if (cur) {
        cur->end();
        delete cur;
    }
    curIdx = (idx % NDEMOS + NDEMOS) % NDEMOS;
    memset(bufs[0], 0, SW * SH);
    memset(bufs[1], 0, LSIZE);
    back = 0;
    fb   = bufs[0];
    memset(pal, 0, sizeof(pal));
    arenaReset();
    cur = factories[curIdx]();
    cur->begin();
    startMs    = millis();
    flashUntil = startMs + 300;
    Serial.printf("[%d] %s  heap %u (max block %u)\n", curIdx, cur->name(), ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void expandStrip(Res r, const uint8_t* buf, const uint16_t* p, int y0, uint16_t* out) {
    if (r == Res::High) {
        const uint8_t* src = buf + y0 * SW;
        for (int i = 0; i < SW * STRIP; i++) out[i] = p[src[i]];
        return;
    }
    for (int y = 0; y < STRIP; y += 2) {
        const uint8_t* src = buf + ((y0 + y) >> 1) * LW;
        uint16_t*      o   = out + y * SW;
        for (int x = 0; x < LW; x++) o[2 * x] = o[2 * x + 1] = p[src[x]];
        memcpy(o + SW, o, SW * 2);
    }
}

// white text with a dark shadow, centred near the top
static void overlayOSD(int y0, uint16_t* out) {
    int len = strlen(osd), x0 = (SW - len * 8) / 2, ty = 8;
    if (y0 + STRIP <= ty - 1 || y0 > ty + 17) return;
    uint16_t white = rgb(255, 255, 255), shade = rgb(0, 0, 0);
    for (int y = y0; y < y0 + STRIP; y++) {
        uint16_t* o = out + (y - y0) * SW;
        for (int x = x0 - 4; x < x0 + len * 8 + 4; x++)
            if (x >= 0 && x < SW && y >= ty - 2 && y < ty + 18) o[x] = shade;
        int gr = y - ty;
        if (gr < 0 || gr >= 16) continue;
        for (int i = 0; i < len; i++) {
            uint8_t bits = font16[(uint8_t)osd[i] * 16 + gr];
            for (int b = 0; b < 8; b++)
                if (bits & (0x80 >> b)) o[x0 + i * 8 + b] = white;
        }
    }
}

static void pushFrame() {
    bool showOsd = millis() < osdUntil;
    tft.startWrite();
    for (int y = 0, i = 0; y < SH; y += STRIP, i ^= 1) {
        if (frameRes == Res::Custom)
            cur->strip(y, STRIP, sbuf[i]);
        else
            expandStrip(frameRes, frameBuf, framePal, y, sbuf[i]);
        if (showOsd) overlayOSD(y, sbuf[i]);
        tft.pushImageDMA(0, y, SW, STRIP, (const lgfx::swap565_t*)sbuf[i]);
    }
    tft.endWrite();
}

static void pushTask(void*) {
    for (;;) {
        xSemaphoreTake(semReady, portMAX_DELAY);
        uint32_t t0 = micros();
        pushFrame();
        pushUs += micros() - t0;
        xSemaphoreGive(semDone);
    }
}

static inline uint16_t towardWhite(uint16_t c, int k) {
    c     = (c >> 8) | (c << 8);
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r += ((31 - r) * k) >> 8;
    g += ((63 - g) * k) >> 8;
    b += ((31 - b) * k) >> 8;
    c = (r << 11) | (g << 5) | b;
    return (c >> 8) | (c << 8);
}

// Software screenshot: "SHOT" w h, then RGB888 rows of the last pushed frame.
static void renderShot() {
    static uint8_t line[SW * 3];
    Serial.write("SHOT", 4);
    uint16_t wh[2] = {SW, SH};
    Serial.write((uint8_t*)wh, 4);
    for (int y = 0; y < SH; y += STRIP) {
        // what the current demo draws now (the last pushed frame may still be the previous demo's)
        if (cur->res() == Res::Custom) cur->strip(y, STRIP, sbuf[0]);
        else expandStrip(cur->res(), fb, pal, y, sbuf[0]);
        for (int k = 0; k < STRIP; k++) {
            for (int x = 0; x < SW; x++) {
                uint16_t c      = sbuf[0][k * SW + x];
                c               = (c >> 8) | (c << 8);
                line[x * 3]     = (c >> 8) & 0xF8;
                line[x * 3 + 1] = (c >> 3) & 0xFC;
                line[x * 3 + 2] = (c << 3) & 0xF8;
            }
            Serial.write(line, sizeof(line));
        }
    }
    Serial.flush();
}

static bool switched = false;
static void go(int idx) {
    startDemo(idx);
    switched = true;
}
static void swipe(int dir) {
    if (!cur->swipe(dir)) go(curIdx + dir);
}
static void toggleHold() {
    holdMode = !holdMode;
    say(holdMode ? "HOLD" : "AUTO PLAY");
}

// "@PUT path size": the file follows in 512 byte chunks, each acked with @K, then
// @DONE with a byte sum. Copies the ANSI art onto the SD card over USB.
static void sdPut(const String& args) {
    int    sp   = args.lastIndexOf(' ');
    String path = args.substring(0, sp);
    size_t size = args.substring(sp + 1).toInt(), done = 0;
    uint32_t sum = 0;
    if (!clockSdMount() || sp < 1) {
        Serial.println("@ERR no sd");
        return;
    }
    SD.mkdir(path.substring(0, path.lastIndexOf('/')));
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("@ERR open");
        return;
    }
    static uint8_t blk[512];  // the heap is in small pieces while the clock runs
    Serial.println("@READY");
    Serial.setTimeout(3000);
    while (done < size) {
        size_t want = min(size - done, sizeof blk);
        if (Serial.readBytes(blk, want) != want) break;
        f.write(blk, want);
        for (size_t i = 0; i < want; i++) sum += blk[i];
        done += want;
        Serial.println("@K");  // the PC waits for this, so the 256 byte RX buffer never overflows
    }
    f.close();
    Serial.printf(done == size ? "@DONE %lu\n" : "@ERR short %lu\n", (unsigned long)(done == size ? sum : done));
}

static void handleSerial() {
    static char buf[16];
    static int  n = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (n == 0) {
            switch (c) {
                case 'n': swipe(1); continue;
                case 'p': swipe(-1); continue;
                case 'h': toggleHold(); continue;
                case 'm':
                    audioMute(!audioMuted());
                    prefs.putBool("mute", audioMuted());
                    say(audioMuted() ? "SOUND OFF" : "SOUND ON");
                    continue;
                case 'r':
                    rotation ^= 2;
                    tft.setRotation(rotation);
                    prefs.putUChar("rot", rotation);
                    continue;
                case 'f':
                    spiMHz = spiMHz == 40 ? 55 : spiMHz == 55 ? 80 : 40;
                    prefs.putUInt("spi", spiMHz);
                    Serial.printf("SPI %lu MHz, rebooting\n", (unsigned long)spiMHz);
                    delay(100);
                    ESP.restart();
                    continue;
                case 'R': renderShot(); continue;
                case 't': touchLog = !touchLog; continue;
                case 'c': {  // force the clock style on its next appearance: -1 (random), 0, 1, 2
                    extern int clockStyleForce;
                    clockStyleForce = clockStyleForce >= 2 ? -1 : clockStyleForce + 1;
                    continue;
                }
                case 'b': {  // force the clock backdrop on its next appearance: -1 (random), 0..5
                    extern int clockBgForce;
                    clockBgForce = clockBgForce >= 5 ? -1 : clockBgForce + 1;
                    continue;
                }
                case 'x': {  // force the matrix clock effect: -1 (saved setting), 0 off .. 5
                    extern int clockFxForce;
                    clockFxForce = clockFxForce >= 5 ? -1 : clockFxForce + 1;
                    continue;
                }
                case 'z': {  // matrix clock: a Sentinel sweep right now
                    extern bool clockSentinelNow;
                    clockSentinelNow = true;
                    continue;
                }
                case 'K': extrasArm(); continue;
                case 'l':
                    for (int i = 0; i < NDEMOS; i++) Serial.printf("%2d %s\n", i, demoNames[i]);
                    continue;
                case 'i': {
                    Sync s = syncNow();
                    static uint32_t lastMs = 0;
                    uint32_t        span   = millis() - lastMs;
                    lastMs                 = millis();
                    if (frames)
                        Serial.printf("  per frame: update %lu us, push %lu us; audio %lu%% of core 0\n",
                                      (unsigned long)(updateUs / frames), (unsigned long)(pushUs / frames),
                                      (unsigned long)(audioBusyUs / 10 / (span ? span : 1)));
                    updateUs = pushUs = frames = audioBusyUs = 0;
                    Serial.printf("[%d] %s  %.1f fps  heap %u  hold %d  SPI %lu  bar %d beat %.2f kick %.2f\n", curIdx,
                                  cur->name(), fps, ESP.getFreeHeap(), holdMode, (unsigned long)spiMHz, s.bar, s.beats, s.kick);
                    continue;
                }
                case 'g': buf[n++] = c; continue;
                case '@': {  // "@WIFI ssid<TAB>password" from tools/wifi_set.py, "@WIFIFORGET"
                    String l = Serial.readStringUntil('\n');
                    l.trim();
                    if (l.startsWith("WIFI ")) {
                        int tab = l.indexOf('\t');
                        netAddNetwork(l.substring(5, tab < 0 ? l.length() : tab), tab < 0 ? "" : l.substring(tab + 1));
                        Serial.println("@OK wifi saved, restart to sync");
                    } else if (l.startsWith("ZIP")) {  // "@ZIP 12345", or "@ZIP" alone: weather off
                        String z = l.substring(3);
                        z.trim();
                        netSetZip(z);
                        Serial.println(z.length() ? "@OK zip saved, restart to fetch the weather" : "@OK weather off");
                    } else if (l.startsWith("PUT ")) {  // "@PUT /ansi/x.cg size" from tools/sd_ansi.py
                        sdPut(l.substring(4));
                    } else if (l.startsWith("RM ")) {  // "@RM /path": delete one file on the card
                        Serial.println(clockSdMount() && SD.remove(l.substring(3)) ? "@OK" : "@ERR");
                    } else if (l.startsWith("LS ")) {  // "@LS /dir": names and sizes, then @END
                        if (clockSdMount())
                            if (DIR* d = opendir(("/sd" + l.substring(3)).c_str())) {
                                while (dirent* e = readdir(d)) {
                                    struct stat st;
                                    String     p = "/sd" + l.substring(3) + "/" + e->d_name;
                                    Serial.printf("%s %ld\n", e->d_name, stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1L);
                                }
                                closedir(d);
                            }
                        Serial.println("@END");
                    } else if (l.startsWith("WIFIDEL ")) {  // "@WIFIDEL ssid": forget one saved network
                        netForgetNetwork(l.substring(8));
                        Serial.println("@OK forgotten");
                    } else if (l.startsWith("STAY ")) {  // "@STAY 0|1|2": auto-play, clock, Cell Pet (as in the clock settings)
                        stayMode = l.substring(5).toInt() % 3;
                        Preferences c;
                        c.begin("clock");
                        c.putUChar("stayto", stayMode);
                        c.putBool("stay", stayMode == 1);
                        c.end();
                        Serial.println("@OK stay saved");
                        if (stayMode == 2) delay(50), ESP.restart();  // Cell Pet runs on its own: restart into it
                    } else if (l == "WIFIKBD") {  // the touch keyboard setup, as from the clock menu
                        netRequestKeyboard();
                    } else if (l == "WIFIFORGET") {
                        netForget();
                        Serial.println("@OK wifi forgotten");
                    }
                    continue;
                }
                default: continue;
            }
        }
        if (c == '\n' || c == '\r' || n == sizeof(buf) - 1) {
            buf[n] = 0;
            n      = 0;
            go(atoi(buf + 1));
        } else {
            buf[n++] = c;
        }
    }
}

// Gestures from the resistive touch panel. Release is debounced because the
// XPT2046 drops out for a sample now and then. Interactive demos get the raw
// finger instead; there only a swipe that starts at the screen edge changes demo.
TouchState touch;

// Swipe/tap sequence matcher. Returns true when the input belongs to a
// sequence in progress, so it should not also act as a normal gesture.
static bool seqInput(char c) {
    static const char* seq = "UUDDLRLRTT";
    static int         pos = 0;
    static uint32_t    lastMs = 0;
    if (millis() - lastMs > 2500) pos = 0;
    lastMs = millis();
    if (c == seq[pos]) pos++;
    else if (pos == 2 && c == 'U') pos = 2;
    else pos = c == seq[0] ? 1 : 0;
    if (pos == 10) {
        pos = 0;
        extrasArm();
    }
    return pos > 4 || (pos == 4 && c == 'D');
}

static void handleTouch() {
    static bool     down = false, dragging = false, longDone = false, edge = false;
    static int      sx, sy, lx, ly, upCount = 0;
    static uint32_t t0;
    int             x, y;
    uint32_t        now   = millis();
    bool            inter = cur->interactive();
    static uint32_t lastLog = 0;
    if (boardTouch(x, y)) {
        if (touchLog && now - lastLog > 120) {  // serial 't' toggles: where the panel thinks the finger is
            lastLog = now;
            Serial.printf("touch %d,%d\n", x, y);
        }
        upCount     = 0;
        touch.vx    = down ? x - lx : 0;
        touch.vy    = down ? y - ly : 0;
        touch.x     = x;
        touch.y     = y;
        touch.lastMs = now;
        if (!down) {
            down = true;
            dragging = longDone = false;
            edge                = x < 24 || x >= SW - 24;
            sx = lx = x;
            sy = ly = y;
            t0      = now;
            touch.down    = !(inter && edge);
            touch.pressed = touch.down;
            return;
        }
        if (!inter) {
            int ady = abs(y - sy), adx = abs(x - sx);
            if (!dragging && ady > 14 && ady > adx) dragging = cur->drag(0);
            if (dragging) cur->drag(y - ly);
            if (!dragging && !longDone && adx < 15 && ady < 15 && now - t0 > 1000) {
                longDone = true;
                say("NEXT DEMO");
                go(curIdx + 1);
            }
        }
        lx = x;
        ly = y;
        return;
    }
    if (!down || ++upCount < 3) return;
    down       = false;
    touch.down = false;
    touch.vx = touch.vy = 0;
    int dx = lx - sx, dy = ly - sy;
    if (inter) {
        if (edge && abs(dx) > 50 && abs(dx) > abs(dy)) swipe(dx < 0 ? 1 : -1);
        return;
    }
    if (longDone) return;
    char g = abs(dy) > 50 && abs(dy) > abs(dx) * 3 / 2   ? (dy < 0 ? 'U' : 'D')
             : abs(dx) > 50 && abs(dx) > abs(dy) * 3 / 2 ? (dx < 0 ? 'L' : 'R')
             : abs(dx) < 20 && abs(dy) < 20 && now - t0 < 700 ? 'T'
                                                              : 0;
    if (g && seqInput(g)) return;
    if (dragging) return;
    if (abs(dx) > 50 && abs(dx) > abs(dy) * 3 / 2) {
        Serial.printf("swipe %s\n", dx < 0 ? "left" : "right");
        swipe(dx < 0 ? 1 : -1);
    } else if (abs(dx) < 20 && abs(dy) < 20 && now - t0 < 700) {
        // double tap: straight to the clock (the first tap's hold toggle is undone)
        static uint32_t lastTap = 0;
        if (now - lastTap < 450) {
            lastTap = 0;
            holdMode = !holdMode;
            for (int i = 0; i < NDEMOS; i++)
                if (factories[i] == makeAnsiClock) go(i);
            return;
        }
        lastTap = now;
        toggleHold();
    }
}

void setup() {
    Serial.begin(921600);
    pinMode(0, INPUT_PULLUP);
    for (int p : {4, 16, 17}) {  // RGB LED is active low, keep it dark
        pinMode(p, OUTPUT);
        digitalWrite(p, HIGH);
    }
    prefs.begin("demoscene");
    rotation = prefs.getUChar("rot", 1);
    spiMHz   = prefs.getUInt("spi", 55);
    const char* panel = boardInit(rotation, spiMHz * 1000000);
    // extras: armed by the swipe code (one shot, so a reset always comes back
    // here), or a finger already on the screen at power-on
    bool held = false;
    for (int i = 0; i < 4 && !held; i++) {
        int x, y;
        held = boardTouch(x, y);
        delay(30);
    }
    if (prefs.getUChar("x", 0) || held) {
        prefs.putUChar("x", 0);
        extrasRun(rotation);
    }
    netPortalIfRequested();  // WiFi setup hotspot, if the clock asked for it
    // STAY ON: CELL PET hands the whole board to Cell Pet, before the demos take its memory
    if (clockStaysOn() == 2) cp::petMain();
    // grab the frame buffers before anything fragments the heap: one full
    // 320x240 buffer, plus a low-res-sized second one for double buffering
    bufs[0] = (uint8_t*)heap_caps_malloc(SW * SH, MALLOC_CAP_8BIT);
    bufs[1] = (uint8_t*)heap_caps_malloc(LSIZE, MALLOC_CAP_8BIT);
    fbBase = bufs[0];
    for (auto& b : sbuf) b = (uint16_t*)heap_caps_malloc(SW * STRIP * 2, MALLOC_CAP_DMA);
    if (!bufs[0] || !bufs[1] || !sbuf[0] || !sbuf[1]) Serial.println("out of memory for frame buffers");
    netSyncAtBoot();  // after the frame buffers, so WiFi can't fragment the space they need
    size_t arenaBytes = arenaInit(40 * 1024);
    Serial.printf("arena %u bytes, heap left %u (max block %u)\n", arenaBytes, ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    clockSdMount();  // early, while the heap is still in one piece
    for (int i = 0; i < NDEMOS; i++) {  // names are string literals, safe to keep
        arenaReset();
        Demo* d      = factories[i]();
        demoNames[i] = d->name();
        delete d;
    }
    gfxInit();
    audioInit();  // claims only GPIO26 (DAC); the touch clock on GPIO25 stays untouched
    audioMute(prefs.getBool("mute", true));
    if (!LittleFS.begin(false)) Serial.println("LittleFS mount failed (run: pio run -t uploadfs)");
    scanAnsi();
    Serial.printf("\nCYD Demoscene, panel %s @ %lu MHz, %d demos\n", panel, (unsigned long)spiMHz, NDEMOS);
    disableCore0WDT();  // the push task keeps core 0 busy when it is the bottleneck
    semReady = xSemaphoreCreateBinary();
    semDone  = xSemaphoreCreateBinary();
    xSemaphoreGive(semDone);
    if (xTaskCreatePinnedToCore(pushTask, "push", 4096, nullptr, 3, nullptr, 0) != pdPASS)
        Serial.println("no memory for the display task");  // would hang on the first frame
    {
        Preferences c;
        c.begin("clock", true);
        clockBetween = c.getBool("between", true);
        c.end();
    }
    int first = 0;
    stayMode = clockStaysOn();  // a desk clock boots straight to it
    for (int i = 0; i < NDEMOS; i++)
        if (stayMode == 1 && factories[i] == makeAnsiClock) first = i;
    startDemo(first);
}

void loop() {
    static uint32_t lastFrame = 0;
    static int      lastRow   = 0;
    uint32_t        now       = millis();
    Sync            s         = syncNow();
    uint32_t u0 = micros();
    cur->update(now - startMs);  // overlaps with core 0 pushing the previous frame
    updateUs += micros() - u0;
    frames++;
    xSemaphoreTake(semDone, portMAX_DELAY);

    handleSerial();
    handleTouch();
    static bool bootPrev = true;
    bool        boot     = digitalRead(0);
    if (!boot && bootPrev) go(curIdx + 1);
    bootPrev = boot;
    // auto-advance lands on a bar line so cuts hit the beat
    uint32_t el = millis() - startMs;
    // someone playing with an interactive scene keeps it on screen until they stop for 15 s
    bool playing = cur->interactive() && touch.lastMs && millis() - touch.lastMs < 15000;
    int  clockIdx = -1;
    for (int i = 0; i < NDEMOS; i++)
        if (factories[i] == makeAnsiClock) clockIdx = i;
    bool     onClock = curIdx == clockIdx;
    uint32_t limit   = cur->duration();
    if (clockBetween && limit != 0xFFFFFFFF) limit = onClock ? 10000 : min<uint32_t>(limit, 30000);
    if (!switched && (cur->finished() || (!holdMode && !playing && el > limit && s.row == 0 && lastRow != 0))) {
        if (!clockBetween) go(curIdx + 1);
        else if (!onClock) go(clockIdx);
        else {  // a random demo that isn't the clock or the one before it
            static int prev = -1;
            int        n;
            do n = esp_random() % NDEMOS;
            while (n == clockIdx || n == prev);
            prev = n;
            go(n);
        }
    }
    lastRow = s.row;
    if (switched) {
        switched = false;
        xSemaphoreGive(semDone);
        return;
    }

    // hand the frame to core 0, with the beat flash folded into its palette
    frameRes = cur->res();
    frameBuf = fb;
    if (frameRes != Res::Custom) {
        int k = (int)(cur->pulse() * s.kick * 255);
        if (now < flashUntil) k += (flashUntil - now) * 200 / 300;  // cut flash
        if (k > 255) k = 255;
        if (k > 0)
            for (int i = 0; i < 256; i++) framePal[i] = towardWhite(pal[i], k);
        else
            memcpy(framePal, pal, sizeof(pal));
    }
    xSemaphoreGive(semReady);
    if (frameRes != Res::Low) {
        // Custom: strip() reads live demo state. High: only one full-size
        // buffer fits next to WiFi, so draw and push take turns.
        xSemaphoreTake(semDone, portMAX_DELAY);
        xSemaphoreGive(semDone);
    } else {
        // low-res: the next frame draws into the other buffer, starting from this one
        int other = back ^ 1;
        memcpy(bufs[other], fb, LSIZE);
        back = other;
        fb   = bufs[back];
    }
    now = millis();
    if (now != lastFrame) fps = fps * 0.9f + 100.0f / (now - lastFrame);
    lastFrame = now;
}
