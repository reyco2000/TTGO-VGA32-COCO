# TTGO-VGA32-COCO Core Modules — Implementation Details

> **Runtime machine selection:** `MACHINE_TYPE` in `config.h` is a compile-time default only; the active machine is `g_machine_type` (declared in `machine.h`, seeded from NVS at boot). Public machine functions (`machine_init`, `machine_read`, `machine_run_frame`, etc.) are dispatchers that call the `_coco2` / `_coco3` variant matching `g_machine_type`. The 6809 hot path is pinned to the active machine's read/write during init, so per-cycle memory access bypasses the dispatcher. See `runtime-machine-switch.md`.

## Overview

The `TTGO-VGA32-COCO/src/core/` directory contains the emulation core: the four major ICs of the CoCo 2 plus the machine integration layer that wires t
hem together. All modules are derived from XRoar's C source by Ciaran Anscomb, adapted to C++ for the ESP32-S3 Arduino environment.

**Source files:**

| File | IC / Role | Lines |
|------|-----------|-------|
| `mc6809.h` / `mc6809.cpp` | Motorola MC6809 CPU | ~2,650 |
| `mc6809_opcodes.h` | Opcode definitions & cycle tables | 88 |
| `mc6821.h` / `mc6821.cpp` | Motorola 6821 PIA (×2) | 175 |
| `mc6847.h` / `mc6847.cpp` | Motorola MC6847 VDG (CoCo 2) | 396 |
| `sam6883.h` / `sam6883.cpp` | SAM6883 Address Multiplexer (CoCo 2) | 164 |
| `tcc1014.h` / `tcc1014.cpp` | TCC1014 GIME — MMU, video, interrupts (CoCo 3) | ~940 |
| `machine.h` / `machine.cpp` | System integration & memory map (CoCo 2 + CoCo 3) | ~640 |

---

## MC6809 — CPU (`mc6809.h`, `mc6809.cpp`)

Full emulation of the Motorola MC6809E 8-bit processor, the heart of the CoCo.

### Registers

| Register | Width | Description |
|----------|-------|-------------|
| `pc` | 16-bit | Program counter |
| `d` | 16-bit | Accumulator D (A = high byte, B = low byte) |
| `x`, `y` | 16-bit | Index registers |
| `s` | 16-bit | Hardware stack pointer |
| `u` | 16-bit | User stack pointer |
| `dp` | 8-bit | Direct page register |
| `cc` | 8-bit | Condition codes (E, F, H, I, N, Z, V, C) |

### Condition Code Flags

| Flag | Bit | Description |
|------|-----|-------------|
| E | 7 | Entire state saved (1 = full push on interrupt, 0 = partial/FIRQ) |
| F | 6 | FIRQ mask (1 = FIRQ disabled) |
| H | 5 | Half carry (bit 3 carry, used by DAA) |
| I | 4 | IRQ mask (1 = IRQ disabled) |
| N | 3 | Negative (MSB of result) |
| Z | 2 | Zero (result == 0) |
| V | 1 | Overflow (signed arithmetic) |
| C | 0 | Carry / borrow |

### Execution Loop (`mc6809_run`)

```
mc6809_run(cpu, budget):
  cycles = 0
  while cycles < budget:
    if cpu->halted:
      cycles = budget; break          // FDC HALT burns budget
    check_interrupts(cpu)             // NMI > FIRQ > IRQ priority
    if cpu->wait_for_interrupt:
      cycles = budget; break          // CWAI/SYNC idles
    execute_one(cpu)                  // Fetch-decode-execute
  return cycles
```

- **HALT support**: When `cpu->halted` is true (set by the FDC's DRQ/HALT mechanism), the CPU burns its entire budget doing nothing — essential fo
r disk I/O synchronization.
- **CWAI/SYNC**: `wait_for_interrupt` flag causes the CPU to idle until an interrupt arrives. Any code that sets `irq_pending` or `firq_pending` must also clear `wait_for_interrupt` to wake the CPU — see "SYNC Wake-up Fix" below.

### Interrupt Handling

Three interrupt types, checked before each instruction in priority order:

**NMI** (`mc6809_nmi(cpu, active)`)
- **Edge-triggered**: Latches `nmi_pending` on inactive→active transition of `nmi_line`. After servicing, `nmi_line` is cleared — a new edge is re
quired for the next NMI.
- **Non-maskable**: Cannot be disabled via CC flags.
- **nmi_armed gate**: NMI is ignored until the first LDS instruction executes (per MC6809 spec). This prevents spurious NMI during reset when S is
 uninitialized.
- **Stack push**: Full state (E=1) — CC, A, B, DP, X, Y, U, PC → 12 bytes on S stack (19 cycles). If `cwai_state` is true, push is skipped (7 cycl
es).
- **Masks**: Sets both I and F flags.
- **Vector**: $FFFC.
- **Used by**: FDC INTRQ for disk transfer completion.

**FIRQ** (`mc6809_firq(cpu, active)`)
- **Level-triggered**: `firq_pending` mirrors the pin state.
- **Masked by**: F flag in CC.
- **Fast**: Pushes only CC and PC (E=0) — 3 bytes (10 cycles). Exception: if `cwai_state` is true, the full state was already pushed with E=1 by C
WAI.
- **Masks**: Sets both I and F flags.
- **Vector**: $FFF6.
- **Used by**: Cartridge interrupt (PIA1 IRQA/IRQB).

**IRQ** (`mc6809_irq(cpu, active)`)
- **Level-triggered**: `irq_pending` mirrors the pin state.
- **Masked by**: I flag in CC.
- **Stack push**: Full state (E=1) — 12 bytes (19 cycles), or 7 cycles if CWAI.
- **Masks**: Sets I flag only (F unchanged).
- **Vector**: $FFF8.
- **Used by**: 60Hz vsync timer (PIA0 CB1) and keyboard.

**CWAI and SYNC operations:**
- **CWAI** ($3C): ANDs an immediate byte with CC (clearing mask bits to allow interrupt), sets E=1, pre-pushes entire state to S stack, then enter
s `wait_for_interrupt`. When an interrupt arrives, the handler skips the redundant push — only vectoring and masking are needed (7 cycles instead
of 19).
- **SYNC** ($13): Enters `wait_for_interrupt` without pushing state. The interrupt that wakes SYNC causes a normal push-and-vector sequence.

### Interrupt Vector Table

| Vector | Address | Use |
|--------|---------|-----|
| RESET | $FFFE | Power-on / reset |
| NMI | $FFFC | FDC disk transfer |
| SWI | $FFFA | Software interrupt |
| IRQ | $FFF8 | 60Hz timer, keyboard |
| FIRQ | $FFF6 | Cartridge |
| SWI2 | $FFF4 | (unused on CoCo) |
| SWI3 | $FFF2 | (unused on CoCo) |

### Opcode Coverage

All documented MC6809 opcodes are implemented across three pages:

- **Page 1** (no prefix): 8-bit ALU (ADD, ADC, SUB, SBC, AND, OR, EOR, CMP, TST, NEG, COM, CLR, INC, DEC, LSR, LSL/ASL, ASR, ROR, ROL), loads/stor
es (LD, ST for A, B, D, X, Y, S, U), branches (BRA, BEQ, BNE, BCC, BCS, BPL, BMI, BVS, BVC, BGE, BGT, BLE, BLT, BHI, BLS, BSR), stack ops (PSHS, P
ULS, PSHU, PULU), LEA (LEAX, LEAY, LEAS, LEAU), TFR, EXG, MUL, DAA, SEX, ABX, NOP, SYNC, CWAI, SWI, RTI, RTS
- **Page 2** (prefix `$10`): 16-bit comparisons (CMPD, CMPY), long branches (LBRA, LBSR, LBcc), LDY/STY, LDS/STS, SWI2
- **Page 3** (prefix `$11`): CMPU, CMPS, SWI3

### Addressing Modes

All MC6809 addressing modes are implemented:

| Mode | Syntax | Example | Notes |
|------|--------|---------|-------|
| Inherent | — | CLRA | No operand |
| Immediate 8 | #nn | LDA #$42 | |
| Immediate 16 | #nnnn | LDD #$1234 | |
| Direct | dp:nn | LDA $30 | DP register provides high byte |
| Extended | nnnn | LDA $1234 | Full 16-bit address |
| Indexed | various | LDA ,X | Complex postbyte decoding (see below) |
| Relative 8 | offset | BNE loop | Signed 8-bit (-128..+127) |
| Relative 16 | offset | LBNE loop | Signed 16-bit |

**Indexed sub-modes** (decoded from postbyte):
- Constant offset: 5-bit signed, 8-bit signed, 16-bit signed
- Register offset: A,R / B,R / D,R
- Auto-increment: ,R+ / ,R++ (post-increment by 1 or 2)
- Auto-decrement: ,-R / ,--R (pre-decrement by 1 or 2)
- Zero offset: ,R
- PC-relative: 8-bit or 16-bit offset from PC
- Indirect: [any of the above] — adds an extra memory read for the effective address
- Extended indirect: [nnnn]

### `mc6809_opcodes.h`

Reference tables for cycle counts (PROGMEM-ready) and opcode constant definitions. The cycle counts in this header are informational — actual coun
ting is done inline within `mc6809.cpp` to avoid lookup overhead. Also defines common opcode constants (`MC6809_OP_LDA_IMM`, `MC6809_OP_SWI`, etc.
) and TFR/EXG register codes.

### Performance Optimizations

- **Branchless flag computation**: ALU helpers (`op_add8`, `op_sub8`, `op_add16`, `op_sub16`, `update_nz8`, `update_nz16`) use a compute-and-mask
pattern — flags are accumulated into a local variable `f` and written to `cpu->cc` in a single masked OR. The `CC_PUT` macro uses a branchless ter
nary that compiles to Xtensa MOVNEZ. This optimization improved performance from ~23.5 to ~25–27 fps.
- **Inline memory helpers**: `mem_read`, `mem_write`, `fetch8`, `fetch16`, push/pull helpers are all `static inline` to eliminate function call ov
erhead in the hot instruction loop.
- **D register as single uint16_t**: A and B are stored as the high and low bytes of a single `uint16_t d`, accessed via `GET_A()` / `GET_B()` mac
ros and `SET_A()` / `SET_B()`. This makes 16-bit D operations (ADDD, SUBD, LDD, STD) naturally efficient.
- **No IRAM_ATTR**: Testing showed that placing CPU functions in IRAM actually hurt performance on ESP32-S3 (flash cache is faster than IRAM for l
arge code).

---

## MC6821 — PIA (`mc6821.h`, `mc6821.cpp`)

Emulates the Motorola 6821 Peripheral Interface Adapter. Two instances are used in the CoCo:

| Instance | Address | Role |
|----------|---------|------|
| **PIA0** | $FF00–$FF03 | Keyboard matrix, joystick comparator, hsync (CA1), vsync (CB1) |
| **PIA1** | $FF20–$FF23 | 6-bit DAC audio (PA2–PA7), single-bit audio (PB1), VDG mode (PB3=CSS, PB7=AG), cartridge IRQ |

### Registers (per port, ×2 ports per PIA)

| Register | Offset | Description |
|----------|--------|-------------|
| Data/DDR A | 0 | Port A data or direction register (selected by CRA bit 2) |
| Control A | 1 | IRQ flags (bits 7–6, read-only), CA2 control, DDR select, CA1 edge/enable |
| Data/DDR B | 2 | Port B data or direction register (selected by CRB bit 2) |
| Control B | 3 | IRQ flags, CB2 control, DDR select, CB1 edge/enable |

### Control Register Bit Layout

```
Bit 7: IRQ1 flag (read-only) — set by CA1/CB1 transition matching edge select
Bit 6: IRQ2 flag (read-only) — set by CA2/CB2 transition (if configured as input)
Bit 5: CA2/CB2 direction (1 = output, 0 = input)
Bit 4: CA2/CB2 output control (when bit 5 = 1)
Bit 3: CA2/CB2 control / edge select (when bit 5 = 0)
Bit 2: DDR/Data select (0 = DDR, 1 = data register)
Bit 1: CA1/CB1 edge select (0 = falling, 1 = rising)
Bit 0: CA1/CB1 IRQ enable (1 = enable IRQ output)
```

### Key Operation Details

**DDR/Data selection:**
- Control register bit 2 selects whether offset 0/2 accesses the Data Direction Register (DDR) or the data register.
- DDR bit = 0 means the corresponding pin is an input; DDR bit = 1 means output.
- After reset, all DDR bits are 0 (all inputs) and CRx bit 2 is 0 (DDR selected). BASIC ROM configures the DDR first, then sets bit 2 to switch to
 data mode.

**Read behavior:**
- Port A reads: output bits come from `data_a & ddr_a`, input bits from `input_a & ~ddr_a` (mixed read).
- Port B reads: identical mixing of output and input bits.
- Reading the data register clears both IRQ flags (bits 7 and 6 of the control register) and recalculates the IRQ output — this is how BASIC ackno
wledges the 60Hz timer.

**Write behavior:**
- Writing the control register preserves bits 7–6 (read-only IRQ flags); only bits 5–0 are written.
- Writing data register stores the value; only bits with DDR=1 appear on the output pins.

**CA1/CB1 edge-triggered interrupts:**
- The `mc6821_ca1_transition()` / `mc6821_cb1_transition()` functions accept a `bool rising` parameter.
- Edge polarity is configurable via control register bit 1: 0 = falling edge, 1 = rising edge.
- When the transition matches the configured edge, IRQ1 flag (bit 7) is set.
- The IRQ output is asserted if `(IRQ1 flag && IRQ1 enable)` OR `(IRQ2 flag && IRQ2 enable && CA2/CB2 is input)`.

**IRQ output callbacks:**
- Each port has an `irq_a_callback` / `irq_b_callback` function pointer.
- The callback fires whenever the computed IRQ output state changes.
- `pia_update_irq_a/b()` is called after any flag set/clear or control register write.

**Reset behavior:**
- All registers zeroed, all pins become inputs (DDR = 0), `input_a/b` set to $FF (pulled high).
- IRQ callbacks are preserved across reset and called with `false` to deassert.

### CoCo-Specific Wiring

```
PIA0 IRQA, IRQB → CPU IRQ    (60Hz vsync timer + keyboard)
PIA1 IRQA, IRQB → CPU FIRQ   (cartridge)
PIA0 CB1 ← VDG FS            (vsync — falling edge triggers 60Hz IRQ)
PIA0 PA0–PA7 ← keyboard rows (active-low, scanned by ROM)
PIA0 PB0–PB7 → keyboard cols (column select)
PIA0 PA7 ← joystick DAC comparator result
PIA0 CRA bit 3 (CA2 output) → joystick axis select (0=X, 1=Y)
PIA0 CRB bit 3 (CB2 output) → joystick port select (0=right, 1=left)
PIA1 PA2–PA7 → 6-bit DAC     (SOUND / PLAY audio)
PIA1 PB1 → single-bit audio  (cassette relay, beep)
PIA1 PB3 → VDG CSS           (color set select)
PIA1 PB7 → VDG AG            (alpha/graphics select)
```

### 60Hz Timer Operation (detailed flow)

```
1. VDG field sync (scanline 192) → FS falling edge
2. mc6821_cb1_transition(&pia0, false)
3. CoCo has ctrl_b bit 1 = 0 → falling edge match
4. Sets IRQ1 flag (ctrl_b bit 7)
5. pia_update_irq_b() → callback fires mc6809_irq(true)
6. CPU services IRQ → reads PIA0 CRB ($FF03) to check flag → reads data B ($FF02)
7. Data register read clears IRQ flags → pia_update_irq_b() → mc6809_irq(false)
8. ROM handler increments TIMVAL ($0112-$0113), processes SOUND/PLAY timing
```

---

## MC6847 — VDG (`mc6847.h`, `mc6847.cpp`)

Emulates the Motorola MC6847 Video Display Generator. Renders 256×192 active pixels into a per-scanline palette-indexed buffer (`line_buffer[256]`
).

### Mode Bits

| Bit | Source | Function |
|-----|--------|----------|
| AG (bit 7) | PIA1 PB7 | 0 = alphanumeric/semigraphics, 1 = graphics |
| CSS (bit 3) | PIA1 PB3 | Color set select (green/orange or alternate palette) |
| GM0–GM2 (bits 0–2) | SAM V0–V2 | Graphics sub-mode (resolution + color depth) |
| INV (bit 4) | VRAM bit 6 | Per-character inverse video |
| AS (bit 6) | VRAM bit 7 | Per-character semigraphics-4 select |

### Color Palette

12 VDG colors mapped to RGB565 values:

| Index | Name | RGB565 | Approximate RGB | Usage |
|-------|------|--------|-----------------|-------|
| 0 | Green | 0x0FE1 | (10,255,10) | Text fg (CSS=0), 2bpp CSS=0 color 0 |
| 1 | Yellow | 0xFFE8 | (255,255,67) | 2bpp CSS=0 color 1 |
| 2 | Blue | 0x20B6 | (34,20,180) | 2bpp CSS=0 color 2, CSS=1 color 1 |
| 3 | Red | 0xB024 | (182,5,34) | 2bpp CSS=0 color 3 |
| 4 | White/Buff | 0xFFFF | (255,255,255) | 2bpp CSS=1 color 3 |
| 5 | Cyan | 0x0EAE | (10,212,112) | Semigraphics |
| 6 | Magenta | 0xF8FF | (255,28,255) | Semigraphics |
| 7 | Orange | 0xFA01 | (255,66,10) | Text fg (CSS=1), 2bpp CSS=1 color 2 |
| 8 | Black | 0x0841 | (9,9,9) | Text bg, 2bpp CSS=1 color 0 |
| 9 | Dark Green | 0x0200 | (0,65,0) | 1bpp CSS=0 background |
| 10 | Dark Orange | 0x6800 | (108,0,0) | 1bpp CSS=1 background |
| 11 | Bright Orange | 0xFDA8 | (255,180,67) | 1bpp CSS=1 foreground |

### Text Mode (AG=0)

- **32×16 characters**, each 8 pixels wide × 12 scanlines tall
- Internal 64-character ROM font (768 bytes, stored in PROGMEM, copied to DRAM at init for fast access)
- Characters 0x00–0x3F: `@`, `A`–`Z`, `[`, `\`, `]`, `↑`, `←`, space, `!`–`?`
- Font data: 6 pixels wide (bits 5–0), centered in 8-pixel cell with 1-pixel padding on each side

**Normal vs Inverse:**
- **Normal** (VRAM bit 6 = 0): bright character on dark background (fg on bg)
- **Inverse** (VRAM bit 6 = 1): dark character on bright background (bg on fg)
- CoCo BASIC stores all visible text as VDG "inverse" ($40–$7F), so the user sees dark-on-green
- CLS fills video memory with $60 (inverse space) — renders as solid green screen

**Semigraphics-4** (VRAM bit 7 = 1):
- 2×2 block per character cell: top-left=bit 3, top-right=bit 2, bottom-left=bit 1, bottom-right=bit 0
- 8 colors from bits 6–4 (VDG color index 0–7)
- Each quadrant is 4 pixels wide × 6 scanlines tall
- Background is always black

### Graphics Modes (AG=1)

| GM | Mode | Resolution | BPP | Bytes/Row | CoCo Name |
|----|------|-----------|-----|-----------|-----------|
| 0 | CG1 | 64×64 | 2 | 16 | — |
| 1 | RG1 | 128×64 | 1 | 16 | — |
| 2 | CG2 | 128×64 | 2 | 32 | PMODE 0 |
| 3 | RG2 | 128×96 | 1 | 16 | PMODE 1 |
| 4 | CG3 | 128×96 | 2 | 32 | PMODE 2 |
| 5 | RG3 | 128×192 | 1 | 16 | PMODE 3 |
| 6 | CG6 | 128×192 | 2 | 32 | PMODE 4 |
| 7 | RG6 | 256×192 | 1 | 32 | — |

**2bpp color sets (4 colors per pixel pair):**
- CSS=0: Green, Yellow, Blue, Red (standard VDG palette)
- CSS=1: Black, Blue, Orange, White (NTSC artifact color approximation — pixel 0 mapped to Black instead of Buff/White so games using pixel 0 as "background" display correctly)

**1bpp color pairs:**
- CSS=0: Dark Green background / Bright Green foreground
- CSS=1: Dark Orange background / Bright Orange foreground

**Upscaling:** All modes are upscaled to 256 pixels wide in the line buffer. Scale factor = 256 / native_width (1× for RG6, 2× for 128-wide modes,
 4× for 64-wide modes).

### Rendering Pipeline

```
1. machine_run_scanline() sets vdg->row_address from SAM running counter
2. mc6847_render_scanline() reads VRAM at row_address:
   - AG=0: render_text_scanline() — font lookup + inverse/semigraphics
   - AG=1: render_graphics_scanline() — pixel unpacking + palette
3. Output: line_buffer[256] contains palette indices (0–11)
4. hal_video_render_scanline() converts palette indices to RGB222 → FabGL VGA scanline
```

### Mode Change Detection

`mc6847_set_mode()` is called by `update_vdg_mode()` in machine.cpp whenever PIA1 port B or SAM V0–V2 changes. It only logs when the mode actually
 changes (avoids debug spam during normal operation).

---

## SAM6883 — Address Multiplexer (`sam6883.h`, `sam6883.cpp`)

Emulates the SAM6883 Synchronous Address Multiplexer, which controls memory mapping, VDG display addressing, and CPU clock speed.

### Register Space ($FFC0–$FFDF)

The SAM uses 16 bit-pair registers (32 addresses). Each bit has two addresses:
- **Even address**: clear the bit
- **Odd address**: set the bit

Any write to the address triggers the action (the data byte is ignored).

| Bits | Name | Function |
|------|------|----------|
| 0–2 | V0–V2 | VDG graphics mode (maps to GM0–GM2) |
| 3–9 | F0–F6 | Display offset → base address = (F-value << 9) = F × 512 |
| 10 | P1 | Page select (all-RAM mode) |
| 11–12 | R0–R1 | CPU rate (not used in our emulation) |
| 13–14 | M0–M1 | Memory size |
| 15 | TY | Map type (ROM/RAM) |

### Register Write Operation

```cpp
sam6883_write(sam, addr):   // addr = offset from $FFC0 (0–31)
  bit_num = addr >> 1       // Which of the 16 bits
  set = addr & 1            // 0 = clear, 1 = set
  if set: reg |= (1 << bit_num)
  else:   reg &= ~(1 << bit_num)
  update_from_register()    // Recompute derived fields
```

`update_from_register()` extracts:
- `vdg_mode` = bits 0–2 (V0–V2)
- `vdg_base` = (bits 3–9) << 6 (64-byte granularity for display start)
- Divide counter parameters from lookup tables indexed by vdg_mode
- `mem_size`, `page1`, `ty` from upper bits

### VDG Address Counter

The SAM maintains a running address counter that feeds the VDG with display data addresses. This is the most complex part of the SAM emulation and
 must match XRoar exactly for all graphics modes to display correctly.

**Counter lifecycle:**

1. **Field sync (vsync)** — `sam6883_vdg_fsync(true)`: Resets counter to `vdg_base`, clears X and Y divide counters.
2. **Data fetch** — `sam6883_vdg_fetch_bytes(nbytes)`: Called once per active scanline with `bytes_per_row` for the current mode. Advances the cou
nter with divide-by-X/Y logic, processing in 16-byte chunks to match XRoar's `sam_vdg_bytes()` behavior.
3. **Horizontal sync** — `sam6883_vdg_hsync(false)`: Supplementary counter adjustment — adds `vdg_mod_add` bytes via divide logic, then clears ali
gnment bits.

**Divide-by-X/Y row repetition (indexed by GM value):**

| GM | X-div | Y-div | Mod-add | Bytes/Row | Effect |
|----|-------|-------|---------|-----------|--------|
| 0 | 1 | 12 | 16 | 16 | Each 16-byte row repeated 12× (64 rows → 192 scanlines) |
| 1 | 3 | 1 | 8 | 16 | Each 16-byte row repeated 3× (64 rows) |
| 2 | 1 | 3 | 16 | 32 | Each 32-byte row repeated 3× (64 rows) |
| 3 | 2 | 1 | 8 | 16 | Each 16-byte row repeated 2× (96 rows) |
| 4 | 1 | 2 | 16 | 32 | Each 32-byte row repeated 2× (96 rows) |
| 5 | 1 | 1 | 8 | 16 | Each 16-byte row 1× (192 rows) |
| 6 | 1 | 1 | 16 | 32 | Each 32-byte row 1× (192 rows) |
| 7 | 1 | 1 | 0 | 32 | Each 32-byte row 1× (no supplementary add) |

### Address Advancement with Divide Logic (`vdg_address_add`)

```
vdg_address_add(sam, n):
  new_addr = vdg_address + n
  if bit 4 flipped (crossed 16-byte boundary):
    xcount = (xcount + 1) % xdiv
    if xcount != 0:
      new_addr -= 0x10                // Rewind: stay on same 16-byte row
    else:
      if bit 5 flipped (crossed 32-byte boundary):
        ycount = (ycount + 1) % ydiv
        if ycount != 0:
          new_addr -= 0x20            // Rewind: stay on same 32-byte group
  vdg_address = new_addr
```

This implements XRoar's bit-boundary crossing logic. The X divider controls repetition at the 16-byte level; the Y divider controls repetition at
the 32-byte level. Together they produce the correct row repetition for all 8 graphics modes.

### Fetch Bytes Implementation

`sam6883_vdg_fetch_bytes()` processes the requested byte count in 16-byte aligned chunks. Within a 16-byte block, the address advances without div
ide logic. At each 16-byte boundary crossing, `vdg_address_add()` applies the divide counters.

```
fetch_bytes(nbytes):
  while nbytes > 0:
    b3_0 = address & 0x0F             // Position within 16-byte block
    chunk = 16 - b3_0                 // Remaining in current block
    if chunk > nbytes: chunk = nbytes
    if doesn't cross boundary:
      address += chunk                // Simple advance
    else:
      vdg_address_add(chunk)          // Apply divide logic
    nbytes -= chunk
```

### Why Two Advancement Steps?

In XRoar, the SAM counter advances in two places:
1. `sam_vdg_bytes()` — driven by the VDG data clock as it fetches bytes during active display
2. `sam_vdg_hsync()` — supplementary fixup at end of each scanline

Without the fetch step, modes like GM=7 (RG6, mod_add=0) never advance the counter — the display shows only the first row repeated 192 times. The
mod_add values are supplementary corrections, not the primary advancement mechanism.

### Clear Mask

After the hsync addition, `vdg_address &= vdg_mod_clear` clears low-order bits to realign the address:
```
GM: 0-5 → clear bits 1-4 (mask ~30 = ~0x1E) or bits 1-3 (mask ~14 = ~0x0E)
GM: 6   → clear bits 1-4 (mask ~30)
GM: 7   → no clear (mask ~0)
```

---

## Machine — System Integration (`machine.h`, `machine.cpp`)

The machine module wires all components into a complete CoCo 2 emulation. It owns all chip instances, memory buffers, and implements the 64KB addr
ess decoder.

### Machine Structure

```c
typedef struct Machine {
    MC6809   cpu;       // 6809 CPU
    MC6821   pia0;      // PIA at $FF00 (keyboard, vsync)
    MC6821   pia1;      // PIA at $FF20 (sound, VDG mode)
    MC6847   vdg;       // Video Display Generator
    SAM6883  sam;       // Address Multiplexer
    SV_DiskController fdc;  // WD1793 FDC at $FF40

    uint8_t* ram;           // 64 KB (PSRAM)
    uint8_t* rom_basic;     // 8 KB Color BASIC ($A000-$BFFF)
    uint8_t* rom_extbas;    // 8 KB Extended BASIC ($8000-$9FFF)
    uint8_t* rom_cart;      // 16 KB Disk BASIC ($C000-$FEFF)

    uint32_t scanline;      // Current scanline (0–261)
    uint32_t frame_count;
    uint32_t cycles_per_frame;  // 14916 for NTSC
    // ...
} Machine;
```

### Memory Map

```
$0000–$7FFF  RAM (64 KB, lower 32 KB directly visible)
$8000–$9FFF  Extended BASIC ROM (8 KB)   — or RAM when SAM TY=1
$A000–$BFFF  Color BASIC ROM (8 KB)      — or RAM when SAM TY=1
$C000–$FEFF  Disk BASIC / Cartridge ROM  — or RAM when SAM TY=1
$FF00–$FF1F  PIA0 (4 regs mirrored every 4 bytes)
$FF20–$FF3F  PIA1 (4 regs mirrored every 4 bytes)
$FF40–$FF5F  WD1793 Disk Controller (DSKREG + FDC regs)
$FF60–$FFBF  Reserved (reads $FF) — except $FF68–$FF6B below
$FF68–$FF6B  MC6551 ACIA — RS-232 Pak, only when enabled (see rs232-hal.md)
$FFC0–$FFDF  SAM control (write-only bit set/clear)
$FFE0–$FFFF  Interrupt vectors (from Color BASIC ROM $BFE0–$BFFF)
```

### SAM All-RAM Mode (MAP TYPE)

Writing to $FFDF sets the SAM MAP TYPE bit (`sam.ty = true`), enabling **all-RAM mode**. In this mode, $8000–$FEFF maps to RAM instead of ROM — the full 64 KB address space becomes read/write RAM. Writing to $FFDE clears the bit, restoring normal ROM mapping.

The I/O space ($FF00–$FFFF) is always hardware-decoded regardless of MAP TYPE, so PIA, FDC, SAM registers, and interrupt vectors continue to work normally.

This mode is required by **OS-9 Level 1**, which uses the DOS command to load a bootstrap from Track 34 into $2600, then the bootstrap enables all-RAM mode to copy itself into upper memory and load the OS-9 kernel.

### Memory Access (`machine_read` / `machine_write`)

The CPU's `read` and `write` function pointers point to `machine_read()` and `machine_write()`, which implement the full address decoder:

- **I/O space** ($FF00–$FFFF) is always checked first — hardware-decoded regardless of MAP TYPE
- **Reads** from I/O space perform side effects (PIA IRQ flag clearing, keyboard matrix scanning, FDC status)
- **Writes** to PIA1 trigger VDG mode updates and audio DAC output
- **Writes** to SAM update the VDG base address and mode bits
- **$8000–$FEFF**: when SAM TY=1, reads/writes go to RAM; when TY=0, reads return ROM and writes are ignored
- ROM vectors at $FFE0–$FFFF read from Color BASIC ROM ($BFE0–$BFFF)

### Keyboard Scanning (in `machine_read`)

When the CPU reads PIA0 data A (the ROM's KEYIN routine), `machine_read()` intercepts the read and:

1. Reads PIA0 port B output bits (column select, active low)
2. For each column driven low, calls `hal_keyboard_scan(col)` to get row data
3. ANDs all returned row data together (multi-column scan for key detection)
4. Reads joystick buttons into PA0 (right button) and PA1 (left button)
5. Performs joystick DAC comparator check for PA7 (see Joystick section below)
6. Calls `mc6821_set_input_a()` to set the row data for PIA to return

### Joystick Comparator (in `machine_read`)

The CoCo reads joystick positions through a DAC comparator loop. The machine layer implements this exactly matching XRoar's `joystick_update()`:

```
port = (PIA0 CRB bit 3) >> 3        // 0=right, 1=left joystick
axis = (PIA0 CRA bit 3) >> 3        // 0=X, 1=Y axis
dac  = (PIA1 DA & 0xFC) + 2         // 8-bit DAC value (range 2–254)
js   = hal_joystick_read_axis(port, axis) * 4 + 2  // Scale 0–63 → 2–254
PA7  = (js >= dac) ? 1 : 0          // Comparator result
```

Joystick ADC is refreshed every 16 scanlines (~16 times per frame) to balance responsiveness and overhead.

### Audio Output (in `machine_write`)

When the CPU writes to PIA1:
- **Port A write** (or CRA write): Extracts 6-bit DAC value from bits 2–7 of `data_a & ddr_a`, calls `hal_audio_write_dac()`.
- **Port B write** (or CRB write): Extracts single-bit audio from bit 1, calls `hal_audio_write_bit()`. Also updates VDG mode (AG from PB7, CSS fr
om PB3).

### VDG Mode Update (`update_vdg_mode`)

Called when PIA1 port B or SAM V0–V2 changes:
```
vdg_mode = 0
if PIA1 PB7 (via data_b & ddr_b): vdg_mode |= VDG_AG
if PIA1 PB3:                       vdg_mode |= VDG_CSS
vdg_mode |= SAM V0–V2 (bits 0–2)
mc6847_set_mode(&vdg, vdg_mode)
```

### IRQ Routing

```
PIA0 IRQA → mc6809_irq()    ← HS (not used by BASIC)
PIA0 IRQB → mc6809_irq()    ← FS / 60Hz vsync timer
PIA1 IRQA → mc6809_firq()   ← cartridge interrupt
PIA1 IRQB → mc6809_firq()   ← cartridge interrupt
```

Wired in `machine_init()` via static callback functions and re-wired after `machine_reset()` (since reset preserves callbacks but the callbacks must be re-assigned to the PIA struct).

### CoCo 3 IRQ Routing

On CoCo 3, PIA interrupts are ORed with GIME interrupt outputs:

```
IRQ  = PIA0 (IRQA | IRQB) OR GIME IRQ
FIRQ = PIA1 (IRQA | IRQB) OR GIME FIRQ
```

Unlike CoCo 2 (which uses PIA callbacks → `mc6809_irq()`/`mc6809_firq()`), CoCo 3 computes the combined interrupt state each scanline via direct assignment to `cpu.irq_pending` and `cpu.firq_pending`. This is necessary because GIME interrupt state changes between scanlines (HBORD, VBORD, TMR).

#### SYNC Wake-up Fix (April 2026)

**Bug:** The direct assignment `cpu.irq_pending = pia0_irq || gime.IRQ` bypassed `mc6809_irq()`, which normally clears `wait_for_interrupt`. When the 6809 executed a SYNC instruction (used by Super Extended BASIC's main loop at $E62D to wait for the 60Hz IRQ), the CPU would never wake up — `wait_for_interrupt` stayed true even though `irq_pending` was true.

**Fix:** After computing the combined interrupt state, explicitly clear `wait_for_interrupt` when any interrupt becomes active:

```cpp
bool new_irq  = pia0_irq || m->gime.IRQ;
bool new_firq = pia1_irq || m->gime.FIRQ;
if (new_irq || new_firq)
    m->cpu.wait_for_interrupt = false;
m->cpu.irq_pending  = new_irq;
m->cpu.firq_pending = new_firq;
```

This correctly wakes both SYNC (wake + continue or service) and CWAI (wake + vector to handler).

### Frame Execution

`machine_run_frame()` processes 262 NTSC scanlines per frame:

```
machine_run_frame(m):
  Precompute scanline_cycle_targets[262] for even distribution
  cycles_this_frame = 0, scanline = 0
  for 262 scanlines:
    machine_run_scanline(m)
  hal_video_present(ram, sam.vdg_base, vdg.mode)  // VRAM shadow compare + conditional push
  frame_count++
```

Each `machine_run_scanline()`:
1. Ticks FDC deferred INTRQ counter (`sv_disk_tick`)
2. Runs CPU for ~57 cycles (from precomputed target table, avoids per-scanline divide)
3. On scanline 0: VDG FS rising edge → PIA0 CB1 → IRQ possible; SAM counter reset
4. For active scanlines (0–191): SAM provides row address → VDG renders → HAL writes to VGA framebuffer
5. SAM data fetch advances counter by `bytes_per_row` (text=32, graphics=table lookup)
6. SAM hsync performs supplementary counter adjustment
7. On scanline 192: VDG FS falling edge → PIA0 CB1 → 60Hz IRQ fires

### Memory Allocation

All large buffers (64 KB RAM, ROM images) are allocated from PSRAM when available (`ps_malloc`), falling back to heap. This keeps internal SRAM fr
ee for stack and DMA buffers. The helper `machine_alloc(size, label)` logs the allocation source.

### Initialization Sequence

1. `machine_init()`: Allocate memory (RAM, 3 ROM buffers from PSRAM), init all chips, wire CPU `read`/`write` callbacks and PIA IRQ routing callba
cks
2. `machine_load_roms()`: Load Color BASIC ($A000), Extended BASIC ($8000), and optionally Disk BASIC ($C000) from SD card via `hal_storage_load_f
ile()`
3. `machine_reset()`: Clear RAM, reset all chips, set VDG VRAM pointer, reset SAM counter, CPU reads reset vector from ROM

---

## TCC1014 GIME — CoCo 3 Graphics/MMU/Interrupt Controller (`tcc1014.h`, `tcc1014.cpp`)

The GIME (TCC1014) replaces the SAM6883 + MC6847 on the CoCo 3. It provides MMU (512KB address space), native graphics modes (up to 640x225x16 colors), palette control, and a programmable interrupt controller. Ported from XRoar's `tcc1014/tcc1014.c`.

### Key Registers

| Address | Register | Description |
|---------|----------|-------------|
| $FF90 | INIT0 | COCO compat mode, MMUEN, MC3/2/1/0, IRQ/FIRQ global enable |
| $FF91 | INIT1 | Timer source (TINS), MMU task select (TR) |
| $FF92 | IRQEN | IRQ enable mask (TMR, HBORD, VBORD, EI2/1/0) |
| $FF93 | FIRQEN | FIRQ enable mask |
| $FF94-$FF95 | Timer | 12-bit countdown timer (MSB write arms) |
| $FF98 | VMODE | Graphics/text (BP), burst phase, monochrome, 50Hz, LPR |
| $FF99 | VRES | Lines per field (LPF), horizontal res (HRES), color res (CRES) |
| $FF9A | BRDR | Border color (6-bit) |
| $FF9D-$FF9E | Vertical offset | 19-bit VRAM start address (Y register) |
| $FF9F | Horizontal offset | HVEN + 7-bit horizontal scroll |
| $FFA0-$FFA7 | MMU Task 1 | 8 x 6-bit bank registers (8KB pages) |
| $FFA8-$FFAF | MMU Task 2 | 8 x 6-bit bank registers |
| $FFB0-$FFBF | Palette | 16 x 6-bit RGBRGB palette entries |
| $FFC0-$FFDF | SAM compat | Set/clear bit pairs (backward compat) |

### MMU Address Translation

The 64KB CPU address space is divided into 8 x 8KB slots. Each slot maps to one of 64 physical 8KB pages (512KB total) via the MMU bank registers:

```
CPU address → slot = addr >> 13 (0-7)
             bank = mmu_bank[TR | slot]     (when MMUEN=1)
                  = 0x38 | slot              (when MMUEN=0, identity map to top 64KB)
Physical addr = (bank << 13) | (addr & 0x1FFF)
```

- **TR** (task register): 0 = task 1 banks ($FFA0-$FFA7), 8 = task 2 ($FFA8-$FFAF)
- **active_banks[8]**: Pre-computed from current MMUEN/TR state for fast-path lookups

### ROM/RAM Overlay

When `TY=0` (normal mode) and bank >= $3C, address decode routes to ROM or CTS:
- `MC1=0`: banks $3C-$3D → S=0 (internal ROM), banks $3E-$3F → S=1 (CTS/cartridge)
- `MC1=1`: all → S = MC0 ? CTS : ROM

When `TY=1` (all-RAM mode), all banks read from physical RAM. The CoCo 3 boot sequence copies ROM to RAM pages $3C-$3F then sets TY=1.

### GIME Interrupts

The GIME has its own interrupt controller, gated by INIT0 bits 5 (IRQ enable) and 4 (FIRQ enable):

```
set_interrupt(flag):
    irq_state  |= (flag & registers[2])    // AND with IRQ enable mask
    firq_state |= (flag & registers[3])    // AND with FIRQ enable mask
    IRQ  = (INIT0 & 0x20) ? irq_state : 0  // Global IRQ gate
    FIRQ = (INIT0 & 0x10) ? firq_state : 0 // Global FIRQ gate
```

Reading $FF92/$FF93 returns and clears the interrupt status (read-acknowledge).

### GIME Video Rendering

Supports two modes:
1. **VDG compat** (COCO=1): Emulates MC6847 via PIA1B snooping — SG, CG, RG modes
2. **Native GIME** (COCO=0): Text (with attributes) and graphics (1/2/4 bpp)

Rendering always produces up to 640 pixels per scanline. The `resolution` field (derived from HRES) controls pixel expansion:
- Resolution 0: 4x expansion (16 pixels per nibble)
- Resolution 1: 2x expansion (8 pixels per nibble)
- Resolution 2: 1:1 (4 pixels per nibble)
- Resolution 3: 1:2 compression (2 pixels per nibble)

### CoCo 3 Machine Structure

```c
typedef struct Machine {
    MC6809   cpu;
    MC6821   pia0, pia1;
    TCC1014  gime;                  // Replaces SAM + VDG
    SV_DiskController fdc;
    uint8_t* ram_physical;          // 512KB in PSRAM
    uint8_t* rom_coco3;             // 32KB Super Extended Color BASIC
    uint8_t* rom_disk;              // 8KB Disk BASIC
    // ...
} Machine;
```

### CoCo 3 Memory Map

```
$0000-$FEFF  MMU-mapped via GIME (8 x 8KB pages from 512KB physical)
$FE00-$FEFF  Constant RAM (when MC3=1, always maps to bank $3F top)
$FF00-$FF3F  PIA0/PIA1 (same as CoCo 2, GIME snoops PIA1B for VDG compat)
$FF40-$FF5F  Disk controller / SCS
$FF90-$FF9F  GIME registers (write-only except $FF92/$FF93 read-acknowledge)
$FFA0-$FFAF  MMU bank registers (2 tasks x 8 banks)
$FFB0-$FFBF  Palette registers (16 x 6-bit)
$FFC0-$FFDF  SAM compat (write-only set/clear bit pairs)
$FFE0-$FFFF  Vectors (always from internal ROM, regardless of MMU/TY)
```

### CoCo 3 Fast-Path Memory Access

For performance on ESP32, `machine_read()`/`machine_write()` use a fast path for the common case (addr < $FE00 mapping to RAM):

```
Read fast path:
  bank = active_banks[addr >> 13]
  if TY=1 or bank < 0x3C:          // RAM access
      return ram_physical[(bank << 13) | (addr & 0x1FFF)]
  // else: fall through to full GIME decode (ROM/CTS)

Write fast path:
  // Writes ALWAYS go to RAM (ROM overlay only affects reads)
  bank = active_banks[addr >> 13]
  ram_physical[(bank << 13) | (addr & 0x1FFF)] = val
```

### CoCo 3 Scanline Timing

```
machine_run_scanline() — CoCo 3:
  1. sv_disk_tick()                    // FDC deferred INTRQ
  2. mc6809_run(cycles_to_run)         // CPU execution
  3. tcc1014_tick_scanline()           // GIME timer decrement
  4. PIA0 CA1 HS transitions           // Horizontal sync → PIA
  5. Combined IRQ routing:             // PIA + GIME → CPU
       new_irq  = pia0_irq || gime.IRQ
       new_firq = pia1_irq || gime.FIRQ
       if (new_irq || new_firq) wait_for_interrupt = false  // SYNC wake-up
  6. Vertical state machine:
       Scanline 0:   FS falling → PIA0 CB1, latch vertical params
       Scanline 4:   FS rising → PIA0 CB1
       Scanline 7:   Reset VRAM address (B = Y)
       7+lTB:        Active area begins
       Active lines: Render scanline, advance row/VRAM address, HBORD interrupt
       End of active: VBORD interrupt
```

---

## HAL Integration — How Core Connects to Hardware

The Hardware Abstraction Layer (`src/hal/`) bridges the emulation core to the TTGO VGA32 board. The integration points are declared in `hal.h` and implemented in the corresponding `.cpp` files.

### Video HAL (`hal_video.cpp`)

**Integration point**: Called from `machine_run_scanline()` for each active scanline, and `machine_run_frame()` at end of frame.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_video_render_scanline(line, pixels, width)` | `machine_run_scanline()` (CoCo 2) | Converts VDG `line_buffer[256]` palette indices to the active backend's pixel format and writes one scanline. |
| `hal_video_render_scanline_gime(...)` | `machine_run_scanline()` (CoCo 3) | Consumes pre-converted RGB565 pixels from the GIME and writes one scanline. |
| `hal_video_present*()` | `machine_run_frame()` | Frame-end no-op — FabGL DMA scans the framebuffer continuously. Updates the FPS counter and clears the dirty flag. |
| `hal_video_get_canvas()` | `supervisor.cpp` | Returns the `OSDCanvas` instance backed by FabGL Canvas for OSD rendering. |

**VGA32 backend**: FabGL `VGAController` @ 640×200 @ 70 Hz (64-color RGB222 direct). Scanline writes go through `s_vga.getScanline(y)` with `createRawPixel(RGB222(...))` per pixel. The 64-entry GIME palette is pre-converted to raw VGA bytes once at init. No per-frame "push" — FabGL's DMA scans the framebuffer continuously. See `docs/video.md`.

**Historical S3+TFT backend (removed)**: used a 320×240 `TFT_eSprite` framebuffer in PSRAM with SPI push, VRAM-shadow compare, and CRC32 dirty-skip. That code has been removed from the codebase.

### Audio HAL (`hal_audio.cpp`)

**Integration point**: `hal_audio_write_dac()` is called from `machine_write()` PIA1 PA handler **gated by the sound MUX** (PIA1 CRB bit 3 + PIA0 CA2/CB2 source select). `hal_audio_write_bit()` from PIA1 PB. Frame timing: `hal_audio_capture_scanline()` after each emulated scanline, `hal_audio_commit_frame()` at frame end.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_audio_write_dac(dac6)` | `machine_write()` PIA1 PA (MUX-gated) | 6-bit DAC value → 8-bit scaled level into `audio_current_level`. |
| `hal_audio_write_bit(value)` | `machine_write()` PIA1 PB | Single-bit audio → 0 or 255 in `audio_current_level`. |
| `hal_audio_capture_scanline()` | `machine_run_scanline()` | Snapshot current level into the active scanline buffer. |
| `hal_audio_commit_frame()` | `machine_run_frame()` | Hand the captured buffer to the ISR. |

**VGA32 backend**: ESP32 internal DAC1 on GPIO25 driven by a 22 050 Hz timer ISR calling `dac_output_voltage()`. 262-sample pitch-corrected double-buffered scanline replay.

**Historical S3+TFT backend (removed)**: LEDC PWM at 78.125 kHz on GPIO17 (ESP32-S3 has no DAC), same ISR model. Removed from the codebase.

See `docs/audio-hal.md` for the MUX gating story and pitch-correction math.

### Keyboard HAL (`hal_keyboard.cpp`)

**Integration point**: Called from `machine_read()` when CPU reads PIA0 PA.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_keyboard_scan(column)` | `machine_read()` PIA0 PA | Returns row data for given column (active-low) |
| `hal_keyboard_tick()` | `hal_process_input()` each frame | Decrements deferred release counters and drains key events |

**VGA32 backend**: FabGL PS/2 driver on GPIO33 (CLK) / GPIO32 (DATA), brought up with `PS2Preset::KeyboardPort0_MousePort1` (the mouse half is used by `hal_joystick.cpp`). VirtualKey events drained from FabGL's queue, dispatched through `process_vk()` → supervisor/hotkey gate → `VK_MAP[]` lookup → matrix write. Keyboard layout selectable (Spanish default). Shifted-symbol VKs (`VK_HASH`, `VK_DOLLAR`, …) have explicit map entries with `needs_shift = true`.

**Historical S3+TFT backend (removed)**: ESP-IDF USB Host + HID class driver on Core 0, events queued and drained on Core 1. Removed from the codebase.

**Matrix representation**: `key_matrix[col]` bit `row` = 0 if pressed (active LOW). When the CPU scans column N by driving PIA0 PB low, `hal_keyboard_scan(N)` returns the corresponding row byte.

**Deferred release**: Keys stay held for `MIN_HOLD_FRAMES = 4` frames minimum so the CoCo ROM's KEYIN routine can detect them.

**Hotkey layer** (both backends):
- F3 → supervisor toggle (always)
- F4 → machine reset + disk flush (emulation mode only)
- F5 → FPS overlay toggle (emulation mode only)
- F6 → quick mount last disk (emulation mode only)
- F1/F2 are forwarded to the CoCo 3 matrix (PA6,PB5/PB6) — never consumed as host shortcuts.

### Joystick HAL (`hal_joystick.cpp`)

**Integration point**: Called from `machine_read()` during PIA0 PA reads.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_joystick_read_axis(port, axis)` | `machine_read()` | Returns 6-bit axis value (0–63) |
| `hal_joystick_read_button(port)` | `machine_read()` | Non-zero = pressed (forces PA0 / PA1 low) |
| `hal_joystick_compare(port, axis, dac)` | `machine_read()` | DAC comparator output (PA7) |
| `hal_joystick_update()` | `hal_process_input()` + `machine_read()` every 16 scanlines | Refresh input state |

**VGA32 backend**: Joystick 1 (port 0) is driven by the PS/2 mouse on the on-board mouse header. `hal_joystick_update()` drains FabGL's MouseDelta queue and integrates each `deltaX`/`deltaY` into an internal `s_pos_x`/`s_pos_y` accumulator (range 0..63, clamped). Left mouse button → joystick fire. Joystick 2 (port 1) is a neutral stub. Sensitivity via `JOYSTICK_MOUSE_SCALE` in `config.h`. See `docs/joystick-hal.md`.

**Historical S3+TFT backend (removed)**: Two analog joysticks on ESP32-S3 ADC pins via the `CoCoJoystick` library. Removed from the codebase.

**Comparator emulation**: The CoCo reads joystick positions via successive approximation in BASIC ROM. `machine_read()` computes `(axis_value * 4 + 2) >= dac_value`, matching XRoar's `joystick_update()`.

### Storage HAL (`hal_storage.cpp`)

**Integration point**: Called from `machine_load_roms()` at boot, and from `sv_disk_mount()` for disk images. Board-independent code — only the pin macros differ.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_storage_init()` | `hal_init()` | SD card init on dedicated HSPI bus |
| `hal_storage_load_file(path, buf, size)` | `machine_load_roms()`, `sv_disk_mount()` | Reads file from SD into buffer |
| `hal_storage_file_exists(path)` | File browser, ROM loader | Checks if file exists on SD |

**SD bus**: Dedicated HSPI (SPI3) peripheral on pins CS=13, MOSI=12, MISO=2, SCK=14 (GPIO2 is a strapping pin — must float/high at boot). There is no SPI display bus on the VGA32, so HSPI is dedicated entirely to the SD card.

---

## Module Interaction Diagram

```
                     ┌──────────────┐
                     │  machine.cpp │ (address decoder + frame loop)
                     └──────┬───────┘
            ┌───────────────┼───────────────┐
            │               │               │
     ┌──────▼──────┐ ┌─────▼─────┐  ┌──────▼──────┐
     │   MC6809    │ │  MC6821   │  │   MC6847    │
     │   (CPU)     │ │ PIA0+PIA1 │  │   (VDG)     │
     └──────┬──────┘ └─────┬─────┘  └──────┬──────┘
            │               │               │
  read/write callbacks  IRQ/FIRQ        vram pointer
            │           callbacks           │
            │               │               │
            └───────┬───────┘        ┌──────┘
                    │                │
             ┌──────▼──────┐  ┌─────▼──────┐
             │  SAM6883    │  │  HAL layer  │
             │ (addr mux)  │  │ (video, kbd,│
             └─────────────┘  │  audio, SD) │
                              └────────────┘
```

### Signal Flow Summary

| Signal | From | To | Mechanism |
|--------|------|----|-----------|
| Memory read/write | CPU | machine.cpp | Function pointers (`cpu->read`, `cpu->write`) |
| IRQ | PIA0 IRQA/IRQB | CPU | Callback → `mc6809_irq()` |
| FIRQ | PIA1 IRQA/IRQB | CPU | Callback → `mc6809_firq()` |
| NMI | FDC INTRQ | CPU | Callback → `mc6809_nmi()` (edge-triggered) |
| HALT | FDC DRQ | CPU | `cpu->halted` flag via callback |
| 60Hz vsync | VDG FS | PIA0 CB1 | `mc6821_cb1_transition()` in scanline loop |
| VDG mode | PIA1 PB + SAM V0–V2 | VDG | `mc6847_set_mode()` via `update_vdg_mode()` |
| Display address | SAM counter | VDG | `vdg->row_address` set each scanline |
| Audio DAC | PIA1 PA | HAL audio | `hal_audio_write_dac()` on PIA1 write |
| Audio bit | PIA1 PB1 | HAL audio | `hal_audio_write_bit()` on PIA1 write |
| Keyboard | HAL keyboard | PIA0 PA | `hal_keyboard_scan()` on PIA0 data A read |
| Joystick | HAL joystick | PIA0 PA7 | `hal_joystick_read_axis()` → comparator in machine_read |
| Video out | VDG line_buffer | HAL video | `hal_video_render_scanline()` per active scanline |
| Frame push | machine | HAL video | `hal_video_present()` after 262 scanlines |
| Disk I/O | FDC | HAL storage | `sv_disk_mount()` loads .DSK into PSRAM cache |
