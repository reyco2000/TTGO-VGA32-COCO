# Runtime CoCo 2 / CoCo 3 Switching

## Overview

`MACHINE_TYPE` in `config.h` is now a **compile-time default** only. Both machine paths (CoCo 2: SAM + VDG, CoCo 3: GIME) are compiled into every firmware image and the active one is selected at boot from a runtime variable `g_machine_type`. The user switches machines from the supervisor menu; the new selection is saved in NVS and the device reboots into it via `esp_restart()`.

This document describes the end-to-end implementation. See the original design brief `coco2and3.md` in the project root for the staged rollout.

## Boot flow

1. `setup()` (`TTGO-VGA32-COCO.ino`) calls `hal_init()`.
2. Before `machine_init()`, the sketch reads the target machine type from NVS:
   ```c
   g_machine_type = supervisor_load_machine_type();
   ```
   Falls back to the compile-time `MACHINE_TYPE` when NVS has no entry.
3. `machine_init(&coco)` dispatches on `g_machine_type` to either `machine_init_coco2()` or `machine_init_coco3()`.
4. `machine_load_roms()` loads **both** ROM sets on every boot (missing ROMs log a warning but are not fatal).
5. `hal_video_init()` brings up a single 640×200 FabGL framebuffer (`DISPLAY_WIDTH × DISPLAY_HEIGHT`); CoCo 2's 256×192 VDG output is runtime-centered inside it via `COCO2_X_OFF` / `COCO2_Y_OFF`.

## Core API (`src/core/machine.h`)

```c
extern uint8_t g_machine_type;   // 3 = CoCo 2, 4 = CoCo 3
```

Public machine functions are **runtime dispatchers** that branch on `g_machine_type`:

| Dispatcher | CoCo 2 impl | CoCo 3 impl |
|------------|-------------|-------------|
| `machine_init`          | `machine_init_coco2`          | `machine_init_coco3`          |
| `machine_load_roms`     | `machine_load_roms_coco2`     | `machine_load_roms_coco3`     |
| `machine_reset`         | `machine_reset_coco2`         | `machine_reset_coco3`         |
| `machine_run_scanline`  | `machine_run_scanline_coco2`  | `machine_run_scanline_coco3`  |
| `machine_run_frame`     | `machine_run_frame_coco2`     | `machine_run_frame_coco3`     |
| `machine_read`          | `machine_read_coco2`          | `machine_read_coco3`          |
| `machine_write`         | `machine_write_coco2`         | `machine_write_coco3`         |
| `machine_free`          | `machine_free_coco2`          | `machine_free_coco3`          |

`machine_load_roms` is the one exception: it calls **both** loaders regardless of active machine and returns the active machine's success flag. This pre-validates both ROM sets on boot.

### Hot-path behaviour

Per-machine `machine_init_cocoN()` pins the CPU callbacks directly:
```c
m->cpu.read  = machine_read_cocoN;
m->cpu.write = machine_write_cocoN;
```
So the 6809 memory-access hot path does **not** go through the dispatcher — zero extra branches per CPU cycle.

## `Machine` struct layout

Both machine paths' state coexists in a single flat `Machine` struct (`machine.h`). Field groups:

- Shared: `cpu`, `pia0`, `pia1`, `fdc`, timing/frame counters.
- CoCo 3 (used when `g_machine_type == 4`): `gime`, `ram_physical` (512 KB PSRAM), `rom_coco3` (32 KB), `rom_disk` (8 KB).
- CoCo 2 (used when `g_machine_type == 3`): `vdg`, `sam`, `ram` (aliased to first 64 KB of `ram_physical`), `rom_basic` (8 KB), `rom_extbas` (8 KB), `rom_cart` (16 KB).

Both init paths allocate the **same** superset: 512 KB RAM + all five ROM buffers. This keeps PSRAM/heap footprint invariant across machine types. The inactive machine's buffers are populated but unused on a given boot.

## Supervisor integration (`src/supervisor/`)

### NVS persistence (`supervisor.cpp`)

```c
uint8_t supervisor_load_machine_type(void);   // read NVS "sv"/"machine_type"
void    supervisor_set_machine_type(uint8_t); // save + esp_restart() — no return
```

- Namespace `"sv"`, key `"machine_type"` (uint8).
- `supervisor_set_machine_type()` calls `supervisor_save_state()` first to preserve mounted disks across the restart.

### OSD submenu (`sv_menu.cpp`)

- Main menu's "Machine:" row shows the active machine (dynamic via `g_machine_type`, not the compile-time name).
- ENTER on "Machine:" opens the `SV_MACHINE_SELECT` state (new case in `supervisor.cpp` on_key / render dispatchers).
- Submenu shows CoCo 2 / CoCo 3 with a `(current)` marker on the active one.
- ENTER on the active machine is a no-op return; ENTER on the other opens `SV_CONFIRM_DIALOG` ("Switch to CoCo X and restart?").
- Accept → `supervisor_set_machine_type()` → persists to NVS → `esp_restart()`.
- Cancel → returns to the submenu. ESC from the submenu restores the main-menu cursor/count.

## Verification

- `arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=16M,PSRAM=opi TTGO-VGA32-COCO/`
- Both `MACHINE_TYPE=3` and `MACHINE_TYPE=4` builds compile cleanly.
- Flash usage ~588 KB / 1.28 MB (~44%). PSRAM usage invariant at ~552 KB for the combined RAM + ROM buffers.
- NVS erase → boots whichever `MACHINE_TYPE` is the compile-time default.
- Mount a disk, switch machines, verify the disk auto-mounts after the reboot (`supervisor_save_state()` persists mount paths before restart).

## Files touched

| File | Change |
|------|--------|
| `config.h` | `MACHINE_TYPE` is now a default; added `MACHINE_NAME_COCO2` / `MACHINE_NAME_COCO3`. |
| `src/core/machine.h` | Unconditionally includes both chip headers; flat superset struct; `extern uint8_t g_machine_type`. |
| `src/core/machine.cpp` | Removed `#if MACHINE_TYPE` guards around the two implementations; all per-machine symbols renamed with `_coco2` / `_coco3` suffix; added 8 runtime dispatchers; invariant superset allocation; dual ROM load. |
| `src/hal/hal_video.cpp` | Removed all `MACHINE_TYPE` guards; Mode-0 sprite unified to display superset; CoCo 2 output runtime-centered via `COCO2_X_OFF` / `COCO2_Y_OFF`. |
| `src/supervisor/supervisor.h` | Added `supervisor_load_machine_type` / `supervisor_set_machine_type`. |
| `src/supervisor/supervisor.cpp` | Wired `SV_MACHINE_SELECT` into on_key / render dispatchers; implemented NVS load/set. |
| `src/supervisor/sv_menu.h`, `sv_menu.cpp` | Dynamic main-menu label; new machine-select submenu handlers with confirm-dialog → restart. |
| `TTGO-VGA32-COCO.ino` | Seeds `g_machine_type` from NVS before `machine_init()`. |
