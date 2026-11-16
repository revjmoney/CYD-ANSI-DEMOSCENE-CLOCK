// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
#include "audio.h"
#include <Arduino.h>
#include <soc/rtc_io_reg.h>
#include <soc/sens_reg.h>
#include <driver/dac.h>
#include <math.h>

static constexpr int RATE = 22050, BPM = 125;
static constexpr int SPR  = RATE * 60 / (BPM * 4);  // samples per 16th row (2646)
static constexpr int RING = 4096;

static uint8_t           ring[RING];
static volatile uint32_t rd = 0, wr = 0;  // free-running sample counters
static volatile bool     muted = true;  // silent by default; the song still runs as the beat clock
static hw_timer_t*       timer;
volatile uint32_t        audioBusyUs = 0;  // render time, for the 'i' report

// ------------------------------------------------------------------ song
// i - VI - III - VII in A minor, one chord per bar
static const uint8_t bassRoot[4] = {45, 41, 48, 43};
static const uint8_t arpNotes[4][4] = {{69, 72, 76, 81}, {65, 69, 72, 77}, {67, 72, 76, 79}, {67, 71, 74, 79}};
// lead: 8 bars of {row, note, len}, terminated by len 0
struct Note {
    uint8_t row, note, len;
};
static const Note lead[8][7] = {
    {{0, 76, 4}, {4, 81, 2}, {6, 79, 2}, {8, 76, 4}, {12, 74, 2}, {14, 72, 2}, {0, 0, 0}},
    {{0, 72, 6}, {6, 74, 2}, {8, 76, 4}, {12, 77, 4}, {0, 0, 0}},
    {{0, 79, 4}, {4, 76, 4}, {8, 72, 2}, {10, 74, 2}, {12, 76, 4}, {0, 0, 0}},
    {{0, 74, 8}, {8, 71, 4}, {12, 67, 4}, {0, 0, 0}},
    {{0, 81, 4}, {4, 84, 4}, {8, 83, 2}, {10, 81, 2}, {12, 79, 4}, {0, 0, 0}},
    {{0, 77, 4}, {4, 81, 4}, {8, 79, 4}, {12, 77, 2}, {14, 76, 2}, {0, 0, 0}},
    {{0, 76, 4}, {4, 79, 2}, {6, 84, 6}, {12, 83, 2}, {14, 79, 2}, {0, 0, 0}},
    {{0, 83, 4}, {4, 81, 4}, {8, 79, 8}, {0, 0, 0}},
};

enum Section { MODEM, ARP_IN, GROOVE, LEAD, BREAK };
// bars 0-1 modem handshake, 2-3 arp, then a 28 bar loop starting at bar 4
static int songBar(int bar) { return bar < 32 ? bar : 4 + (bar - 4) % 28; }
static Section section(int sbar) {
    if (sbar < 2) return MODEM;
    if (sbar < 4) return ARP_IN;
    if (sbar < 12) return GROOVE;
    if (sbar < 20) return LEAD;
    if (sbar < 24) return BREAK;
    return LEAD;
}
static bool drumsIn(Section s) { return s == GROOVE || s == LEAD; }
static bool kickAt(int row) { return (row & 3) == 0; }
static bool snareAt(int row) { return row == 4 || row == 12; }
static bool hatAt(int row) { return (row & 3) == 2; }

// ------------------------------------------------------------------ synth
static uint32_t noteInc[128];
static int16_t  sine[1024];
static uint32_t kickBase, kickSweep, snareInc;  // precomputed phase increments
struct Voice {
    uint32_t phase = 0, inc = 0;
    int32_t  amp = 0;  // 16.16-ish, 0..65535
    int32_t  decay = 65535;
};
static Voice    bass, arp, ld, ld2, kick, snare, hat, modemA, modemB;
static uint32_t lfsr = 0xACE1;
static int32_t  bassLp = 0, hatPrev = 0;
static int      kickT = 999999;

static inline int32_t noise() {
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return (int32_t)(lfsr & 0xFFFF) - 32768;
}
static void trigger(Voice& v, int note, int amp, int decay) {
    if (note >= 0) v.inc = noteInc[note];
    v.amp   = amp;
    v.decay = decay;
}
static uint32_t hz(float f) { return (uint32_t)(f * 4294967296.0 / RATE); }

static void rowEvents(int bar, int row) {
    int     sbar = songBar(bar);
    Section s    = section(sbar);
    int     ch   = sbar & 3;
    if (s == MODEM) {
        // DTMF dial, answer tone, then the scrambled carrier
        static const uint16_t lo[] = {697, 770, 852, 941}, hi[] = {1209, 1336, 1477};
        if (sbar == 0 && row < 8) {
            modemA.inc = hz(lo[(row * 7) % 4]);
            modemB.inc = hz(hi[(row * 5) % 3]);
            modemA.amp = modemB.amp = (row & 1) ? 0 : 9000;
            modemA.decay = modemB.decay = 65535;
        } else if (sbar == 0) {
            modemA.inc   = hz(2100);
            modemA.amp   = 9000;
            modemB.amp   = 0;
        } else {
            modemA.inc = hz((row & 1) ? 1200 : 2400);
            modemA.amp = 5000;
            modemB.inc = hz(980 + 200 * (row & 2));
            modemB.amp = 4000;
        }
        return;
    }
    modemA.amp = modemB.amp = 0;
    // arp on every 16th
    int arpVol = s == ARP_IN ? 2500 + (sbar - 2) * 2000 + row * 120 : s == BREAK ? 5000 : 6000;
    trigger(arp, arpNotes[ch][row & 3] + ((row & 4) ? 12 : 0) * (s == BREAK), arpVol, 63200);
    if (s == GROOVE || s == LEAD) {
        if ((row & 1) == 0) trigger(bass, bassRoot[ch] + ((row & 2) ? 12 : 0), 14000, 64600);
        if (kickAt(row)) {
            kickT = 0;
            kick.amp   = 30000;
            kick.decay = 65000;
        }
        if (snareAt(row)) trigger(snare, -1, 14000, 63000);
        if (hatAt(row)) trigger(hat, -1, 7000, 60000);
        else if (row & 1) trigger(hat, -1, 2500, 58000);
    } else if (s == BREAK && row == 0) {
        trigger(bass, bassRoot[ch], 12000, 65300);
    }
    if (s == LEAD || s == BREAK) {
        int lbar = (sbar - (s == BREAK ? 20 : sbar < 20 ? 12 : 24)) & 7;
        for (const Note* n = lead[lbar]; n->len; n++)
            if (n->row == row) {
                int a = s == BREAK ? 5000 : 8000;
                trigger(ld, n->note, a, 65300);
                trigger(ld2, n->note, a / 2, 65300);
                ld2.inc += ld2.inc / 400;  // detune
            }
    }
}

static void render(uint8_t* out, int n, uint32_t pos) {
    for (int i = 0; i < n; i++, pos++) {
        if (pos % SPR == 0) {
            uint32_t r = pos / SPR;
            rowEvents(r / 16, r % 16);
        }
        if ((pos & 15) == 0) {
            for (Voice* v : {&bass, &arp, &ld, &ld2, &kick, &snare, &hat}) v->amp = (v->amp * v->decay) >> 16;
        }
        int32_t mix = 0;
        // bass: saw through a one pole lowpass
        bass.phase += bass.inc;
        int32_t saw = (int32_t)(bass.phase >> 16) - 32768;
        bassLp += (saw - bassLp) >> 3;
        mix += (bassLp * bass.amp) >> 16;
        // arp: 25% pulse
        arp.phase += arp.inc;
        mix += ((arp.phase >> 30) == 0 ? arp.amp : -arp.amp) >> 1;
        // lead: two detuned squares with vibrato (about 4 Hz)
        int32_t vib = ((int32_t)sine[(pos >> 4) & 1023] * (int32_t)(ld.inc >> 12)) >> 11;
        ld.phase += ld.inc + vib;
        ld2.phase += ld2.inc + vib;
        mix += ((ld.phase >> 31) ? ld.amp : -ld.amp) >> 1;
        mix += ((ld2.phase >> 31) ? ld2.amp : -ld2.amp) >> 1;
        // kick: sine with a fast pitch drop
        if (kickT < RATE) {
            if (kickT == 0) kickSweep = hz(110);
            kickSweep = (uint32_t)(((uint64_t)kickSweep * 65477) >> 16);  // exp pitch drop
            kick.phase += kickBase + kickSweep;
            mix += (sine[kick.phase >> 22] * kick.amp) >> 15;
            kickT++;
        }
        // snare: noise + body
        if (snare.amp > 50) {
            snare.phase += snareInc;
            mix += ((noise() * snare.amp) >> 16) + ((sine[snare.phase >> 22] * snare.amp) >> 16);
        }
        // hat: high passed noise
        if (hat.amp > 50) {
            int32_t nz = noise(), hp = nz - hatPrev;
            hatPrev    = nz;
            mix += (hp * hat.amp) >> 17;
        }
        if (modemA.amp || modemB.amp) {
            modemA.phase += modemA.inc;
            modemB.phase += modemB.inc;
            mix += ((modemA.phase >> 31) ? modemA.amp : -modemA.amp) + ((modemB.phase >> 31) ? modemB.amp : -modemB.amp);
            if (songBar(pos / SPR / 16) == 1) mix += noise() >> 3;
        }
        int v  = 128 + (mix >> 8);
        out[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

static void IRAM_ATTR onTimer() {
    if (rd == wr) return;  // underrun: hold, so the clock never runs ahead of the music
    uint8_t v = muted ? 128 : ring[rd & (RING - 1)];
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, v, RTC_IO_PDAC2_DAC_S);
    rd = rd + 1;
}

static void audioTask(void*) {
    // the timer interrupt lands on the core that attaches it
    timer = timerBegin(0, 2, true);  // 40 MHz
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmWrite(timer, 40000000 / RATE, true);
    timerAlarmEnable(timer);
    uint8_t blk[256];
    for (;;) {
        while (wr - rd <= RING - sizeof(blk)) {
            uint32_t t0 = micros();
            if (muted) memset(blk, 128, sizeof(blk));  // sync comes from the song data, no need to synthesize
            else render(blk, sizeof(blk), wr);
            audioBusyUs += micros() - t0;
            for (int i = 0; i < (int)sizeof(blk); i++) ring[(wr + i) & (RING - 1)] = blk[i];
            wr = wr + sizeof(blk);
        }
        vTaskDelay(2);
    }
}

void audioInit() {
    for (int n = 0; n < 128; n++) noteInc[n] = hz(440.0f * powf(2, (n - 69) / 12.0f));
    for (int i = 0; i < 1024; i++) sine[i] = (int16_t)(32767 * sinf(i * 2 * PI / 1024));
    kickBase = hz(45);
    snareInc = hz(185);
    dac_output_enable(DAC_CHANNEL_2);  // GPIO26 only; GPIO25 is the touch clock
    dac_output_voltage(DAC_CHANNEL_2, 128);
    // core 1: core 0 is busy streaming frames to the panel, core 1 has headroom
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 5, nullptr, 1);
}

void  audioMute(bool m) { muted = m; }
bool  audioMuted() { return muted; }
float bpm() { return BPM; }

// Everything visual is derived from the sample now playing, straight from the
// song data, so sync is exact and needs no event queue.
Sync syncNow() {
    uint32_t pos = rd;
    Sync     s;
    uint32_t grow = pos / SPR;
    s.beats       = pos / (float)(SPR * 4);
    int bar       = grow / 16;
    s.row         = grow % 16;
    s.bar         = songBar(bar);
    s.drums       = drumsIn(section(s.bar));
    s.kick = s.snare = s.hat = 0;
    for (int back = 0; back < 16 && back <= (int)grow; back++) {
        uint32_t g  = grow - back;
        int      sb = songBar(g / 16), r = g % 16;
        if (!drumsIn(section(sb))) continue;
        float dt = (pos - g * SPR) / (float)RATE;
        if (s.kick == 0 && kickAt(r)) s.kick = expf(-dt * 7);
        if (s.snare == 0 && snareAt(r)) s.snare = expf(-dt * 6);
        if (s.hat == 0 && hatAt(r)) s.hat = expf(-dt * 18);
    }
    return s;
}
