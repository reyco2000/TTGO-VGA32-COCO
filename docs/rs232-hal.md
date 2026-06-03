# RS-232 Pak HAL — `hal_rs232.cpp` + `mc6551.cpp`

Emulation of the Tandy Deluxe RS-232 Program Pak (26-2226), an MC6551 ACIA at
`$FF68–$FF6B`, bridged to the ESP32's default serial port (UART0). Lets CoCo
software (`OPEN/PRINT #-2`, OS-9 `/t2`, terminal programs) exchange data with a
host PC.

See the design doc `2026-05-11-rs232-pack-design.md` for the full spec.

## The single-UART constraint

The TTGO VGA32 (ESP32-WROVER) has **no native USB**. `Serial.begin(115200)` is
**UART0** (GPIO1 TX / GPIO3 RX) over the onboard USB-UART bridge — the same port
debug output uses. There is only one serial port, so the RS-232 Pak and debug
logging **cannot both own it**. This is enforced by a single mode enum, not two
booleans (see below).

## Serial-port ownership: `SerialPortMode` (`utils/debug.h`)

```c
enum SerialPortMode : uint8_t {
    SERIAL_MODE_OFF   = 0,  // UART0 idle
    SERIAL_MODE_DEBUG = 1,  // UART0 carries debug logs; pak decode inactive
    SERIAL_MODE_RS232 = 2,  // UART0 carries pak data; debug muted
};
extern volatile SerialPortMode g_serial_mode;   // defined in hal.cpp
```

- `rs232_pak_enabled()` (`hal.h`) == `(g_serial_mode == SERIAL_MODE_RS232)` and
  gates the `$FF68` decode, the per-scanline tick, and the FIRQ OR-in.
- The `DEBUG_PRINT` / `DEBUG_PRINTF` macros emit only when
  `g_serial_mode == SERIAL_MODE_DEBUG` (runtime gate, on top of the
  compile-time `DEBUG_ENABLED` guard).
- `serial_mode_apply(mode)` (`hal.cpp`) switches ownership: resets the ACIA and,
  when leaving debug, flushes buffered host input. It does **not** persist.
- Persistence is NVS (`"sv"` namespace, key `serial_mode`) via
  `supervisor_load_serial_mode()` / `supervisor_save_serial_mode()`. The mode is
  applied in `setup()` **before** the boot banner so a persisted RS-232 link is
  never corrupted by debug output. First-ever boot defaults to
  `SERIAL_MODE_FIRST_BOOT_DEFAULT` (= DEBUG, `config.h`).

The OSD presents this as two linked toggles in **Settings** (`Debug Log`,
`RS-232 Pak`); enabling one forces the other off.

## Layers

```
machine.cpp  ── $FF68-$FF6B decode ──►  mc6551_read / mc6551_write
             ── per-scanline       ──►  mc6551_tick(cycles)
             ── FIRQ OR-in         ◄──  mc6551_irq_active()
                       │ tx_byte_out         ▲ rx_byte_in
                       ▼                      │
mc6551.cpp   (pure ACIA: 4 regs, baud throttle, 1-byte RX FIFO, IRQ latch)
                       │ Serial.write         ▲ ring_pop
                       ▼                      │
hal_rs232.cpp (UART0 bridge: 256B RX ring, callback wiring, flush/poll)
```

### `mc6551.cpp` — pure chip (no Arduino includes)

- Registers: `$FF68` data, `$FF69` status / programmed-reset, `$FF6A` command,
  `$FF6B` control. Baud bits drive a `cycles_per_byte` countdown; baud index 0
  ("16x ext clock") means no throttle.
- The TX byte hits the host **immediately**; only the CoCo's TDRE/RDRF view is
  paced, matching XRoar. 1-deep TX queue; 1-byte RX FIFO with overrun.
- IRQ latch (status bit 7) set on enabled RDRF/TDRE, cleared by reading status.
  `mc6551_irq_active()` feeds the 6809 **FIRQ** line (Deluxe Pak routes ACIA IRQ
  to CART → FIRQ).
- Host transport is injected via `mc6551_tx_byte_out` / `mc6551_rx_byte_in`
  function pointers, wired by `hal_rs232_begin()`.

### `hal_rs232.cpp` — UART0 transport

- `hal_rs232_begin()` wires the callbacks and clears the ring.
- `hal_rs232_poll()` drains `Serial` into a 256-byte static RX ring; called from
  `hal_process_input()` only while in RS-232 mode. UART0's driver FIFO provides
  backpressure once the ring is full.
- `hal_rs232_flush()` discards buffered host input on a mode change.
- `hal_rs232_ring_fill()` / `_capacity()` feed the debug page.

## OSD debug page

`sv_debug.cpp` adds `SV_DBG_PAGE_RS232` (Left/Right to reach it): control/status/
command registers, baud, TDRE/RDRF, TX/RX/overrun counts, ring fill, FIRQ count.

## Host tooling

`picocom -b 115200 /dev/ttyUSB0` (or `/dev/ttyACM0`). This is a real UART, so the
line baud matters — match 115200. The CoCo's programmed baud only paces the
emulated flags, not the wire.
