# MC6809 CPU Enhancements for ESP32_CoCo2_XRoar_Port

Comparative analysis of `ESP32_CoCo2_XRoar_Port/src/core/mc6809.cpp` against XRoar upstream
(`xroar-src/src/mc6809.c`) and MAME's `m6809` implementation. Categorized by
impact: correctness fixes first, then performance.

Baseline performance: ~23.5 fps (0.4x realtime). CPU execution is the critical path.
Current performance after 2.1: **~25–27 fps** (idle: 27, active program: 25–25.5).

---

## 1. Correctness Issues

### 1.1 NMI Edge Detection — IMPLEMENTED (2026-03-21)

**Status:** DONE. Edge detection implemented matching XRoar pattern.
The 2-cycle assertion delay (item 1.2) is deferred as a separate enhancement.

**Changes made:**
1. Added `nmi_line` (bool) field to MC6809 struct — tracks the NMI pin level.
2. `mc6809_nmi(cpu, active)` — API changed to match `mc6809_firq`/`mc6809_irq`.
   Only latches `nmi_pending` on inactive→active transition (`!nmi_line && active`).
3. After servicing NMI in `check_interrupts()`, `nmi_line` is cleared — a new
   edge (low→high transition) is required for the next NMI.
4. FDC `signal_nmi(fdc, bool active)` updated; `set_intrq()` now deasserts the
   NMI line (`signal_nmi(fdc, false)`) when INTRQ clears, enabling proper edge
   detection for subsequent disk operations.

**Files changed:** `mc6809.h`, `mc6809.cpp`, `sv_disk.h`, `sv_disk.cpp`,
`supervisor.cpp`.

### 1.2 Interrupt Assertion Latency Missing (Medium Impact)

**Problem:** All three interrupt lines (NMI/FIRQ/IRQ) are serviced immediately in
the ESP32 code. XRoar adds a **2-cycle pipeline delay** for each
(mc6809.h:108–124):

```c
cpu->nmi_cycle  = cpu->cycle + 2;
cpu->firq_cycle = cpu->cycle + 2;
cpu->irq_cycle  = cpu->cycle + 2;
```

This models the real MC6809's internal synchronization — an interrupt asserted
during an instruction must pass through an internal latch before being
recognized.

**Impact:** Programs that rely on precise interrupt timing (e.g., raster effects,
cycle-counted audio routines) may behave incorrectly. For CoCo BASIC this is
unlikely to matter, but it could affect demos and games.

**Fix:**
Add `firq_cycle` and `irq_cycle` fields to the MC6809 struct. When
`mc6809_firq()`/`mc6809_irq()` transitions from inactive to active, record
`cycles + 2`. In `check_interrupts()`, only recognize the interrupt when
`cycles >= xxx_cycle`.

### 1.3 SYNC Instruction Wakeup Semantics Incomplete (Medium Impact)

**Problem:** The ESP32 SYNC burns the entire cycle budget (mc6809.cpp:2597–2601):

```c
if (cpu->wait_for_interrupt) {
    cpu->cycles = budget;
    break;
}
```

XRoar's SYNC (mc6809.c:289–300) wakes on **any** interrupt edge (even masked
ones), adds 2 NVMA cycles, then proceeds to `label_b` for normal interrupt
dispatch:

```c
case mc6809_state_sync:
    if (nmi_active || firq_active || irq_active) {
        NVMA_CYCLE;
        NVMA_CYCLE;
        instruction_posthook(cpu);
        cpu->state = mc6809_state_label_b;
        continue;
    }
    NVMA_CYCLE;
```

Key difference: a **masked** interrupt (F=1 for FIRQ, I=1 for IRQ) still wakes
SYNC, but the CPU resumes at the *next instruction* rather than vectoring.

**Impact:** SYNC is used in CoCo software for cycle-precise synchronization.
Wrong wakeup semantics could cause hangs (if masked interrupts don't wake) or
incorrect timing (missing the 2 NVMA cycles).

**Fix:**
- In the SYNC check, test for any active interrupt line regardless of mask bits.
- On wakeup, add 2 dead cycles before entering the interrupt dispatch check.
- If the waking interrupt is masked, resume at PC (next instruction) instead of
  vectoring.

### 1.4 TFR/EXG 8↔16-bit Size Mismatch Handling (Low Impact)

**Problem:** The ESP32 code zero-extends 8-bit registers to 16 bits on read, and
truncates on write (mc6809.cpp:324–353). For undefined register codes, it
returns `0xFF`.

Real MC6809 behavior (per Motorola documentation and MAME): when transferring
between mismatched sizes (e.g., TFR A,X), the 8-bit value goes into the **low
byte** and the high byte becomes `0xFF`. The MAME implementation explicitly
handles this with sign-extension (the 8-bit value is OR'd with `0xFF00`).

**Impact:** Only affects programs using undocumented TFR/EXG combinations
(8→16 or 16→8 mixed transfers). Unlikely in normal CoCo software.

**Fix (optional):**
In `tfr_read_reg()`, when reading an 8-bit register, OR with `0xFF00` instead
of zero-extending:

```c
case 0x8: return 0xFF00 | GET_A();
case 0x9: return 0xFF00 | GET_B();
case 0xA: return 0xFF00 | cpu->cc;
case 0xB: return 0xFF00 | cpu->dp;
```

---

## 2. Performance Optimizations

### 2.1 Unified Flag-Setting with Bitmask — IMPLEMENTED (2026-03-21)

**Status:** DONE. Measured improvement: **~23.5 → 25–27 fps** (+15% idle, +6–8% active).

**Changes made:**
1. `CC_PUT` macro rewritten as branchless ternary (`(c) ? (f) : 0`) — affects
   all ~60 call sites in opcode handlers (shifts, rotates, INC/DEC, etc.).
2. Six ALU helpers (`update_nz8`, `update_nz16`, `op_add8`, `op_sub8`,
   `op_add16`, `op_sub16`) rewritten to compute flags into a local `uint8_t f`
   and do a single `cpu->cc = (cc & ~mask) | f` write.

The branchless ternary compiles to Xtensa MOVNEZ (conditional move) instructions,
and the compute-and-mask pattern reduces multiple read-modify-write cycles on
`cpu->cc` to a single write per ALU operation.

### 2.2 Branch Condition Lookup Table (Medium Impact)

**Problem:** `eval_branch()` uses a 16-case switch (mc6809.cpp:415–436). Branches
are among the most frequent instructions in 6809 code (loops, conditionals).

**Fix:** Replace with a precomputed lookup table that maps `(cc, condition)` to
a boolean. Since CC is 8 bits and condition is 4 bits, a full table would be
256×16 = 4KB. Alternatively, use a 16-entry function pointer table or a
16-entry packed bitmask table.

Compact approach — 16 functions reduced to a 16-byte flags-of-interest table
plus branchless evaluation:

```c
// For conditions that test a single flag:
static const uint8_t branch_flag[16] = {
    0,    0,    0,    0,                    // BRA/BRN/BHI/BLS: special
    MC6809_FLAG_C, MC6809_FLAG_C,           // BCC, BCS
    MC6809_FLAG_Z, MC6809_FLAG_Z,           // BNE, BEQ
    MC6809_FLAG_V, MC6809_FLAG_V,           // BVC, BVS
    MC6809_FLAG_N, MC6809_FLAG_N,           // BPL, BMI
    0, 0, 0, 0                              // BGE/BLT/BGT/BLE: compound
};
```

For the compound conditions (BHI/BLS/BGE/BLT/BGT/BLE), a small switch or
dedicated inline is still needed. But the 10 simple conditions (BRA through BMI)
can be evaluated with a single flag test and XOR for the sense bit.

### 2.3 Indexed Addressing Register Pointer (Low-Medium Impact)

**Problem:** `idx_reg()` uses a switch with 4 cases (mc6809.cpp:187–195). This is
called for every indexed addressing mode instruction.

MAME uses a 4-entry pointer array indexed by `(postbyte >> 5) & 3`:

```c
uint16_t* const idx_regs[4] = { &cpu->x, &cpu->y, &cpu->u, &cpu->s };
// Then: uint16_t* reg = idx_regs[(postbyte >> 5) & 3];
```

**Fix:** Replace the switch with a 4-element array lookup. This eliminates
branch prediction pressure and compiles to a single indexed load.

```c
// In MC6809 struct or as static array:
static inline uint16_t* idx_reg(MC6809* cpu, uint8_t postbyte) {
    uint16_t* regs[] = { &cpu->x, &cpu->y, &cpu->u, &cpu->s };
    return regs[(postbyte >> 5) & 0x03];
}
```

### 2.4 Force-Inline Critical Helpers (Low-Medium Impact)

**Problem:** The `static inline` hint is advisory — the compiler may not inline
on `-Os` (size-optimized, common for ESP32). Functions like `update_nz8`,
`op_add8`, `op_sub8`, `fetch8`, and `mem_read16` are called from every opcode.

**Fix:** Use `__attribute__((always_inline))` on the hottest helpers:

```c
static inline __attribute__((always_inline))
uint8_t op_add8(MC6809* cpu, uint8_t a, uint8_t b, uint8_t carry) { ... }
```

**Caveat:** The existing memory note (`feedback_iram_attr.md`) records that
`IRAM_ATTR` on CPU functions **hurt** performance (tested 2026-03-20). This is
different — `IRAM_ATTR` forces placement in instruction RAM (limited, can cause
cache eviction); `always_inline` eliminates function call overhead without
affecting placement. These are orthogonal and `always_inline` should be tested
independently.

### 2.5 Struct Field Ordering for Cache Locality (Low Impact)

**Problem:** The MC6809 struct (mc6809.h:33–59) places `cycles` (accessed every
instruction) after the boolean flags at offset ~26+. On ESP32's 32-byte cache
lines, the most-accessed fields should be in the first cache line.

**Suggested reordering** (most-accessed fields first):

```c
typedef struct MC6809 {
    // Hot: accessed every instruction cycle
    uint16_t pc;
    uint16_t d;
    uint8_t  cc;
    uint8_t  dp;
    unsigned long cycles;

    // Warm: accessed by indexed/stack instructions
    uint16_t x;
    uint16_t y;
    uint16_t u;
    uint16_t s;

    // Cold: accessed only on interrupts
    bool     halted;
    bool     wait_for_interrupt;
    bool     cwai_state;
    bool     nmi_armed;
    bool     nmi_pending;
    bool     firq_pending;
    bool     irq_pending;

    // Function pointers (cold, read once at startup effectively)
    uint8_t  (*read)(uint16_t addr);
    void     (*write)(uint16_t addr, uint8_t val);
} MC6809;
```

This puts `pc`, `d`, `cc`, `dp`, and `cycles` in the first 16 bytes (one cache
line on Xtensa: LX6 on ESP32-WROVER, LX7 on ESP32-S3).

### 2.6 Cycle Count Table Lookup (Low Impact)

**Problem:** Cycle counts are added inline in each opcode case
(`cpu->cycles += N`). This is fine for correctness but adds code size to the
giant switch statement.

The cycle count table already exists in `mc6809_opcodes.h` (PROGMEM) but is only
used for debug/trace.

**Fix:** For page-1 opcodes, read the base cycle count from the table at the top
of `execute_one()` and add it once. Opcode cases only need to add *extra* cycles
for addressing mode penalties (indexed mode extra cycles, etc.):

```c
uint8_t opcode = fetch8(cpu);
cpu->cycles += pgm_read_byte(&mc6809_cycles_page1[opcode]);
switch (opcode) {
    // Cases no longer need: cpu->cycles += base;
    // Only add extra cycles for variable-timing operations
}
```

**Caveat:** Some opcodes have variable cycle counts (PSH/PUL depend on register
list, indexed modes add variable cycles). These would still need per-case
adjustments. The table would provide the *minimum* count, with cases adding the
delta.

---

## 3. Summary and Priority

| # | Issue | Type | Impact | Effort |
|---|-------|------|--------|--------|
| 1.1 | NMI edge detection | Correctness | **DONE** | edge-triggered |
| 1.2 | Interrupt 2-cycle latency | Correctness | Medium | Low |
| 1.3 | SYNC wakeup semantics | Correctness | Medium | Low |
| 1.4 | TFR/EXG size mismatch | Correctness | Low | Trivial |
| 2.1 | Unified flag bitmask | Performance | **DONE** | ~+15% fps |
| 2.2 | Branch lookup table | Performance | Medium | Low |
| 2.3 | Indexed reg array | Performance | Low-Med | Trivial |
| 2.4 | Force-inline helpers | Performance | Low-Med | Trivial |
| 2.5 | Struct field reorder | Performance | Low | Trivial |
| 2.6 | Cycle count table | Performance | Low | Medium |

**Recommended order:**
1. ~~**2.1** (unified flags)~~ — **DONE** (+15% fps)
2. ~~**1.1** (NMI edge)~~ — **DONE** (edge-triggered, 2-cycle latency deferred to 1.2)
3. **1.3** (SYNC semantics) — small change, medium correctness value
4. **2.3 + 2.4** (indexed reg + force-inline) — trivial changes, stack them
5. **1.2** (interrupt latency) — easy addition alongside 1.1
6. **2.2** (branch table) — measure before/after
7. **2.5** (struct reorder) — requires updating all struct initializers
8. **1.4 + 2.6** — low priority, do if convenient
