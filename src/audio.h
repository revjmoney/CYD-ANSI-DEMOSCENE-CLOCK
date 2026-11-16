// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rev. J. Money
// Chip synth + built-in demo track on the DAC (GPIO26, the CYD's SPEAK
// connector). The song is also the master clock: visuals sync to it whether
// or not a speaker is plugged in.
#pragma once
#include <stdint.h>

struct Sync {
    float    beats;  // continuous beat count since boot
    int      bar;    // song bar (after loop mapping)
    int      row;    // 16th within the bar, 0..15
    float    kick, snare, hat;  // 1 on the hit, decaying to 0
    bool     drums;  // true while the drums are playing
};

void  audioInit();
Sync  syncNow();
void  audioMute(bool m);
bool  audioMuted();
float bpm();
