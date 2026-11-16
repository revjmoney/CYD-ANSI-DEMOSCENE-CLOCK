#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// buttons, active high
enum { GBK_A = 1, GBK_B = 2, GBK_SELECT = 4, GBK_START = 8, GBK_RIGHT = 16, GBK_LEFT = 32, GBK_UP = 64, GBK_DOWN = 128 };
// one 160 pixel line: bits 0-1 shade, bits 4-5 palette (0 OBJ0, 1 OBJ1, 2 BG)
typedef void (*gbc_line_fn)(const uint8_t* px, int line);

int         gbc_init(const uint8_t* rom, gbc_line_fn fn);
size_t      gbc_save_size(void);
void        gbc_set_ram(uint8_t* ram);
int         gbc_ram_dirty(void);
void        gbc_frame(uint8_t pressed);
void        gbc_frame_skip(int on);
void        gbc_reset(void);
const char* gbc_title(char* buf);  // buf needs 17 bytes

// save states: write gbc_state_size() bytes from gbc_state_ptr(); to load,
// call _begin, read the same bytes into gbc_state_ptr(), then _end
struct gbc_keep {
    void *rom_read, *ram_read, *ram_write, *error, *serial_tx, *serial_rx, *boot, *line, *priv;
};
size_t      gbc_state_size(void);
const void* gbc_state_ptr(void);
void        gbc_state_load_begin(struct gbc_keep* k);
void        gbc_state_load_end(const struct gbc_keep* k);

#ifdef __cplusplus
}
#endif
