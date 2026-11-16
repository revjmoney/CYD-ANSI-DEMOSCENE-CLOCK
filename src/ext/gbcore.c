// Thin C wrapper so the header-only core compiles once, as C.
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#include <stdlib.h>
#include "peanut_gb.h"
#include "gbcore.h"

static struct gb_s*   gb;  // heap: only exists while a game runs
static const uint8_t* rom;
static uint8_t*       ram;
static int            ramDirty;
static gbc_line_fn    lineFn;

static uint8_t rom_read(struct gb_s* g, const uint_fast32_t a) { return rom[a]; }
static uint8_t ram_read(struct gb_s* g, const uint_fast32_t a) { return ram ? ram[a] : 0xFF; }
static void    ram_write(struct gb_s* g, const uint_fast32_t a, const uint8_t v) {
    if (ram) {
        ram[a]   = v;
        ramDirty = 1;
    }
}
static void on_error(struct gb_s* g, const enum gb_error_e e, const uint16_t addr) {}
static void on_line(struct gb_s* g, const uint8_t* px, const uint_fast8_t line) { lineFn(px, line); }

int gbc_init(const uint8_t* data, gbc_line_fn fn) {
    rom    = data;
    lineFn = fn;
    ram    = NULL;
    if (!gb) gb = (struct gb_s*)malloc(sizeof(struct gb_s));
    if (!gb || gb_init(gb, rom_read, ram_read, ram_write, on_error, NULL) != GB_INIT_NO_ERROR) return 0;
    gb_init_lcd(gb, on_line);
    return 1;
}
size_t gbc_save_size(void) {
    size_t n = 0;
    gb_get_save_size_s(gb, &n);
    return n;
}
void        gbc_set_ram(uint8_t* r) { ram = r; }
int         gbc_ram_dirty(void) {
    int d    = ramDirty;
    ramDirty = 0;
    return d;
}
void        gbc_frame(uint8_t pressed) {
    gb->direct.joypad = ~pressed;  // the core wants active-low bits
    gb_run_frame(gb);
}
void        gbc_frame_skip(int on) { gb->direct.frame_skip = on ? 1 : 0; }
void        gbc_reset(void) { gb_reset(gb); }
const char* gbc_title(char* buf) { return gb_get_rom_name(gb, buf); }

/* Save states: the whole core is one struct. Its function pointers belong to
   this build and session, so they are kept across a load. */
size_t gbc_state_size(void) { return sizeof(struct gb_s); }
const void* gbc_state_ptr(void) { return gb; }
void gbc_state_load_begin(struct gbc_keep* k) {
    k->rom_read  = (void*)gb->gb_rom_read;
    k->ram_read  = (void*)gb->gb_cart_ram_read;
    k->ram_write = (void*)gb->gb_cart_ram_write;
    k->error     = (void*)gb->gb_error;
    k->serial_tx = (void*)gb->gb_serial_tx;
    k->serial_rx = (void*)gb->gb_serial_rx;
    k->boot      = (void*)gb->gb_bootrom_read;
    k->line      = (void*)gb->display.lcd_draw_line;
    k->priv      = gb->direct.priv;
}
void gbc_state_load_end(const struct gbc_keep* k) {
    gb->gb_rom_read             = (uint8_t (*)(struct gb_s*, const uint_fast32_t))k->rom_read;
    gb->gb_cart_ram_read        = (uint8_t (*)(struct gb_s*, const uint_fast32_t))k->ram_read;
    gb->gb_cart_ram_write       = (void (*)(struct gb_s*, const uint_fast32_t, const uint8_t))k->ram_write;
    gb->gb_error                = (void (*)(struct gb_s*, const enum gb_error_e, const uint16_t))k->error;
    gb->gb_serial_tx            = (void (*)(struct gb_s*, const uint8_t))k->serial_tx;
    gb->gb_serial_rx            = (enum gb_serial_rx_ret_e (*)(struct gb_s*, uint8_t*))k->serial_rx;
    gb->gb_bootrom_read         = (uint8_t (*)(struct gb_s*, const uint_fast16_t))k->boot;
    gb->display.lcd_draw_line   = (void (*)(struct gb_s*, const uint8_t*, const uint_fast8_t))k->line;
    gb->direct.priv             = k->priv;
}
