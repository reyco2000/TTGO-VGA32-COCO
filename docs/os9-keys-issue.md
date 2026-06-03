# OS-9 Supervisor Keys & FPS Overlay Problem

> **Historical — ESP32-S3 + TFT build only.**
>
> This document analyzes a bug observed on the original ESP32-S3 + ILI9341 TFT + USB HID build, which used a `pushSprite` SPI display path with a VRAM-shadow `memcmp` optimization and a USB HID Core 0 queue. The root cause described below (VRAM shadow compare skipping both the SPI push *and* the FPS overlay draw) **does not apply to the active TTGO VGA32 build**, where:
>
> - There is no `pushSprite` — FabGL's DMA scans the framebuffer continuously, so there is no per-frame "push" step to skip.
> - The VRAM-shadow compare (`OPT-16`) is gated out under `BOARD_TYPE_VGA32`.
> - The FPS overlay draw is unconditional whenever the overlay is enabled (`fps_tick_and_draw()` in `hal_video.cpp`, called every frame from `hal_video_present_gime()` / `hal_video_present()`).
> - The keyboard path is FabGL PS/2 with a VirtualKey queue, not USB HID with a Core-0 FreeRTOS task — F-key intercept happens in `process_vk()` on Core 1.
>
> If a similar OS-9 symptom appears on the VGA32 build, start a fresh investigation: the architecture below is the wrong starting point. The Spanish-keyboard layout selection in `hal_keyboard_init()` and the supervisor/CoCo-matrix dispatch in `process_vk()` would be the first places to look.

## Symptom (S3+TFT build)

Under BASIC/Extended BASIC ROMs, F1 (supervisor OSD), F2 (soft reset), F5 (FPS overlay) all work correctly. After booting into OS-9, F1 and F2 become unresponsive and the FPS overlay disappears.

## Architecture Summary

### F-Key Processing Path
```
USB HID keyboard (Core 0)
  → FreeRTOS queue (32 slots)
  → hid_host_process() drains queue (Core 1, once per loop())
  → on_hid_key() callback in hal_keyboard.cpp
  → F1/F2/F3/F5 intercepted BEFORE CoCo matrix
```

### Main Loop (TTGO-VGA32-COCO.ino)
```
loop() {
  hal_process_input()              ← drains USB queue, calls on_hid_key()
  supervisor_update_and_render()   ← if active, renders OSD, skips emulation
  machine_run_frame(&coco)         ← 262 scanlines of CPU + VDG
  hal_render_frame()               ← (no-op, video present done in machine_run_frame)
}
```

### FPS Overlay (hal_video.cpp)
- `fps_update()` counts frames (called every frame)
- `fps_overlay_draw()` renders text to TFT (called only when TFT push happens)
- Both are called inside `hal_video_present()`, which has a VRAM shadow compare optimization

## Root Cause Analysis

### Confirmed: FPS disappears due to VRAM shadow compare
In `hal_video_present()` (hal_video.cpp:294-296):
```cpp
if (ram && force_push_count == 0 &&
    memcmp(vram_shadow, ram + vdg_base, vram_size) == 0) {
    return;  // No change — skip SPI push
}
```
When VRAM is unchanged (OS-9 idle screen is static), this early return skips **both** the TFT sprite push **and** the FPS overlay draw. Under BASIC, cursor blink causes periodic VRAM changes so FPS keeps drawing. Under OS-9, the screen is truly static.

### Suspected: F1/F2 display issue after supervisor close
When F1 closes the supervisor:
1. `restore_snapshot()` fills OSD area with black
2. Emulation resumes → `hal_video_present()` runs
3. VRAM shadow compare finds no change (emulation was paused)
4. TFT push skipped → display stays broken (black rect where OSD was)
5. User perceives F1 as "not working"

There is no `hal_video_force_repaint()` mechanism to invalidate the shadow after supervisor close.

### To investigate: Are USB events reaching Core 1 at all?
It's possible the USB host on Core 0 stops delivering events under OS-9 for an unknown reason (heap corruption, task starvation, etc.). Debug prints will confirm.

## Proposed Fixes (pending diagnosis)

### Fix 1: Decouple FPS overlay from VRAM push
Move `fps_overlay_draw()` outside the early-return gate so it draws every frame regardless of VRAM changes. FPS draws directly to TFT (~50x16 px), not through the sprite, so it doesn't need a full push.

### Fix 2: Add `hal_video_force_repaint()`
New function: `force_push_count = 3;` — reuses existing mechanism. Call it from `supervisor_toggle()` on deactivate and after F2 `machine_reset()`.

### Fix 3: Force repaint on supervisor deactivate
In `supervisor_toggle()`, after `restore_snapshot()`, call `hal_video_force_repaint()`.

### Fix 4: Force repaint after F2 reset
After `machine_reset()` in F2 handler, call `hal_video_force_repaint()`.

## What Was Implemented (Step 5 — Diagnostics)

**File modified:** `src/hal/hal_keyboard.cpp`

Added `DEBUG_PRINTF` / `DEBUG_PRINT` calls at key points in `on_hid_key()`:

| Location | Debug Output | Purpose |
|----------|-------------|---------|
| Entry of `on_hid_key()` | `HID key: usage=0x%02X mod=0x%02X DOWN/UP` | Confirms USB events arrive at all |
| F1 handler (usage 0x3A) | `KEY: F1 pressed — supervisor toggle (state was active/inactive)` | Confirms F1 event reaches handler |
| F2 handler (usage 0x3B) | `KEY: F2 pressed — soft reset (machine_ptr=valid/NULL)` | Confirms F2 event + machine pointer |
| F3 handler (usage 0x3C) | `KEY: F3 pressed — quick mount` | Confirms F3 event |
| F5 handler (usage 0x3E) | `KEY: F5 pressed — FPS overlay toggle` | Confirms F5 event |

### How to Test

1. Compile and upload
2. Open serial monitor
3. Boot into BASIC — press F1/F2/F5, confirm debug messages appear
4. Boot into OS-9 — press F1/F2/F5, check serial monitor

### Interpreting Results

| Serial Output | Diagnosis | Next Step |
|---------------|-----------|-----------|
| No `HID key` messages under OS-9 | USB host on Core 0 stopped | Investigate USB task / heap |
| `HID key` messages but no `KEY: F1` | Wrong usage codes or logic skip | Check usage code values |
| `KEY: F1` appears, no visual change | Display issue (VRAM shadow compare) | Implement Fixes 1-4 |
| `KEY: F2` with `machine_ptr=NULL` | Machine pointer lost | Investigate `hal_keyboard_set_machine()` |

## Key Files Reference

| File | Role |
|------|------|
| `src/hal/hal_keyboard.cpp:231-265` | F-key interception in `on_hid_key()` — **modified** |
| `src/hal/hal_video.cpp:266-312` | `hal_video_present()` with VRAM shadow compare |
| `src/supervisor/supervisor.cpp:387-402` | `supervisor_toggle()` activate/deactivate |
| `src/supervisor/supervisor.cpp:57-69` | `capture_snapshot()` / `restore_snapshot()` |
| `src/hal/usb_kbd_host.cpp` | USB HID host on Core 0, FreeRTOS queue |
| `src/hal/hal.cpp:37-44` | `hal_process_input()` — drains queue each frame |
| `src/core/machine.cpp:577-601` | `machine_run_frame()` — 262 scanlines per frame |
