# CoCo 3 GIME (TCC1014) — Porting Guide & Reference

## Overview

The **TCC1014 GIME** (Graphics Interrupt Memory Enhancement) chip is the heart of the CoCo 3. It replaces the SAM6883 + MC6847 VDG combination from CoCo 2 and adds:

- 512 KB physical RAM with 8-bank MMU (64 × 8 KB pages)
- 16-color programmable palette (64 possible RGB colors)
- Native graphics modes up to 640 pixels wide
- Integrated interrupt controller (VBORD, HBORD, TMR, EI0/EI1/EI2)
- VDG compatibility mode (runs CoCo 2 software unmodified)

### Porting Strategy

Rather than rewriting GIME emulation from scratch, the ESP32 port adapts XRoar's battle-tested CoCo3 implementation. Key adaptations:

| XRoar Concept | ESP32 Replacement |
|---|---|
| `DELEGATE_T1(void, bool)` | Direct function calls |
| `event_queue` + `event_current_tick` | Scanline-based timing |
| `struct part` / `part_create()` | Flat C struct + init function |
| `VRAM fetch delegate` | Direct `ram_physical[addr]` reads |
| `render_line delegate` → `pixel_data[912]` | Write to `line_buffer[640]` for HAL |

### XRoar Source Files Ported

| XRoar File | Lines | Content |
|---|---|---|
| `tcc1014/tcc1014.c` | ~1550 | GIME registers, MMU, address decode, rendering, timer, interrupts |
| `tcc1014/tcc1014.h` | ~94 | Public interface / struct fields |
| `tcc1014/font-gime.c` | ~200 | 1536-byte GIME font ROM (128 chars × 12 rows) |
| `coco3.c` | ~1504 | Machine wiring, memory dispatch, ROM mapping, interrupt routing |

### ESP32 Constraints

| Resource | Available | CoCo3 Needs |
|----------|-----------|-------------|
| PSRAM | 4 MB (TTGO VGA32) / 8 MB (ESP32-S3) | 512 KB RAM + 32 KB ROM = 544 KB (≈ 14% / 7%) |
| Display | VGA 640×200 (FabGL) | CoCo3 hires 640-wide modes are 1:1; narrower modes are pixel-doubled |
| CPU speed | ~2.6× real-time | GIME decode adds overhead per memory access |
| Frame rate | ~25 FPS (graphics) | GIME rendering heavier — profiling ongoing |

---

## GIME Architecture

### Register Map ($FF90–$FF9F)

| Addr | Reg | Name | Fields |
|------|-----|------|--------|
| $FF90 | 0 | INIT0 | COCO[7] MMUEN[6] IEN[5] FEN[4] MC3[3] MC2[2] MC1[1] MC0[0] |
| $FF91 | 1 | INIT1 | TINS[5] TR[0] |
| $FF92 | 2 | IRQ Enable | TMR[5] HBORD[4] VBORD[3] EI2[2] EI1[1] EI0[0] |
| $FF93 | 3 | FIRQ Enable | TMR[5] HBORD[4] VBORD[3] EI2[2] EI1[1] EI0[0] |
| $FF94 | 4 | Timer MSB | bits 11:8 |
| $FF95 | 5 | Timer LSB | bits 7:0 |
| $FF98 | 8 | VMODE | BP[7] BPI[5] MOCH[4] H50[3] LPR[2:0] |
| $FF99 | 9 | VRES | LPF[7:5] HRES[4:2] CRES[1:0] |
| $FF9A | A | BRDR | Border color (6-bit GIME color index) |
| $FF9C | C | VSC | Vertical scroll (4-bit) |
| $FF9D | D | Y MSB | VRAM base address bits 18:11 |
| $FF9E | E | Y LSB | VRAM base address bits 10:3 |
| $FF9F | F | HOFF | HVEN[7] X[6:0] — horizontal virtual enable + offset |

**Key INIT0 fields:**
- `COCO=1` — VDG compatibility mode (CoCo 2 behavior); `COCO=0` — GIME native mode
- `MMUEN` — enables the 8-bank MMU
- `IEN/FEN` — master IRQ/FIRQ enable bits
- `MC3` — constant RAM at $FE00-$FEFF (overrides MMU; used by OS-9)
- `MC1/MC0` — ROM mapping (internal ROM, external CTS, or all-RAM)

### MMU Translation

The MMU maps the 64 KB CPU address space to 512 KB physical RAM using 8 × 8 KB banks per task. Two task register sets (Task 0 and Task 1) are selected by the `TR` bit in INIT1.

```
MMU Bank Registers: $FFA0-$FFAF
  Task 0: $FFA0-$FFA7 (banks 0-7)
  Task 1: $FFA8-$FFAF (banks 0-7)

Physical address = bank_number[6-bit] × 8192 + (addr & 0x1FFF)
Maximum physical: 64 banks × 8 KB = 512 KB
```

At reset: identity map — banks 0x38 through 0x3F (CPU $0000-$FFFF = physical $70000-$7FFFF).

#### RAM sizes: 128 KB, 512 KB, 1 MB, 2 MB

The size is chosen in F3 → Setup → Machine → CoCo 3 Memory (`g_coco3_ram_kb`, NVS `"sv"` / `coco3_ram`, default `COCO3_RAM_KB` = 512) and applied at boot. `tcc1014_set_ram()` gives the GIME the buffer and derives the masks below.

| Size | MMU register bits | Behaviour |
|---|---|---|
| 128 KB | 6 | Only 16 pages exist; the top address lines are unconnected, so every bank number mirrors onto them (`$30` and `$00` are the same RAM). Video addresses wrap at 128 KB. |
| 512 KB | 6 | The stock maximum: 64 pages. |
| 1 MB | 7 | A memory expansion board: bit 6 of each MMU register selects the second 512 KB. |
| 2 MB | 8 | Bits 7-6 select one of four 512 KB banks. |

The 1 MB and 2 MB models follow XRoar's "DAT" board:

- The extra bits are stored with the bank number (`mmu_bank[]` is 8 bits wide, masked by `bank_mask`) and read back from `$FFA0-$FFAF`.
- ROM select uses the GIME's own six bits only, so page `$3C-$3F` of any bank is ROM while TY = 0.
- With the MMU off, and for `$FE00-$FEFF` with MC3 set, the address is in bank 0 (`$38-$3F`).
- Video is read from one 512 KB bank, chosen by `$FF9B` bits 1-0 (`vram_base`); the register reads back on these sizes only. Reset returns it to bank 0.

Every physical address is wrapped with `ram_mask` (`ram_size - 1`), in `machine_read_coco3()` / `machine_write_coco3()` and in the CPU's direct page tables, so no access can leave the buffer.

RAM comes from PSRAM. 2 MB takes half of the board's 4 MB, shared with the disk caches (about 161 KB per mounted 35-track disk) and the screenshot buffers; if the block cannot be allocated at boot the machine falls back to 512 KB. `tools/gime_ram_test/run.sh` checks the translation for all four sizes on the host.

**ROM mapping (banks 0x3C-0x3F) by MC1/MC0/TY:**

| MC1 | MC0 | TY | $8000-$BFFF | $C000-$FEFF |
|-----|-----|----|-------------|-------------|
| 1 | 0 | 0 | Internal ROM | Internal ROM |
| 0 | 0 | 0 | Internal ROM | External (CTS) |
| 1 | 1 | 0 | External (CTS) | External (CTS) |
| x | x | 1 | RAM (all-RAM) | RAM (all-RAM) |

At reset: MC1=1, MC0=0, TY=0 → full 32 KB internal ROM at $8000-$FEFF.

### Video Modes

#### INIT0 COCO=1 (VDG Compatibility Mode)

Fully compatible with CoCo 2 video. Reads VDG mode bits from PIA1B:

| PIA1B[7:5] | Mode | Resolution | Colors |
|------------|------|-----------|--------|
| 0xx | Alphanumeric/Semigraphics | 32×16 chars | 2 (CSS-selected) |
| 100 | CG1 | 64×64 | 4 |
| 101 | RG1 | 128×64 | 2 |
| 110 | CG2 | 128×64 | 4 |
| 111 | RG2/RG3/RG6/PMODE 0-4 | up to 256×192 | 2 or 4 |

#### INIT0 COCO=0 (GIME Native Mode)

Selected by VMODE (BP bit) and VRES (HRES/CRES/LPF):

| BP | HRES | CRES | Description | Pixels Wide |
|----|------|------|-------------|-------------|
| 0 | 0 | 0 | Text 32-col, 1 color/char | 256 |
| 0 | 1 | 0 | Text 40-col | 320 |
| 0 | 2 | 0 | Text 32-col (2 bytes/char) | 256 |
| 0 | 3 | 1 | Text 80-col | 640 |
| 1 | 0 | 0 | Graphics 128px, 2 colors | 128 |
| 1 | 2 | 1 | Graphics 320px, 4 colors | 320 |
| 1 | 4 | 2 | Graphics 512px, 16 colors | 512 |
| 1 | 6 | 2 | Graphics 640px, 16 colors | 640 |

Lines per field (LPF): 192 (default), 199, 225.

### Palette Registers ($FFB0–$FFBF)

16 palette entries, each 6-bit GIME color: `--RRGGBB` (interleaved bit format).

**IMPORTANT:** The GIME uses **interleaved** bit layout, not packed. For palette value `i`:
```cpp
uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1)) * 85;  // bits 5,2
uint8_t g = (((i >> 3) & 2) | ((i >> 1) & 1)) * 85;  // bits 4,1
uint8_t b = (((i >> 2) & 2) | ((i >> 0) & 1)) * 85;  // bits 3,0
```
Example: value $12 = GREEN (R=0, G=3, B=0), NOT purple.

64-entry RGB565 lookup table built at init time for fast rendering.

### PMODE 4 Artifacts (optional)

CoCo 2 software drawn in PMODE 4 (VDG RG6) expects NTSC artifact colour: on a
composite TV, pairs of pixels show up as blue or orange. The GIME shows them as
black and white. **Settings → Machine → PMODE 4 Artifacts** (off by default,
NVS key `"artifacts"`) makes PMODE 4 look the same as it does on our CoCo 2 machine:

- Scope: VDG-compatible RG6 (`(PIA1B & 0x70) == 0x70`, 256-pixel geometry
  `resolution == 1`) with CSS=1, the same condition as the CoCo 2 renderer
  (`mc6847.cpp`). MAME's GIME has the same scope (`m_legacy_video`); XRoar
  applies artifacts to every mode, which would need a second pass over every line.
- Colours: each bit pair 00/01/10/11 → black / blue / orange / white, the
  fixed GIME codes `0x00 0x08 0x26 0x3F` (not palette registers: the colour
  comes from the TV).
- Cost: `render_line()` picks `render_coco_rg_artifact` instead of
  `render_coco_rg` once per line; every other mode runs unchanged code. The
  setting is part of the dirty-line signature, so toggling redraws at once.
- Test: `tools/gime_render_test/run.sh` checks PMODE 4 lines against the
  table, checks that other lines still match the legacy renderer with the setting
  on, and checks that toggling defeats the line skip.

### Interrupt Controller

**Sources:**

| Bit | Source | Notes |
|-----|--------|-------|
| TMR (bit 5) | Timer underflow | 12-bit auto-reload timer |
| HBORD (bit 4) | Horizontal border | End of each active scanline |
| VBORD (bit 3) | Vertical border | End of active area |
| EI2 (bit 2) | External IL2 | Serial port |
| EI1 (bit 1) | External IL1 | Keyboard (pressed key) |
| EI0 (bit 0) | External IL0 | Cartridge |

**IRQ/FIRQ routing:** `IRQ = IEN && (irq_state & irq_enable_reg)`, `FIRQ = FEN && (firq_state & firq_enable_reg)`

**Combined interrupt routing (CoCo 3 machine):**
```cpp
cpu.irq_pending  = pia0.irq_a || pia0.irq_b || gime.IRQ;
cpu.firq_pending = pia1.irq_a || pia1.irq_b || gime.FIRQ;
```
This differs from CoCo 2, which uses direct PIA → CPU callbacks.

**Read-acknowledge:** Reading $FF92 (IRQ status) clears all IRQ flags and deasserts IRQ output. Same for $FF93 (FIRQ).

**Timer modes:**
- `TINS=0`: Decrement at scanline rate (~15.7 kHz, 1 tick/scanline)
- `TINS=1`: Approximate 3.58 MHz/8 ≈ 28 ticks/scanline (integer approximation of 28.4)

---

## Implementation Phases

### Phase 1: GIME Core — Port tcc1014.c Structure, Registers, MMU ✅

**Completed:** 2026-04-05

**Files created:**
- `src/core/tcc1014.h` (236 lines) — Flattened TCC1014 struct, public API
- `src/core/tcc1014.cpp` (656 lines) — Core GIME logic

**Key implementation notes:**
- XRoar's split `TCC1014`/`TCC1014_private` merged into single flat struct
- `tcc1014_mem_cycle()` signature extended with `is_reg_access` flag — GIME register/MMU/palette/SAM writes handled inside address decode, never reaching the machine dispatch layer
- `active_banks[8]` cache pre-computes `mmu_bank[TR|0..7]` for fast-path access
- Timer: '87 GIME variant used (`timer_offset=1`)

**Design decision:** No `active_banks[]` pre-computation in Phase 1 — added in Phase 5 after profiling confirmed it was needed.

### Phase 2: Machine Integration — Port coco3.c ✅

**Completed:** 2026-04-06

**Files modified:** `config.h`, `machine.h`, `machine.cpp`, `hal.h`, `hal_video.cpp`, `integration_test.h/cpp`

**Key implementation notes:**
- `MACHINE_TYPE == 4` → CoCo3 path; `#else` → CoCo2 path throughout codebase
- PIA callbacks are no-ops for CoCo3: interrupt routing done by per-scanline polling rather than edge-triggered callbacks (matches XRoar coco3.c approach)
- GIME register writes ($FF90-$FFDF) handled inside `tcc1014_decode_address()` — machine dispatch never sees them
- ROM offset: `rom_off = g->Z - (0x3C << 13)` converts 19-bit physical address to 0-32767 ROM index
- Integration tests disabled for CoCo3 (test_mmu_* are CoCo3-specific; old CoCo2 tests use flat 16-bit addresses incompatible with GIME MMU)

### Phase 3: GIME Video — Port render_scanline() ✅

**Completed:** 2026-04-06

**Files created:** `src/core/font_gime.h` (161 lines — 128 chars × 12 rows = 1536 bytes PROGMEM)

**Files modified:** `src/core/tcc1014.cpp` (903 lines), `src/hal/hal_video.cpp`

**Rendering pipeline:**
1. `fetch_byte_vram()` — 16-bit cached fetch from physical RAM (Xoff increments by 2 per actual fetch, not cached returns — critical detail)
2. Mode decode: VDG compat (SG/CG/RG/Text) or GIME native (1/2/4 bpp graphics, text with attributes)
3. Pixel expansion by `resolution` field (0=4×, 1=2×, 2=1:1, 3=skip)
4. Output to `line_buffer[640]` (palette register values, 6-bit GIME colors)
5. HAL un-swaps each byte-swapped RGB565 pixel (OPT-C4) and re-encodes it as a 6-bit RGB222 raw VGA byte via `s_vga.createRawPixel()` before writing to the FabGL scanline.

**Key deviations from plan:**
- `line_buffer[]` contains `palette_reg[idx]` values (6-bit GIME colors 0-63), not raw palette indices (0-15). HAL does the final mapping.
- Font is 128 chars × 12 rows = 1536 bytes, not 192 × 8 as originally planned
- No border rendering — active pixels only, border color set as sprite background
- No mid-scanline beam-tracking — entire scanline rendered in one pass

**VGA32 scaling strategy:**

| Source Width | HAL behavior on 640-wide framebuffer |
|-------------|-----------------|
| 640px | 1:1, no scaling |
| 320px | 2× horizontal pixel-double |
| 256px (VDG) | 2× pixel-double, centered with 64-px margins |
| < 640px | Centered, side margins filled with border color |

All vertical modes (192, 199, 225 lines) fit within the 200-line framebuffer. See `docs/video.md` for the full scaling rules.
- `1`: box-filter — averages each adjacent pixel pair in RGB565 (bswap → `& 0xF7DE` mask → halve-and-sum → bswap back). Recovers odd-column detail. **Measured: 80-column text becomes readable**, but FPS drops from **32 → 23 on idle BASIC** due to ~320 extra averages per scanline. See `video.md` for full details.

### Phase 4: GIME Interrupts ✅

**Completed:** 2026-04-06

**Key fixes:**
- Read-acknowledge bug: must explicitly set `gime->IRQ = false` / `gime->FIRQ = false` after clearing state registers, otherwise IRQ/FIRQ lines remain asserted causing spurious re-interrupts
- HS both edges fire in sequence within one scanline (acceptable for scanline-based model)
- FS timing corrected: falling at scanline 0, rising at scanline 4, VRAM reset at scanline 7 (matches XRoar; CoCo BASIC's 60Hz IRQ depends on this)
- Row counter mask: `(row + 1) & 15` — without masking, VRAM address incremented per row overflowed

**Vertical state machine frame layout:**
```
Scanlines 0-3:   Vertical sync (FS low / PIA0 CB1 falling)
Scanlines 4-6:   Post-sync blanking (FS high / PIA0 CB1 rising)
Scanline 7:      VRAM address reset to Y register, lcount = 0
Scanlines 7+lTB: Active area begins (lTB = top border lines)
After lAA lines: VBORD interrupt, bottom border
```

### Phase 5: Testing & Optimization ✅

**Completed:** 2026-04-07

**Key optimizations:**

1. **MMU fast-path with `active_banks[8]` cache** — For addresses $0000-$FDFF (~90% of accesses), `machine_read/write` skip the full `tcc1014_decode_address()` call using pre-computed bank mapping. Boundary at $FE00 (not $FF00) to exclude MC3 constant-RAM region.

2. **VRAM dirty-frame tracking** — `dirty_frame` flag set on any RAM write, palette write, or VMODE/VRES/BRDR register write. `hal_video_present_gime()` skips SPI push (~4ms) when unchanged. Conservative (any RAM write = dirty) — exact VRAM range tracking would be too complex.

3. **CoCo3 integration test suite** — 14 tests: MMU (4), GIME registers (3), ROM mapping, timer, interrupts, SAM compat, VDG text rendering, palette RGB565 mapping, scanline output width, PIA DAC audio path.

**Performance estimates after Phase 5:**
- Static screens: ~40+ FPS (dirty-frame skips SPI push)
- Active graphics: ~20-25 FPS (every frame dirty, full render + SPI push)

---

## Hardware Testing & Bugs Fixed

**Date:** 2026-04-07 (Phase 5 hardware testing session)

After Phase 5 features were implemented, live hardware testing on an ESP32-S3 (the original target) with real CoCo3 ROMs revealed 7 critical bugs fixed iteratively. The same fixes apply to the current TTGO VGA32 build — these are GIME-core bugs, not HAL bugs.

### Bug 1: Vector Space Z Not Computed

- **Symptom:** Black screen — CPU never executes ROM code after reset
- **Root cause:** `tcc1014_mem_cycle()` set `S=0` (ROM) for vector addresses $FFE0-$FFFF but never computed `Z`. The ROM handler used `Z` to calculate ROM offset, got garbage, returned $FF for the reset vector.
- **Fix:** Added bank/Z computation for the vector path:
  ```cpp
  gime->S = 0;
  unsigned bank = gime->MMUEN ? gime->mmu_bank[gime->TR | (addr >> 13)]
                              : (0x38 | (addr >> 13));
  gime->Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
  ```
- **Lesson:** Address decode must ALWAYS compute Z when setting S=0, even for "special" address ranges.

### Bug 2: GIME Palette Bit Layout Wrong

- **Symptom:** Blue screen instead of green — all colors garbled
- **Root cause:** Palette LUT assumed packed `RRGGBB` (bits 5:4=R, 3:2=G, 1:0=B). Actual GIME hardware uses **interleaved** `R1G1B1R0G0B0`.
- **Fix:**
  ```cpp
  uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1)) * 85;
  uint8_t g = (((i >> 3) & 2) | ((i >> 1) & 1)) * 85;
  uint8_t b = (((i >> 2) & 2) | ((i >> 0) & 1)) * 85;
  ```
- **Source:** Confirmed from XRoar `coco3.c:595-598`
- **Lesson:** GIME palette value $12 = GREEN (R=0, G=3, B=0), not purple. Always verify hardware color encodings against the reference emulator, not documentation alone.

### Bug 3: CTS (Cartridge) Returns $FF

- **Symptom:** ROM partially executes but code at $C000-$FEFF inaccessible
- **Root cause:** With MC1=0, banks 0x3E-0x3F ($C000-$FEFF) route to CTS. The 32KB ROM covers this range but the CTS handler returned $FF.
- **Fix:** CTS handler now serves from `rom_coco3[]` (and `rom_disk[]` for Disk BASIC).
- **Lesson:** On CoCo3, $8000-$BFFF routes to ROM (S=0), $C000-$FEFF routes to CTS (S=1) depending on MC1/MC0. Both paths must serve ROM data.

### Bug 4: PIA IRQ Routing Checks Flag Without Enable Bit

- **Symptom:** CPU stuck in infinite IRQ re-entry loop at $010C (`JMP $894C`). System frozen with BASIC partially initialized.
- **Root cause:** Interrupt routing checked only the PIA IRQ1 flag (ctrl bit 7), not the enable (ctrl bit 0):
  ```cpp
  // WRONG: checks flag only
  bool pia0_irq = (m->pia0.ctrl_a & PIA_CR_IRQ1) || ...;
  ```
  HS transitions set PIA0 CA1 IRQ1 flag every scanline. Since CA1 IRQ wasn't enabled (bit 0=0), the flag was never cleared by the ROM. CPU saw perpetual IRQ.
- **Fix:** PIA IRQ output is `flag AND enable`:
  ```cpp
  auto pia_irq = [](uint8_t cr) -> bool {
      bool irq1 = (cr & 0x80) && (cr & 0x01);
      bool irq2 = (cr & 0x40) && (cr & 0x08) && !(cr & 0x20);
      return irq1 || irq2;
  };
  ```
- **Diagnostic:** Frame dump showed `CA=$B4` (bit 7=flag set, bit 0=enable clear) with `irq_pend=1`.
- **Lesson:** PIA IRQ output is `(flag AND enable)`, not just `flag`. CoCo3's polled routing must replicate the full PIA logic.

### Bug 5: ROM-Area Writes Silently Dropped

- **Symptom:** CPU stuck at $010C during RAM test. Code at target address in RAM contains zeros.
- **Root cause:** Write fast-path dropped writes to ROM-mapped addresses (bank >= 0x3C, TY=0). On real CoCo3 hardware, **writes always go to underlying RAM** even when ROM overlays reads. The boot code writes data to RAM underneath ROM before switching to all-RAM mode (TY=1).
- **Fix:** Write fast-path always writes to RAM regardless of bank/TY:
  ```cpp
  if (__builtin_expect(addr < 0xFE00, 1)) {
      unsigned bank = g->active_banks[addr >> 13];
      uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
      if (Z < COCO3_PHYSICAL_RAM) {
          m->ram_physical[Z] = val;
          g->dirty_frame = true;
      }
      return;
  }
  ```
- **Lesson:** On CoCo3, ROM is a read overlay — writes always reach the RAM chip underneath. Critical for the boot sequence which prepares RAM before enabling TY=1.

### Bug 6: VRAM Fetch Xoff Increment on Cached Returns

- **Symptom:** Screen content doubled/garbled — "EXTENDED COLOR BASIC" displayed as "EXND CORNS 2"
- **Root cause:** `fetch_byte_vram()` incremented `Xoff += 2` on EVERY call, including cached returns. In XRoar, `Xoff += 2` is inside the `else` block (only on actual 16-bit fetch). The code had it outside, causing the address to skip every other byte pair.
- **Fix:** Move `g->Xoff += 2` inside the `else` block:
  ```cpp
  } else {
      uint32_t addr = g->B + (g->Xoff & 0xFF);
      r = g->ram[addr];
      g->vdata_cache = g->ram[(addr + 1) & (g->ram_size - 1)];
      g->have_vdata_cache = true;
      g->Xoff += 2;  // Only on actual fetch, NOT cached return
  }
  ```
- **Lesson:** When porting XRoar's 16-bit fetch optimization, Xoff increment placement is critical. A single line outside vs inside the if/else causes half the VRAM data to be skipped.

### Bug 7: Disk BASIC Not Loading — Missing External ROM

- **Symptom:** Boots to "EXTENDED COLOR BASIC" but no "DISK" — `DIR` command not available
- **Root cause:** The CoCo3 ROM checks for the 'DK' signature at $C000 to detect Disk BASIC. The 32KB `coco3.rom` has boot initialization code at $C000 (starting with $1A,$50 = ORCC #$50), not 'DK'. Disk BASIC on CoCo3 is a **separate 8KB ROM** (`disk11.rom`) that maps via the CTS/cartridge port.
- **Fix:** Added `rom_disk[]` buffer (8KB), loaded `disk11.rom` at startup, updated CTS read handler:
  ```cpp
  case 1: // CTS (cartridge)
      if (m->rom_disk_loaded) {
          uint32_t disk_off = g->Z - (0x3E << 13);
          if (disk_off < 8192)
              return m->rom_disk[disk_off];
      }
  ```
- **Discovery:** Used `/disassemble` skill to trace the boot code at $80A6-$80AC which revealed the 'DK' check.
- **Lesson:** CoCo3 has a 32KB internal ROM (SECB) plus a separate 8KB Disk BASIC ROM (cartridge). Unlike CoCo2 where all ROMs are individual 8KB files, CoCo3 combines most into one 32KB image but keeps Disk BASIC external.

### Bug 8: Active Area Re-Entry Causes Border Corruption

- **Symptom:** Bottom border area showed garbage characters and green rectangles instead of a clean black border. Worsened after disk I/O (e.g., booting OS-9 or running `DIR`). Six black rectangles visible at the bottom of the screen during BASIC; random characters/green artifacts after OS-9 boot.
- **Root cause:** The vertical state machine in `machine_run_scanline()` used `>=` for the active area entry condition:
  ```cpp
  // BUG: allows re-entry after active area ends
  if (!g->vertical.active_area && vline >= top_border_end) {
  ```
  After the active area ended (192 lines rendered), `vline` continued incrementing past `top_border_end`. With `>=`, every subsequent scanline re-triggered the active area entry, causing the renderer to write beyond the intended 192 sprite rows into the bottom border region.
- **Fix:** Changed to exact match, preventing re-entry:
  ```cpp
  // FIX: == fires exactly once, preventing re-entry
  if (!g->vertical.active_area && vline == top_border_end) {
  ```
- **Related changes:**
  - `hal_video_render_scanline_gime()` signature updated to accept `total_lines` and `border_colour` for dynamic vertical centering (`y_off = (240 - total_lines) / 2`)
  - On geometry or border color change, the entire sprite framebuffer is cleared and refilled with the new border color to prevent stale pixels
  - Each active row is pre-filled with border color before rendering content pixels, handling left/right margins for modes narrower than 320px
  - Removed `hal_video_begin_frame_gime()` — border fill now consolidated into the per-scanline renderer
- **Discovery:** A corruption detector in `hal_video_present_gime()` found pixel value 0xE007 (pure green) at row 216, x=32 — corresponding to `display_line=192` (one past the limit) with VDG centering offset 32. This proved the renderer was writing past the active area.
- **Lesson:** In a vertical state machine where a condition guards a one-time state transition (entering the active area), use `==` not `>=`. With `>=`, any scanline after the threshold re-triggers the transition, causing unbounded rendering overflow.

---

## XRoar Code Reference

### Function-to-Function Mapping

| XRoar Function | File:Line | ESP32 Target |
|---|---|---|
| `tcc1014_allocate()` | tcc1014.c:551 | `tcc1014_init()` |
| `tcc1014_reset()` | tcc1014.c:661 | `tcc1014_reset()` |
| `tcc1014_mem_cycle()` | tcc1014.c:689 | `tcc1014_decode_address()` |
| `tcc1014_set_register()` | tcc1014.c:895 | `tcc1014_write_register()` |
| `tcc1014_set_sam_register()` | tcc1014.c:847 | `tcc1014_write_sam()` |
| `update_from_gime_registers()` | tcc1014.c:1558 | `tcc1014_update_mode()` |
| `render_scanline()` | tcc1014.c:1261 | `tcc1014_render_scanline()` |
| `fetch_byte_vram()` | tcc1014.c:1233 | (internal) |
| `do_hs_fall()` | tcc1014.c:1003 | scanline loop |
| `do_vb_irq()` | tcc1014.c:1087 | scanline loop |
| `SET_INTERRUPT` macro | tcc1014.c:396 | `set_interrupt()` inline |
| `read_byte()` | coco3.c:1104 | `machine_read_coco3()` |
| `write_byte()` | coco3.c:1170 | `machine_write_coco3()` |
| `coco3_reset()` | coco3.c:839 | `machine_reset()` (CoCo3 path) |
| `cpu_cycle()` | coco3.c:1255 | `machine_run_scanline_coco3()` |
| `gime_hs()` | coco3.c:1416 | PIA0 CA1 transition in scanline loop |
| `gime_fs()` | coco3.c:1429 | PIA0 CB1 transition in scanline loop |
| `keyboard_update()` | coco3.c:1344 | existing kbd handler + IL1 update |
| `font_gime[]` | font-gime.c | `font_gime.h` (PROGMEM array) |

### Key XRoar Source Locations

| Purpose | Path |
|---------|------|
| XRoar GIME implementation | `xroar-port/src/tcc1014/tcc1014.c` |
| XRoar GIME header | `xroar-port/src/tcc1014/tcc1014.h` |
| XRoar CoCo3 machine wiring | `xroar-port/src/coco3.c` |
| XRoar GIME font | `xroar-port/src/tcc1014/font-gime.c` |

### Lookup Tables (from tcc1014.c:406-424)

```cpp
// Lines per active area by LPF — tcc1014.c:412
static const unsigned VRES_LPF_lAA[4] = { 192, 199, 0xffff, 225 };

// Bytes per row in graphics mode by HRES — tcc1014.c:415
static const unsigned VRES_HRES_BPR[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };

// Bytes per row in text mode by HRES — tcc1014.c:418
static const unsigned VRES_HRES_BPR_TEXT[8] = { 32, 40, 32, 40, 64, 80, 64, 80 };

// Row mask by LPR value — tcc1014.c:421
static const unsigned LPR_rowmask[8] = { 0, 1, 7, 8, 9, 10, 0xffff, 0xffff };

// SAM V rowmask — tcc1014.c:422
static const unsigned SAM_V_rowmask[8] = { 11, 0, 2, 0, 1, 0, 0, 0 };
```

---

## Validation suite

`tools/gime_validate/` is a host-side suite (it never runs on the board) that drives our GIME
(`src/core/tcc1014.cpp` plus a mirror of the GIME part of `machine.cpp`'s scanline loop) side by side
with two reference implementations compiled unmodified: XRoar's `src/tcc1014/tcc1014.c` and MAME's
`src/mame/trs/gime.cpp`, the latter built as a 1987 and a 1986 GIME. Each scenario applies the same
register writes, IL changes and line advances to every model and records one check per observation
(register reads, MMU decode, timer and interrupt event ticks and line indices, sync timing, rendered line
hashes and border colours).
The common time base is the 14.31818 MHz pixel clock, 912 ticks per scanline. Only accuracy is
compared, never speed.

### Running it

```bash
tools/gime_validate/run.sh                  # build and run (~90 s); exit 0 = no new failures
tools/gime_validate/run.sh --seed 7         # different random runs (exploration: never gates)
tools/gime_validate/run.sh --selftest       # the comparator's own tests
tools/gime_validate/run.sh --update-baseline
```

`XROAR_SRC` and `MAME_SRC` default to `~/proyectos/coco-emu-other-projects-source/{xroar,mame}`.
A missing tree or a failed oracle build prints a NOTICE and that model's checks are SKIPPED. The run
writes `${TMPDIR:-/tmp}/gime_validate/results.json` and `gime_validation_report.md` (summary by area,
failures, baseline, resolution-limited passes with ours' offset from each reference, discrepancy table,
every verdict with the checks it covered, UNRESOLVED list, and the reference edits). When a reference is
missing the console header says `gating references: N/2 (xroar, mame)`, and the 1986 (`v86.`) pass,
which needs both, is skipped with a NOTICE.

Before building, `run.sh` checks that the copy of `machine.cpp`'s scanline loop in `model_ours.cpp`
(the `MIRROR-BEGIN`/`MIRROR-END` blocks) still matches `src/core/machine.cpp`: it finds the ranges by
anchor text, normalises the intended differences (rules in the comment above the check in `run.sh`)
and stops the build with `MIRROR DRIFT` and a diff if anything else differs.

### Statuses

| Status | Meaning |
|---|---|
| PASS | All models agree exactly. |
| RES | Ours differs from the references by no more than its resolution for that capability (ours resolves events to a whole line, 912 ticks). A pass; the report lists each one with ours' offset from each reference. |
| FAIL | Ours differs from the hardware-true value: both references agree against ours, or the verdict's model disagrees with ours. Fails the run. |
| KNOWN | A FAIL listed in `known_failures.txt` (the baseline), with a reason. Does not fail the run. |
| FIXED | A baselined check that now passes, or now matches the verdict's model (would be DISC). Fails the run until it is removed from the baseline. |
| STALE | A baseline id that no longer exists (renamed or deleted check). Fails the run until removed. |
| DISC | XRoar and MAME disagree, a verdict names the accurate one, and ours matches it. Also every `v86.` (1986 GIME, references only) split, which is information only. |
| UNRES | XRoar and MAME disagree and no verdict covers the check. Does not fail the run. |
| SKIP | A model lacks the capability or has no value for the check (for example `first_active_px`, see Known limits). |

Random checks (`random.<area>.<run>.<observation>`) gate the run only with the default seed (0x6809)
and run count (200), because their ids depend on both.

### Updating the baseline

After a change to the GIME (or to a scenario), run `run.sh`. A new FAIL means our GIME moved away
from the references; a FIXED means a known difference went away. When the change is intended, run
`run.sh --update-baseline`: it rewrites `known_failures.txt` with every current FAIL and KNOWN id,
keeps the existing reasons, and writes `# TODO reason` for new ones. Replace each TODO with a one-line
reason naming the root cause, reusing the existing wording for the same cause, then run `run.sh`
again and check `exit=0`.

### Adding a verdict

When the references split (UNRES), find out which is hardware-true and add an entry to
`tools/gime_validate/verdicts.md`: a `## <check id>` heading, an `accurate: xroar|mame` line (a verdict
must name a reference; `accurate: ours` is rejected, since it would compare ours with itself), an
optional `confidence: high|medium|low` line, and free markdown with what each model does (file:line),
the evidence, and whether ours is a bug or a simplification. An id ending in `*` covers every check
whose id starts with the text before it; matching is a plain string prefix (`a.b*` also matches
`a.bc`), so end a family prefix at a `.` or `_` and check the report's prefix listing. An exact id beats
a prefix and the longest prefix wins. Evidence, by weight: measured-hardware statements in XRoar
(its comments and commit messages), Sock's GIME reference (https://www.6809.org.uk/sock/gime.html) and
the Tandy service manual, VCC as a third opinion, and software known to depend on the behaviour.
A malformed entry is a hard error naming the line.

### Findings

The first verdicts (2026-10-25; XRoar 53acd7e3, MAME 9c525573728). "Favours" is the model the verdict
names as hardware-true; full evidence is in `verdicts.md`.

| Root cause | Favours | Confidence | Ours |
|---|---|---|---|
| Interrupt latching: XRoar's post-2025 logic (d9bba8a3): TMR re-latched when $FF92/$FF93 enables it with the timer at 0 (shown by Tim Lindner's GIME-MMU-TESTER), EI inputs edge-detected (shown for the keyboard by coco_key_test), no TMR re-latch on a status read; the re-latch on enable was assumed by XRoar for IL0-2 | XRoar = MAME | high (TMR, keyboard edge), low (re-latch on enable for IL0-2, assumed) | bug: ours still has the pre-2025 logic (level IL, re-latch on read, no latch on enable) |
| HBORD on every line, borders included | XRoar = MAME = VCC | high | bug: only on active-area lines |
| TINS=1 timer: one fire per 4·(n+1) pixel ticks, 228 counts per 912-tick line, reload re-read at each expiry | XRoar = MAME | high | bug: at most one fire per line; 227.5 per line; re-arm keeps the old count |
| X offset ($FF9F) | XRoar = MAME | high | bug: ignored |
| Field length: 263 lines at 60 Hz (also the 2-line split of `video.vsc_latch` and `video.voffset_latch_*`, the latch line counted across a field) | XRoar | medium | bug: 262 lines (as MAME and VCC) |
| VBORD 7 + lTB + lAA lines after FS fall | XRoar | medium; low for the first VBORD after reset (`irq.enable_vbord_*`, mixed with the power-on frame phase) | one line early |
| TINS=0 timer fires on an HS fall (its line index in the model's own HS grid; `irq.enable_tmr_*`, `random.timer.*`) | XRoar (scope-verified) | high | same as XRoar; MAME schedules line periods from the reload, not on HS |
| First HBORD after reset: line 1 (XRoar, right-border point) or line 0 (MAME, HS callback); `random.irq.*` cause 2, `irq.enable_hbord_*` | XRoar | low | bug: HBORD only on active-area lines (the HBORD row above) |
| Mid-line register and palette writes take effect at the beam | XRoar | high | simplification: whole line rendered at line end (MAME too) |
| Font character $15 row 4 = $54 | MAME = VCC | medium | bug: $5C copied from XRoar |
| 16-colour 16-byte-per-row modes drop the second byte of each 16-bit fetch | XRoar | medium | same as XRoar |
| CRES 0 at 128/160 bytes per row | XRoar | low | same as XRoar |
| EI source enabled while its input is already high latches | XRoar | low | same as XRoar (by level sampling) |
| MMU registers $38+i after reset | MAME (= ours = VCC) | low | — |
| $FE00 page with MC3 clear after a slot 7 write | XRoar (MAME's bank 8 is stale) | high | same as XRoar |
| $FE00 page with MC3 set in ROM mode: S code | XRoar (MAME has no S) | low | same as XRoar |
| Undriven bits 7-6 of $FF92, MMU and palette reads (bits 7-6 only; TMR is in `regs.readback_FF92_*`) | XRoar (harness convention: the adapters float the bus as $FF; MAME reads 0 for $FF92) | low | drives 0; not a GIME accuracy claim |

`random.render.*` splits (364 checks) have no verdict yet: they mix causes found but not judged (the X
offset in COCO mode, which XRoar applies and MAME does not, LPF in COCO mode, LPF 1 as 199 or 200 lines,
the attribute underline row at LPR < 3 and LPR 5, the X offset in native text modes, LPR 7), and stay
UNRESOLVED. The border colour of the VDG-compatible graphics modes (`render.vdg_cg1/cg6/rg6/rg6_css.border_colour`)
has no verdict either: ours and XRoar take it from palette slot RGCSS0_1/RGCSS1_1 (`tcc1014.c:1160-1162`), MAME
uses the fixed colours $12/$3F (`gime.cpp:1287-1291`), and no hardware source was found; they stay UNRESOLVED.
1986-GIME (`v86.`) splits are information only; the timer offset (+2 for 1986, +1 for 1987) is the same in both
references.

### Known limits

- **Line-quantised events.** Ours resolves timer, sync and interrupt events to a whole line (912 ticks), so tick
  checks of those events pass (RES) within 912 ticks and a whole-line shift could hide inside that tolerance. Every
  line-rate event (TINS=0 timer fires, and IRQ/FIRQ rises from TMR at TINS=0, HBORD and VBORD) also has a `_line`
  check: the event's line index counted in each model's own HS grid, compared exactly. Where the anchor sits on a
  line boundary (`random.timer`), the count starts half a line on so that the models' HS falls (ours at the
  boundary, XRoar 10 ticks after it) don't straddle it.
- **machine.cpp mirror.** `model_ours.cpp` runs a copy of the GIME part of `machine_run_scanline_coco3()`, not
  `machine.cpp` itself. `run.sh` diffs the copy against `machine.cpp` before every build and fails on drift, but
  the render and HAL calls are excluded from the diff by design (next item).
- **Firmware render path not exercised.** The suite forces `raw_output = false` and clears the line signatures, so
  the firmware's raw VGA output (OPT-G6) and the OPT-G5 `line_clean` line skipping are never tested; ours is
  compared through `line_buffer` (RGB565) only.
- **512 KB only.** The 2 MB memory expansion is excluded.
- **NTSC only.** No 50 Hz fields; random `$FF98` writes keep H50 = 0.
- **`first_active_px` is not comparable.** Ours renders only the active area and keeps no horizontal position, and
  MAME's bitmap columns have no fixed relation to HS fall, so the check is SKIP for both and only XRoar reports a
  value. A horizontal-position bug such as the $FF9F X offset being ignored is caught by the line hashes instead.
