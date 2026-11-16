# GIME interrupt delivery tests

Run against the board's debug API (`COCO_HOST`, default `http://192.168.50.111`).
Needs `lwasm`. See `improvements.11.16.md` (Phase 0 results) for the reference counts.

## firqtest.py: FIRQ handler entries per frame

Injects a 6809 program at `$4000` that counts FIRQ handler entries over 600 frames
(IRQ masked, frames counted by polling the PIA0 FS flag), then compares them with the
`gime_acks` counters in `/api/status`.

    python3 firqtest.py <label> FAST MASK TINS RELOAD

| Test | Arguments | Correct result |
|---|---|---|
| HBORD, 0.89 MHz | `0 '$10' 0 0` | 262 entries/frame, 0 % zero |
| HBORD, 1.79 MHz | `1 '$10' 0 0` | 262 entries/frame, 0 % zero |
| Timer, scanline, reload 10 | `0 '$20' '$00' 10` (and `1 ...`) | 23.82/frame, 0 % zero |
| Timer, 3.58 MHz, reload 600 | `0 '$20' '$20' 600` (and `1 ...`) | 99.40/frame, 0 % zero |

"zero" = acknowledges (`$FF93` reads) that returned 0: the handler was entered with
nothing latched.

## sample.py: a program that is already running

    python3 sample.py <label> [seconds]

Prints speed, fps, frame time, timer fires, FIRQ/IRQ acknowledges per frame with the
zero share, and audio underruns over the interval.
