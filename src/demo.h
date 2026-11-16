// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Every effect is a Demo. The main loop calls update() once per frame, then
// strip() for each 16-line band of the screen, which is DMA'd to the panel.
#pragma once
#include <FS.h>
#include <Arduino.h>
#include "gfx.h"
#include "audio.h"
#include <stdlib.h>

enum class Res { Low, High, Custom };

extern bool holdMode;

// Raw finger for interactive demos (screen pixels, 320x240).
struct TouchState {
    bool     down = false, pressed = false;  // pressed: set on touch-down, demos clear it
    int      x = 0, y = 0, vx = 0, vy = 0;   // position and per-frame motion
    uint32_t lastMs = 0;                     // last time anyone touched the screen
};
extern TouchState touch;
void        scanAnsi();  // index the ANSI art once at boot (SD /ansi, else LittleFS /ansi)  // user tapped "hold": stay on this demo / piece

struct Demo {
    // demo objects live in the per-demo arena (heap only as a fallback), so
    // switching demos all day never fragments the heap
    static void* operator new(size_t n) {
        void* p = arenaAlloc(n);
        return p ? p : malloc(n);
    }
    static void operator delete(void* p) {
        if (!inArena(p)) free(p);
    }
    virtual ~Demo() {}
    virtual const char* name() const = 0;
    virtual Res         res() const { return Res::Low; }
    virtual void        begin() {}
    virtual void        end() {}
    // t = milliseconds since begin()
    virtual void update(uint32_t t) = 0;
    // only called for Res::Custom; Low/High are expanded from fb + pal
    virtual void strip(int y0, int h, uint16_t* out) {}
    // how long auto-advance lets it run, and whether it wants to stop early
    virtual uint32_t duration() const { return 25000; }
    virtual bool     finished() const { return false; }
    // swipe: dir +1 = next, -1 = previous. Return true if the demo used it
    // (the ANSI gallery flips pieces); otherwise the engine changes demo.
    virtual bool swipe(int dir) { return false; }
    // vertical finger drag in pixels (positive = finger moved down)
    virtual bool drag(int dy) { return false; }
    // true: touches go to the effect (edge swipes still change demo)
    virtual bool interactive() const { return false; }
    // how much the palette brightens on each kick (0 = no beat flash)
    virtual float pulse() const { return 0.12f; }
};

// factories, one per effect (defined across demos_*.cpp)
Demo* makeAnsiExplosion();
Demo* makeRotatoBBS();
Demo* makeAnsiGallery();
Demo* makeMatrix();
Demo* makePlasma();
Demo* makeFire();
Demo* makeTunnel();
Demo* makeRotozoom();
Demo* makeStarfield();
Demo* makeCopperScroller();
Demo* makeMetaballs();
Demo* makeTwister();
Demo* makeKefrens();
Demo* makeVoxel();
Demo* makeGlenz();
Demo* makeVectorBobs();
Demo* makeRaycaster();
Demo* makeWater();
Demo* makeLife();
Demo* makeMandelbrot();
Demo* makeTouchFinale();
Demo* makeAnsiClock();
Demo* makeRotatoCAM();
Demo* makeRotatoRotary();
Demo* makeRaymarch();
Demo* makeSecondReality();
Demo* makeAcidWarp();
int   clockStaysOn();   // 0 auto-play, 1 boots to and stays on the clock, 2 boots into Cell Pet (src/cellpet)
extern int stayMode;  // the same, kept current by the clock settings
bool  clockSdMount();  // mounts the SD card for the clock's fonts (once)
extern bool clockBetween;  // auto-play alternates: random demo 30 s, clock 10 s
int    ansiCount();        // ANSI gallery pieces (SD card, or the small set in LittleFS)
// one ANSI piece (.cg): a whole file, or its slice of the SD card's /ansi.pak
struct AnsiFile {
    File     f;
    uint32_t base     = 0;
    char     name[32] = {};
    explicit operator bool() { return (bool)f; }
    int      read(void* p, size_t n) { return f.read((uint8_t*)p, n); }
    bool     seek(uint32_t pos) { return f.seek(base + pos); }
};
AnsiFile ansiOpen(int i);
