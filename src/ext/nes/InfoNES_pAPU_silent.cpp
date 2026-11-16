/* Modified for this port: the board has no speaker hooked up, so the sound
   unit is replaced by silent stubs (saves ~125 KB of RAM and the per-frame
   wave synthesis). Original: InfoNES_pAPU.cpp. */
#include "InfoNES.h"
#include "InfoNES_pAPU.h"

static void ApuIgnore(WORD, BYTE) {}

ApuWritefunc pAPUSoundRegs[20] = {ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore,
                                  ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore,
                                  ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore, ApuIgnore};

void ApuWriteControl(WORD, BYTE) {}
void InfoNES_pAPUInit(void) {}
void InfoNES_pAPUDone(void) {}
void InfoNES_pAPUVsync(void) {}

/* length counters read back through $4015; silent, so always zero */
BYTE  ApuC1Atl, ApuC2Atl, ApuC3a, ApuC3Atl, ApuC4Atl;
DWORD ApuC3Llc;
