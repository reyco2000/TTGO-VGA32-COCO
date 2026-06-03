# Graph Report - TTGO-VGA32-COCO/docs  (2026-05-30)

## Corpus Check
- 18 files · ~51,392 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 653 nodes · 635 edges · 35 communities
- Extraction: 100% EXTRACTED · 0% INFERRED · 0% AMBIGUOUS
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 1|Community 1]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 6|Community 6]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 10|Community 10]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 18|Community 18]]
- [[_COMMUNITY_Community 19|Community 19]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 21|Community 21]]
- [[_COMMUNITY_Community 22|Community 22]]
- [[_COMMUNITY_Community 23|Community 23]]
- [[_COMMUNITY_Community 24|Community 24]]
- [[_COMMUNITY_Community 25|Community 25]]
- [[_COMMUNITY_Community 26|Community 26]]
- [[_COMMUNITY_Community 27|Community 27]]
- [[_COMMUNITY_Community 28|Community 28]]
- [[_COMMUNITY_Community 29|Community 29]]
- [[_COMMUNITY_Community 30|Community 30]]
- [[_COMMUNITY_Community 31|Community 31]]
- [[_COMMUNITY_Community 32|Community 32]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 34|Community 34]]

## God Nodes (most connected - your core abstractions)
1. `2. Optimization Catalog` - 19 edges
2. `TTGO-VGA32-COCO Disk HAL — Implementation Details` - 15 edges
3. `Machine — System Integration (`machine.h`, `machine.cpp`)` - 14 edges
4. `Keyboard HAL — TTGO VGA32` - 13 edges
5. `Video Subsystem — TTGO-VGA32-COCO (TTGO VGA32)` - 13 edges
6. `Supervisor Module — CoCo_ESP32 OSD` - 12 edges
7. `CoCo3 Performance Optimization Plan` - 12 edges
8. `CoCo 3 Extensions` - 11 edges
9. `ANALYSIS.md — XRoar 0.32.0 ESP32-S3 Port Analysis` - 11 edges
10. `Performance Analysis — ESP32 CoCo Emulator` - 10 edges

## Surprising Connections (you probably didn't know these)
- None detected - all connections are within the same source files.

## Communities (35 total, 0 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.05
Nodes (40): Baseline Analysis, CoCo3 Performance Optimization Plan, CoCo3-Specific Bottlenecks (vs CoCo2), code:cpp (uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);), code:cpp (if (Z >= RAM_DRAM_BASE && Z < RAM_DRAM_BASE + RAM_DRAM_SIZE)), code:cpp (// hal_video.cpp), code:cpp (void tcc1014_render_scanline(TCC1014* gime, unsigned scanlin), code:cpp (// Core 0 task) (+32 more)

### Community 1 - "Community 1"
Cohesion: 0.05
Nodes (39): 60Hz Timer Operation (detailed flow), Addressing Modes, Audio HAL (`hal_audio.cpp`), CoCo-Specific Wiring, code:block1 (mc6809_run(cpu, budget):), code:block2 (Bit 7: IRQ1 flag (read-only) — set by CA1/CB1 transition mat), code:block24 (┌──────────────┐), code:block3 (PIA0 IRQA, IRQB → CPU IRQ    (60Hz vsync timer + keyboard)) (+31 more)

### Community 2 - "Community 2"
Cohesion: 0.05
Nodes (38): 1.1 Core Source Files (`src/`), 1.2 Platform-Specific Subdirectories (all SKIP), 1.3 Portable Library (`portalib/`) — all PORT, 1.4 Summary Counts, 1. File Inventory, 2.1 MC6809 CPU Core, 2.2 MC6847 VDG (Video Display Generator), 2.3 MC6821 PIA (Peripheral Interface Adapter) (+30 more)

### Community 3 - "Community 3"
Cohesion: 0.06
Nodes (34): Build Notes, CoCo 3 GIME (TCC1014) — Porting Guide & Reference, code:block1 (MMU Bank Registers: $FFA0-$FFAF), code:bash (# Requires esp32:esp32@2.0.x (FabGL 1.0.9 is incompatible wi), code:bash (arduino-cli compile \), code:cpp (// Lines per active area by LPF — tcc1014.c:412), code:cpp (uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1)) * 85;  // bits), code:cpp (cpu.irq_pending  = pia0.irq_a || pia0.irq_b || gime.IRQ;) (+26 more)

### Community 4 - "Community 4"
Cohesion: 0.06
Nodes (30): Adding a new CoCo key mapping, Adding a new hotkey, Architecture, CoCo Keyboard Matrix, code:block1 (PS/2 keyboard ──GPIO33 (CLK) ──┐), code:c (s_ps2.begin(fabgl::PS2Preset::KeyboardPort0_MousePort1,), code:block3 (PB0    PB1    PB2    PB3    PB4    PB5    PB6    PB7), code:cpp (struct VkMap {) (+22 more)

### Community 5 - "Community 5"
Cohesion: 0.07
Nodes (30): 10. Data Flow Diagrams, 11. Source File Reference, 3. Hardware Abstraction Layer (`src/hal/`), 4. Supervisor / OSD (`src/supervisor/`), 5. Configuration (`config.h`), 6. Pin Assignments (fixed by the TTGO VGA32 v1.4 board), 7. Build System, 8. ROM Requirements (+22 more)

### Community 6 - "Community 6"
Cohesion: 0.07
Nodes (27): BASIC ROM ADC Routine (GETJOY), Button Mapping, code:block1 (Joystick pot (0-5V)          PIA1 PA bits 2-7), code:block2 (GETJOY:), code:cpp (int joy_port  = (pia0.ctrl_b & 0x08) >> 3;     // CB2 = port), code:c (static int16_t s_pos_x    = 32;   // 6-bit position, 0..63), code:c (void hal_joystick_update(void) {), code:c (uint8_t hal_joystick_read_axis(int port, int axis) {) (+19 more)

### Community 7 - "Community 7"
Cohesion: 0.07
Nodes (26): Audio HAL — TTGO VGA32, Audio Paths, Bandwidth, code:block1 (PIA1 Port A bits 2-7           PIA1 Port B bit 1), code:block2 (machine_run_frame():), code:block3 (machine_write() PIA1 handler:), ESP32-S3 Variant (gated, historical), Hardware Being Emulated (unchanged across boards) (+18 more)

### Community 8 - "Community 8"
Cohesion: 0.08
Nodes (25): Adding a New Menu Screen (Checklist), code:block1 (SV_INACTIVE ──F3──> SV_MAIN_MENU), code:block2 (if (supervisor_update_and_render()) {), code:block3 (if (supervisor_is_active()) {), code:block4 (========================================), code:block5 (SV_BORDER_X=32  SV_BORDER_Y=24  (top-left of OSD area)), code:cpp (m->fdc.nmi_callback = [](void* ctx) {), Data Flow (+17 more)

### Community 9 - "Community 9"
Cohesion: 0.08
Nodes (24): 1.1 NMI Edge Detection — IMPLEMENTED (2026-03-21), 1.2 Interrupt Assertion Latency Missing (Medium Impact), 1.3 SYNC Instruction Wakeup Semantics Incomplete (Medium Impact), 1.4 TFR/EXG 8↔16-bit Size Mismatch Handling (Low Impact), 1. Correctness Issues, 2.1 Unified Flag-Setting with Bitmask — IMPLEMENTED (2026-03-21), 2.2 Branch Condition Lookup Table (Medium Impact), 2.3 Indexed Addressing Register Pointer (Low-Medium Impact) (+16 more)

### Community 10 - "Community 10"
Cohesion: 0.08
Nodes (24): Adjacent-Pixel Blend (640 → 320 downscale), code:block1 (GIME / VDG          Machine Loop            HAL Video       ), code:c (// Unswap, then top-2-bits of each channel.), code:c (static uint8_t cache[16];), code:c (for (int i = 0; i < 64; i++) {), code:block5 (Scanline 0:    FS falling edge, latch lAA/lTB for this frame), Display Layout, Files (+16 more)

### Community 11 - "Community 11"
Cohesion: 0.09
Nodes (23): Audio Output (in `machine_write`), CoCo 3 IRQ Routing, code:c (typedef struct Machine {), code:block11 ($0000–$7FFF  RAM (64 KB, lower 32 KB directly visible)), code:block12 (port = (PIA0 CRB bit 3) >> 3        // 0=right, 1=left joyst), code:block13 (vdg_mode = 0), code:block14 (PIA0 IRQA → mc6809_irq()    ← HS (not used by BASIC)), code:block15 (IRQ  = PIA0 (IRQA | IRQB) OR GIME IRQ) (+15 more)

### Community 12 - "Community 12"
Cohesion: 0.09
Nodes (22): Architecture Summary, code:block1 (USB HID keyboard (Core 0)), code:block2 (loop() {), code:cpp (if (ram && force_push_count == 0 &&), Confirmed: FPS disappears due to VRAM shadow compare, F-Key Processing Path, Fix 1: Decouple FPS overlay from VRAM push, Fix 2: Add `hal_video_force_repaint()` (+14 more)

### Community 13 - "Community 13"
Cohesion: 0.09
Nodes (22): 2. Optimization Catalog, Branchless CC Flag Helpers — DONE, code:cpp (m->ram_fast = (uint8_t*)malloc(2048);               // DRAM:), code:cpp (if (frame_time_us > 20000) frame_skip = 2;), code:cpp (static inline uint8_t mem_read_fast(uint16_t addr) {), GIME Adjacent-Pixel Blend — OPTIONAL QUALITY FEATURE (default OFF), OPT-10: Adaptive Frame Skip, OPT-11: Compiler -O2 for Hot Files — DONE (+14 more)

### Community 14 - "Community 14"
Cohesion: 0.10
Nodes (19): code:c (#define CRC_DRAGON32    0xE3879310  // Dragon 32 Extended BA), code:c (#if MACHINE_TYPE == 0  // Dragon 32), code:c (#if IS_DRAGON), code:c (#if IS_DRAGON), Context, Files NOT modified (no changes needed), Files to Modify, Implementation Steps (+11 more)

### Community 15 - "Community 15"
Cohesion: 0.11
Nodes (18): Bug 1: Vector Space Z Not Computed, Bug 2: GIME Palette Bit Layout Wrong, Bug 3: CTS (Cartridge) Returns $FF, Bug 4: PIA IRQ Routing Checks Flag Without Enable Bit, Bug 5: ROM-Area Writes Silently Dropped, Bug 6: VRAM Fetch Xoff Increment on Cached Returns, Bug 7: Disk BASIC Not Loading — Missing External ROM, Bug 8: Active Area Re-Entry Causes Border Corruption (+10 more)

### Community 16 - "Community 16"
Cohesion: 0.11
Nodes (17): 1. Current Performance Profile, 3. Quick Wins Summary, 4. Medium-Term Optimizations, 5. Major Refactors, 6. Boot Time Improvements, 7. Projected FPS with All Remaining Optimizations, 8. Memory Placement Reference, 9. Re-Analysis Methodology (+9 more)

### Community 17 - "Community 17"
Cohesion: 0.12
Nodes (16): 1. Entry Point: `TTGO-VGA32-COCO.ino`, 2. Emulation Core (`src/core/`), code:block1 (+-----------------------------------------------------------), code:block2 ($0000-$7FFF  RAM (64 KB, SAM page select)), code:block3 (CPU $0000-$DFFF  Eight 8 KB MMU slots → any 8 KB block of 51), ESP32 CoCo Emulator — System Architecture, Machine Integration — `machine.cpp` / `machine.h`, MC6551 ACIA — `mc6551.cpp` / `mc6551.h` (+8 more)

### Community 18 - "Community 18"
Cohesion: 0.12
Nodes (16): 1. How FabGL is pulled in, 2. The decisive test: what's in the actual image?, 3. Conclusion, 4. If the goal is faster compiles (optional, low value), 5. Where the flash actually goes (if size reduction is the real goal), code:block1 (src/hal/hal_video.cpp     #include "fabgl.h"), code:block2 (-ffunction-sections  -fdata-sections     (compile: each func), code:block3 (text     data     bss      total) (+8 more)

### Community 19 - "Community 19"
Cohesion: 0.12
Nodes (16): CoCo 3 Fast-Path Memory Access, CoCo 3 Machine Structure, CoCo 3 Memory Map, CoCo 3 Scanline Timing, code:block18 (CPU address → slot = addr >> 13 (0-7)), code:block19 (set_interrupt(flag):), code:c (typedef struct Machine {), code:block21 ($0000-$FEFF  MMU-mapped via GIME (8 x 8KB pages from 512KB p) (+8 more)

### Community 20 - "Community 20"
Cohesion: 0.12
Nodes (15): Boot flow, code:c (g_machine_type = supervisor_load_machine_type();), code:c (extern uint8_t g_machine_type;   // 3 = CoCo 2, 4 = CoCo 3), code:c (m->cpu.read  = machine_read_cocoN;), code:c (uint8_t supervisor_load_machine_type(void);   // read NVS "s), Core API (`src/core/machine.h`), Files touched, Hot-path behaviour (+7 more)

### Community 21 - "Community 21"
Cohesion: 0.15
Nodes (13): 5.1 Entry Point and Flow, 5.2 Frame Timing, 5.3 CPU Clock Speed, 5.4 VDG Scanline Rendering Sequence, 5.5 Interrupt Timing, 5.6 Event System, 5. Main Emulation Loop, code:c (MC6809_IRQ_SET(CPU0, PIA0->a.irq | PIA0->b.irq);    // PIA0 ) (+5 more)

### Community 22 - "Community 22"
Cohesion: 0.17
Nodes (12): Address Advancement with Divide Logic (`vdg_address_add`), Clear Mask, code:cpp (sam6883_write(sam, addr):   // addr = offset from $FFC0 (0–3), code:block7 (vdg_address_add(sam, n):), code:block8 (fetch_bytes(nbytes):), code:block9 (GM: 0-5 → clear bits 1-4 (mask ~30 = ~0x1E) or bits 1-3 (mas), Fetch Bytes Implementation, Register Space ($FFC0–$FFDF) (+4 more)

### Community 23 - "Community 23"
Cohesion: 0.17
Nodes (12): 4.1 ROM List Definitions (xroar.c:301-336), 4.2 ROM Filenames Searched, 4.3 CRC Validation (xroar.c:352-361), 4.4 ROM Loading into Memory (machine.c:596-673), 4.5 ROM Address Mapping at Runtime, 4.6 ESP32 ROM Strategy, 4. ROM Loading Path, code:block1 (romlist coco=bas13,bas12,bas11,bas10) (+4 more)

### Community 24 - "Community 24"
Cohesion: 0.18
Nodes (10): code:cpp (// hal_storage.cpp), code:block7 (DSKCON (WRITE):), DSKCON Write Flow (Disk BASIC ROM), TTGO-VGA32-COCO Disk HAL — Implementation Details, FDC Command Summary, Key Lessons Learned, OS-9 Boot Sequence (DOS Command), Overview (+2 more)

### Community 25 - "Community 25"
Cohesion: 0.18
Nodes (10): code:c (enum SerialPortMode : uint8_t {), code:block2 (machine.cpp  ── $FF68-$FF6B decode ──►  mc6551_read / mc6551), `hal_rs232.cpp` — UART0 transport, Host tooling, Layers, `mc6551.cpp` — pure chip (no Arduino includes), OSD debug page, RS-232 Pak HAL — `hal_rs232.cpp` + `mc6551.cpp` (+2 more)

### Community 26 - "Community 26"
Cohesion: 0.20
Nodes (10): code:cpp (// config.h line 49), code:cpp (#pragma GCC optimize("O2")), code:cpp (// Current: fill ALL 320 pixels with border), code:cpp (// Left margin), OPT-C1: Set DEBUG_ENABLED=0, OPT-C2: Add `#pragma GCC optimize("O2")` to hal_video.cpp ✅ APPLIED 2026-04-16, OPT-C3: Copy font_gime[] to DRAM ✅ APPLIED 2026-04-16, OPT-C4: Direct RGB565 Rendering (Eliminate Double-Pass) ✅ APPLIED 2026-04-16 (+2 more)

### Community 27 - "Community 27"
Cohesion: 0.29
Nodes (7): code:block10 (1. Open .DSK file on SD card (FILE_READ)), code:block11 (1. Seek to header_size in SD file), Memory Budget, Mount Sequence, PSRAM Disk Cache, Why Bounce Buffer?, Write-Back (Flush/Eject)

### Community 28 - "Community 28"
Cohesion: 0.29
Nodes (7): code:cpp (void machine_run_scanline(Machine* m) {), code:cpp (// $FF40-$FF5F: Disk controller), code:cpp (m->fdc.nmi_callback = [](void* ctx, bool active) {), Integration Points, machine.cpp — Memory Map, machine.cpp — Scanline Loop, supervisor.cpp — Callback Wiring

### Community 29 - "Community 29"
Cohesion: 0.29
Nodes (7): code:block13 (data_size = image_size - header_size), code:block14 (linear_track = physical_track × sides + side), Disk Image Formats, .DSK (JVC), Geometry Detection, Sector Offset Calculation (Double-Sided), .VDK

### Community 30 - "Community 30"
Cohesion: 0.33
Nodes (6): code:block8 (Per sector in the stream:), code:block9 (States: WT_IDLE → WT_ID_FIELD → WT_WAIT_DAM → WT_DATA_FIELD ), How DSKINI Formats a Track, Key Details, State Machine Implementation, WRITE TRACK (Format) — DSKINI Support

### Community 31 - "Community 31"
Cohesion: 0.33
Nodes (6): code:cpp (// DRQ callback — used by DSKREG write and WRITE TRACK/SECTO), code:cpp (signal_halt(fdc, fdc->halt_enable && !fdc->drq && fdc->busy)), HALT Deadlock Fix (April 2026), HALT + DRQ Synchronization, How It Works, Implementation

### Community 32 - "Community 32"
Cohesion: 0.33
Nodes (6): code:cpp (if (!fdc->density && fdc->intrq) {), code:block5 (Command written ($FF48)     → set_intrq(false)  — clear any ), Deferred INTRQ for Sector Reads (Byte-257 Mechanism), INTRQ Lifecycle, NMI Gating (matching XRoar rsdos.c), NMI / INTRQ Mechanism

### Community 33 - "Community 33"
Cohesion: 0.40
Nodes (5): code:block1 ($FF40 DSKREG (drive select latch)), DSKREG Bit Assignments ($FF40), Hardware Being Emulated, Real CoCo Disk Controller, Register Map

### Community 34 - "Community 34"
Cohesion: 0.67
Nodes (3): code:block6 (DSKCON:), Deferred Sector Load (April 2026), DSKCON Read Flow (Disk BASIC ROM)

## Knowledge Gaps
- **411 isolated node(s):** `Project Overview`, `code:block1 (+-----------------------------------------------------------)`, `1. Entry Point: `TTGO-VGA32-COCO.ino``, `MC6809 CPU — `mc6809.cpp` / `mc6809.h``, `MC6821 PIA — `mc6821.cpp` / `mc6821.h`` (+406 more)
  These have ≤1 connection - possible missing edges or undocumented components.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `TTGO-VGA32-COCO Core Modules — Implementation Details` connect `Community 1` to `Community 19`, `Community 11`, `Community 22`?**
  _High betweenness centrality (0.016) - this node is a cross-community bridge._
- **Why does `Machine — System Integration (`machine.h`, `machine.cpp`)` connect `Community 11` to `Community 1`?**
  _High betweenness centrality (0.008) - this node is a cross-community bridge._
- **Why does `ANALYSIS.md — XRoar 0.32.0 ESP32-S3 Port Analysis` connect `Community 2` to `Community 21`, `Community 23`?**
  _High betweenness centrality (0.008) - this node is a cross-community bridge._
- **What connects `Project Overview`, `code:block1 (+-----------------------------------------------------------)`, `1. Entry Point: `TTGO-VGA32-COCO.ino`` to the rest of the system?**
  _411 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Community 0` be split into smaller, more focused modules?**
  _Cohesion score 0.04878048780487805 - nodes in this community are weakly interconnected._
- **Should `Community 1` be split into smaller, more focused modules?**
  _Cohesion score 0.05 - nodes in this community are weakly interconnected._
- **Should `Community 2` be split into smaller, more focused modules?**
  _Cohesion score 0.05128205128205128 - nodes in this community are weakly interconnected._