# Plan: Add Dragon 32 Support to ESP32 CoCo Emulator

## Context

The ESP32 CoCo emulator currently only runs as a CoCo 2 (MACHINE_TYPE=3). The user wants to add Dragon 32 support. The Dragon 32 and CoCo 2 share nearly identical hardware (same MC6809 CPU, MC6821 PIAs, MC6847 VDG, SAM6883), but differ in:

1. **ROM structure** — Dragon 32 has a single 16KB combined BASIC ROM ($8000-$BFFF) vs CoCo's split 8KB ExtBASIC ($8000-$9FFF) + 8KB BASIC ($A000-$BFFF)
2. **Keyboard matrix row order** — Dragon rows 0-5 map to CoCo rows 4,5,0,1,2,3 (numbers first, then letters). Row 6 (ENTER/CLEAR/BREAK/SHIFT) is the same.
3. **DOS system** — Dragon uses DragonDOS, not RS-DOS
4. **RAM default** — Dragon 32 = 32KB (but expandable); CoCo 2 = 64KB
5. **Printer port** — Dragon has Centronics printer on PIA1-PB0 (we can ignore this)
6. **TV standard** — Dragon 32 defaults to PAL (50Hz), CoCo 2 to NTSC (60Hz)

The good news: memory map, PIA addresses, SAM, VDG, I/O decode logic are all identical. No changes needed to CPU, PIA, VDG, or SAM core code.

## Implementation Steps

### Step 1: Add Dragon 32 ROM CRCs to `rom_loader.h`

**File:** `ESP32_CoCo2_XRoar_Port/src/roms/rom_loader.h`

Add Dragon 32 ROM CRC constants (from XRoar's crclist):
```c
#define CRC_DRAGON32    0xE3879310  // Dragon 32 Extended BASIC (16KB)
#define CRC_DRAGON32_W1 0xFF7BF41E  // Dragon 32 Woolham variant
#define CRC_DRAGON32_W2 0x9C7EED69  // Dragon 32 Woolham variant 2
#define CRC_DRAGONDOS   0xB44536F6  // DragonDOS 1.0
```

Update `rom_validate()` in `rom_loader.cpp` to recognize Dragon ROMs.

### Step 2: Make MACHINE_TYPE functional in `config.h`

**File:** `ESP32_CoCo2_XRoar_Port/config.h`

Add machine-type-conditional ROM filenames:
```c
#if MACHINE_TYPE == 0  // Dragon 32
  #define ROM_BASIC_FILE        "d32.rom"      // 16KB combined BASIC
  #define ROM_EXT_BASIC_FILE    ""             // Not used (combined ROM)
  #define ROM_DISK_FILE         "ddos10.rom"   // DragonDOS
  #define IS_DRAGON             1
  #define IS_COCO               0
  #define DEFAULT_RAM_KB        32
#elif MACHINE_TYPE == 3  // CoCo 2
  #define ROM_BASIC_FILE        "bas13.rom"
  #define ROM_EXT_BASIC_FILE    "extbas11.rom"
  #define ROM_DISK_FILE         "disk11.rom"
  #define IS_DRAGON             0
  #define IS_COCO               1
  #define DEFAULT_RAM_KB        64
#endif
```

Also add PAL timing conditionals:
```c
#if IS_DRAGON
  #define TARGET_FPS            50
  #define SCANLINES_PER_FRAME   312
#else
  #define TARGET_FPS            60
  #define SCANLINES_PER_FRAME   262
#endif
```

### Step 3: Modify ROM loading for Dragon 32 combined ROM

**File:** `ESP32_CoCo2_XRoar_Port/src/core/machine.cpp` — `machine_load_roms()`

Dragon 32 loads a single 16KB ROM into a contiguous buffer covering $8000-$BFFF, rather than two separate 8KB ROMs:

```c
#if IS_DRAGON
    // Dragon 32: single 16KB ROM covers $8000-$BFFF
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_BASIC_FILE);
    if (hal_storage_load_file(path, m->rom_extbas, 16384)) {
        m->rom_extbas_loaded = true;
        m->rom_basic_loaded = true;  // Both halves in one ROM
        // Copy upper 8KB to rom_basic buffer for compatibility
        memcpy(m->rom_basic, m->rom_extbas + 8192, 8192);
    }
#else
    // CoCo: separate BASIC + ExtBASIC ROMs (existing code)
    ...
#endif
```

Alternatively, allocate a single 16KB `rom_extbas` for Dragon and adjust `machine_read()` to serve $8000-$BFFF from it. The simpler approach is to split the 16KB ROM into the two existing 8KB buffers (`rom_extbas` for $8000-$9FFF, `rom_basic` for $A000-$BFFF) so `machine_read()` needs no changes.

**Recommended approach:** Load 16KB ROM, split into `rom_extbas[0..8191]` and `rom_basic[0..8191]`. This avoids changing the memory read dispatch at all.

### Step 4: Add Dragon keyboard matrix to `hal_keyboard.cpp`

**File:** `ESP32_CoCo2_XRoar_Port/src/hal/hal_keyboard.cpp`

The Dragon 32 keyboard matrix has the same keys but different row assignments:

| Row | Dragon 32                              | CoCo 2                                 |
|-----|----------------------------------------|----------------------------------------|
| 0   | 0 1 2 3 4 5 6 7                       | @ A B C D E F G                        |
| 1   | 8 9 : ; , - . /                       | H I J K L M N O                        |
| 2   | @ A B C D E F G                       | P Q R S T U V W                        |
| 3   | H I J K L M N O                       | X Y Z UP DOWN LEFT RIGHT SPACE         |
| 4   | P Q R S T U V W                       | 0 1 2 3 4 5 6 7                        |
| 5   | X Y Z UP DOWN LEFT RIGHT SPACE        | 8 9 : ; , - . /                        |
| 6   | ENTER CLEAR BREAK _ _ _ _ SHIFT       | ENTER CLEAR BREAK _ _ _ _ SHIFT        |

Add a second `KEY_MAP_DRAGON[]` table with the Dragon row assignments, or apply the inverse transform `(row + 2) % 6` to the CoCo rows (for rows 0-5):
- CoCo row 0 → Dragon row 2
- CoCo row 1 → Dragon row 3
- CoCo row 2 → Dragon row 4
- CoCo row 3 → Dragon row 5
- CoCo row 4 → Dragon row 0
- CoCo row 5 → Dragon row 1
- CoCo row 6 → Dragon row 6 (unchanged)

Use `#if IS_DRAGON` to select the correct table at compile time.

### Step 5: Handle Dragon 32 RAM size (32KB default)

**File:** `ESP32_CoCo2_XRoar_Port/config.h` and `machine.cpp`

Dragon 32 has 32KB RAM by default. In `machine.cpp`, respect the `RAM_SIZE_KB` setting. For Dragon 32 with 32KB:
- RAM addresses $0000-$7FFF: lower 32KB accessible
- Writes to $8000+ hit ROM (read-only) — already handled
- The unexpanded Dragon 32 masks RAM to 32KB (`ram_mask = 0x7FFF`)

In `machine_read()`/`machine_write()`, the RAM check `if ((size_t)addr < m->ram_size)` already handles this correctly since `ram_size` = 32768 for 32KB.

### Step 6: DragonDOS disk controller support

**File:** `ESP32_CoCo2_XRoar_Port/src/supervisor/sv_disk.cpp`

DragonDOS uses the same WD1793 FDC at the same addresses ($FF40-$FF5F), but the DSKREG control register semantics differ slightly:

- **RS-DOS (CoCo):** DSKREG at $FF40, bit 7 = HALT enable
- **DragonDOS:** DSKREG at $FF48, different bit mapping for drive select and motor

For initial Dragon 32 support, we can skip disk support (boot to BASIC only) and add DragonDOS in a follow-up. Mark this as TODO.

### Step 7: Update supervisor/OSD for machine identification

**File:** `ESP32_CoCo2_XRoar_Port/src/supervisor/supervisor.cpp`

Update the OSD/status display to show "Dragon 32" instead of "CoCo 2" when `IS_DRAGON` is set. Update the boot message.

### Step 8: PAL timing adjustments

**File:** `ESP32_CoCo2_XRoar_Port/src/core/machine.cpp`

Dragon 32 uses PAL (50Hz, 312 scanlines):
- `cycles_per_frame` = 895000 / 50 = 17900 (vs 14916 for NTSC)
- `scanlines_per_frame` = 312 (vs 262)
- Active display scanlines remain 192

Update `machine_init()` to use PAL constants when `IS_DRAGON`.

## Files to Modify

| File | Change |
|------|--------|
| `config.h` | Machine-conditional ROM filenames, IS_DRAGON/IS_COCO flags, PAL timing |
| `src/roms/rom_loader.h` | Add Dragon ROM CRC constants |
| `src/roms/rom_loader.cpp` | Recognize Dragon CRCs in validation |
| `src/core/machine.cpp` | Dragon ROM loading (16KB combined), PAL timing in init |
| `src/hal/hal_keyboard.cpp` | Dragon keyboard matrix (row reorder) |
| `src/supervisor/supervisor.cpp` | OSD machine name display |

## Files NOT modified (no changes needed)

- `src/core/mc6809.cpp` — identical CPU
- `src/core/mc6821.cpp` — identical PIA
- `src/core/mc6847.cpp` — identical VDG
- `src/core/sam6883.cpp` — identical SAM
- `src/core/machine.h` — struct unchanged (two 8KB ROM buffers work for Dragon too)
- `machine_read()`/`machine_write()` — no changes if we split the 16KB Dragon ROM into the existing two 8KB buffers

## ROM Files Needed on SD Card

For Dragon 32, user must place on SD `/roms/`:
- `d32.rom` — Dragon 32 BASIC ROM (16KB, CRC `0xE3879310`)
- `ddos10.rom` — DragonDOS 1.0 (optional, for future disk support)

## Verification

1. Set `MACHINE_TYPE 0` in config.h, place `d32.rom` on SD card
2. Compile and upload: `arduino-cli compile ...` / `arduino-cli upload ...`
3. Verify boot message shows "Dragon 32" and Dragon BASIC prompt appears
4. Test keyboard: letters, numbers, ENTER, CLEAR, BREAK, SHIFT+key
5. Test BASIC commands: `PRINT "HELLO"`, `FOR I=1 TO 10:PRINT I:NEXT`
6. Verify `PRINT MEM` shows ~24KB free (32KB RAM minus system overhead)
7. Disk support deferred to follow-up task
