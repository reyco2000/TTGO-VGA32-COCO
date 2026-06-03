# Performance Analysis — ESP32 CoCo Emulator

> ## TTGO VGA32 (ESP32-WROVER) — Current Target
>
> Measured on the VGA32 port **without applying any of the S3-era optimizations below** — these are out-of-the-box numbers from the straight port:
>
> | Scenario | FPS |
> |---|---|
> | Idle BASIC ROM (`OK` prompt, cursor blink) | **58 FPS** |
> | CoCo 2 program with animation | **47 FPS** |
>
> The jump over the S3+TFT baseline (~26 FPS, see below) comes almost entirely from removing the per-frame SPI `pushSprite` step — FabGL's DMA scans the framebuffer continuously, so optimizations targeting SPI transfer time (OPT-16 VRAM shadow compare, OPT-C9 CRC32 dirty skip, frame-skip) have no equivalent cost to remove on VGA32. The remaining headroom (animation case is ~80% of the idle case) is now bound by GIME / VDG render and CPU emulation cost rather than display I/O.
>
> ### CoCo 3 (GIME) — VGA32 framerate optimization (2026-05-29)
>
> CoCo 3 mode ran far slower than CoCo 2 (~17 FPS vs ~47 FPS) — an inherent cost difference, not a regression. Profiling with the built-in `perf_probe` (config `PERF_PROBE_ENABLED`) showed a compound video-path cost dominated by per-pixel color conversion in the HAL, not the CPU/memory path (`cpu_run` was only ~10–16%).
>
> | Workload | Before | After | Gain |
> |---|---|---|---|
> | Idle BASIC (`OK` prompt) | 27.9 FPS | **38.0 FPS** | **+36%** |
> | App with animation | 17.5 FPS | **21.5 FPS** | **+23%** |
>
> **OPT-G1 — Active-pixel LUT (DONE).** `hal_video_render_scanline_gime()` (VGA32 path) converted every active pixel with a `__builtin_bswap16` → `rgb565_to_rgb222` → `s_vga.createRawPixel()` chain (~144K conversions/frame), costing ~18 ms/frame (`hal_scn`) regardless of workload. Replaced with a precomputed 64 KB LUT (`s_gime_pixel_raw_lut[65536]`, internal DRAM, built once in `init_gime_lut()`) keyed on the byte-swapped RGB565 value the core emits (OPT-C4), so the inner loop is a single `row[x^2] = lut[pixels[x]]`. Halved `hal_scn` (~18 → ~9 ms) in both modes — this is the bulk of the gain above.
>
> **Rejected attempts (recorded so they aren't retried):**
> - *640→320 hal-side downscale* (`GIME_VGA_DOWNSCALE`, **default 0**): halves `hal_scn` lookups for 640-wide modes (~+1 FPS on the app) but **makes 80-column text unreadable** (drops odd columns). Disabled.
> - *Core render-loop hoist* (caching `gime->` mode fields in `const` locals in `tcc1014_render_scanline()`): **regressed** `render_scn` 31.5 → 34.4 ms. The compiler already optimizes the loop well (`fetch_byte_vram` is `static inline`, so it can see which fields change); forcing locals added stack spills under `-O2`. Reverted.
>
> **Remaining CoCo 3 bottleneck:** under animation the frame is dominated by `render_scn` (`tcc1014_render_scanline`, ~31.5 ms / ~68%) — the per-pixel GIME decode+expand work in the core. The only further levers are a fully unrolled per-mode render (medium risk on a faithful XRoar port) or a dual-core render offload (OPT-9-style), both higher effort for modest gain.
>
> ## ⚠ Everything below is historical — ESP32-S3 + TFT build only
>
> All other numbers and optimization entries in this document were measured on the original **ESP32-S3 + ILI9341 / ST7789 / ST7796 TFT** build. They are kept as a reference for the methodology and as a baseline for understanding the codebase's optimization history.
>
> Optimizations rooted in `pushSprite` cost (OPT-16, OPT-C9, frame-skip, sprite alloc tuning, SPI clock tuning) **do not apply** to the VGA32 build. CPU / GIME / audio optimizations (cache layout, inline opcodes, branchless flags, ISR placement) may still help if the VGA32 build is ever profiled and targeted.

> **Baseline:** ~26 FPS (~0.43x real-time), boot to "DISK EXTENDED BASIC" ~7 seconds
> **Target:** 30-35 FPS (practical target); DMA ceiling ~48 FPS (physical SPI limit)
> **Hardware:** ESP32-S3 N16R8 (240 MHz dual-core, 8 MB PSRAM, 16 MB flash)
> **Last updated:** 2026-03-26
>
> **Implementation log:**
> - [x] OPT-3: Replace drawPixel() with direct FB write (2026-03-16)
> - [x] OPT-6: Set DEBUG_ENABLED=0 (2026-03-16) — **REGRESSED to 1, needs re-fix**
> - [x] OPT-4: Copy font ROM to DRAM (2026-03-16)
> - [x] OPT-1: IRAM_ATTR on hot CPU functions — TESTED, NOT NEEDED (2026-03-20)
> - [x] OPT-5: Precompute scanline cycle targets (2026-03-20)
> - [x] Branchless CC flag helpers (2026-03-21) — ~+2 FPS
> - [ ] OPT-6b: Re-fix DEBUG_ENABLED=0 regression
> - [x] OPT-11: Compiler -O2 for hot files (2026-03-25) — +1.5 FPS idle, +0.5 FPS graphics
> - [x] OPT-12: Reduce check_interrupts frequency — REJECTED (violates 6809 spec)
> - [ ] OPT-2: Split RAM: zero page + stack in DRAM
> - [ ] OPT-13: Inline machine_read/write fast path for RAM
> - [ ] OPT-14: Move ROM arrays to DRAM
> - [ ] OPT-15: Dirty-tile text mode optimization
> - [ ] OPT-7: Overlap USB wait with emulation — DEFERRED (keyboard needs full 3s enumeration window)
> - [x] OPT-16: VRAM shadow compare — skip SPI push on unchanged screen (2026-03-26) — +37 FPS text, +18 FPS static gfx
> - [ ] OPT-10: Adaptive frame skip
> - [ ] OPT-9: Dual-core rendering on Core 0
> - [ ] ~~OPT-8: Pack CPU flags into bitfield~~ — SKIP (negligible)
>
> **Known issue:** Disk read corruption (LOAD/LOADM) occurred during past memory-related optimizations. Fixed, but any change to memory layout or machine_read/write must be validated with LOAD/LOADM.

---

## 1. Current Performance Profile

**Budget:** 16,667 us per frame at 60 FPS
**Actual:** ~38,462 us per frame at 26 FPS (2.3x over budget)

### Estimated Per-Frame Microsecond Breakdown

| Component | Est. us/frame | % of frame | Notes |
|-----------|--------------|------------|-------|
| **CPU emulation** (`execute_one()` x ~14,916 cycles) | ~20,000 | 52% | ~1.34 us/instruction avg, includes memory callbacks |
| **Memory access overhead** (PSRAM latency) | ~6,600 | 17% | ~170K accesses/frame, 65% to first 2 KB hitting PSRAM at 50-100 ns penalty |
| **Function pointer indirection** (`mem_read/write`) | ~2,550 | 7% | ~170K indirect calls x ~15 ns (pipeline flush) |
| **VDG rendering** (`mc6847_render_scanline` x 192) | ~4,500 | 12% | Font lookup + pixel expansion to RGB565 |
| **SPI frame push** (`pushSprite`) | ~1,750 avg | 5% | ~3,500 us every 2nd frame (FRAME_SKIP=1), blocks CPU |
| **check_interrupts overhead** | ~1,500 | 4% | Called every instruction, ~4,500 calls/frame x ~0.33 us |
| **SAM + PIA + FDC per-scanline** | ~1,500 | 4% | sv_disk_tick + sam6883 + PIA transitions |
| **Total** | **~38,400** | **230%** | **Need to cut to 16,667** |

### Top 5 Remaining Bottlenecks

1. **CoCo RAM in PSRAM** — 65% of ~170K memory accesses/frame hit PSRAM for zero page/stack/workspace (~6,600 us penalty)
2. **Function pointer indirection in memory access** — every mem_read/write goes through indirect call + address decode chain (~2,550 us)
3. **check_interrupts() called every instruction** — 3 boolean checks + function call overhead per instruction (~1,500 us)
4. ~~**SPI push blocks CPU emulation**~~ — **SOLVED by OPT-16** for static screens; still ~3,500 us per push for scrolling/active graphics
5. **Compiler -Os instead of -O2** — size optimization leaves performance on the table for switch/case dispatch, inlining, loop unrolling (~2,000-3,000 us estimated)

---

## 2. Optimization Catalog

### OPT-1: IRAM_ATTR on Hot CPU Functions — NOT NEEDED

- **Status:** TESTED 2026-03-20 — FPS dropped from 27 to 25. **Changes reverted.**
- **Root cause:** `execute_one()` is ~1,800 lines of switch/case. Forcing into IRAM caused DRAM pressure. ESP32-S3 flash cache handles hot code paths adequately at the current execution rate.
- **Conclusion:** IRAM_ATTR is only beneficial for small ISRs (like `audio_timer_isr()` which already has it). Do not apply to large functions.

### OPT-2: Move CoCo Zero Page + Stack to DRAM

- **Impact:** HIGH (est. +5-8 FPS)
- **Effort:** Medium (split allocation, add fast-path check in machine_read/write)
- **Risk:** Medium (must handle all edge cases in address decoding)
- **Disk I/O check:** REQUIRED — modifies machine_read/write. Must validate LOAD/LOADM after implementation.
- **Details:** Currently ALL 64KB of CoCo RAM is in PSRAM (~50-100 ns random access vs ~10 ns for DRAM). The 6809 spends ~60-70% of memory accesses in the first 2 KB (zero page $0000-$00FF, stack $01xx, system variables, text screen $0400-$05FF). Allocate first 2 KB from DRAM, rest from PSRAM:
  ```cpp
  m->ram_fast = (uint8_t*)malloc(2048);               // DRAM: $0000-$07FF
  m->ram_slow = (uint8_t*)ps_malloc(COCO_RAM_SIZE - 2048); // PSRAM: $0800+
  ```
  Math: 65% of 170,000 accesses/frame x 60 ns PSRAM penalty = ~6,600 us saved.

### OPT-3: Replace drawPixel() with Direct Framebuffer Write — DONE

- **Status:** IMPLEMENTED 2026-03-16 — `hal_video.cpp:191-198`: replaced `drawPixel()` loop with direct `sprite->getPointer()` + `palette_swapped[]` row write.
- **Note:** Must use `palette_swapped` (byte-swapped RGB565) for direct framebuffer writes.

### OPT-4: Copy VDG Font ROM to DRAM — DONE

- **Status:** IMPLEMENTED 2026-03-16 — `mc6847.cpp`: renamed PROGMEM array to `font_6847_flash`, added DRAM `font_6847[768]` copy via `memcpy_P()`, replaced `pgm_read_byte()` with direct array access.

### OPT-5: Precompute Per-Scanline Cycle Targets — DONE

- **Status:** IMPLEMENTED 2026-03-20 — `machine.cpp`: added static `scanline_cycle_targets[262]` table (uint16_t, 524 bytes DRAM), precomputed before scanline loop. Flash reduced by ~1.8 KB.

### OPT-6: Disable DEBUG_ENABLED for Production Builds — DONE (REGRESSED)

- **Status:** IMPLEMENTED 2026-03-16, but **REGRESSED back to DEBUG_ENABLED=1** as of 2026-03-25. `config.h:34` currently reads `#define DEBUG_ENABLED 1`. Needs re-fix (OPT-6b).

### OPT-7: Overlap USB Keyboard Wait with Emulation Start — DEFERRED

- **Impact:** LOW for FPS, HIGH for boot time (saves ~2-3 seconds)
- **Status:** DEFERRED 2026-03-28 — Removing the 3s wait causes keyboard to miss initial keypresses. USB enumeration takes >1.5s. The 3s blocking wait in setup() is required for reliable keyboard detection.

### OPT-8: Pack CPU Interrupt Flags into Bitfield — SKIP

- **Status:** SKIP — MC6809 struct is only 44 bytes (fits in a single cache line). Packing adds complexity with negligible benefit.

### OPT-9: Move VDG Rendering + SPI Push to Core 0

- **Impact:** HIGH (est. +4-6 FPS)
- **Effort:** Hard (dual-core synchronization, double-buffering)
- **Risk:** High (race conditions, FreeRTOS overhead)
- **Details:** Double-buffer the sprite framebuffer (two ~98 KB sprites in PSRAM). Core 1 runs CPU emulation + renders into buffer A. Core 0 pushes buffer B via SPI while Core 1 works on next frame. Net gain: ~3,500 us SPI push becomes zero-cost on Core 1.

### OPT-10: Adaptive Frame Skip

- **Impact:** LOW-MEDIUM (est. +1-2 FPS)
- **Effort:** Easy
- **Details:**
  ```cpp
  if (frame_time_us > 20000) frame_skip = 2;
  else if (frame_time_us < 15000) frame_skip = 0;
  else frame_skip = 1;
  ```

### OPT-11: Compiler -O2 for Hot Files — DONE

- **Status:** IMPLEMENTED 2026-03-25 — Added `#pragma GCC optimize("O2")` to `mc6809.cpp` and `machine.cpp`. Measured: +1.5 FPS idle BASIC, +0.5 FPS graphics mode.

### OPT-12: Reduce check_interrupts Frequency — REJECTED

- **Status:** REJECTED 2026-03-25 — Violates 6809 hardware specification. The MC6809E samples interrupt pins after every instruction. NMI is edge-triggered and must be checked promptly (disk FDC). Batching would corrupt disk I/O timing.

### OPT-13: Inline machine_read/write Fast Path for RAM

- **Impact:** HIGH (est. +3-5 FPS)
- **Effort:** Medium
- **Disk I/O check:** REQUIRED
- **Details:** Replace 3-layer indirection (function pointer → machine_read → address decode) with inline fast path:
  ```cpp
  static inline uint8_t mem_read_fast(uint16_t addr) {
      if (addr < 0x8000) return g_coco_ram[addr];  // ~65% of reads
      return machine_read_slow(addr);
  }
  ```

### OPT-14: Move ROM Arrays to DRAM

- **Impact:** LOW (est. +0.5-1 FPS)
- **Effort:** Easy
- **Disk I/O check:** REQUIRED
- **Details:** The 8K+8K+16K ROM arrays (32 KB total) are in PSRAM via `ps_malloc()`. Use regular `malloc()` to place in DRAM. DRAM has ~200 KB free; 32 KB is affordable.

### OPT-15: Dirty-Tile Text Mode Optimization

- **Impact:** MEDIUM (est. +1-2 FPS in text mode)
- **Effort:** Medium
- **Details:** Add a 512-byte "previous VRAM" shadow. Compare current VRAM ($0400-$05FF) with shadow each frame. Only re-render character rows with changed bytes. For idle BASIC prompt: ~94% savings on VDG rendering (~4,200 us saved).

### OPT-16: VRAM Shadow Compare — Skip SPI Push When Screen Unchanged — DONE

- **Status:** IMPLEMENTED 2026-03-26 — **MEASURED: 64 FPS text, 45 FPS static graphics, 27 FPS scrolling graphics.**
- **Details:** 6,144-byte VRAM shadow buffer + mode/base tracking. `memcmp()` current VDG region against shadow; if identical, skip `pushSprite()` (~3,500 us saved). Added 10-frame force-push after mode/base changes for multi-frame title screen drawing.

### Branchless CC Flag Helpers — DONE

- **Status:** IMPLEMENTED 2026-03-21 — Replaced branching CC_PUT + ALU helpers with branchless compute-and-mask variants. ~+2 FPS (from ~23.5 to ~25-27 FPS).

### GIME Adjacent-Pixel Blend — OPTIONAL QUALITY FEATURE (default OFF)

- **Status:** IMPLEMENTED 2026-04-23 — Opt-in via `GIME_ADJACENT_PIXEL_BLEND` in `config.h`. See `video.md` and `coco3-gime.md` for details.
- **What:** Replaces the 640→320 nearest-neighbor downscale (drop every odd pixel) with a 2-tap box-filter average of each adjacent pair.
- **Why:** All GIME text modes emit `line_width = 640`. For 40-col the core already pixel-doubles, so the skip is harmless. For 80-col and 640-wide graphics, the skip loses half the horizontal detail — thin vertical strokes on odd columns vanish.
- **Measured cost (CoCo3, idle BASIC, `WIDTH 80`):** **32 FPS → 23 FPS** with blend on. **Tested result: 80-column text becomes readable** (strokes that vanished under skip mode now render as averaged dim pixels).
- **Kept off by default** because the ~28% FPS hit is significant. Users who want readable 80-col text can flip the flag at build time.

---

## 3. Quick Wins Summary

| # | Optimization | Est. FPS Gain | Status |
|---|-------------|---------------|--------|
| ~~OPT-3~~ | ~~Direct FB write~~ | ~~+3-5~~ | DONE |
| ~~OPT-4~~ | ~~Font ROM to DRAM~~ | ~~+1-3~~ | DONE |
| ~~OPT-5~~ | ~~Precompute cycle targets~~ | ~~+0.5-1~~ | DONE |
| ~~CC~~ | ~~Branchless CC helpers~~ | ~~+2~~ | DONE |
| ~~OPT-11~~ | ~~-O2 pragma~~ | +1.5/+0.5 | DONE |
| ~~OPT-16~~ | ~~VRAM shadow compare~~ | **+37/+18/+0** | DONE |
| OPT-6b | Re-fix DEBUG_ENABLED=0 | +0.5 | **TODO** |
| OPT-10 | Adaptive frame skip | +1-2 | **TODO** |

## 4. Medium-Term Optimizations

| # | Optimization | Est. FPS Gain | Status |
|---|-------------|---------------|--------|
| ~~OPT-1~~ | ~~IRAM_ATTR~~ | ~~+8-12~~ | **REJECTED: -2 FPS** |
| OPT-2 | Split RAM: zero page to DRAM | +5-8 | **TODO** |
| OPT-13 | Inline RAM fast path | +3-5 | **TODO** |
| OPT-14 | ROM arrays to DRAM | +0.5-1 | **TODO** |
| OPT-15 | Dirty-tile text rendering | +1-2 | **TODO** |
| OPT-7 | Overlap USB wait | boot -2-3s | **DEFERRED** |

## 5. Major Refactors

| # | Optimization | Est. FPS Gain | Risk |
|---|-------------|---------------|------|
| OPT-9 | Dual-core: video + SPI on Core 0 | +4-6 | High — synchronization, double-buffer |
| — | Computed goto dispatch | +1-3 | Medium — GCC extension |
| — | Flatten memory read (eliminate function pointer) | +3-6 | Medium — large refactor |
| — | Lazy flag evaluation (deferred CC) | +3-6 | High — flag semantics |

## 6. Boot Time Improvements

**Current:** ~7 seconds to "DISK EXTENDED BASIC" prompt

### Boot Time Breakdown (Estimated)

| Phase | Time | Notes |
|-------|------|-------|
| Serial.begin + delay(500) | 500 ms | Fixed startup delay |
| hal_init (SD + audio + USB HID) | ~600 ms | SD ~200ms, USB bus reset 250ms + re-enum 150ms |
| machine_init + ROM loading | ~300 ms | SD reads for 3 ROM files (24 KB total) |
| hal_video_init (TFT) | ~200 ms | SPI display init + fillScreen |
| machine_reset | ~10 ms | |
| supervisor_init + NVS load | ~100 ms | |
| USB keyboard wait (up to 3s) | 0-3000 ms | Blocks until keyboard or timeout |
| CoCo BASIC ROM cold-start | ~2000 ms | Emulated CPU init at 26 FPS (~0.43x real-time) |

### Boot Optimizations

| Optimization | Time Saved | Effort |
|-------------|-----------|--------|
| OPT-7: Overlap USB wait | ~1-3 s | Medium |
| Reduce setup() `delay(500)` to 100ms | ~0.3 s | Easy |
| Increase SD SPI frequency | ~0.05-0.1 s | Easy |
| Defer supervisor/NVS state load | ~0.1-0.2 s | Easy |
| Fix DEBUG_ENABLED regression | ~0.05 s | Easy |

**Estimated boot time after all optimizations: ~3-4 seconds**

---

## 7. Projected FPS with All Remaining Optimizations

**Current measured (after OPT-16):**
- Text mode (static): **64 FPS** — exceeds DMA ceiling, CPU-bound frames are free
- Graphics mode (static): **45 FPS** — near DMA ceiling
- Graphics mode (scrolling VRAM): **27 FPS** — every frame is dirty, SPI is the bottleneck

**Key insight:** OPT-16 solved the static screen case. Remaining optimizations (OPT-2, OPT-13) matter most for scrolling/active graphics (27 FPS). OPT-9 (Core 0 SPI offload) becomes the highest-impact remaining optimization for scrolling.

| Optimization | Est. FPS Gain (scrolling) |
|-------------|--------------------------|
| OPT-2: DRAM zero-page mirror | +5-8 |
| OPT-13: Inline RAM fast path | +3-5 |
| OPT-14: ROM to DRAM | +0.5-1 |
| OPT-10: Adaptive frame skip | +1-2 |
| OPT-9: Core 0 video offload | +2-4 |

---

## 8. Memory Placement Reference

| Data | Location | Hot Path? | Status |
|------|----------|-----------|--------|
| MC6809 struct (44 bytes) | DRAM | Per-instruction | OK |
| Machine struct (~600 bytes) | DRAM | Per-instruction | OK |
| CoCo RAM (64 KB) | PSRAM | Per-instruction | **OPT-2: Split first 2KB to DRAM** |
| CoCo ROM (32 KB) | PSRAM | Per-fetch | **OPT-14: Move to DRAM** |
| VDG font (768 bytes) | DRAM | Per-scanline | DONE (OPT-4) |
| TFT sprite buffer (~98 KB) | PSRAM | Per-frame | Must stay in PSRAM |
| Disk images (~161 KB each) | PSRAM | On disk access | Must stay in PSRAM |

---

## 9. Re-Analysis Methodology

Use this section when re-running performance analysis after adding new features.

### Re-Run Checklist

1. Update the "Current baseline" FPS at the top of this document
2. Move newly completed optimizations to DONE status
3. Move newly rejected optimizations to REJECTED status
4. Check if new files were added that belong in the profiling list below
5. Verify constraints still apply (new audio backend? different display?)
6. Look for new hot paths introduced by the new feature
7. Re-validate that previous optimizations haven't regressed
8. **Test LOAD/LOADM after any memory-related changes** — this is a regression-prone area

### Constraints (DO NOT violate)

1. **Audio HAL must remain functional.** LEDC PWM timer ISR at 22,050 Hz on GPIO17 (`hal_audio.cpp`) provides cycle-accurate audio. Do not remove, disable, or desynchronize it.
2. **USB keyboard initialization waits are required.** The 250 ms bus reset + 150 ms re-enumeration in `usb_kbd_host.cpp` and the 3-second timeout in the main sketch are hardware requirements. Do not reduce.
3. **Cycle accuracy must be preserved for IRQ-dependent code.** The 60 Hz PIA0 CB1 timer IRQ drives SOUND, PLAY, TIMER, and keyboard scanner. Per-scanline CPU budget via fixed-point accumulator must stay correct.
4. **PSRAM disk cache must remain.** Entire disk images (~161 KB each) are cached in PSRAM to eliminate SD latency.
5. **Target hardware is ESP32-S3 with Arduino framework.** No ESP-IDF-only APIs unless wrapped for Arduino compatibility.
6. **Disk I/O integrity after memory changes.** Any optimization modifying memory layout or machine_read/write must be validated with LOAD and LOADM from disk.

### Files to Profile

```
TTGO-VGA32-COCO.ino
config.h
src/core/mc6809.cpp / mc6809.h / mc6809_opcodes.h
src/core/machine.cpp / machine.h
src/core/mc6821.cpp / mc6821.h
src/core/mc6847.cpp / mc6847.h        (CoCo 2)
src/core/sam6883.cpp / sam6883.h      (CoCo 2)
src/core/tcc1014.cpp / tcc1014.h      (CoCo 3)
src/hal/hal.h / hal.cpp
src/hal/hal_video.cpp
src/hal/hal_audio.cpp
src/hal/hal_keyboard.cpp
src/hal/usb_kbd_host.cpp
src/supervisor/sv_disk.cpp / sv_disk.h
```
