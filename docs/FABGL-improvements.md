# FabGL Footprint Analysis — Are Unused Features Bloating Our Firmware?

**Date:** 2026-05-23
**Question:** FabGL ships many emulation/UI add-ons we don't use. Are they compiled
into our firmware? Can we identify what's unneeded and trim the library to shrink the build?

**Short answer:** No, the unused FabGL features are **not** in the flashed binary. The
toolchain already strips them at link time. Trimming the library would save **compile
time**, not flash. The analysis and the (modest) options are below.

> This is analysis only — no code was changed.

---

## 1. How FabGL is pulled in

Every HAL file includes the master umbrella header:

```
src/hal/hal_video.cpp     #include "fabgl.h"
src/hal/hal_keyboard.cpp  #include "fabgl.h"
src/hal/hal_joystick.cpp  #include "fabgl.h"
src/hal/osd_canvas.cpp    #include "fabgl.h"
```

`fabgl.h` (lines 312-388) `#include`s **everything**: the VGA/VGA2/4/8/16/text/direct
controllers, SSD1306 + TFT (ST7789/ILI9341) drivers, CVBS composite drivers, the ANSI
terminal, the `fabui` GUI toolkit, `InputBox`, the sound engine, PS/2 keyboard+mouse,
I²C, DS3231 RTC, MCP23S17 expander, serial port, the scene/collision game engine, and a
pile of `using fabgl::...` declarations.

Arduino compiles **each** of FabGL's ~52 `.cpp` files into its own object file. So yes —
the whole library *compiles*. The question is what survives to the **link**.

### What we actually instantiate

| FabGL class | Where | Purpose |
|---|---|---|
| `fabgl::VGAController` | `hal_video.cpp:345` | 64-color VGA, 640×200@70Hz, `createRawPixel`, `getScanline` |
| `fabgl::Canvas` | `hal_video.cpp:346` + `osd_canvas` | OSD / supervisor drawing surface |
| `fabgl::PS2Controller` | `hal_keyboard.cpp` | PS/2 host (`KeyboardPort0_MousePort1` preset) |
| `fabgl::Keyboard` | `hal_keyboard.cpp` | VirtualKey events, `SpanishLayout` |
| `fabgl::Mouse` | `hal_joystick.cpp` | PS/2 mouse → joystick deltas |
| `VirtualKey` enums, `RGB222`, `GlyphOptions`, `FontInfo` | various | constants/types |

Everything else FabGL offers — **CPU emulators (Z80, i8086, i8080, MOS6502), the PC
chipset (PIC8259, PIT8253, PC8250, MC146818, i8042, VIA6522, graphicsadapter), the ANSI
Terminal, `fabui`, `InputBox`, the SSD1306/TFT/CVBS display drivers, DS3231, MCP23S17** —
is never referenced by our code.

---

## 2. The decisive test: what's in the actual image?

The ESP32 Arduino core compiles **and links** with dead-code elimination enabled
(confirmed in `platform.txt`):

```
-ffunction-sections  -fdata-sections     (compile: each function/datum its own section)
-Wl,--gc-sections                          (link: drop any section nothing references)
```

So unreferenced FabGL code is discarded at link. To prove it, I parsed the linker map
of the most recent build
(`~/.cache/arduino/sketches/1BFF96FF06F3AF097288497CDE0D292A/…ino.map`), summing only
**loadable** sections (`.text`/`.literal`/`.rodata`/`.iram`/`.dram`) that survived GC
(real load address, i.e. excluding the "Discarded input sections" block).

### Whole-app size

```
text     data     bss      total
413,337  154,764  15,441   568 KB flash    (≈18% of huge_app's 3 MB partition)
```

### FabGL's real contribution to that image: **~60 KB** (≈11% of the app)

| FabGL object kept in image | bytes |
|---|---:|
| `displaycontroller.cpp.o` | 13,581 |
| `dispdrivers/vgacontroller.cpp.o` | 10,760 |
| `fabfonts.cpp.o` (font glyph data) | 5,680 |
| `dispdrivers/vgabasecontroller.cpp.o` | 5,155 |
| `devdrivers/kbdlayouts.cpp.o` | 5,105 |
| `comdrivers/ps2controller.cpp.o` | 3,584 |
| `devdrivers/cvbsgenerator.cpp.o` | 3,508 |
| `devdrivers/keyboard.cpp.o` | 3,131 |
| `fabutils.cpp.o` | 2,574 |
| `devdrivers/mouse.cpp.o` | 1,690 |
| `ulp_macro_ex.cpp.o` (PS/2 ULP) | 1,662 |
| `fabui.cpp.o` | 1,187 |
| `comdrivers/ps2device.cpp.o` | 1,013 |
| `canvas.cpp.o` | 937 |
| `devdrivers/swgenerator.cpp.o` | 886 |
| `codepages.cpp.o` | 788 |
| `emudevs/graphicsadapter.cpp.o` | 503 |
| `dispdrivers/TFTControllerGeneric.cpp.o` | 24 |
| **TOTAL kept** | **~61,768 (60.3 KB)** |

### The features you were worried about contribute essentially **zero**

| Suspected "bloat" | Bytes actually in image |
|---|---:|
| All CPU emulators (`emudevs/*`: Z80, i8086, i8080, MOS6502, PIC/PIT/PC8250/MC146818/i8042/VIA6522) | **503** (one stray `graphicsadapter` ref; the CPUs themselves: **0**) |
| ANSI `terminal` + `terminfo` | **0** |
| `inputbox` | **0** |
| Other VGA color-depth controllers (vga2/4/8/16, vgatext, vgadirect, paletted) | **0** |
| SSD1306 / ST7789 / ILI9341 TFT drivers | **24** (a vtable stub) |
| `fabui` GUI toolkit (151 KB of source) | **1,187** |

> Sanity check on method: the *input* object files total ~6.8 MB and ~590 KB of
> nominally-loadable sections, but after `--gc-sections` only ~60 KB is linked. That gap
> **is** the dead-code elimination doing its job. `fabui.cpp` alone is 151 KB of source
> and 49 KB of compiled `.text`, of which **1.2 KB** survives.

---

## 3. Conclusion

- **Is the unused FabGL emulation/UI code compiled into the firmware?**
  It is *compiled* (object files are produced) but **not linked**. None of the Z80/x86/6502
  emulators, the terminal, the GUI, or the non-VGA display drivers reach the flashed `.bin`.
- **Would editing the library to remove features shrink the firmware?**
  **No meaningfully.** The linker already removed them. Best-case flash savings from deleting
  all unused FabGL source ≈ the rounding error (a few stray vtable/ref bytes, <1 KB).
- **What it *would* save:** compile time and the cache footprint. `terminal.cpp` (141 KB),
  `fabui.cpp` (151 KB), `i8086.cpp`, `Z80.cpp`, etc. are all fully compiled on a clean build
  only to be thrown away. On the Pi this is a real chunk of a from-scratch build.

So the premise ("unused features make the binary bigger") is, for **flash**, not true here.
The 568 KB image is dominated by our own emulation core + the ESP32 SDK/WiFi stack, not by
FabGL's dead code.

---

## 4. If the goal is faster compiles (optional, low value)

These do **not** reduce firmware size; they only cut build time. None are recommended unless
clean-build time is genuinely painful.

| Option | Effort | Saves flash? | Saves build time? | Risk |
|---|---|---|---|---|
| **A. Do nothing** | — | — | — | none — *recommended* |
| **B. Stop including the umbrella `fabgl.h`**; include only the headers we use (`dispdrivers/vgacontroller.h`, `canvas.h`, `comdrivers/ps2controller.h`, `devdrivers/keyboard.h`, `devdrivers/mouse.h`, `fabfonts.h`) | low | no | marginal (headers only; `.cpp`s still compile) | low; lose the `using fabgl::` aliases — must qualify or re-add them |
| **C. Delete unused `.cpp` files from the FabGL library** (emudevs/, terminal, terminfo, inputbox, fabui, ssd1306/tft/cvbs drivers, DS3231, MCP23S17, serialport, scene, collisiondetector) | medium | no (<1 KB) | **yes, the biggest win** — those large files stop compiling | **fork maintenance**; library updates overwrite it; `fabgl.h` still `#include`s deleted headers so it must be edited too; easy to break |
| **D. `git`-vendor a trimmed FabGL** inside the repo and point Arduino at it | high | no | yes | full ownership of the fork; loses upstream fixes |

**Why B/C/D are weak:** Arduino has no per-library "compile only these files" knob, and FabGL
has no compile-time feature `#define`s to exclude the emulators (they're plain `.cpp` files).
The only way to skip compiling them is to physically remove the files — which forks the library.
Given the linker already wins the flash battle, that maintenance cost buys only build-time speed.

### If you do pursue build-time savings

The highest-ratio deletions (big source, zero bytes in our image) would be, in order:
`fabui.cpp/.h` (151 KB), `terminal.cpp/.h` (141 KB) + `terminfo.*`, `emudevs/*` (Z80 76 KB,
i8086 66 KB, i8080 38 KB, MOS6502 27 KB, …), `inputbox.cpp` (28 KB), and the unused display
drivers (`SSD1306Controller`, `TFTController*`, `cvbs16controller`, `vga2/4/8/16/text/direct`).
Each requires also removing its `#include` line from `fabgl.h` and any `using` alias that
references it. Keep: `displaycontroller`, `dispdrivers/vgacontroller` + `vgabasecontroller`,
`canvas`, `fabfonts`, `fabutils`, `codepages`, `comdrivers/ps2controller` + `ps2device`,
`devdrivers/keyboard` + `kbdlayouts` + `mouse`, `ulp_macro_ex`, and `devdrivers/swgenerator`.

---

## 5. Where the flash actually goes (if size reduction is the real goal)

Since FabGL isn't the lever, real flash wins would come from elsewhere:

- **Drop unused Arduino/ESP-IDF subsystems** we don't need (e.g. WiFi/BT/lwIP if no networking
  is used). The map shows `network/ICMP.cpp.o` from FabGL is pulled in (587 B) — a hint the
  IP stack is being dragged along. Pruning WiFi/BT typically dwarfs anything FabGL-side.
- Our own `src/core/` emulation + `src/supervisor/` are the larger movable pieces after the SDK.

A follow-up pass on the **non-FabGL** map entries (core.a, WiFi, lwIP) would be the productive
direction if shrinking the image is the actual objective.

---

### Reproducing this analysis

```bash
# Build dir of the latest compile (newest .map):
cd ~/.cache/arduino/sketches/1BFF96FF06F3AF097288497CDE0D292A

# Whole-app size:
~/.arduino15/packages/esp32/tools/xtensa-esp32-elf-gcc/*/bin/xtensa-esp32-elf-size \
    TTGO-VGA32-COCO.ino.elf

# Per-FabGL-object bytes kept after GC: parse the map after "Memory Configuration",
# sum loadable sections (.text/.literal/.rodata/.iram/.dram) with a non-zero load
# address for paths matching libraries/FabGL/*.o  (see the Python used to build §2's table).
```
