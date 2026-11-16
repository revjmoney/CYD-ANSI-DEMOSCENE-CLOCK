/* Platform layer for this port: ROM from a memory pointer, scanlines and
   input through callbacks, no sound. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "InfoNES.h"
#include "InfoNES_System.h"
#include "nescore.h"

WORD NesPalette[64] = {
    0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20, 0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
    0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521, 0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67, 0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75, 0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000};

static const uint8_t* romData;
static size_t         romLen;
static nes_line_fn    lineFn;
static nes_frame_fn   frameFn;
static DWORD          pad1, sys;
static char           lastError[64];

int InfoNES_ReadRom(const char*) {
    if (romLen < 16 || memcmp(romData, "NES\x1a", 4)) return -1;
    memcpy(&NesHeader, romData, sizeof NesHeader);
    memset(SRAM, 0, SRAM_SIZE);
    const uint8_t* p = romData + 16;
    if (NesHeader.byInfo1 & 4) {  // trainer
        memcpy(&SRAM[0x1000], p, 512);
        p += 512;
    }
    size_t need = (p - romData) + NesHeader.byRomSize * 0x4000 + NesHeader.byVRomSize * 0x2000;
    if (need > romLen) return -1;
    // straight out of the memory-mapped flash, no copy
    ROM  = (BYTE*)p;
    VROM = NesHeader.byVRomSize ? (BYTE*)(p + NesHeader.byRomSize * 0x4000) : NULL;
    return 0;
}
void  InfoNES_ReleaseRom() { ROM = VROM = NULL; }
void  InfoNES_LoadFrame() {
    int r = frameFn();
    if (r < 0) {
        sys = PAD_SYS_QUIT;
        return;
    }
    // touch pad bits -> NES order: A B Select Start Up Down Left Right
    pad1 = (r & NES_A ? 0x01 : 0) | (r & NES_B ? 0x02 : 0) | (r & NES_SELECT ? 0x04 : 0) | (r & NES_START ? 0x08 : 0) |
           (r & NES_UP ? 0x10 : 0) | (r & NES_DOWN ? 0x20 : 0) | (r & NES_LEFT ? 0x40 : 0) | (r & NES_RIGHT ? 0x80 : 0);
}
void  InfoNES_LineDone(int line, WORD* px) { lineFn(line, px); }
void  InfoNES_PadState(DWORD* p1, DWORD* p2, DWORD* s) {
    *p1 = pad1;
    *p2 = 0;
    *s  = sys;
}
int   InfoNES_Menu() { return 0; }
void* InfoNES_MemoryCopy(void* d, const void* s, int n) { return memcpy(d, s, n); }
void* InfoNES_MemorySet(void* d, int c, int n) { return memset(d, c, n); }
void  InfoNES_DebugPrint(char*) {}
void  InfoNES_Wait() {}
void  InfoNES_SoundInit(void) {}
int   InfoNES_SoundOpen(int, int) { return 0; }
void  InfoNES_SoundClose(void) {}
void  InfoNES_SoundOutput(int, BYTE*, BYTE*, BYTE*, BYTE*, BYTE*) {}
void  InfoNES_MessageBox(char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(lastError, sizeof lastError, fmt, a);
    va_end(a);
}

const char* nes_run(const uint8_t* rom, size_t len, nes_line_fn line, nes_frame_fn frame) {
    romData     = rom;
    romLen      = len;
    lineFn      = line;
    frameFn     = frame;
    pad1 = sys  = 0;
    lastError[0] = 0;
    if (InfoNES_Alloc() < 0) return "Not enough memory";
    if (InfoNES_Load(NULL) < 0) return lastError[0] ? lastError : "Not a NES ROM";
    InfoNES_Cycle();  // returns once frame() asks to quit
    return NULL;
}
void nes_reset() { InfoNES_Reset(); }

// ---- save states ----
void InfoNES_StateCore(void (*mem)(void*, int), void (*ptr)(BYTE**));
void InfoNES_StateCPU(void (*mem)(void*, int));
void InfoNES_StateMapper(void (*mem)(void*, int));

static FILE* stateFile;
static bool  stateSaving, stateOk;
static void  stMem(void* p, int n) {
    if (!stateOk) return;
    size_t r = stateSaving ? fwrite(p, 1, n, stateFile) : fread(p, 1, n, stateFile);
    if ((int)r != n) stateOk = false;
}
// a pointer is stored as (region, offset), so a state still loads after a
// reboot maps the ROM somewhere else
static void stPtr(BYTE** p) {
    struct R {
        BYTE*  base;
        size_t len;
    } regions[] = {{ROM, (size_t)NesHeader.byRomSize * 0x4000}, {VROM, (size_t)NesHeader.byVRomSize * 0x2000},
                   {RAM, RAM_SIZE}, {SRAM, SRAM_SIZE}, {PPURAM, PPURAM_SIZE}, {ChrBuf, 256 * 2 * 8 * 8}};
    uint8_t  id  = 0xFF;
    uint32_t off = 0;
    if (stateSaving) {
        for (uint8_t i = 0; i < 6; i++)
            if (regions[i].base && *p >= regions[i].base && *p < regions[i].base + regions[i].len) {
                id  = i;
                off = *p - regions[i].base;
            }
        stMem(&id, 1);
        stMem(&off, 4);
    } else {
        stMem(&id, 1);
        stMem(&off, 4);
        if (stateOk) *p = id < 6 && regions[id].base ? regions[id].base + off : NULL;
    }
}
static bool stRun(const char* path, bool save) {
    stateFile = fopen(path, save ? "wb" : "rb");
    if (!stateFile) return false;
    stateSaving = save;
    stateOk     = true;
    uint32_t magic = 0x4E455331;  // "NES1"
    uint8_t  mapper = MapperNo;
    stMem(&magic, 4);
    stMem(&mapper, 1);
    if (!save && (magic != 0x4E455331 || mapper != MapperNo)) stateOk = false;
    InfoNES_StateCPU(stMem);
    InfoNES_StateCore(stMem, stPtr);
    InfoNES_StateMapper(stMem);
    fclose(stateFile);
    return stateOk;
}
bool nes_save_state(const char* path) { return stRun(path, true); }
bool nes_load_state(const char* path) { return stRun(path, false); }
