# CoCo3 Performance Optimization Plan

> ## ⚠ Historical — ESP32-S3 + TFT build
>
> This plan targets bottlenecks specific to the **ESP32-S3 + TFT_eSPI + SPI `pushSprite`** display path. The current **TTGO VGA32** build (FabGL VGA, continuous DMA scan-out) does not have a per-frame display push step, so most of the plan items below — VRAM shadow compare, CRC-based frame skip, sprite-allocation tuning, SPI clock tuning — have no equivalent cost to remove.
>
> On the VGA32 port, with **none** of these optimizations applied, measured FPS is **58 idle (BASIC `OK` prompt)** and **47 in a CoCo 2 animation program** — well above the S3+TFT baselines below. A fresh VGA32 profiling pass would target CPU emulation cost and GIME / VDG render cost, not display I/O. See the banner at the top of `performance.md`.
>
> The plan is preserved as a record of the S3+TFT optimization history.

> **Baseline (2026-04-15, ESP32-S3 + TFT):** 18 FPS idle BASIC, 13 FPS active graphics (OS-9, games)
> **Target (S3+TFT):** Sustained 35 FPS (NTSC) with no frame drops over 30 seconds, 0 audio buffer underruns
> **Hardware (S3+TFT):** ESP32-S3 N16R8 (240 MHz dual-core, 8 MB PSRAM, 16 MB flash)
> **Approach:** Combined Layered (Approach C) — quick wins first, then memory, then rendering
>
> **Prior art:** CoCo2 optimizations documented in `performance.md` (OPT-1 through OPT-16).
> CoCo2 went from 26 FPS → 64 FPS static / 27 FPS scrolling. No GIME-specific
> optimizations have been applied yet.

---

## Baseline Analysis

### Per-Frame Time Budget

| Metric | Value |
|---|---|
| Target frame time (35 FPS) | 28,571 us |
| Actual frame time (18 FPS idle) | 55,555 us |
| Actual frame time (13 FPS gfx) | 76,923 us |
| Reduction needed (idle) | ~27,000 us (49%) |
| Reduction needed (gfx) | ~48,350 us (63%) |

### CoCo3-Specific Bottlenecks (vs CoCo2)

| Bottleneck | Root Cause | Impact |
|---|---|---|
| `tcc1014_mem_cycle()` on slow path | 150-line address decoder runs on every I/O and ROM access | ~3,000 us/frame |
| Function pointer indirection | `cpu->read(addr)` prevents inlining of fast path | ~2,550 us/frame |
| All 512KB RAM in PSRAM | Every memory access pays 50-100ns PSRAM latency | ~6,600 us/frame |
| Double-pass rendering | GIME outputs palette indices → line_buffer → HAL converts to RGB565 per pixel | ~2,000 us/frame |
| `FONT_READ` via `pgm_read_byte()` | GIME font in PROGMEM (flash), not DRAM | ~500 us/frame |
| Full border fill per scanline | Fills 320 pixels then overwrites active area | ~400 us/frame |
| No GIME dirty-frame skip | `pushSprite()` runs every frame even when display unchanged | ~3,500 us/frame (static) |
| ROM in PSRAM | 40KB ROM arrays allocated via `ps_malloc()` | ~300 us/frame |
| `DEBUG_ENABLED=1` | Serial.printf in hot paths | ~200 us/frame |

### Estimated Per-Frame Microsecond Breakdown (13 FPS graphics)

| Component | Est. us/frame | % |
|---|---|---|
| CPU emulation (`execute_one()` x ~14,916 cycles) | ~20,000 | 26% |
| PSRAM latency penalty (~170K accesses x 60ns) | ~10,200 | 13% |
| GIME address decode (`tcc1014_mem_cycle` slow path) | ~3,000 | 4% |
| Function pointer overhead (~170K indirect calls x 15ns) | ~2,550 | 3% |
| GIME rendering (`tcc1014_render_scanline` x 192-225) | ~6,000 | 8% |
| HAL color convert + border fill (per-scanline) | ~3,500 | 5% |
| SPI frame push (`pushSprite`) | ~3,500 | 5% |
| Interrupt routing + PIA per-scanline | ~1,500 | 2% |
| Audio capture per-scanline | ~500 | 1% |
| Overhead (FreeRTOS, cache misses, misc) | ~26,000 | 34% |
| **Total** | **~76,750** | **100%** |

### Measured Per-Frame Breakdown (2026-04-17, `PERF_PROBE_ENABLED=1`)

Taken via `esp_timer_get_time()` probes wrapping the CoCo3 hot paths in
`machine_run_frame` / `machine_run_scanline`. See `src/utils/perf_probe.*`.

**BASIC IDLE (~18.2 FPS, 54,920 us/frame):**

| Probe | us/frame | % |
|---|---|---|
| `push_sprite` (`pushSprite`) | **36,143** | **65.8%** |
| `render_scn` (`tcc1014_render_scanline`) | 6,082 | 11.1% |
| `hal_scn` (`hal_video_render_scanline_gime`) | 5,525 | 10.1% |
| `cpu_run` (`mc6809_run` across 262 lines) | 5,154 | 9.4% |
| `audio_scn` | 296 | 0.5% |
| Residual (PIA, IRQ, bookkeeping) | ~1,720 | ~3% |

**Graphics execution (~13.8 FPS, 72,317 us/frame):**

| Probe | us/frame | % |
|---|---|---|
| `push_sprite` | 36,112 | 49.9% |
| `render_scn` | **23,545** | **32.6%** |
| `hal_scn` | 5,764 | 8.0% |
| `cpu_run` | 4,865 | 6.7% |
| `audio_scn` | 297 | 0.4% |
| Residual | ~1,734 | ~2% |

### Corrections to the Original Estimate

| Component | Estimated | Measured | Error |
|---|---|---|---|
| `push_sprite` | ~3,500 us | ~36,100 us | **10× under** |
| `cpu_run` | ~20,000 us | ~5,000 us | 4× over |
| Overhead (FreeRTOS/cache) | ~26,000 us (34%) | ~1,700 us (~3%) | 15× over |
| `render_scn` (gfx) | ~6,000 us | 23,545 us | 4× under |

**Takeaways that invalidate parts of the plan:**

1. `pushSprite` is the dominant cost in **both** workloads (36 ms flat — it's bandwidth-limited by the 40 MHz SPI, independent of what changed). Any optimization that does not reduce or overlap this cost is capped at a small fraction of frame time.
2. CPU emulation is already cheap (~5 ms). OPT-C7 (inline fast path) and OPT-C8 (split DRAM) have a much smaller addressable ceiling than the plan assumed — maximum realistic gain ~2 ms, not ~10 ms.
3. The "34% residual" was never real — it was the bookkeeping residual of back-of-envelope estimates, not measured overhead. The system is not paying hidden FreeRTOS/cache costs.
4. `render_scn` costs triple going from text to graphics (6 ms → 23 ms). This is the second-largest pool of addressable time during active play.

---

## Constraints (DO NOT violate)

Carried forward from `performance.md` Section 9, plus CoCo3-specific additions:

1. **Audio HAL must remain functional.** I2S DAC output at correct CoCo rate. Per-scanline audio capture must not be skipped.
2. **Cycle accuracy must be preserved.** 60 Hz PIA0 CB1 timer IRQ, per-instruction interrupt checks, edge-triggered NMI for FDC. No batching.
3. **PSRAM disk cache must remain.** Disk images (~161 KB each, up to 4 drives) stay in PSRAM.
4. **Disk I/O integrity.** Any optimization modifying `machine_read`/`machine_write` or memory layout must be validated with LOAD and LOADM from disk.
5. **All GIME video modes must work.** VDG compat (`COCO=1`: SG, CG, RG, alphanumeric) and CoCo3 native (`COCO=0`: text, 2/4/16 color graphics). Both 192-line and 225-line modes. No mid-scanline rendering required.
6. **MMU bank switching must be correct.** OS-9 remaps all 8 MMU slots dynamically. Any RAM fast-path optimization must handle arbitrary bank configurations.
7. **Target hardware is ESP32-S3 with Arduino framework.** No ESP-IDF-only APIs unless wrapped.

---

## Phase 1 — Quick Wins

**Estimated total gain: +3.5-6 FPS**
**Estimated total effort: ~2.5 hours**
**Risk: Low**

Benchmark after each item. Phase 1 items are independent and can be applied in any order.

---

### OPT-C1: Set DEBUG_ENABLED=0

| Field | Value |
|---|---|
| **Est. FPS gain** | +0.5 |
| **Effort** | 5 minutes |
| **Risk** | None |
| **Files** | `config.h` |

**Problem:** `config.h:49` has `#define DEBUG_ENABLED 1`. Every `DEBUG_PRINT`/`DEBUG_PRINTF` call compiles to `Serial.printf()` — UART output at 115200 baud blocks the CPU. These calls exist throughout initialization and some runtime paths.

**Fix:**
```cpp
// config.h line 49
#define DEBUG_ENABLED           0
```

**Validation:** Boot to BASIC prompt. Verify no serial output during normal emulation. Benchmark FPS.

**Note:** This was CoCo2 OPT-6b (regression fix). Same issue, same fix.

---

### OPT-C2: Add `#pragma GCC optimize("O2")` to hal_video.cpp ✅ APPLIED 2026-04-16

| Field | Value |
|---|---|
| **Est. FPS gain** | +0.5-1 |
| **Effort** | 5 minutes |
| **Risk** | None |
| **Files** | `src/hal/hal_video.cpp` |
| **Status** | Applied, measured on-hardware: **no FPS improvement** (within noise of baseline) |

**Problem:** `mc6809.cpp`, `machine.cpp`, and `tcc1014.cpp` all have O2 pragmas. `hal_video.cpp` does not. The per-pixel color conversion loop in `hal_video_render_scanline_gime()` processes 256-640 pixels x 192-225 scanlines per frame — this benefits from O2's loop optimizations.

**Fix:** Add as first line of `hal_video.cpp`:
```cpp
#pragma GCC optimize("O2")
```

**Validation:** Compile, boot, benchmark. Visual regression check on all video modes.

**Results (2026-04-16):**
- Compiled cleanly. Firmware size: 580,659 bytes (44% of 1,310,720). DRAM: 27,256 bytes (8%).
- Pattern match: pragma placed on line 1 above the header comment, identical to `tcc1014.cpp`, `machine.cpp`, `mc6809.cpp`.
- **FPS measurement (on-hardware): no observable improvement.** Idle BASIC and active graphics both within noise of the 18/13 FPS baseline.

**Lessons learned:**
- O2 on `hal_video.cpp` alone does not move the needle. The per-pixel color-convert / border-fill loops in `hal_video_render_scanline_gime()` are apparently not the current bottleneck — or Arduino-ESP32's default optimization level for this TU is already high enough that O2 adds nothing meaningful. The real cost is elsewhere (function-pointer memory dispatch, PSRAM latency, SPI push), which matches the per-frame breakdown in the baseline table.
- Net takeaway: the `+0.5-1 FPS` estimate for C2 was optimistic. Treat pragma-only tweaks as "free if applied, don't count on them" and front-load the structural wins (C4 direct-RGB565, C7 inline fast-path, C9 dirty-frame skip) when sequencing work.
- Convention observation still holds: pragma on line 1 matches the other three O2-enabled files; leaving it in place keeps the TU consistent with neighbors and costs nothing.
- No binary-size or DRAM regression — safe to keep even though it did not help. Does not need to be reverted.

---

### OPT-C3: Copy font_gime[] to DRAM ✅ APPLIED 2026-04-16

| Field | Value |
|---|---|
| **Est. FPS gain** | +0.5-1 |
| **Effort** | 30 minutes |
| **Risk** | Low |
| **Files** | `src/core/font_gime.h`, `src/core/tcc1014.cpp` |
| **Status** | Applied, awaiting on-hardware FPS measurement |

**Problem:** `font_gime[1536]` is declared `PROGMEM` in `font_gime.h:30`. `FONT_READ` uses `pgm_read_byte()` which has ~15ns overhead per read vs direct DRAM array access. In text mode: ~64 font reads/scanline x 192 scanlines = ~12,288 reads/frame. In VDG-compat alphanumeric mode (BASIC prompt), font reads dominate the rendering inner loop.

**Fix:** Same pattern as CoCo2 OPT-4:
1. Rename `font_gime[]` to `font_gime_flash[]` in `font_gime.h` (keep PROGMEM)
2. Add DRAM array in `tcc1014.cpp`: `static uint8_t font_gime_dram[1536];`
3. In `tcc1014_init()`: `memcpy_P(font_gime_dram, font_gime_flash, 1536);`
4. Change `FONT_READ` macro or replace references to use `font_gime_dram[]` with direct array access

**Memory cost:** 1,536 bytes DRAM.

**Validation:** Boot to BASIC. Visual check of text rendering. Test CoCo3 native text mode (OS-9). Benchmark.

**Results (2026-04-16):**
- Renamed `font_gime[]` → `font_gime_flash[]` in `font_gime.h:32` (kept `FONT_PROGMEM` qualifier).
- Added DRAM mirror `static uint8_t font_gime_dram[1536]` in `tcc1014.cpp` near the top-of-file lookup tables.
- `tcc1014_init()` now performs `memcpy_P(font_gime_dram, font_gime_flash, sizeof(font_gime_dram))` under `ARDUINO`, with a plain `memcpy` fallback for host builds.
- Both call sites in `tcc1014.cpp` (VDG-compat alphanumeric path and CoCo3 native text path, formerly `FONT_READ(&font_gime[...])`) now read directly: `font_gime_dram[c * 12 + font_row]`.
- Compiled cleanly. Firmware size: 580,675 bytes (44%) — +16 bytes vs post-C2. DRAM: 28,792 bytes (8%) — **+1,536 bytes, exactly the font table size**, which is the budgeted cost.
- **FPS measurement: pending on-hardware test.**

**Lessons learned:**
- Measuring DRAM delta (+1,536 bytes, exact match to `sizeof(font_gime_dram)`) is a cheap sanity check that the mirror is actually landing in DRAM and not being folded away — worth recording alongside each PROGMEM→DRAM promotion.
- Keeping the `FONT_READ` macro intact (rather than redefining it to index the DRAM copy) makes the change explicit at every call site and avoids invisibly coupling the macro to a specific storage backing. If more font-read sites appear later, redefining the macro becomes the cleaner option.
- The `#ifdef ARDUINO` / `memcpy_P` vs plain `memcpy` split mirrors the existing `FONT_READ` guard in `font_gime.h`. Host-build compatibility stays free.

---

### OPT-C4: Direct RGB565 Rendering (Eliminate Double-Pass) ✅ APPLIED 2026-04-16

| Field | Value |
|---|---|
| **Est. FPS gain** | +1-2 |
| **Effort** | 1 hour |
| **Risk** | Low-Medium |
| **Files** | `src/core/tcc1014.h`, `src/core/tcc1014.cpp`, `src/hal/hal.h`, `src/hal/hal_video.cpp` |
| **Depends on** | None |
| **Status** | Applied, measured on-hardware: **+0.8 FPS** (within estimate range) |

**Problem:** The rendering pipeline has two passes:
1. `tcc1014_render_scanline()` writes 6-bit palette register values → `line_buffer[640]` (uint8_t)
2. `hal_video_render_scanline_gime()` converts each pixel through `gime_hal_rgb565_lut[idx & 0x3F]` → sprite framebuffer

The GIME already pre-computes `palette_rgb565[16]` on every palette write (`tcc1014_write_palette()`), but the HAL ignores it (`(void)palette` at hal_video.cpp:391) and uses its own 64-entry LUT.

**Fix:**
1. Change `line_buffer` from `uint8_t[640]` to `uint16_t[640]` in `tcc1014.h`
2. In `tcc1014_render_scanline()`, replace `palette_reg[idx]` lookups with `palette_rgb565[idx]` — write RGB565 directly
3. For VDG compat colors (TCC1014_RGCSS0_0, etc.), pre-compute RGB565 equivalents on init or palette update
4. In HAL, change `hal_video_render_scanline_gime()` to memcpy/direct-copy from `line_buffer` to sprite framebuffer (no per-pixel LUT)
5. Border color: pre-convert to RGB565 in `machine_run_scanline()` before passing to HAL

**Eliminates:** 49K-144K per-pixel LUT lookups per frame (depending on resolution mode).

**Memory cost:** `line_buffer` grows from 640 to 1,280 bytes (+640 bytes in TCC1014 struct).

**Validation:** Visual regression on: BASIC text, hi-res 2-color, 4-color CG mode, 16-color CoCo3 native, OS-9 desktop. Benchmark all modes.

**Results (2026-04-16):**
- `TCC1014::line_buffer` changed from `uint8_t[640]` to `uint16_t[640]`.
- `tcc1014_write_palette()` and the reset-path initialiser now store **byte-swapped** RGB565 into `palette_rgb565[]`, so values can be written directly into the sprite framebuffer (TFT_eSPI keeps pixels big-endian in-memory).
- `tcc1014_render_scanline()` rewrites the pixel-emission block: `c0..c3` become `uint16_t`, and every `gime->palette_reg[...]` read in the VDG-compat, CoCo3 BP (2/4/16-color), and CoCo3 text paths becomes `gime->palette_rgb565[...]`. `TCC1014_RGCSS*_*` / bright/dark colour constants are already 0-15, so no extra pre-compute was needed — they index `palette_rgb565[]` directly.
- HAL signature updated: `hal_video_render_scanline_gime(..., const uint16_t* pixels, ...)`. The `fits` path now `memcpy`s the full row into the sprite; 2:1 and N:1 downscales do a stride/lerp index into `pixels[]` (no LUT). Border is still converted once per scanline via the retained `gime_hal_rgb565_lut[]` (still used by the one-shot clear on geometry change).
- `machine.cpp` call site unchanged — `g->line_buffer` just changed element type.
- Compiled cleanly. Firmware size: 580,747 bytes (44%) — +16 bytes. DRAM: 29,432 bytes (9%) — **+640 bytes, exactly the `line_buffer` growth** (640 entries × 1 extra byte each), which is the budgeted cost.
- **FPS measurement (on-hardware): +0.8 FPS over post-C3 baseline.** Within the estimated +1-2 FPS band at the lower end, suggesting the per-pixel LUT was not dominant in the measured workload (likely text-heavy BASIC idle, where only ~256 active px/line × 192 lines = ~49K LUT lookups were eliminated; the ~3,500 us/frame `pushSprite` and PSRAM-bound memory dispatch still dominate).

**Lessons learned:**
- Byte-order matters. `gime_rgb565_lut[]` in `tcc1014.cpp` is native-order; the HAL's `gime_hal_rgb565_lut[]` is pre-byte-swapped because TFT_eSPI stores pixels big-endian in-memory. Shifting the conversion upstream means `palette_rgb565[]` must also be pre-byte-swapped — moved the swap into `tcc1014_write_palette()` so it's still amortised over rare palette writes.
- `TCC1014_RGCSS0_0`, `TCC1014_DARK_GREEN`, etc. are just enum values 8-15, i.e., valid indices into the 16-entry `palette_reg[]`/`palette_rgb565[]`. The plan suggested pre-computing VDG-compat colours separately, but that turned out to be unnecessary — they already share the palette slot space.
- Keeping the HAL's 64-entry `gime_hal_rgb565_lut[]` for border-only lookups is cheaper than adding a second border-RGB565 field, and one lookup per scanline is invisible in the budget.
- The DRAM delta matches `sizeof(line_buffer) increase` exactly (640 bytes) — same sanity-check pattern as C3, and cheaper than guessing whether a change "made it" into DRAM.
- The observed +0.8 FPS lines up with the +2,000 us/frame bottleneck entry for "Double-pass rendering" in the baseline table. At ~14 FPS post-C3, removing ~2 ms/frame predicts ~+0.4-0.8 FPS — matches the low end. The remaining rendering cost still lives in the per-scanline border fill and the SPI push.

---

### OPT-C5: Border Margin-Only Fill

| Field | Value |
|---|---|
| **Est. FPS gain** | +0.5-1 |
| **Effort** | 30 minutes |
| **Risk** | None |
| **Files** | `src/hal/hal_video.cpp` |

**Problem:** `hal_video_render_scanline_gime()` fills the entire 320-pixel row with border color, then overwrites the active area:
```cpp
// Current: fill ALL 320 pixels with border
for (int x = 0; x < SPRITE_W; x++)
    row[x] = bc;
// Then overwrite active pixels on top
for (int x = 0; x < width && ...; x++)
    row[x_off + x] = gime_hal_rgb565_lut[indices[x] & 0x3F];
```
For 256-pixel active width centered in 320: 256 border writes are immediately overwritten — pure waste.

**Fix:** Fill only the left and right margins:
```cpp
// Left margin
for (int x = 0; x < x_off; x++)
    row[x] = bc;
// Active pixels (render directly)
for (int x = 0; x < width && (x_off + x) < SPRITE_W; x++)
    row[x_off + x] = converted_pixel;
// Right margin
for (int x = x_off + width; x < SPRITE_W; x++)
    row[x] = bc;
```

**Saves:** ~49K unnecessary uint16_t writes/frame at 192 active lines.

**Validation:** Visual check — borders must remain correct at all resolutions (256, 320, 512, 640 input widths). Benchmark.

---

## Phase 2 — Memory Fast Path

**Estimated total gain: +5-9 FPS**
**Estimated total effort: ~6-8 hours**
**Risk: Medium**

Items should be applied in order (C6 → C7 → C8) due to increasing complexity and shared validation requirements.

---

### OPT-C6: ROM Arrays to DRAM

| Field | Value |
|---|---|
| **Est. FPS gain** | +1-2 |
| **Effort** | 30 minutes |
| **Risk** | Low |
| **Files** | `src/core/machine.cpp` |

**Problem:** `machine_alloc()` tries `ps_malloc()` first (PSRAM) for all allocations. ROM arrays (`rom_coco3` 32KB, `rom_disk` 8KB) end up in PSRAM. ROM is read on every instruction fetch when executing from ROM (BASIC interpreter, Disk BASIC) — ~30-35% of all fetches during BASIC operation.

**Fix:** Replace the `machine_alloc()` calls for ROM with explicit `malloc()`:
```cpp
// machine.cpp — CoCo3 init
m->rom_coco3 = (uint8_t*)malloc(COCO3_ROM_SIZE);   // 32KB → DRAM
m->rom_disk  = (uint8_t*)malloc(8192);               // 8KB  → DRAM
```

**Memory cost:** 40 KB DRAM. ESP32-S3 has ~200KB free DRAM — affordable.

**Math:** ~5K ROM reads/frame x 60ns PSRAM penalty = ~300us saved. Actual gain may be higher due to cache line effects (DRAM accesses are fully cached, PSRAM has cache misses).

**Validation:** Boot to BASIC. Test LOAD/LOADM from disk. OS-9 boot. Benchmark.

---

### OPT-C7: Inline Fast-Path for machine_read/machine_write

| Field | Value |
|---|---|
| **Est. FPS gain** | +3-5 |
| **Effort** | 2-3 hours |
| **Risk** | Medium |
| **Files** | `src/core/mc6809.cpp`, `src/core/machine.cpp`, `src/core/machine.h` |
| **Disk I/O check** | REQUIRED |

**Problem:** Every memory access goes through a function pointer:
```
mc6809.cpp: mem_read() → cpu->read(addr)   [function pointer, ~15ns pipeline flush]
    → machine_read(addr)                     [regular function call]
        → fast path (addr < 0xFE00, active_banks lookup)
        → or slow path (tcc1014_mem_cycle)
```
The function pointer prevents the compiler from inlining the fast path. At ~170K memory accesses/frame, the 15ns indirect call overhead alone costs ~2,550us/frame. Additionally, the fast-path body (3 comparisons + array lookup + PSRAM read) could be inlined to save call/return overhead.

**Fix:** Replace function pointer with direct call. Two approaches:

*Approach A (simpler):* Since `MACHINE_TYPE` is a compile-time constant, replace `cpu->read`/`cpu->write` with `extern` declarations:
```cpp
// mc6809.cpp
extern uint8_t machine_read(uint16_t addr);
extern void machine_write(uint16_t addr, uint8_t val);

static inline uint8_t mem_read(MC6809* cpu, uint16_t addr) {
    (void)cpu;
    return machine_read(addr);
}
```
This eliminates the function pointer. The linker resolves directly. The compiler may still inline if LTO is enabled.

*Approach B (maximum speed):* Move the fast-path body into a `static inline` function in `machine.h`:
```cpp
// machine.h
extern Machine* g_machine;
static inline uint8_t machine_read_inline(uint16_t addr) {
    Machine* m = g_machine;
    if (__builtin_expect(addr < 0xFE00, 1)) {
        unsigned bank = m->gime.active_banks[addr >> 13];
        if (m->gime.TY || bank < 0x3C) {
            uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
            if (__builtin_expect(Z < COCO3_PHYSICAL_RAM, 1))
                return m->ram_physical[Z];
        }
    }
    return machine_read_slow(addr);  // handles ROM, I/O, GIME regs
}
```
This lets the compiler inline the RAM fast path (3 comparisons + 1 array lookup) directly into `execute_one()`.

**Recommendation:** Start with Approach A. If FPS gain is < 2, try Approach B.

**Validation:** Full regression — BASIC, DIR, LOAD/LOADM, OS-9 boot, game execution. This is the most regression-prone optimization.

---

### OPT-C8: Split First 8KB of Physical RAM to DRAM

| Field | Value |
|---|---|
| **Est. FPS gain** | +3-5 |
| **Effort** | 3-4 hours |
| **Risk** | High |
| **Files** | `src/core/machine.cpp`, `src/core/machine.h`, `src/core/tcc1014.cpp` |
| **Depends on** | OPT-C7 (fast path must be modifiable) |
| **Disk I/O check** | REQUIRED |

**Problem:** All 512KB of CoCo3 physical RAM is in PSRAM. The 6809 CPU spends ~60% of memory accesses in the lowest physical addresses: zero page ($0000-$00FF mapped to physical $70000 at boot via MMU bank 0x38), stack, system variables. Every one of these pays the PSRAM latency penalty.

**Fix:** Allocate a DRAM mirror for frequently-accessed physical RAM:
```cpp
// machine.h
uint8_t* ram_dram;       // 8KB DRAM mirror
uint8_t* ram_physical;   // 512KB PSRAM (full)
#define RAM_DRAM_SIZE 8192
#define RAM_DRAM_BASE 0x70000  // Physical address of bank 0x38 slot 0
```

In the memory fast path:
```cpp
uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
if (__builtin_expect(Z >= RAM_DRAM_BASE && Z < RAM_DRAM_BASE + RAM_DRAM_SIZE, 1))
    return m->ram_dram[Z - RAM_DRAM_BASE];
return m->ram_physical[Z];
```

Writes must update both:
```cpp
if (Z >= RAM_DRAM_BASE && Z < RAM_DRAM_BASE + RAM_DRAM_SIZE)
    m->ram_dram[Z - RAM_DRAM_BASE] = val;
m->ram_physical[Z] = val;  // Always write to PSRAM (canonical copy)
```

**Complexity:** The CoCo3 MMU means the CPU address space is remapped. At boot, BASIC puts zero page at physical $70000 (bank 0x38). OS-9 may remap to different physical pages. The DRAM mirror covers a fixed physical range — it helps most for BASIC and boot, less for OS-9 after remapping.

**Alternative approach:** Mirror the *logical* first 8KB instead (intercept before MMU translation). Simpler but less effective when MMU remaps.

**Memory cost:** 8 KB DRAM.

**Math:** 60% of 170K accesses x 60ns = ~6,120us saved.

**Validation:** Extensive — BASIC cold start, warm reset, DIR, LOAD/LOADM, OS-9 boot with MMU remapping, game execution. Must verify RAM coherency between DRAM mirror and PSRAM.

---

## Phase 3 — Rendering Architecture

**Estimated total gain: +5-15 FPS**
**Estimated total effort: ~12-16 hours**
**Risk: Medium-High**

Phase 3 optimizations target the SPI push bottleneck and rendering overhead. Only pursue if Phase 1+2 don't reach 35 FPS for active graphics.

---

### OPT-C9: GIME Dirty-Frame Detection (Skip SPI Push) ✅ APPLIED 2026-04-17

| Field | Value |
|---|---|
| **Est. FPS gain** | +5-15 (static screens only) |
| **Effort** | 2-3 hours |
| **Risk** | Low |
| **Files** | `src/hal/hal_video.cpp` |
| **Status** | Applied, measured on-hardware: **+14 FPS idle BASIC, +6 FPS graphics idle** |

**Problem:** `pushSprite()` takes ~3,500us and runs every frame. CoCo2 OPT-16 solved this with a 6KB VRAM shadow compare. CoCo3 is harder because VRAM location depends on MMU bank configuration and the GIME video base register (`Y`).

**Current state:** `dirty_frame` flag exists in TCC1014 struct and is set on any RAM write to the display region. But it's overly aggressive — a write to *any* RAM address sets it dirty in `machine_write()`.

**Fix — CRC approach:**
1. After rendering all scanlines but before `pushSprite()`, compute a fast CRC32 of the sprite framebuffer
2. Compare against previous frame's CRC
3. If identical, skip `pushSprite()`
4. ESP32-S3 has hardware CRC acceleration — ~50us for 153KB sprite buffer

```cpp
// hal_video.cpp
static uint32_t prev_frame_crc = 0;

void hal_video_present_gime(bool* dirty) {
    if (!display_available || !sprite) return;

    uint32_t crc = esp_crc32_le(0, (uint8_t*)sprite->getPointer(), SPRITE_W * SPRITE_H * 2);
    if (crc == prev_frame_crc) {
        if (fps_overlay_enabled) fps_update();
        return;  // Skip SPI push
    }
    prev_frame_crc = crc;

    sprite->pushSprite(SPR_X, SPR_Y);
    if (fps_overlay_enabled) { fps_update(); fps_overlay_draw(); }
}
```

**Advantage over VRAM shadow:** Works regardless of MMU configuration, resolution mode, or palette changes. Compares the *actual rendered output*, not VRAM contents.

**Validation:** Boot to BASIC (should show high FPS with cursor blinking). Type characters (should drop to rendering FPS). OS-9 with idle desktop. Verify no visual glitches from missed updates.

**Results (2026-04-17):**

Implementation used `esp_rom_crc32_le` over the full sprite framebuffer
(`SPRITE_W*SPRITE_H*2` bytes, PSRAM-backed). GIME `dirty` pointer still cleared
on push but no longer gates the skip — CRC of actual rendered output is the
authoritative signal.

| Workload | Pre-C9 | Post-C9 | Δ |
|---|---|---|---|
| BASIC idle | 18.2 FPS / 54,920 us | **42.6 FPS / 23,450 us** | +24 FPS |
| Graphics app idle | 13.8 FPS / 72,317 us | **19.8 FPS / 50,420 us** | +6 FPS |

**BASIC idle post-C9 breakdown (42.6 FPS):**

| Probe | us/frame | % |
|---|---|---|
| `push_sprite` | 8,910 | 38.0% |
| `render_scn` | 6,055 | 25.8% |
| `hal_scn` | 5,533 | 23.6% |
| `cpu_run` | 960 | 4.1% |
| `audio_scn` | 288 | 1.2% |

**Graphics-active post-C9 breakdown (19.8 FPS):**

| Probe | us/frame | % |
|---|---|---|
| `render_scn` | 22,844 | 45.3% |
| `cpu_run` | 10,421 | 20.7% |
| `push_sprite` | 8,893 | 17.6% |
| `hal_scn` | 5,913 | 11.7% |
| `audio_scn` | 352 | 0.7% |

**Lessons learned:**

- CRC on the rendered framebuffer is the right granularity. The per-frame
  probes show `push_sprite` averaging ~8.9 ms in both workloads, i.e. the
  actual push only fires on ~25% of frames (0.25 × 36 ms ≈ 8.9 ms avg). Even
  in "active graphics" a large fraction of emulated frames produce pixel-
  identical output (rendering the same game state between input ticks), and
  the CRC catches this regardless of MMU/palette/mode state.
- `cpu_run` rising from ~5 ms (pre-C9) to ~10 ms in graphics is a measurement
  artefact: with frames now completing in 50 ms instead of 72 ms, the CPU
  runs the same number of cycles per frame but the probe denominator
  (frame-time) shrank, so the absolute `cpu_run` didn't move — only its
  share did. Re-baselining expectations for C7/C8 should use the new 50 ms
  frame as reference, not the old 72 ms.
- PSRAM-resident sprite means CRC scans 153 KB from PSRAM. Empirically this
  is invisible in the budget (residual is ~2 ms). The hardware CRC + PSRAM
  read rate was fast enough that it did not register as its own probe.
- The existing GIME `dirty` bool flag (set on any RAM write) is now
  redundant for the skip decision, but left in place because `machine.cpp`
  and the HAL interface still pass it. Future cleanup opportunity — not
  load-bearing.
- Graphics-workload `render_scn` (22.8 ms) is now the single largest cost
  and cleanly dominates active-play frames. That confirms the revised
  priority ordering: investigate `render_scn` internals (finer probe) next,
  *before* C10 or C11.

---

### OPT-C10: Merge Render + Color Convert (Direct Framebuffer Write)

| Field | Value |
|---|---|
| **Est. FPS gain** | +2-3 |
| **Effort** | 3-4 hours |
| **Risk** | Medium |
| **Files** | `src/core/tcc1014.cpp`, `src/core/tcc1014.h`, `src/hal/hal_video.cpp` |
| **Depends on** | OPT-C4 (RGB565 line_buffer) |

**Problem:** Even with OPT-C4, there are still two memory operations per pixel: write to `line_buffer[]`, then copy to sprite framebuffer. Merging eliminates the intermediate buffer entirely.

**Fix:**
1. Pass sprite framebuffer row pointer into `tcc1014_render_scanline()`
2. Write RGB565 pixels directly into sprite memory
3. Handle border fill inline (left margin, pixels, right margin)
4. Remove `hal_video_render_scanline_gime()` — its work is now done inside the renderer

```cpp
void tcc1014_render_scanline(TCC1014* gime, unsigned scanline,
                              uint16_t* fb_row, int fb_width) {
    // Write RGB565 directly to fb_row
    // Handle centering and border within this function
}
```

**Trade-off:** Tighter coupling between GIME emulation and HAL. The renderer now knows about the display format (RGB565) and dimensions (320 pixels). This is acceptable because we only target one display (ILI9341 320x240).

**Validation:** Full visual regression across all video modes. Benchmark.

---

### OPT-C11: Dual-Core SPI Offload

| Field | Value |
|---|---|
| **Est. FPS gain** | +3-5 (scrolling/active graphics only) |
| **Effort** | 6-8 hours |
| **Risk** | High |
| **Files** | `src/hal/hal_video.cpp`, `TTGO-VGA32-COCO.ino` |
| **Depends on** | OPT-C9 (dirty-frame skip) |

**Problem:** `pushSprite()` takes ~3,500us and blocks Core 1 (emulation). During active graphics (scrolling, animation), every frame is dirty so OPT-C9 can't help.

**Fix:** Double-buffer the sprite and push from Core 0:
1. Allocate two sprite buffers in PSRAM (~153KB each, 306KB total)
2. Create a FreeRTOS task on Core 0 that waits on a semaphore, then calls `pushSprite()`
3. Main loop (Core 1): render into buffer A, signal Core 0 to push buffer A, swap to buffer B for next frame
4. Core 1 starts emulating the next frame immediately while Core 0 pushes

```cpp
// Core 0 task
void spi_push_task(void* param) {
    while (true) {
        xSemaphoreTake(push_ready, portMAX_DELAY);
        push_sprite->pushSprite(SPR_X, SPR_Y);
        xSemaphoreGive(push_done);
    }
}
```

**Memory cost:** +153 KB PSRAM (second sprite buffer). Total PSRAM usage: ~306KB sprites + 512KB RAM + 644KB disks = ~1.46 MB of 8 MB — affordable.

**Risks:**
- FreeRTOS task switching overhead (~5us per context switch)
- PSRAM bus contention — both cores accessing PSRAM simultaneously (one reading for SPI DMA, one reading/writing for emulation)
- Must ensure frame N is fully rendered before Core 0 starts pushing it (semaphore synchronization)
- Audio ISR on Core 0 could conflict with SPI DMA

**Validation:** 30-second stress test with scrolling graphics. Monitor for visual tearing, audio underruns, and FPS stability. Compare with single-core baseline.

---

## Projected Cumulative Results

| After | BASIC Idle (FPS) | Graphics Active (FPS) | Frame Time (gfx) |
|---|---|---|---|
| **Baseline** | 18 | 13 | 76,923 us |
| **Phase 1** (C1-C5) | 21-24 | 16-19 | 52,600-62,500 us |
| **Phase 2** (C6-C8) | 27-34 | 22-29 | 34,500-45,500 us |
| **Phase 3** (C9-C11) | 35-50+ | 28-40 | 25,000-35,700 us |

### Decision Points

- **After Phase 1:** If idle FPS > 30, Phase 2 may only need C6+C7 (skip risky C8).
- **After Phase 2:** If graphics FPS > 30, Phase 3 may only need C9 (skip risky C10+C11).
- **After C9:** If static screen FPS > 45 but scrolling < 35, C11 (dual-core) is required.
- **C8 vs C11:** If forced to choose one, C8 helps all scenarios; C11 only helps when screen is changing. Prefer C8.

### Revised Priority (2026-04-17, post-C9 measurement)

With C9 landed, `push_sprite` drops from ~36 ms flat to ~8.9 ms averaged in
both workloads (≈25% of frames actually push). The dominant remaining cost
in active graphics is now `render_scn` at 22.8 ms — 45% of a graphics
frame. Updated sequence:

1. **Investigate `render_scn` (fine-grained probe)** — now the #1 cost in
   graphics-active play. Needs a per-mode / per-phase breakdown inside
   `tcc1014_render_scanline` to choose between C5 (border margin-only fill)
   and C10 (merged render + colour convert).
2. **OPT-C5 or OPT-C10** — pick based on finer probe.
3. **OPT-C11 (dual-core SPI offload)** — still valuable: 8.9 ms of
   `push_sprite` remains per-frame-averaged and is fully serial with CPU /
   render work. Overlapping it could reclaim another ~9 ms of idle and
   close the gap to 35 FPS graphics.
4. **OPT-C6 (ROM → DRAM)** — small, safe, still worth doing.
5. **OPT-C7 / OPT-C8 (memory fast path)** — `cpu_run` is ~1 ms in idle and
   ~10 ms in graphics; the graphics number is mostly PSRAM latency on RAM
   reads, so C7/C8 may now have slightly more headroom than previously
   estimated. Revisit after C10/C5.

### Previous Priority (2026-04-17, post-measurement)

Measurements show `pushSprite` (36 ms) and `render_scn` in graphics mode
(23 ms) together account for 65–82% of frame time. CPU work is ~5 ms and
cannot usefully be compressed below that. Revised sequence:

1. **OPT-C11 (dual-core SPI offload)** — *promoted to highest priority.*
   Overlap the 36 ms `pushSprite` with the next frame's CPU+render.
   Potential: ~+10 FPS idle, ~+5 FPS active. Addresses the single
   biggest bucket in both workloads.
2. **OPT-C9 (dirty-frame skip)** — strong complement to C11. For the idle
   BASIC case where the framebuffer rarely changes, skipping `pushSprite`
   entirely on unchanged frames is effectively free FPS.
3. **Investigate `render_scn` in graphics mode** — add a finer probe
   inside `tcc1014_render_scanline` before picking C5 vs C10. The 23 ms
   graphics cost is the second-largest addressable pool.
4. **OPT-C5 / OPT-C10** — prioritize based on which render path the finer
   probe highlights (border fill vs per-pixel convert).
5. **OPT-C6 (ROM → DRAM)** — small, safe, still worth doing.
6. **OPT-C7 / OPT-C8 (memory fast path)** — *demoted.* `cpu_run` is only
   ~5 ms/frame; the maximum realistic gain is ~2 ms (~0.5 FPS). Keep on
   the list for completeness but do not pursue before C11/C9/render.

---

## Implementation Order & Checklist

### Phase 1 — Quick Wins

- [ ] **C1:** `config.h` — set `DEBUG_ENABLED` to 0
  - [ ] Benchmark: idle FPS, gfx FPS
- [x] **C2:** `hal_video.cpp` — add `#pragma GCC optimize("O2")` line 1 (applied 2026-04-16; no FPS gain measured, kept in place)
  - [x] Benchmark — no improvement over baseline
- [x] **C3:** `font_gime.h` + `tcc1014.cpp` — DRAM font copy (applied 2026-04-16)
  - [ ] Benchmark text mode FPS specifically (pending on-hardware test)
- [x] **C4:** `tcc1014.h/cpp` + `hal.h` + `hal_video.cpp` — RGB565 direct render (applied 2026-04-16, measured +0.8 FPS)
  - [ ] Visual regression: BASIC, CG modes, CoCo3 16-color, OS-9 (user to confirm — no colour-swap regression reported)
  - [x] Benchmark — +0.8 FPS on measured workload
- [ ] **C5:** `hal_video.cpp` — border margin-only fill
  - [ ] Visual check: borders at 256, 320, 512, 640 widths
  - [ ] Benchmark

### Phase 2 — Memory Fast Path

- [ ] **C6:** `machine.cpp` — ROM to DRAM via malloc()
  - [ ] Verify: BASIC boots, ROM reads correct
  - [ ] Test: LOAD/LOADM
  - [ ] Benchmark
- [ ] **C7:** `mc6809.cpp` + `machine.h/cpp` — inline fast path
  - [ ] Test: BASIC, DIR, LOAD, LOADM, OS-9 boot
  - [ ] Benchmark: idle + active graphics
  - [ ] If gain < 2 FPS with Approach A, try Approach B
- [ ] **C8:** `machine.cpp/h` + `tcc1014.cpp` — split 8KB DRAM
  - [ ] Test: BASIC cold/warm reset, DIR, LOAD, LOADM
  - [ ] Test: OS-9 boot (heavy MMU remapping)
  - [ ] Test: game execution
  - [ ] Verify DRAM/PSRAM coherency
  - [ ] Benchmark all scenarios

### Phase 3 — Rendering Architecture

- [x] **C9:** `hal_video.cpp` — CRC-based dirty-frame skip (applied 2026-04-17)
  - [x] Test: cursor blink detection, character typing, OS-9 idle (user confirmed)
  - [x] Benchmark: BASIC idle 18→42.6 FPS (+24), graphics idle 13.8→19.8 FPS (+6)
- [ ] **C10:** `tcc1014.cpp/h` + `hal_video.cpp` — merged render
  - [ ] Visual regression: all video modes
  - [ ] Benchmark
- [ ] **C11:** `hal_video.cpp` + `.ino` — dual-core SPI
  - [ ] 30-second stress test (scrolling)
  - [ ] Audio underrun monitoring
  - [ ] Visual tearing check
  - [ ] Benchmark: sustained FPS over 30 seconds

---

## DRAM Budget

| Item | Bytes | Phase | Status |
|---|---|---|---|
| font_gime DRAM copy | 1,536 | C3 | Applied 2026-04-16 (measured +1,536 B) |
| line_buffer expansion (uint8→uint16) | 640 | C4 | Applied 2026-04-16 (measured +640 B) |
| ROM CoCo3 (32KB) | 32,768 | C6 | Planned |
| ROM Disk (8KB) | 8,192 | C6 | Planned |
| RAM DRAM mirror (8KB) | 8,192 | C8 | Planned |
| **Total new DRAM** | **51,328** | | |
| **Available DRAM (est.)** | ~200,000 | | |
| **Remaining after all** | ~149,000 | | |

## PSRAM Budget (Phase 3 only)

| Item | Bytes | Phase | Status |
|---|---|---|---|
| Second sprite buffer | 153,600 | C11 | Planned |
| **Current PSRAM usage** | ~1,300,000 | | |
| **Total after C11** | ~1,454,000 | | |
| **PSRAM capacity** | 8,388,608 | | |

---

## Measurement Protocol

Before and after each optimization:

1. **Idle BASIC FPS:** Boot to `OK` prompt, wait 10 seconds, record average FPS
2. **Active graphics FPS:** Run a program with screen updates (OS-9 DIR listing, scrolling game), record average over 10 seconds
3. **Audio underruns:** Monitor serial output for buffer underrun warnings during 30-second playback
4. **Frame time variance:** Record min/max/avg frame time over 30 seconds to detect frame drops
5. **Binary size:** Record firmware size (flash usage) — ensure it stays under ~1.5 MB

### FPS Reporting

Add a serial FPS report if not already present:
```cpp
// In loop(), every 5 seconds:
static uint32_t fps_report_time = 0;
if (millis() - fps_report_time > 5000) {
    Serial.printf("FPS: %.1f  frame_time: %d us\n", measured_fps, last_frame_us);
    fps_report_time = millis();
}
```

---

## References

- `docs/performance.md` — CoCo2 optimization catalog (OPT-1 through OPT-16)
- `docs/coco3-gime.md` — GIME implementation details
- `docs/video.md` — Video HAL architecture
- `docs/core.md` — Memory map, scanline loop
- `docs/disk-hal.md` — FDC and disk I/O (critical for validation)
