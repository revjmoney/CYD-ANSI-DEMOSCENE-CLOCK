#pragma once
#include <stddef.h>
#include <stdint.h>

// buttons, same bit values as the GB side
enum { NES_A = 1, NES_B = 2, NES_SELECT = 4, NES_START = 8, NES_RIGHT = 16, NES_LEFT = 32, NES_UP = 64, NES_DOWN = 128 };
// a finished scanline: 256 RGB555 pixels (bit 15 is an internal flag, mask it off)
typedef void (*nes_line_fn)(int line, uint16_t* px);
// once per frame: return the pressed buttons, or -1 to stop
typedef int (*nes_frame_fn)();

// Runs until frame() returns -1. Returns NULL, or an error message.
const char* nes_run(const uint8_t* rom, size_t len, nes_line_fn line, nes_frame_fn frame);
void        nes_reset();
// save states to a VFS path such as /sd/roms/nes/game.st0 (only while running)
bool        nes_save_state(const char* path);
bool        nes_load_state(const char* path);
