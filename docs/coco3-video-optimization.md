# CoCo 3 (GIME) Video Performance Optimization Plan — TTGO VGA32

## Context

The emulator currently runs CoCo 3 at **38 FPS idle BASIC / 21.5 FPS animated** (after OPT-G1, the 64 KB HAL pixel LUT). Perf probes show the dominant cost is the core scanline decoder `tcc1014_render_scanline()` (~31.5 ms, ~68% of an animated frame); `hal_scn` ≈ 9 ms; CPU emulation ≈ 5 ms (not the bottleneck). The goal is to reach **≥30 FPS animated and 55-60 FPS static**, with zero pixel differences in any video mode.

The plan is informed by how MAME and XRoar structure their video paths:
- **MAME**: per-mode templated scanline emitters (`render_scanline<bytes, emit_gime_graphics_samples<bpp, xscale>>`) — mode dispatch happens **once**, then a tight specialized loop runs. Legacy (CoCo-compat) vs native is decided at frame/update level, not per byte.
- **XRoar**: fetch a whole line's bytes from memory in one call, then decode; LUT-driven color mapping.

Our current renderer does the opposite: one monolithic function re-evaluates the `COCO`/`BP`/`CRES`/`resolution` branch tree **per byte** (~10K nibble iterations/frame), and fetches VRAM from PSRAM one byte at a time.

**Documented dead ends (do not re-attempt):** hoisting `gime->` mode fields into const locals inside the monolith regressed 31.5→34.4 ms (stack spills, docs/performance.md:27); `IRAM_ATTR` on hot CPU functions cost -2 FPS; `GIME_VGA_DOWNSCALE` breaks 80-col text. All detailed probe numbers except OPT-G1's are from the retired ESP32-S3+SPI build — re-measure first.

## Key files

| File | Role |
|---|---|
| `src/core/tcc1014.cpp` | Renderer (730-951), VRAM fetch (699-716), mode derivation `update_from_gime_registers()` (602-661), palette write (505-513) |
| `src/core/tcc1014.h` | `TCC1014` struct, `line_buffer[640]` (uint16, DRAM) |
| `src/core/machine.cpp` | Scanline loop (469-656), frame loop (662-689) |
| `src/hal/hal_video.cpp` | `hal_video_render_scanline_gime()` (278-349), 64 KB PSRAM LUT, capture path (221-236) |
| `config.h` | `PERF_PROBE_ENABLED` (line 71), new flags |
| `src/utils/perf_probe.*` | Probes already wired around the exact hot calls |

Each phase is independently shippable, measurement-gated, and gets an `OPT-Gn` entry in `docs/performance.md` (continuing from OPT-G1). Build with the `build-firmware` skill; verify with the `coco3-debug` MCP tools.

---

## Phase 0 — Re-baseline on VGA32 + golden screenshot suite (no code risk)

1. Set `PERF_PROBE_ENABLED 1` (config.h:71), build, flash.
2. Capture probe tables (`render_scn`/`hal_scn`/`cpu_run`/`frame` µs) + FPS for 7 scenarios:
   - Boot `OK` prompt (VDG-compat 32-col text), `WIDTH 40`, `WIDTH 80` (native text ± attr bytes)
   - `PMODE 4:SCREEN 1,1` with drawn pattern (VDG-compat RG); `PMODE 3:SCREEN 1,0` (CG 4-color)
   - `HSCREEN 2` + pattern (native 320×192×16); `HSCREEN 3` (native 640×192×2, the 1:1 HAL path)
   - Animated variants of PMODE 4 and HSCREEN 2 (BASIC loop or `inject_code` VRAM scroller) — these are the gate numbers
3. Save `mcp__coco3-debug__screenshot` goldens per scenario (FPS overlay OFF). The capture path reads RGB565 `line_buffer` upstream of the VGA LUT, so goldens diff core output directly.
4. Build a mid-frame-palette fidelity probe: `inject_code` a tight 6809 loop rewriting `$FFB0`; keep for reuse.
5. Record everything in a new "VGA32 re-baseline (2026-07)" section of `docs/performance.md`.

---

## Phase 1 — OPT-G2: cheap hoists (low risk)

- **machine.cpp:592-611**: delete the per-scanline recompute of `row_stride` and `border_colour` (duplicated in `update_from_gime_registers()`, tcc1014.cpp:640-660) and the dead `g->Xoff = g->X;` (renderer resets `Xoff = 0` at tcc1014.cpp:740).
- **Required guard**: in COCO graphics mode `border_colour` reads `palette_reg[]`, and `tcc1014_write_palette()` does *not* refresh derived state — add a border-colour refresh (or `update_from_gime_registers()` call) to `tcc1014_write_palette()` so mid-program `PALETTE` changes still update the border.
- **machine.cpp:667-669**: compute `scanline_cycle_targets[]` only when `cycles_per_frame` changes, not 262 divisions every frame.

**Gate:** zero golden diffs (specifically re-test border color after a `PALETTE` change); no FPS regression.

---

## Phase 2 — OPT-G3: line-batched VRAM fetch (XRoar-style; foundation for Phases 3-4)

Add a file-static DRAM staging buffer `static uint8_t s_line_bytes[264];` in tcc1014.cpp and a `fetch_line_bytes()` helper. The consumed stream is exactly sequential `ram[(B+k) & (ram_size-1)]` (Xoff starts at 0; `fetch_byte_vram`'s pair-cache makes it linear), so stage it with one or two wrap-aware `memcpy`s from PSRAM, then have the renderer read `s_line_bytes[k++]` — no per-byte call, no cache-flag bookkeeping, DRAM reads only.

- Replicate the `HRES==0 && CRES>=2` quirk (tcc1014.cpp:820-822) by zeroing odd-indexed staging bytes in that mode.
- Max consumption: 2×BPR ≤ 162 bytes (attr text); assert at mode-selection time.
- Note (don't silently "fix"): the `g->X` horizontal-scroll register is currently ignored by the renderer — pre-existing behavior, out of scope.

**Gate:** `render_scn` improved or neutral (±0.5 ms) with zero golden diffs; watch attr-text blink/underline (`WIDTH 80`) and the HRES=0/CRES≥2 quirk mode.

---

## Phase 3 — OPT-G4: per-mode specialized scanline renderers (MAME-style — the big win)

Replace the per-byte branch tree with a renderer function pointer selected **once in `update_from_gime_registers()`** (all mode-affecting writes already funnel there, so mid-frame register changes still take effect on the next scanline).

```c
typedef unsigned (*gime_line_renderer)(TCC1014* g, const uint8_t* src, uint16_t* dst);
// TCC1014 gains: gime_line_renderer line_renderer;
```

`tcc1014_render_scanline()` becomes a thin dispatcher: early-outs → `fetch_line_bytes()` → `line_width = line_renderer(...)` → 640 clamp.

Variants (template/macro-generated, ~20 small leaf functions, est. 4-8 KB flash):
- **Native graphics** (`!COCO && BP`): `template<CRES, RES>` — CRES∈{0,1,2} × RES∈{0..3} = 12. Byte load → unrolled nibble emit (from tcc1014.cpp:880-895) → fixed expansion pattern (907-942). No switches.
- **Native text**: `template<ATTR, RES>` (RES∈{1,2}) = 4. Blink/underline/rowmask logic verbatim in-loop; `font_row` computed once per line. Read `gime->blink` per line (it toggles mid-run on the GIME timer) — never cache at selection time.
- **COCO graphics**: `render_coco_cg`, `render_coco_rg<RES>`, `render_coco_rg2` — mode choice (tcc1014.cpp:767-774) is line-stable. Resolve fg/bg/cg RGB565 once per line at function entry (small leaf functions register-allocate cleanly — this is why the split differs from the rejected monolith hoist).
- **COCO text/SG**: single `render_coco_text` — the `SnA = vdata & 0x80` branch (tcc1014.cpp:759) is data-dependent per byte and **must stay in-loop**; CSS colors, GM1/GM0 invert config, `font_row` hoist per line.

Keep the current monolith behind a `GIME_RENDER_LEGACY` config flag (default 0) for A/B probing and screenshot bisecting during bring-up; delete after acceptance.

**Expected:** `render_scn` 31.5 → ~18-23 ms, +4-6 FPS animated.
**Gate:** ≥15% `render_scn` reduction on animated 320×200×16 AND zero golden diffs. If under, check codegen (pragma O2 applies to instantiations; objdump for spills) before abandoning. Verify with full golden suite + mid-frame palette probe + manual smoke (boot, disk game, WIDTH 80 editing).

---

## Phase 4 — OPT-G5: dirty-line skip (big win for static screens)

Content-compare, not write-tracking (avoids instrumenting the CPU write fast path; automatically correct under row-replication and scrolling):

- PSRAM shadow `ps_malloc(225 * 264)` ≈ 58 KB; DRAM `uint32_t line_gen[225]` + `uint16_t line_width_cache[225]` (~1.4 KB).
- Global `video_gen` counter bumped on: `update_from_gime_registers()`, `tcc1014_write_palette()`, blink toggle, `tcc1014_set_inverted_text()`, reset/`set_machine`, plus a new `tcc1014_video_invalidate()`.
- Dispatcher: after `fetch_line_bytes()`, if `line_gen[line] == video_gen` and `memcmp(staging, shadow) == 0` → restore cached `line_width`, flag `line_clean`, return. `machine_run_scanline_coco3()` then also skips `hal_video_render_scanline_gime()` — safe because FabGL's framebuffer persists across frames (continuous DMA scanout).
- **Invalidation hooks (the risk area):** force-render while screenshot capture is armed (add `hal_video_capture_is_armed()`); wire `hal_video_force_repaint()` (currently a no-op, hal_video.cpp:357) to invalidate for 2 frames on OSD/menu close (grep callers in `src/supervisor/`); invalidate on FPS-overlay toggle.

**Expected:** idle BASIC skips ~190/192 lines → 38 → ~55-60 FPS; animated overhead (memcmp+memcpy per line) ≈ 0.5-1 ms.
**Gate:** idle ≥ +10 FPS AND animated regression ≤ 1 ms AND no visual residue after OSD close / overlay toggle / mode switch. If animated regression exceeds 1 ms, make compare adaptive (stop comparing after N consecutive dirty frames).

---

## Phase 5 — OPT-G6: direct raw VGA byte emission (merge core+HAL pass)

Every active pixel is one of 16 palette entries, so the per-pixel 64 KB PSRAM LUT lookup equals `raw(palette_entry)` — provably bit-identical moved to palette-write time:

- Add `uint8_t palette_vga[16]` filled in `tcc1014_write_palette()` via a HAL-registered callback wrapping `s_gime_raw_lut[]` (hal_video.cpp:101-104).
- Renderer templates gain a pixel-type parameter; two dispatch tables (uint16 RGB565 / uint8 raw). Use raw normally; fall back to RGB565 when screenshot capture is armed (keeps goldens valid) or `GIME_ADJACENT_PIXEL_BLEND` is compiled in.
- New `hal_video_render_scanline_gime_raw()`: the `x^2` DMA swizzle within an aligned word is a 16-bit rotate — 640-wide path becomes a `uint32_t` loop (`(w<<16)|(w>>16)`); 320→640 doubling composes one word from two source bytes. `line_buffer` reused as a `uint8_t[1280]` union view (halves buffer traffic).

**Expected:** eliminates ~128K PSRAM LUT loads/frame; `hal_scn` 9 → ~3-5 ms, +3-4 FPS animated; also cheapens every dirty line in Phase 4.
**Gate:** `hal_scn` ≥ 40% reduction, zero golden diffs, screenshot-during-animation still correct.

---

## Phase 6 — Dual-core render offload (contingency only)

Only if ≥30 FPS animated is unmet after Phase 5. Core 1 pushes per-line packets (staged bytes + ~16 B mode-state snapshot + line number) into a ring; a core-0 task renders + writes the framebuffer. Must first assess: WiFi/debug-server contention on core 0, FabGL I2S interrupt affinity, PSRAM bus contention. Abort on tearing, audio underruns, or debug-server timeouts.

---

## Verification (every phase)

1. Build via `build-firmware` skill; flash; probes on.
2. Re-run the 7 Phase-0 scenarios; append probe table to `docs/performance.md` under the OPT-Gn entry (record rejected sub-attempts too, per doc convention).
3. **Golden diff = hard merge gate**: `mcp__coco3-debug__screenshot` per scenario, pixel-compare against Phase-0 goldens (any diff blocks).
4. Mid-frame palette probe (Phases 3-5).
5. Manual smoke: boot to prompt, LOADM+RUN a disk game, keyboard input, OSD menu round-trip, FPS overlay toggle, WiFi debug server responsive.

**Projected cumulative outcome:** animated 21.5 → ~30-33 FPS (Phases 3+5), idle 38 → ~55-60 FPS (Phase 4). Cost: ~1.6 KB DRAM, ~58 KB PSRAM, ~8-12 KB flash.
