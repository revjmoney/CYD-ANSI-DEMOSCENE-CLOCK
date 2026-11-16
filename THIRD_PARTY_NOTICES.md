# Third-party notices

- **IBM VGA 8x16 font** (in `include/rotatocam_data.h`, and copied for Cell Pet in `include/font8x16.h`), from [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/) by VileR. Licensed [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/). The font data stays under that license.
- **LovyanGFX** by lovyan03 ([github.com/lovyan03/LovyanGFX](https://github.com/lovyan03/LovyanGFX)). Fetched by PlatformIO at build time under its own composite BSD/MIT license.
- **CYD panel and touch pin map and the ILI9341/ST7789 probe** follow bdring's [FluidDial](https://github.com/bdring/FluidDial) CYD support.
- **Acid Warp** (`src/acidwarp.cpp`): the picture functions, palettes and palette rolling are ported from Acid Warp by Noah Spurrier and Mark Bilk (1992, 1993), from the Linux port by Steven Wills, which is distributed under the GNU GPL.
- **Raycaster**: an original implementation following Lode Vandevenne's [raycasting tutorial](https://lodev.org/cgtutor/raycasting.html) (BSD-2-Clause).
- **ANSI art** is not distributed here. `tools/fetch_ansi.py` downloads it from [16colo.rs](https://16colo.rs) at build time. Every piece remains the property of its artist and group.
