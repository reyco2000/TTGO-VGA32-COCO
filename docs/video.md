# Video Subsystem — TTGO-VGA32-COCO (TTGO VGA32)

> **Unified framebuffer for runtime-switchable machines:** `hal_video.cpp` has no `#if MACHINE_TYPE` guards. FabGL allocates one 640×200 framebuffer that serves both CoCo 2 and CoCo 3. The VDG render path (`hal_video_render_scanline`) centers and pixel-doubles CoCo 2's 256×192 output inside the 640×200 surface; the GIME render path (`hal_video_render_scanline_gime`) writes at the surface's native dimensions. The inactive path's code is present but unused on a given boot. See `runtime-machine-switch.md`.

## Overview

The video pipeline renders MC6847 VDG (CoCo 2) or TCC1014 GIME (CoCo 3) scanlines to a **640×200 @ 70 Hz VGA framebuffer** managed by the FabGL library. Unlike the earlier TFT/SPI variant of this project, **there is no per-frame `present()` push step**: FabGL's hardware DMA scans the framebuffer out to the VGA DAC continuously, so a write to a scanline byte is visible on the monitor on its next scan cycle.

## Hardware

| Parameter | Value |
|-----------|-------|
| Board | LilyGo TTGO VGA32 v1.4 (ESP32-WROVER-E) |
| Output | VGA via on-board 6-bit resistor-ladder DAC (RGB222) |
| Mode | `VGA_640x200_70Hz` (FabGL built-in modeline) |
| Pixel format | 64-color direct (2 bits per channel, packed with HSYNC/VSYNC bits into one byte per pixel) |
| Library | FabGL 1.0.9 — `fabgl::VGAController` |
| VGA pins | R0=GPIO21, R1=GPIO22, G0=GPIO18, G1=GPIO19, B0=GPIO4, B1=GPIO5, HSYNC=GPIO23, VSYNC=GPIO15 |
| Framebuffer location | PSRAM (via FabGL allocator) |
| Framebuffer size | 640 × 200 = 128,000 bytes (≈ 125 KB) |

**Why VGAController (64-color direct) and not VGA16Controller (16-color paletted):** the GIME core emits pre-converted RGB565 pixels (OPT-C4), not palette indices. VGA16Controller would require either touching the GIME core to emit indices, or doing a per-pixel RGB565 → palette-slot reverse lookup in the HAL. VGAController takes raw RGB222 per pixel, so a straight RGB565 → RGB222 channel-truncation is the only conversion needed and the core stays untouched.

## Pipeline

```
GIME / VDG          Machine Loop            HAL Video                 VGA Monitor
─────────────      ──────────────          ──────────────             ─────────
line_buffer    →   render_scanline*  →  FabGL framebuffer (scanline N)
                                          │
                                          ▼
                                       FabGL DMA  ───────→  HSYNC/VSYNC + RGB222 pixels
                                       (continuous,
                                        70 Hz)
```

### Per-Scanline Flow

1. `machine_run_scanline()` runs CPU cycles for one scanline
2. For active scanlines:
   - **CoCo 2:** `mc6847_render_scanline()` fills `vdg.line_buffer[]` with palette indices (0–15)
   - **CoCo 3:** `tcc1014_render_scanline()` fills `line_buffer[]` with byte-swapped RGB565 pixels (OPT-C4 — the GIME does the palette-to-RGB lookup once per palette write, not per pixel)
3. The HAL writes that line directly into the FabGL framebuffer via `s_vga.getScanline(y)` — one byte per pixel using the `X ^ 2` indexing required by FabGL's DMA byte order
4. **No present step.** FabGL's DMA is already scanning the same framebuffer to the monitor.

### `hal_video_present()` / `hal_video_present_gime()`

These are **no-ops** on the VGA32 build (no SPI push, no shadow compare needed) — they only:
- Update the FPS counter when the overlay is enabled
- Clear the dirty flag the GIME core uses for its own bookkeeping

Note: this also means the OPT-16 VRAM shadow compare and OPT-C9 CRC32 dirty-frame skip from the earlier SPI/TFT variant of this project are *not used* here. The continuous-DMA architecture turns those into pure overhead with no benefit — a "skip the push" optimization is meaningless when there is no push.

## Display Layout

`DISPLAY_WIDTH × DISPLAY_HEIGHT` is fixed at **640 × 200** for the VGA32 build (see `config.h`). The HAL uses FabGL's `getViewPortWidth()` / `getViewPortHeight()` at runtime to discover the actual surface, so the rendering code adapts if the modeline is changed in `hal_video_init()`.

There is no equivalent of the old `DISPLAY_SCALE_MODE 0/1/2` for the VGA32 path. The HAL has a single rule per width:

| Source width | HAL behavior on 640-wide framebuffer |
|--------------|--------------------------------------|
| 640 (GIME hi-res, 80-col text, RG6) | 1:1, no scaling |
| 320 (GIME lo-res text, 320-wide graphics) | 2× horizontal pixel-double |
| 256 (CoCo 2 VDG) | 2× horizontal pixel-double, centered with 64-px left/right margin |
| anything `< 640` | Centered, side margins filled with GIME border color |
| anything `> 640` | Nearest-neighbor downscale |

Vertically, 192-line (VDG) and 199-line (GIME) modes are centered within the 200-line surface (small top/bottom margin); 200-line modes fit 1:1; 225-line modes (rare GIME LPF=3) are clipped or rendered with `y_off = 0`.

## Pixel Format Conversion

### GIME path: RGB565 → RGB222

The GIME core emits pre-converted, byte-swapped RGB565 pixels. The HAL converts each pixel to FabGL's 6-bit raw byte:

```c
// Unswap, then top-2-bits of each channel.
uint16_t c = __builtin_bswap16(pixels[x]);
uint8_t r = (c >> 14) & 0x03;
uint8_t g = (c >>  9) & 0x03;
uint8_t b = (c >>  3) & 0x03;
row[x ^ 2] = s_vga.createRawPixel(fabgl::RGB222(r, g, b));
```

`createRawPixel()` returns a single byte with the HSYNC/VSYNC bits already merged in, so the write is one byte store per pixel.

### VDG path: 16-color palette → cached RGB222 byte

The 16 MC6847 palette entries (12 used + 4 reserved) are converted to RGB222 once and cached on first use:

```c
static uint8_t cache[16];
if (!ready) {
    for (int i = 0; i < 16; i++)
        cache[i] = s_vga.createRawPixel(fabgl::RGB222(r3[i], g3[i], b3[i]));
}
// per pixel:
uint8_t b = cache[pixels[x] & 0x0F];
row[(x_off + x*2)     ^ 2] = b;
row[(x_off + x*2 + 1) ^ 2] = b;
```

### GIME 64-color border LUT

A 64-entry lookup `s_gime_raw_lut[64]` is built once at init from the GIME palette format (interleaved `R1 G1 B1 R0 G0 B0`). Used to fill side margins for modes narrower than the full framebuffer, and the entire row's left/right border on every GIME scanline.

```c
for (int i = 0; i < 64; i++) {
    uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1));   // 0..3
    uint8_t g = (((i >> 3) & 2) | ((i >> 1) & 1));
    uint8_t b = (((i >> 2) & 2) | ((i >> 0) & 1));
    s_gime_raw_lut[i] = s_vga.createRawPixel(fabgl::RGB222(r, g, b));
}
```

## FPS Overlay

Toggled via `hal_video_toggle_fps_overlay()` (mapped to F5 in the supervisor).

Counts emulated frames and updates the FPS value once per second. On the VGA32 build the overlay is **not yet drawn into the framebuffer** — the counter runs and you can read it from the serial debug printout, but the visible-on-screen FPS overlay still needs to be wired through the FabGL canvas. See "Known Gaps" below.

## OSD Canvas

The supervisor OSD code (`sv_render.cpp`, `sv_debug.cpp`, etc.) draws into the FabGL framebuffer through `src/hal/osd_canvas.{h,cpp}`:

- `hal_video_init()` binds the canvas to the live `fabgl::Canvas` via `OSDCanvas::bind_canvas(&s_canvas)`
- `hal_video_get_canvas()` returns a pointer to the `OSDCanvas` instance for the supervisor
- Each supervisor call (`fillRect`, `drawString`, `setTextColor`, datum-based alignment, etc.) is forwarded to FabGL's `Canvas` primitives, converting RGB565 → RGB888 with bit replication
- Font 1 / font 2 map to FabGL `FONT_8x8` / `FONT_8x14`

## GIME-Specific Notes

The GIME's video subsystem is implemented in `src/core/tcc1014.cpp`. Highlights relevant to the HAL:

- The GIME core has a 64-entry palette LUT it builds from the live palette registers ($FFB0-$FFBF). Pixels are pre-converted to RGB565 in the line buffer.
- The HAL receives `border_colour` as a 6-bit GIME index (not RGB565) and uses `s_gime_raw_lut[]` to convert it for margin fill.
- `total_lines` (the LPF-derived active line count: 192, 199, or 225) is passed per scanline so the HAL can compute `y_off` and clamp.
- The `palette` parameter is unused on this build — the GIME core already resolved each pixel to RGB565. It's retained in the function signature for ABI stability with older code.

### Vertical State Machine (Active Area Control)

The GIME vertical timing in `machine_run_scanline()`:

```
Scanline 0:    FS falling edge, latch lAA/lTB for this frame
Scanline 4:    FS rising edge
Scanline 7:    VRAM address reset, lcount = 0
Scanline 7+N:  lcount increments each scanline
               When lcount == lTB: active area ENTERS (exactly once)
               After lAA active lines: active area EXITS, VBORD interrupt
```

**Critical:** the entry condition uses `==`, not `>=`, to prevent re-entry after the active area ends.

## Key Functions

| Function | Purpose |
|----------|---------|
| `hal_video_init()` | Init FabGL VGAController, set resolution, build GIME LUT, bind OSD canvas |
| `hal_video_render_scanline()` | Convert one VDG scanline to RGB222 bytes in the FabGL scanline (CoCo 2) |
| `hal_video_render_scanline_gime()` | Convert one GIME scanline (RGB565 → RGB222) to the FabGL scanline (CoCo 3) |
| `hal_video_present()` / `hal_video_present_gime()` | No-op apart from FPS counter / dirty flag (FabGL scans continuously) |
| `hal_video_set_mode()` | Stub — mode changes handled by VDG/GIME directly |
| `hal_video_force_repaint()` | No-op on continuously-scanned VGA |
| `hal_video_toggle_fps_overlay()` | Toggle FPS counter |
| `hal_video_get_canvas()` | Return the `OSDCanvas` instance for the supervisor OSD |

## Files

- `TTGO-VGA32-COCO/config.h` — `BOARD_TYPE`, `DISPLAY_WIDTH`/`HEIGHT`, VGA pin defines
- `TTGO-VGA32-COCO/src/hal/hal_video.cpp` — both backend implementations (gated on `BOARD_TYPE`)
- `TTGO-VGA32-COCO/src/hal/hal.h` — HAL interface declarations
- `TTGO-VGA32-COCO/src/hal/osd_canvas.h/.cpp` — OSD drawing canvas (backed by FabGL Canvas)
- `TTGO-VGA32-COCO/src/core/mc6847.h/.cpp` — VDG scanline rendering (palette index output)
- `TTGO-VGA32-COCO/src/core/tcc1014.h/.cpp` — GIME scanline rendering (pre-converted RGB565 output)
- `TTGO-VGA32-COCO/src/core/machine.cpp` — frame loop calling render_scanline + present

## Known Gaps

- **FPS overlay** counter runs but is not drawn into the framebuffer yet — needs a `OSDCanvas::drawString` call once per frame wired into `hal_video_present()`.
- **OSD coordinate space** — the supervisor was laid out for 320×240. On a 640×200 surface the OSD currently sits in the upper-left ~half of the screen. A coordinate-mapping pass in `sv_render.cpp` (or in `osd_canvas`) would re-center it.
- **CoCo 2 VDG palette colors** are approximated to the closest RGB222 (2 bits per channel = only 4 levels) — fine for distinguishability but visually coarser than the original RGB565 table.
