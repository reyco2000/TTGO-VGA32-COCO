# CoCo Max III opening at 32–39 FPS: analysis and emulator optimization plan

**Status:** plan only. No code has been changed.
**Target:** LilyGo TTGO VGA32 v1.4 (ESP32-WROVER, 240 MHz, 4 MB PSRAM), firmware v0.21.0.
**Sources:**
- Emulator: this repository at `0f411d0` (v0.21.0).
- Guest: the annotated CoCo Max III v3.6 disassembly at [reyco2000/ccmax3](https://github.com/reyco2000/ccmax3) (`listing/COCOMAX3_src.asm`, `listing/PAGE3B_printer_joystick_src.asm`, `CM3_analysis.md`).

> **Ground rule.** CoCo Max III is the test case here, not the target. Nothing in this plan detects
> CoCo Max, patches its code, or adds high-level emulation of its SWI calls. Every item speeds up a
> general emulator path that CoCo Max happens to stress. Each item also lists other software that
> benefits. If a change only helps CoCo Max, it is out of scope.

---

## 1. Summary

The CoCo Max III title screen hits four costs at once. Real software often combines them, but the
current emulator handles each one poorly:

1. **Palette animation defeats the dirty-line skip (OPT-G5).** CoCo Max's 60 Hz IRQ rotates palette
   registers 10–14 ("Color Cycle" and "Animate"). `line_signature()` hashes all 16 palette registers
   into every line's signature. So each palette write forces a full re-render of all 192 lines and a
   full copy into the VGA framebuffer, including lines that don't use the changed colours. A
   320×192×16 screen is the most expensive case: 160 VRAM bytes per line, expanded to 640 output bytes.
2. **The re-render runs two passes with byte stores and `memw`.** The renderer first writes 640 bytes
   to `line_raw`. The HAL then rotates and copies them into the DMA framebuffer. `hal_video.cpp` (and
   `machine.cpp`) do not include `no_psram_memw.h`, so the compiler adds a `memw` after **every
   store** in the framebuffer copy loop. That is ~30,000 `memw` per full frame. OPT-M1 removed the
   same cost from the CPU and GIME files.
3. **I/O-heavy polling at 1.79 MHz.** While the title is shown, CoCo Max runs a tight loop:
   `SWI` → `Sys39_ReadPointer` → `Sys00_ReadInput` (a successive-approximation joystick read through
   `$FF20`/`$FF00`), plus keyboard scans. Every `SWI` writes `$FFA3` (MMU), and the loop writes
   `$FFD9`/`$FFDF` (SAM). None of these hit the CPU's fast page path. Each access goes through the
   machine callback, three peripheral checks and the full `tcc1014_mem_cycle()` decode. MMU and SAM
   writes also rebuild all 8 page pointers and re-derive the video mode, even when the value hasn't
   changed. Fast mode doubles the per-frame budget to ~29,832 CPU cycles.
4. **No video frame-skip.** When a frame takes longer than 16.7 ms, the whole machine slows down:
   CPU, timers and audio, not only the display. 39 FPS means the guest runs at 65% of real speed.

The plan below (sections 5–6) tackles these in order of value per risk. Phases 1–3 are general,
bit-exact and low to medium risk. By the cost model in section 4 they should bring the CoCo Max
title close to 60 FPS. That projection needs confirming with the Phase 0 measurements first.
Palette-cycling games, OS-9 (MMU and SAM traffic), BASIC `JOYSTK` loops and any `HSCREEN` program
benefit as well.

---

## 2. What CoCo Max III does during the opening

All addresses are from the CoCo Max III listing (main image, 512K configuration).

### 2.1 Boot and title load

`BOOT.BAS` ends with `POKE &HFFD9,0` (fast CPU), `HSCREEN2`, `POKE &HFF9A,63` and `EXEC &H30C0`.
`ColdStart` ($30C0) sets the IRQ vector to `IRQ_VSync` and the FIRQ vector to `FIRQ_RasterPalette`,
writes `$FFD9` (1.79 MHz) and `$FFDF` (all-RAM), and enables interrupts. `InitProgram` then runs a
chain of SWI calls. The first, `SUB_5D83` → `L_5D9D`, is the opening sequence:

| Step | Routine | What it does | Emulator paths hit |
|---|---|---|---|
| 1 | `SUB_5047`, `SUB_50CC` | Opens `TITLEPGE.SYS` through DSKCON. Each byte is read with `JSR $A176` at **0.89 MHz** (`SAM_R1CLR` around the ROM call, `SAM_R1SET` after). | FDC, SAM writes, ROM reads |
| 2 | `SUB_5724` | Reads the file header: 16 palette bytes to `$FFB0`, then `CycleSpeed`, `AnimSpeed` and an 8-colour animate table, then the **Color Cycle** flag (`D_3FDD`) and the **Animate** flag (`D_3FDA`). So the title picture chooses its own palette animation. | Palette writes |
| 3 | `SUB_58A4` / `L_58AC` | Decompresses the picture into `$8000`, mapping each 8 KB screen page through the MMU. Each pixel byte is a literal, a repeat of the previous byte, or a copy of the byte one line up (`LDA -160,X`, so 160 bytes per line = 320×16). | RAM writes into VRAM, MMU writes |
| 4 | `STA $FF9D` (`#$F0`) | Points the GIME at the decoded screen. | GIME register |
| 5 | `SUB_5E49` | Loads `FONTBLOX.SYS` into pages from `$8000`. | FDC, MMU |
| 6 | `L_5DD8` | **Waits for the pointer:** `SWI $B9,$3A` (`Sys39_ReadPointer`, `Sys3A_PointerMoved`), `BCC L_5DD8`. Loops until the pointer moves or the button is pressed. | see 2.2 |

The "opening animation" is step 6: the title picture with the IRQ-driven palette animation running,
while the CPU spins in the pointer loop. Steps 1–5 (the load) are I/O bound and run partly at
0.89 MHz.

### 2.2 The pointer loop (main CPU, fast mode)

Each pass of `L_5DD8` runs:

- **`SWI_Dispatch` ($4200)** for every system call. It pulls 7 registers, pushes 4, writes
  `#$37` to `$FFA3` (**an MMU write on every SWI, nearly always the same value**), reads the code
  byte, and calls through `TBL_SysCalls` via a self-modified `JSR`.
- **`Sys00_ReadInput` ($42E1, JOYSTICK.BIN)**:
  - Fire button: writes `$FF02`, reads `$FF00`.
  - Then 4 axis conversions (`LDB #$03 … DECB / BPL`). Each one writes `$FF01` and `$FF03` to
    select the analog mux, then runs a 6-step successive approximation: `STB $FF20` (DAC), then
    `LDA $FF00` (comparator, bit 7).
  - A conversion is repeated, up to 10 times (`JoyTemp=$0A`), while the result differs from the
    previous stored value. So moving the mouse multiplies this cost by up to 10.
  - Finally it saves `$FFA3`, maps page `$3B`, calls `JoyRelative` ($77A0), and restores `$FFA3`
    (**two more MMU writes**).
- **`Sys4C_KeyScan`** (twice or more): `STA $FF02` / `ORB $FF00`.
- `Sys34_ClampToRect` and `Sys3A_PointerMoved`: plain RAM work through SWI.

Per pass that is roughly 30–60 PIA reads, 30–40 PIA writes and 5–10 MMU writes, plus SWI
stack traffic. At 1.79 MHz a frame is 29,832 CPU cycles, so the loop runs many passes per frame.
That is **hundreds of slow-path I/O accesses and dozens of MMU writes per frame**. The rest of
the main loop (`Sys54_MainLoop`, $1F60) also writes `$FFD9` and `$FFDF` on every pass, so the
editor itself keeps up the same I/O pattern after the title.

### 2.3 The 60 Hz IRQ (`IRQ_VSync`, $326D)

On each PIA0 CB1 (field sync) interrupt:

- Disk motor timeout, `DelayCount`.
- `CLR $FF93` (GIME FIRQ enable).
- **Color Cycle** (if `D_3FDD`): every `CycleSpeed` frames, rotate `$FFBB`–`$FFBE` (palettes 11–14).
- **Animate** (if `D_3FDA`): every `AnimSpeed` frames, write the next of 8 colours into `$FFBA`
  (palette 10).

`CycleSpeed`/`AnimSpeed` default to 7, and the title file can override them. So every few frames
at least one palette register changes. The two timers drift against each other, which explains why
the FPS wanders between 32 and 39 instead of settling.

`FIRQ_RasterPalette` ($3310) runs only while the 64-colour chart is shown (`D_336C`). It rewrites
palettes 3–11 every 14 scanlines from the GIME timer, so it does not run during the opening.
It is listed here because any palette optimization must keep per-scanline palette changes exact
(section 3.4).

---

## 3. How the emulator spends a frame on this workload

### 3.1 Frame structure (`machine_run_frame_coco3`, `machine.cpp:878`)

```
262 x machine_run_scanline_coco3:
    sv_disk_tick, mc6809_run_variant(budget ~114 cycles in fast mode)
    rs232 / GIME timer tick / 2x PIA0 CA1 edges / IRQ+FIRQ routing
    vertical state machine; for each of 192 active lines:
        tcc1014_render_scanline()            -> line_raw[640]   (or skip via OPT-G5)
        hal_video_render_scanline_gime_raw() -> FabGL framebuffer row
    becker, ssc, hal_audio_capture_scanline
hal_video_gime_frame / hal_video_present_gime (FPS overlay)
pace_frame()  (never waits when the frame was slow)
```

### 3.2 Bottleneck B1: palette writes invalidate every line

`line_signature()` (`tcc1014.cpp:1362`) mixes `palette_reg[0..15]` into every line's hash. Any
palette write therefore makes `sig != s_line_sig[line]` on **every** active line. Each line is then:

1. **hashed:** 160 VRAM bytes read from PSRAM, two 32-bit multiply lanes per word;
2. **rendered:** `render_native_gfx<2,3,uint8_t>`, with 2 palette loads and 4 byte stores per
   source byte, into `line_raw`;
3. **copied:** `hal_video_render_scanline_gime_raw()`, 160 word loads + rotate + store into the DMA
   framebuffer.

That is 192 × (160-byte hash + 640-byte render + 640-byte copy) on a palette-change frame, against
192 hashes only on a quiet frame. Most of a picture usually doesn't use the cycling colours, but it
gets redrawn anyway.

### 3.3 Bottleneck B2: the redraw path itself

- **Two passes over 640 bytes.** The renderer produces logical-order bytes, then the HAL reads them
  back to apply the FabGL `x^2` swizzle. One pass straight into the framebuffer would do.
- **Byte stores.** In 320×16 mode each source byte becomes 4 identical-pair bytes, written one at a
  time (`emit4<3>`). One pre-swizzled 32-bit word per source byte would replace them.
- **`memw` after every store in `hal_video.cpp`.** The board builds with
  `-mfix-esp32-psram-cache-issue`, and `hal_video.cpp` does not include `utils/no_psram_memw.h`.
  So each `dst[i] = …` in the copy loop serializes the store buffer. The FabGL framebuffer comes
  from `heap_caps_malloc(..., MALLOC_CAP_DMA)`, which is internal RAM, so those `memw`s buy nothing.
  OPT-M1 measured this workaround at ~40% of a CoCo 3 frame in the core files. The same applies,
  less heavily, to `machine.cpp` (scanline loop, I/O callbacks, mostly stores to the `Machine`
  struct in internal RAM).

### 3.4 Correctness constraints any video change must keep

- Mid-frame palette changes take effect on the next scanline. `tcc1014_render_scanline()` resolves
  `pal8[]` per line, and CoCo Max's 64-colour chart relies on it.
- Screenshot capture (`hal_video_capture_armed()`) must still get every line in RGB565.
- `tcc1014_invalidate_lines()` callers (OSD close, overlay toggle, resolution switch) must still
  force a full redraw.
- The border, the 225-line squeeze (`gime_display_y`), the 512×240 layout and artifact colours must
  stay pixel-identical. The `docs/perf-baseline-2026-07` goldens and `tools/perf/cmpfb.py` are the
  gate.

### 3.5 Bottleneck B3: I/O and register writes on the slow path

`mem_read`/`mem_write` (`mc6809_core_impl.h`) take the fast path only below `$FE00`. Everything
else calls `machine_read_coco3`/`machine_write_coco3` (~80 host cycles just for the call, per
`mc6809.h`), which then runs:

| Access | Path today | Avoidable work |
|---|---|---|
| `LDA $FF00` (PIA0 PA) | rs232/ssc/vhd checks → `tcc1014_mem_cycle()` (IL edge detect, full decode) → 8-column keyboard loop calling `hal_keyboard_scan()` out of line → joystick button calls → `scanline>>4` joystick refresh → `mc6821_set_input_a` → `mc6821_read` | Everything before the PIA. The column loop could be one table lookup. |
| `STA $FF01/$FF03/$FF02` | same chain → `mc6821_write` → `sound_pia0_written` | The decode chain |
| `STB $FF20` | already direct (Rick Dangerous fix) | none |
| `STA $FFA3` (MMU) | chain → `mem_cycle` → `tcc1014_write_mmu` → `tcc1014_update_active_banks` (8 slots) → `update_fast_pages` (8 slots) | **Same value written again** (every CoCo Max SWI): nothing needs doing. Otherwise only 1 of 8 slots changes. |
| `STA $FFD9`/`$FFDF` (SAM) | chain → `tcc1014_write_sam` → `update_fast_pages` + `update_from_gime_registers` | **Bit already in that state** (every CoCo Max main-loop pass): nothing needs doing. |
| `CLR $FF93`, palette `$FFBx` | chain → `mem_cycle` → register write | The decode chain |

Hardware stays exact without any of the avoidable work: the GIME decode for `$FF00`–`$FF3F`,
`$FFA0`–`$FFBF` and `$FFC0`–`$FFDF` does not depend on MMU or ROM state.

### 3.6 Bottleneck B4: interpreter and cache pressure

- `docs/cpu.md` records 125–223 host cycles per emulated instruction for small loops, a 44-cycle
  switch dispatch, and 26 cycles measured for computed-goto dispatch with the table in DRAM.
- Those are hot-cache numbers. On this workload the ESP32's 32 KB flash/PSRAM cache is shared by:
  - the interpreter switch (flash);
  - the guest's code and data (PSRAM: `$1C00`–`$7FFF`, plus page `$37` mapped on every SWI);
  - the renderer and HAL code (flash);
  - on palette frames, ~30 KB of VRAM read for hashing and rendering (PSRAM).

  A full-repaint frame can therefore evict the interpreter and slow the next frame's CPU work.
  This needs measuring (Phase 0) before any interpreter change.
- `docs/cpu.md` also warns that the MC6809 build's code generation is fragile: adding the idle-loop
  check alone once dropped CoCo Max 3 from 39 to 27 FPS. Interpreter changes are therefore last in
  this plan and gated on `-DPERF_BENCH=1` for both CPUs.

### 3.7 Bottleneck B5: no headroom policy

`pace_frame()` only limits the top speed. A heavy video frame delays the next CPU frame, so the guest
slows down. The audio rate follows FPS (`hal_audio_commit_frame`), so sound pitch drops too. There is
no option to drop *display* frames to keep *emulation* at 60 Hz.

### 3.8 Cost model (to be confirmed in Phase 0)

| Component | Quiet frame | Palette-change frame |
|---|---|---|
| CPU at 1.79 MHz (~6–7K instructions) | dominant | same |
| Slow-path I/O and MMU/SAM writes (hundreds per frame) | significant | same |
| Line hashing (192 × 160 bytes from PSRAM) | moderate | same |
| Render + copy (192 × 640 + 640 bytes, `memw` per word store) | ~0 (skipped) | **large** |
| Cache refill after a full repaint | small | possibly large |

The FPS range fits this model: 39 on quiet frames, which are CPU and I/O bound, and lower when
palette frames bunch together. Phase 0 replaces the qualitative column with microseconds.

---

## 4. Principles

1. **General paths only.** No program detection, no SWI high-level emulation, no guest patches.
2. **Bit-exact.** Pixels (golden screenshots), CPU registers, memory and cycle counts
   (`tools/cpu_test`) stay identical.
3. **Measure first, gate every step.** Each step gets an entry in `docs/performance.md` with
   before/after probe tables, including rejected attempts (repo convention).
4. **Both CPUs.** Any change that touches the interpreter or memory helpers is benchmarked with
   `-DPERF_BENCH=1` on MC6809 and HD6309.
5. **Spend internal RAM carefully.** WiFi and the 128 KB FabGL framebuffer already compete for it.
   Each phase states its RAM cost.

---

## 5. Phase 0: baseline and general benchmarks (no emulator code change)

**0.1 Probes on the real workload.** Build with `PERF_PROBE_ENABLED 1`. Capture the probe table and
the CPU profile (`perf_cpu`: opcode histogram, `slow_rd`/`slow_wr` by kind with host cycles,
`mmu_wr`, `page_rebuild`, banks touched) for three cases:

- (a) CoCo Max title, mouse still;
- (b) CoCo Max title, mouse moving (joystick conversions repeat up to 10×);
- (c) CoCo Max editor idle.

Record the FPS overlay range and `/api/status` (`fps`, `frame_us_avg/max`, palette registers) next
to each table.

**0.2 Split the video probe.** Add probe IDs (instrumentation only) for:

- the signature hash, separately from render;
- the count of lines rendered vs skipped per frame;
- the count of palette writes per frame.

This shows directly how much each palette write costs.

**0.3 New generic benchmarks in `tools/perf/bench/`** (BASIC, so they need no proprietary disk):

| File | Exercises | Sketch |
|---|---|---|
| `hs2pal.bas` | Palette animation over a static 320×16 picture (B1, B2) | `HSCREEN 2`, draw 16 bars, then `PALETTE` rotate slots 10–14 in a loop |
| `hs2pal1.bas` | Same, but only a small area uses the cycling slots | Tests a palette-aware skip (Phase 2) |
| `joyloop.bas` | SAR joystick polling (B3) | `POKE 65497,0` (fast), loop `J=JOYSTK(0)+JOYSTK(1)` |
| `mmuloop.bas` | MMU and SAM write traffic (B3) | Fast mode, loop `POKE &HFFA3,PEEK(&HFFA3)` and `POKE 65497,0` |

Add **NitrOS-9 boot to shell plus a `dir`**, with the time-to-prompt, as the OS-level macro
benchmark: it is heavy on MMU, SAM and GIME register traffic.

**0.4 Cache-pressure check.** From the probe, compute host cycles per emulated instruction:
`exec_cc / insns`. Compare quiet frames with palette frames on the CoCo Max title. If CPU cost per
instruction rises after a full repaint, the cache-pressure item in Phase 4 moves up.

**Gate:** baseline tables committed under a "CoCo Max III / palette animation (2026-10)" section in
`docs/performance.md`, plus goldens for the new benchmarks.

---

## 6. Optimization plan

### Phase 1: low-risk general wins (I/O decode, idempotent writes, `memw`)

Each item stands alone and can be reverted on its own.

**1.1 Drop the `memw` workaround from `hal_video.cpp`.**
- Every store there goes to internal RAM:
  - the FabGL DMA framebuffer;
  - static tables and border cache;
  - the capture buffer, which **is PSRAM** (`ps_malloc`). Add `PSRAM_STORE_BARRIER()` after each
    capture-buffer write (or after the `memcpy`), as `no_psram_memw.h` requires.
- Include `utils/no_psram_memw.h`, after auditing every store in the file the way OPT-M1 did for the
  core files.
- Benefits: every CoCo 3 line that gets redrawn (any animated program), and CoCo 2
  `hal_video_render_scanline`.
- Cost: 0 RAM.

**1.2 Same for `machine.cpp`, with explicit barriers.**
- PSRAM stores in this file:
  - `ram_physical[...] = val` in `machine_write_coco3`/`_coco2` slow paths;
  - the reset `memset`;
  - cartridge and ROM buffers if any are written.
- Put `PSRAM_STORE_BARRIER()` after each of those and include the header. Everything else is the
  `Machine` struct and statics in internal RAM.
- Benefits: every scanline of every program (262 calls per frame), and every I/O access.

**1.3 Idempotent register writes do nothing.**
- `tcc1014_write_mmu`: if `mmu_bank[offset]` already equals the masked value, return before
  `update_active_banks()`.
- `tcc1014_write_sam`: if the bit is already in the requested state, return before
  `update_fast_pages()` + `update_from_gime_registers()`.
- Keep any side effect the hardware has (none for these: the MMU and SAM are plain latches).
- Benefits: CoCo Max (every SWI, every main-loop pass), BASIC's `$FFD9`/`$FFDF` pokes, OS-9 code
  that rewrites the same MMU slots.

**1.4 Incremental page-table update.**
- On a real MMU change, update only the affected slot: when `offset`'s task equals `TR`, rebuild
  `active_banks[slot]`, `rd_page[slot]` and `wr_page[slot]`.
- A full rebuild stays only for `TR`, `MMUEN`, `TY` and `set_ram` changes.
- Benefits: OS-9 task switches and DAT-style bank switching, Rick Dangerous, anything that maps
  pages.

**1.5 Direct I/O decode for the fixed-function ranges** in `machine_read_coco3`/`machine_write_coco3`.
Add these before the rs232/ssc/vhd chain and before `tcc1014_mem_cycle()`:
- `$FF00`–`$FF3F` (bit 4 clear): PIA0/PIA1, plus the PIA1B snoop on `$FF22`/`$FF23` writes and
  `edge_detect_il` (the `mem_cycle` side effects that matter, kept in the same order).
- `$FFA0`–`$FFAF` MMU, `$FFB0`–`$FFBF` palette, `$FFC0`–`$FFDF` SAM, `$FF90`–`$FF9F` GIME registers.

The rs232 (`$FF68`), ssc (`$FF7D`), vhd (`$FF80`) and FDC/Becker (`$FF40`–`$FF5F`) ranges don't
overlap these, so their checks can move below. Write a table-driven host test that checks every
address `$FE00`–`$FFFF`, read and write, gives the same result and side effects as the old chain
(extend `tools/gime_ram_test`).
- Benefits: every program that touches I/O. Keyboard polling (BASIC `INKEY$`, OS-9), joystick,
  sound (`$FF20` is already direct), interrupt acknowledges.

**1.6 Cheaper PIA0 port-A read.**
- Replace the 8-call `hal_keyboard_scan()` loop with an inline AND over `key_matrix[]`. Either
  expose the array read-only in `hal.h`, or keep a 256-entry "rows for column-select" cache that is
  invalidated when the key matrix changes (a one-byte generation counter in `hal_keyboard`).
- Keep the joystick refresh gate as it is.
- Benefits: every keyboard scan and every joystick comparator read (BASIC `JOYSTK` performs 6 per
  axis).

**Phase 1 gate:**
- `joyloop.bas` and `mmuloop.bas` improve measurably, and NitrOS-9 boot time does not regress.
- `tools/cpu_test` passes.
- Zero golden diffs.
- `slow_*_cc` in the CPU profile drops for `IO`.

### Phase 2: palette-aware dirty-line skip (fixes B1 in general)

Today any palette change marks every line dirty. Instead, a line should be dirty only when a palette
slot **it actually uses** changed.

**Design:**
- While hashing a line's VRAM bytes, OR in a **slot-usage mask** from a per-mode table:
  - native 16-colour mode (`CRES≥2`): `mask |= (1<<(v>>4)) | (1<<(v&15))`, via a 256-entry
    `uint16_t` table;
  - 4-colour (`CRES 1`) and 2-colour (`CRES 0`): their own tables, slots 0–3 and 0–1;
  - text with attributes: fg/bg slots from the attribute byte;
  - COCO modes: the fixed CSS-selected slots, a constant per line mode;
  - border: always uses `BRDR` / its CSS slot.

  That is one table load and one OR per byte, in the loop that already reads the byte.
- Mix into the signature **only the palette values of used slots**:
  `for each set bit i in mask: SIG_MIX(palette_reg[i] | i<<8)`. For speed, mix the 16 values masked
  by the used bits: `pal & usedmask_bytes`, as four 32-bit words.
- Unused slots no longer affect the hash, so lines that don't show the cycling colours stay skipped.

**Optional extra:** add a global `palette_gen` counter, bumped by `tcc1014_write_palette()` only
when the value actually changes, and skip the per-line palette mixing when nothing changed this
frame. A small win, only if Phase 0.2 shows the 4 `SIG_MIX` calls matter.

**Why it is general:** palette cycling is a standard CoCo 3 technique: waterfalls and water, title
screens, fades, flashing text, and OS-9 programs changing palettes. It also helps programs that
write the **same** value to a palette register every frame. Many IRQ handlers do this, and the
current code already handles them via the hash, at no extra cost.

**Risks:**
- The used-slot tables must match each renderer exactly. In particular:
  - `HRES==0 && CRES>=2` zeroes odd bytes, so slot 0 is always used;
  - artifact mode uses fixed colours, not palette slots;
  - the COCO text and semigraphics `SnA` path uses `pal[(v>>4)&7]` and `RGCSS0_0`;
  - blink and underline use `fg`/`bg`.

  Mitigation: an assertion build that renders every line anyway and compares the output when the
  new skip says "clean" (debug flag, off in release).

**Expected:** depends on the picture. A cycling area under a third of the screen cuts palette-frame
render work by over two thirds. A fully cycling picture gains nothing from Phase 2 and relies on
Phase 3.

**Gate:**
- `hs2pal1.bas` lines rendered per frame falls to the cycling area only.
- `hs2pal.bas` shows no regression.
- Zero golden diffs, including `10-palette-probe.png` (mid-frame palette).
- The assertion build runs 10 minutes of the CoCo Max title and editor, NitrOS-9 and the BASIC
  benchmarks with no mismatch.
- Cost: +192 bytes RAM (a `uint16_t` mask per line, if kept) and ~1.5 KB of tables (`const`, flash
  or DRAM).

### Phase 3: one-pass, word-wide redraw straight into the framebuffer (fixes B2)

When a line does need drawing, write it once, 32 bits at a time, into the FabGL row.

**Design:**
- **Per-line byte→pixels LUT.** For the line's mode, keep a table that maps one VRAM byte to its
  output pixels, already converted to raw VGA bytes **and pre-swizzled** for FabGL's `x^2` order.

  | Mode | Output pixels per byte | Table size |
  |---|---|---|
  | 320×16 (RES 3, CRES 2) | 4 = one `uint32_t` | 256 × 4 B = 1 KB |
  | 640×4 / 320×4 | 8 or 16 | 2–4 KB |
  | 640×2 / 1bpp VDG-compatible RG | 8 or 16 | 2–4 KB |
  | 256-wide modes (centred) | as above, plus border words | |

  Text modes keep the current per-glyph path; they are rarely the full-redraw case. They can still
  write words through a 16-entry nibble table.
- **Rebuild a table only when the colours it encodes change.** Key the table on (mode, the 16 raw
  palette bytes): compare a 16-byte snapshot, or use a `palette_gen` from Phase 2.
  - With CoCo Max's cycling that is about one rebuild per palette-change frame: 256 entries, a few
    thousand host cycles.
  - The raster FIRQ case (palette changed between lines) rebuilds per affected line, still cheaper
    than today's 640 byte stores plus a 640-byte copy.
  - Build only the half the line needs: 16×16 composition for 4bpp.
- **Write directly into `s_vga.getScanline(y)`** (a HAL entry point that returns the row pointer
  for a display line, or nullptr when the line is not shown). This removes `line_raw` and the
  second pass for the raw path. `line_raw` and the RGB565 path stay for screenshot capture and as
  the fallback for unaligned or downscaled geometries.
- The 320×16 inner loop becomes `dst[k] = lut32[src[k]]` over 160 bytes, instead of 640 byte
  stores + 160 loads + 160 rotated stores.

**Why it is general:** every native graphics line of every animated CoCo 3 program, scrolling
(every line changes), full-screen palette fades, and VDG-compatible PMODE screens on the CoCo 3.
The same trick can later serve the CoCo 2 `hal_video_render_scanline` (12-colour palette, 2× doubling).

**Risks:**
- The FabGL swizzle and the border and centring geometry. These are already encoded in
  `hal_video_render_scanline_gime_raw()`; reuse its cases, and reuse `gime_display_y()` for the
  row.
- The direct write must respect `set_tall()` (512×240) and the `line < 0` drop rows.

**Gate:**
- `PROBE_RENDER_SCANLINE + PROBE_HAL_SCANLINE` on `hs2pal.bas` and `hs2anim.bas` drops ≥ 50%.
- Zero golden diffs for all baseline scenarios and the new ones.
- Framebuffer-readback checks (`/api/screenshot.png?fb=1` vs core capture, `cmpfb.py`) match.
- Cost: 1–4 KB DRAM for the active LUT. A two-entry cache (current + previous palette) avoids
  thrash when two palettes alternate per line, if Phase 0 shows that pattern.

### Phase 4: CPU-side and cache experiments (measure-gated, may be rejected)

Run these only after Phases 1–3, and keep each only if CoCo Max *and* the generic benchmarks
improve without hurting `PERF_BENCH` on either CPU.

**4.1 Placement under real cache pressure.** Earlier IRAM tests used microbenchmarks with a hot
cache. Under this workload, retest:
- `IRAM_ATTR` on the Phase 3 line writer and `line_signature`;
- the computed-goto dispatch already measured at 26 vs 44 cycles, with its table in DRAM;
- moving the hottest `execute_one` cases to IRAM, if 0.4 showed cache eviction.

IRAM budget: check `heap_caps_get_free_size(MALLOC_CAP_EXEC)` with WiFi on.

**4.2 Cheaper SWI/PSHS/PULS stack traffic.** The S stack lives in a fast page. Check in the opcode
profile whether `SWI`, `PSHS`/`PULS` with many registers, and `RTI` (which CoCo Max runs on every
IRQ) dominate. If they do, a tighter inline sequence for full-register push and pull (one page
lookup, then 8–12 direct byte stores when the stack range doesn't cross a page) could help. This is
interpreter code, so it falls under the codegen-sensitivity warning; benchmark on both CPUs.

**4.3 Per-scanline overhead.** 262 calls per frame of `sv_disk_tick`, the rs232 check,
`tcc1014_tick_scanline`, two CA1 transitions, IRQ routing lambdas, becker, ssc, and audio capture.
Hoist the rarely-enabled peripheral checks into one per-frame bitmask. Also compute
`scanline_cycle_targets[]` only when `cycles_per_frame` changes; that was OPT-G2 in the old plan,
so check whether it landed. Expected small, but it applies to every frame of every program.

**4.4 Hashing cost on static screens (only if Phase 0.2 shows it matters).** Ideas:
- hash 8 bytes per step with the two lanes interleaved;
- or skip re-hashing lines whose VRAM 8 KB page had no CPU writes, using a per-page write flag set
  in the **slow** path only. To get those, map displayed VRAM pages with `wr_page = nullptr`, but
  only while a frame is static. This is complex and hurts drawing programs, so it is research only.

### Phase 5: automatic video frame-skip (fixes B5, user option)

When the previous frame's work exceeded the 16.7 ms period, skip **display** work for the next
frame. The CPU, timers, interrupts and audio still run for the full frame, so the guest keeps real
time.

- In a skipped frame, `machine_run_scanline_coco3` still runs the vertical state machine and
  sets VBORD/HBORD and the row and address counters. It does not call `tcc1014_render_scanline`
  or the HAL, and it leaves `s_line_sig` untouched. The next drawn frame then compares against the
  last *drawn* content, which is correct.
- Cap the skip at 1 in 2 (display ≥ 30 FPS), with hysteresis.
- Off by default at first. Add a supervisor setting ("Video frame-skip: Off / Auto"), stored in
  NVS. Report `frames_drawn` and `frames_emulated` in `/api/status`, and show both in the FPS
  overlay ("60/38").
- Screenshot capture forces a drawn frame.
- **Why it is general:** this is the standard answer, in MAME and XRoar alike, for a host that is
  occasionally too slow. It keeps music tempo and game speed correct in any heavy scene.

**Gate:**
- With Auto on, the CoCo Max title reports 60 emulated FPS, and audio pitch in a music program
  stays constant during heavy video (`tools/frequency_meter.py`).
- With it off, behaviour is identical to today.

### Phase 6: render offload to core 0 (contingency only)

Same as Phase 6 of `docs/coco3-video-optimization.md`: per-line state packets go to a core-0
renderer. Consider it only if Phases 1–5 leave common software below 60 FPS. The risks are
unchanged: WiFi and debug-server contention, PSRAM bus sharing, and tearing.

---

## 7. Order of work and expected effect

| Order | Item | Effort | Risk | Helps CoCo Max title by | General beneficiaries |
|---|---|---|---|---|---|
| 0 | Baseline, probes, 4 new benchmarks | S | none | (measurement) | all future work |
| 1 | 1.1–1.2 `memw` removal (video, machine) | S | low | redraw and per-line overhead | all CoCo 3/2 video, every scanline |
| 2 | 1.3–1.4 idempotent and incremental MMU/SAM | S | low | dozens of writes per frame | OS-9, BASIC, bank-switching games |
| 3 | 1.5–1.6 direct I/O decode, inline keyboard AND | M | low–med | hundreds of accesses per frame | keyboard/joystick polling, OS-9 |
| 4 | 2 palette-aware line skip | M | med | lines not using cycling slots | palette-animating software |
| 5 | 3 one-pass word-wide redraw | M–L | med | every redrawn line | all animated, scrolling, fading programs |
| 6 | 5 auto frame-skip (option) | M | low (off by default) | keeps 60 Hz emulation | any too-heavy scene |
| 7 | 4.x CPU and cache experiments | M | med–high (codegen) | CPU share | everything, if they pass |
| 8 | 6 dual-core render | L | high | (contingency) | — |

These are **estimates** and need Phase 0 data. If Phase 0 confirms the cost model, Phases 1–3
should give back most of the gap on palette frames and a meaningful part of the CPU/I-O share on
quiet frames. That would bring the CoCo Max title near 60 FPS with no CoCo Max-specific code.
Phase 5 then guarantees real-time emulation even where it doesn't.

---

## 8. Verification for every phase

1. `tools/build_firmware.sh`, flash with `PERF_PROBE_ENABLED 1`.
2. Re-run the baseline scenarios (BASIC prompt, `WIDTH 40/80`, `PMODE 4`, `HSCREEN 2/3`,
   `hs2anim`) plus the new benchmarks (`hs2pal`, `hs2pal1`, `joyloop`, `mmuloop`), NitrOS-9 boot,
   and the CoCo Max title and editor.
3. **Golden diff is the merge gate:** `tools/perf/measure.sh` screenshots (core capture and `fb=1`
   framebuffer readback) compared with `cmpfb.py`. Any diff blocks the merge.
4. Mid-frame palette probe (`10-palette-probe.png`) and the CoCo Max **Colors > Set Colors…** chart
   (raster FIRQ, 64 colours). The chart is a good real-world check of per-scanline palette changes.
5. `tools/cpu_test/run.sh` (and `PERF_BENCH` on both CPUs for any interpreter-adjacent change).
6. Manual smoke:
   - OSD open/close (repaint);
   - F5 overlay toggle;
   - 512×240 switch (Donkey Kong / Rick Dangerous);
   - disk game;
   - audio pitch at 60 FPS;
   - WiFi debug server responsive.
7. Log the results, rejected attempts included, in `docs/performance.md`, and add a line to
   `ChangeHistory.md` on release.

---

## 9. Explicitly out of scope

- Detecting CoCo Max (by name, by code bytes, or by the `$FFA3` write pattern) or special-casing it.
- Trapping `SWI` to run CoCo Max's system calls natively.
- Faking the joystick comparator, for example by answering a whole SAR conversion in one read. The
  6-step read is guest behaviour and must stay cycle-exact.
- Lowering the CPU clock or skipping CPU cycles in fast mode.
- `GIME_VGA_DOWNSCALE`: it breaks 80-column text (`config.h`).

---

## Appendix A: CoCo Max III reference points used above

| Address | Name | Relevance |
|---|---|---|
| `$30C0` | `ColdStart` | Vectors, fast CPU (`$FFD9`), all-RAM (`$FFDF`) |
| `$1F60` | `Sys54_MainLoop` | Writes `$FFD9` and `$FFDF` every pass |
| `$4200` | `SWI_Dispatch` | Writes `$FFA3`=`$37` on every SWI |
| `$42E1` | `Sys00_ReadInput` | Fire button + 4×6-step SAR conversions via `$FF20`/`$FF00`, mux via `$FF01`/`$FF03`, retries ≤10 when the value changes; maps page `$3B` |
| `$77A0` (page `$3B`) | `JoyRelative` | Relative pointer window |
| `$3A7C` | `Sys4C_KeyScan` | `$FF02` write + `$FF00` read |
| `$326D` | `IRQ_VSync` | Palette cycle (11–14) / animate (10), `CLR $FF93` |
| `$3310` | `FIRQ_RasterPalette` | 64-colour chart only: palettes 3–11 every 14 lines (GIME timer) |
| `$5D83`/`$5D9D` | opening sequence | Loads `TITLEPGE.SYS`, then waits in `L_5DD8` |
| `$5724` | title header | Palette, `CycleSpeed`/`AnimSpeed`, cycle and animate enables |
| `$58A4` | title decompressor | Literal / repeat / line-above (`-160,X`) into `$8000` pages |
| `$5920` | file byte reader | `JSR $A176` at 0.89 MHz |

## Appendix B: emulator code reference

| Area | Location |
|---|---|
| Frame / scanline loop | `src/core/machine.cpp`, `machine_run_frame_coco3` (878), `machine_run_scanline_coco3` (659) |
| I/O callbacks | `machine_read_coco3` (203), `machine_write_coco3` (355) |
| GIME decode | `src/core/tcc1014.cpp`, `tcc1014_mem_cycle` |
| MMU / SAM / palette writes | `tcc1014_write_mmu`, `tcc1014_write_sam`, `tcc1014_write_palette` |
| Line signature / skip | `line_signature` (1362), `tcc1014_render_scanline` (1406) |
| Renderers | `render_native_gfx` and siblings (1122–1320) |
| HAL raw copy | `src/hal/hal_video.cpp`, `hal_video_render_scanline_gime_raw` |
| `memw` workaround | `src/utils/no_psram_memw.h` (not included by `hal_video.cpp` or `machine.cpp`) |
| Probes | `src/utils/perf_probe.h/.cpp`, `tools/perf/` |
| Previous video plan | `docs/coco3-video-optimization.md` (OPT-G2–G6) |
