# GIME validation verdicts

When XRoar and MAME disagree on a check, an entry here says which one is
accurate (hardware-true). The runner then reports DISC if our value matches
that model and FAIL if it does not. A split with no entry is UNRES.
`v86.` checks never need an entry.

Entry format: a level-2 heading with the check id, then the machine-readable
lines, then free markdown (what each model does with file:line, evidence, and
whether ours is a bug or a simplification).

```
## timer.tins1_period_596
accurate: xroar
confidence: high
<free markdown>
```

- `accurate:` (required) is one of `ours`, `xroar`, `mame`.
- `confidence:` (optional, default `medium`) is `high`, `medium` or `low`.
- A `## ` line always starts a new entry, so sub-headings inside an entry
  must use `###` or deeper.
- Lines inside a fenced block (like the example above) are ignored.
- A malformed entry (unknown model, bad confidence, missing `accurate:`, a
  duplicate id, a bad id) is a hard error naming the line.

### Prefix verdicts

An id may end in `*` (and only there) to cover every check whose id starts
with the text before it, e.g. `## random.irq.*`. Use it for a family of checks
with one cause. An exact id beats any prefix; among prefixes the longest wins.
`results.json` records the entry applied to each check (`verdict_id`) and the
report lists every prefix verdict with the checks it covered.

```
## random.timer.*
accurate: xroar
confidence: medium
<free markdown>
```

Prefix matching is a plain string prefix, with no notion of id segments:
`a.b*` also matches `a.bc`. End a family prefix at a `.` or `_` that
closes the name (`random.mmu.6.*`, not `random.mmu.6*`, which would also
match runs 60-69), and check the report's prefix listing after adding one.

### Coverage of the random checks

`random.regs.*`, `random.video.*` and `random.irq.*` each have a single
verdict, because every split in them comes from causes that favour the same
model. `random.mmu.*` has an area verdict plus one entry per run for the
runs whose `$FE00` page splits have a different cause. `random.render.*`
has no verdict: replaying each run's writes shows its splits mix several
causes that are not judged yet (the X offset in COCO mode, which XRoar
applies and calls "needs verifying" in a403bc6a, in 47 of 81 runs; LPF in
COCO mode; LPF 1 as 199 or 200 lines; the attribute underline row at
LPR < 3 and LPR 5; the X offset in native text modes; LPR 7), several runs
have more than one, and the run ids carry no cause. Those checks stay
UNRESOLVED (non-failing) until a cause can be attached to each run.

# Verdicts

Model sources: ours `src/core/tcc1014.cpp`, `src/core/machine.cpp`,
`config.h` at the repository root (branch test/gime-validation-suite); XRoar 53acd7e3
`src/tcc1014/tcc1014.c` (cited as `tcc1014.c`); MAME 9c525573728
`src/mame/trs/gime.cpp` (cited as `gime.cpp`) and
`src/devices/video/mc6847.cpp`; VCC (third opinion) at
`coco-emu-other-projects-source/VCC`. "Sock" is Sock's GIME register
reference, https://www.6809.org.uk/sock/gime.html. XRoar commit b24f5281
("gime: update timings", 2024-06-19) is described by its author as "Lots of
measurements made using real '86 and '87 GIMEs"; XRoar commit d9bba8a3
("coco3/gime: fix up a lot of interrupt behaviour", 2025-08-28) is based on
Tim Lindner's real-hardware test tools
(https://github.com/tlindner/GIME-MMU-TESTER,
https://github.com/tlindner/coco_key_test).

## reset.read_FFA*
accurate: ours
confidence: low

### MMU registers after reset ($FFA0-$FFAF)

- Ours: `tcc1014.cpp:196` sets bank `i` to `$38 | (i & 7)` at reset.
- MAME: `gime.cpp:310` does the same (`56 + i`).
- VCC: `tcc1014mmu.cpp:120-122` (`MmuReset`) also sets `Index1 + StateSwitch[...]`, the same identity map for 512 KB.
- XRoar: `tcc1014_reset` (`tcc1014.c:683-708`) clears the GIME registers and palette but leaves `mmu_bank[]` at the zero it got at allocation, so the banks read 0.

Evidence: no hardware source gives the power-on or reset contents of the MMU registers. Sock and the reference
pages found (cocopedia "Understanding and Using the MMU") describe only what BASIC writes there ($38-$3F), and the
CoCo 3 ROM writes all 16 registers itself, so software never depends on the reset value. MAME's comment at
`gime.cpp:305` ("Tepolt verifies that the GIME registers are all cleared") is about $FF90-$FF9F, not the MMU.
Two of three emulators (MAME, VCC) agree with ours; XRoar's zero is a by-product of allocation, not a stated
model. `accurate: ours` (= MAME) rather than `mame` so that the `random.mmu.*` checks where MAME reports no Z (ROM
or cartridge slots) also resolve. Not an ours bug either way; confidence is low because the hardware value is
unknown.

## random.regs.*
accurate: ours
confidence: low

Every split in this area is a read of an MMU register that the run never wrote (`random.regs.<k>.read_FFAx`),
where XRoar reads 0 and ours and MAME read `$38+i`. Same cause and evidence as `reset.read_FFA*`.

## regs.undriven_bits_*
accurate: xroar
confidence: low

### Undriven bits 7-6 of $FF92, the MMU and the palette registers (harness convention)

This is a convention of the harness, not a GIME accuracy claim. These checks read the register unmasked. The XRoar
and MAME adapters supply $FF as the floating bus value, and both references OR their six bits into it
(`tcc1014.c:789`, `:803` and `:812`, `*CPUD = (*CPUD & ~0x3f) | value`; `gime.cpp:724`, `:742`, where MAME notes "Bits 7/6 are floating, and behave oddly" at `:735`). Ours drives the
undriven bits as 0 (`tcc1014.cpp:566-568` returns the 6-bit register value).

Sock: "the upper 2 bits must be masked out as they return bleedover from the bus (sometimes zero, sometimes
one)". VCC returns `(lastBus() & 0xC0) | (data & 0x3F)` for the MMU registers (`tcc1014registers.cpp:318`). A mametesters report (https://mametesters.org/view.php?id=5135) says bit 6 reads 1 on a real CoCo 3. So
neither 0 nor $FF is "the" hardware value; the references pass the bus through and ours does not.

`undriven_bits_ff92` also includes a real difference: ours does not latch TMR when $FF92 is written with the
timer at 0 (see the baseline reason "no TMR latch on enable"). MAME returns `m_irq` with no floating bits
(`gime.cpp:690-693`), which is why it reads 32 against XRoar's 224. All three FAILs are baselined with the
harness-convention reason.

## mmu.mc3_fe00_clr_*
accurate: xroar
confidence: high

### $FE00-$FEFF with MC3 clear after the MMU was reprogrammed (MAME stale bank 8)

- Ours and XRoar map $FE00 through MMU slot 7 when MC3 = 0 (`tcc1014.cpp:251-262`, `tcc1014.c:734-751`), so it is
  RAM here (S = 7).
- MAME keeps $FE00-$FEFF in a separate bank 8 that is derived from slot 7 (`gime.cpp:521-531`), but
  `write_mmu_register` refreshes only bank `offset & 7` (`gime.cpp:1031`). Bank 8 keeps the mapping it had at the
  last full `update_memory()` (the $FF90 write, when $FFA7 still held 63), so MAME reports ROM (S = 1).

Evidence: MAME's own `update_memory(8)` computes bank 8 from slot 7, so the stale value is an internal
inconsistency (a missing `update_memory(8)` after a slot 7/15 write), not a model of hardware. Ours: no
difference.

## mmu.mc3_fe00_set_*
accurate: xroar
confidence: low

### $FE00-$FEFF with MC3 set while ROM is mapped (TY = 0)

- XRoar sets RAS for the page and then still applies the ROM select logic to bank $3F (`tcc1014.c:734-751`): S = 1
  (CTS) with RAS asserted. In coco3.c the RAM read comes last and wins (`coco3.c:1132-1143`), so the CPU reads RAM.
- Ours copies that decode (`tcc1014.cpp:251-262`).
- MAME has no chip-select output. The adapter derives S from MAME's bank pointers, and `force_ram`
  (`gime.cpp:530-531`) points the page at RAM, so the adapter reports S = 7.

The data seen by the CPU agrees in all three (RAM, Sock: "1=RAM at FExx is constant"); the split is only the S
code, and MAME's S is an adapter construction. Whether the real GIME also asserts CTS* for this page is
undocumented. XRoar is the only model that defines S here, so it is taken as accurate, with low confidence.
Ours: no difference.

## random.mmu.*
accurate: ours
confidence: low

Every split in this area except the run-level `random.mmu.<k>.decode_FE*` entries below is the MMU reset value:
the run enabled the MMU without writing the slot the probe uses, XRoar maps it to bank 0 and ours and MAME to
`$38+i` (or MAME reports no Z for a ROM slot). Same cause and evidence as `reset.read_FFA*`.

## irq.enable_pending_*
accurate: xroar
confidence: low

### External interrupt (EI0-EI2) enabled while its input is already high

- XRoar latches the interrupt at the enable write if the input is high: `set_int` includes `IL*_state`, masked by
  the bits that changed and are now enabled (`tcc1014.c:950-968`).
- Ours latches it at the next CPU access, because it samples the IL inputs as levels on every cycle
  (`tcc1014.cpp:383-387`). The check sees the same result as XRoar.
- MAME sets an EI bit only on an input rising edge while that source is enabled (`interrupt_rising_edge`,
  `gime.cpp:1138-1147`). Enabling an already-high input gives nothing.
- VCC (third opinion) agrees with MAME: a $FF92/$FF93 write only clears and handles the zero-timer case
  (`tcc1014registers.cpp:158-190`); external sources are asserted only by events (`GimeAssertKeyboardInterupt`,
  `keyboard.cpp:148`).

Evidence: XRoar commit d9bba8a3 says Tim Lindner's GIME-MMU-TESTER "highlighted incorrect re-latching behaviour of
timer on modification of the enable registers" (a real-hardware result for TMR), and then "I've also assumed the
same re-latching behaviour for all of IL0..2 for now". So the TMR behaviour is measured and the EI behaviour is
XRoar's stated assumption by analogy. Sock describes the cartridge interrupt as edge-generated ("on the falling
edge of the signal on PIN 8") and the keyboard interrupt as level ("whenever a zero appears on any one of
PA0-PA6"), but says nothing about enabling a pending source. XRoar is taken as accurate because its rule is the
one derived from a hardware test (the same enable-register latch path); confidence is low because no test of an
EI source exists. Ours: no difference (by a different mechanism; see the EI level re-latch baseline reason).

## irq.enable_vbord_*
accurate: xroar
confidence: low

### First VBORD after power-on

The check measures the first VBORD rise from reset, so it combines the power-on frame phase with the VBORD line:

- XRoar starts in vertical sync at reset (`tcc1014.c:683-708`, `vertical.sync = 1`) and raises VBORD 7 + 36 + 192
  lines later (215379 ticks).
- MAME starts at top-border line 0 (`mc6847.cpp:220`), so its first VBORD comes 216 lines in (196992 ticks). The
  power-on phase of the video counters is not specified by any hardware source and no software can depend on it.
- Ours is 214320: ours starts at scanline 0 like XRoar, but raises VBORD one line early (`machine.cpp:836-839`),
  which is the `video.vbord_line_*` difference.

The verdict follows `video.vbord_line_*` for the part that can be judged. Ours FAILs by the VBORD-line cause;
MAME's value is mostly reset phase. Low confidence because of the phase part.

## video.lines_per_field_*
accurate: xroar
confidence: medium

### Lines per 60 Hz field: 263 (XRoar) or 262 (ours, MAME, VCC)

- XRoar: FS falls when `scanline >= lF` with `lF = H50 ? 314 : 263` (`tcc1014.c:1192-1198`), so a field is 263
  lines (239856 ticks) in every LPF and mode.
- Ours: `SCANLINES_PER_FRAME 262` (`config.h:256`), wrap at `machine.cpp:845-848`.
- MAME: the GIME is driven by the MC6847 frame, 25 + 192 + 26 + 6 + 13 = 262 lines (`mc6847.cpp:112-119`).
- VCC: 4 + 3 + 11 + 239 + 5 = 262 lines for LPF 0 (`coco3.cpp:205-275`, `tcc1014graphics.h:23-26`).

Evidence: XRoar changed its field length from 262 to 263 (and its 50 Hz field from 312 to 314) in commit b24f5281,
the commit whose message says "Lots of measurements made using real '86 and '87 GIMEs"; the same commit
introduced the top-border table marked "Measured" (`tcc1014.c:407-412`) and moved the line-rate timer to HS fall
("confirmed on scope"). The 262 in MAME is inherited from the MC6847 VDG timing (a different chip), and VCC's 262
is a hand-built frame. No independent hardware document was found (web search, Sock). So XRoar is the only
measured source; confidence medium because the 263 itself is not called out as a measurement.

Ours: bug (or simplification): the field is one line short, which changes the 60 Hz frame rate by 0.4% (59.83
against 60.06 Hz) and the position of VBORD/vsync-relative effects. The `random.video.*` field checks have the
same cause.

## video.vbord_line_*
accurate: xroar
confidence: medium

### VBORD position relative to FS fall

- XRoar: both FS fall and VBORD happen at the vertical timing point of a line (`do_vb_irq`); VBORD fires when
  `lcount >= lAA` at the line after the last active line (`tcc1014.c:1249-1251`), 7 + lTB + lAA lines after FS
  fall (235, 240 and 251 lines for LPF 0, 1 and 3).
- Ours: raises VBORD at the end of the last active line, `display_line + 1 >= lAA` (`machine.cpp:836-839`), one
  line earlier (234, 239, 250 lines).
- MAME: FS falls 1696 clocks into a line (`mc6847.cpp:287`) and VBORD comes at the HS rise of the first
  bottom-border line (`gime.cpp:1269-1273`, `enter_bottom_border`), 848 ticks before XRoar for LPF 0 and 3 and 64 ticks after for LPF 1
  (MAME's LPF 1 has a 23-line top border and 200 lines, `gime.cpp:1627-1631`).

Evidence: XRoar's vertical timing point is described from scope measurements ("you can see on a scope that there
is a slight offset, which varies by GIME model", `tcc1014.c:106-110`) and its top-border line counts are marked
"Measured" (`tcc1014.c:407-412`), both from commit b24f5281. MAME derives its vertical timing from the MC6847.
VCC (third opinion) raises VBORD at FS fall (`coco3.cpp:305-311`), which matches none of the three and follows
Sock's wording ("generated on the falling edge of VSYNC"); it is not used as evidence for either side.

Ours: bug, one line early. Software that changes the palette or mode at VBORD (many CoCo 3 games and demos use
VBORD as their 60 Hz tick) sees the change one line earlier than on XRoar.

## video.vsc_latch
accurate: xroar
confidence: medium

### Line count until a VSC / vertical offset write takes effect

The latch line counts differ by 2 lines between XRoar (210, 286, 206) and ours/MAME (208, 284, 204). One line is
ours' sync resolution (R11 gives these checks 1 line of tolerance) and the other is the field length: the write
lands in the next field, which is one line longer in XRoar. The `_value` checks (what is latched) PASS in all three.
Ours latches VSC at active-area entry like XRoar (`machine.cpp:717-848`; XRoar `tcc1014.c:1231-1241`); MAME
latches in `new_frame` (`gime.cpp:1231-1250`). Same verdict and evidence as `video.lines_per_field_*`.

## video.voffset_latch_*
accurate: xroar
confidence: medium

Same cause as `video.vsc_latch`: the vertical offset ($FF9D/$FF9E) is latched at the end of vertical blanking in
XRoar (`tcc1014.c:1218-1223`, commit 3fca003e "move video base setting to end of vblank", which notes Androne is
sensitive to it) and ours; the 2-line split is sync resolution plus the 263-line field. The `_vsync` and `_value`
checks PASS.

## video.reg_effect_same_line_first_px
accurate: xroar
confidence: high

### A mode write in the middle of a line

- XRoar renders up to the beam before applying any register or palette write (`render_scanline(gime,
  event_current_tick)` at `tcc1014.c:808` and `:923`), so a $FF98 write 400 ticks after HS fall changes the line
  from pixel 224 on.
- Ours renders the whole line at the line end (`tcc1014.cpp:235`, "XRoar calls render_scanline() before register
  writes; we skip"), so the write affects the whole line (first pixel 0).
- MAME records each line whole at the HS rise; partial-line recording is not implemented
  (`gime.cpp:1604-1607`, `fatalerror("NYI")`), so it also gives 0.

Evidence: XRoar commit b24f5281: "Looking at real CoCo 3 output, mode switches don't take effect until the next
byte at least (could be 16 bits, need to experiment)", i.e. on hardware a mode switch takes effect within the
line, at byte granularity. Sock: "Both reading and writing to the palette registers causes a small 'glitch' on
the screen ... If you want to avoid them, you can change the palettes while the video is in the vertical or
horizontal border", which only makes sense if a mid-line write shows mid-line. Demos that change the palette
mid-line (raster effects) depend on it. MAME's and ours' whole-line behaviour are both simplifications.

Ours: simplification (whole-line rendering, chosen for speed on the ESP32).

## render.gfx_bpr16_cres2.*
accurate: xroar
confidence: medium

### 16-colour modes at 16 bytes per row (HRES 0, CRES 2/3)

- XRoar fetches video data 16 bits at a time and, in these modes only, discards the second byte of each fetch
  (`tcc1014.c:1388-1392`, "16 colour, 16 byte-per-row modes zero the second half of the data"; fetch at
  `:1262-1280`).
- Ours copies the 16-bit fetch (`tcc1014.cpp:780-795`) and the HRES 0 / CRES >= 2 case.
- MAME renders every byte (`gime.cpp:1893-1894`, `emit_gime_graphics_samples<16, 4>`), so from pixel 32 on its
  line shows byte 1's colours where XRoar shows colour 0.

Evidence: XRoar commit a1c45489 "gime: implement 16-colour 16 byte-per-row bug: These modes lose (zero) the
second 8 bits of each 16-bit fetch", added right after commit 092ae309 "fetch 16 bits of video data at a time".
Calling it a "bug" that was implemented means it reproduces an observed hardware quirk, not a design choice.
Neither Sock nor MAME mentions it. Confidence medium: the commit gives the behaviour but not the test.
Ours: no difference.

## render.gfx_bpr16_cres3.*
accurate: xroar
confidence: medium

Same cause and evidence as `render.gfx_bpr16_cres2.*` (CRES 3 is also 16 colours).

## render.gfx_bpr128_cres0.*
accurate: xroar
confidence: low

### 2-colour (CRES 0) at 128 and 160 bytes per row

- XRoar draws 1 bit per pixel and, at its highest resolution, shows every other bit (bits 7, 5, 3 and 1;
  `tcc1014.c:1454-1458` with `resolution` 3 at `:1525-1529`), 4 pixels per byte.
- Ours copies that.
- MAME treats CRES 0 like CRES 1 at these widths (`gime.cpp:1915-1920`, `case 0x18: case 0x19:` and `case 0x1c:
  case 0x1d:` both `emit_gime_graphics_samples<1, 2>`), i.e. 2 bits per pixel.

Evidence: none found. 128 or 160 bytes at 1 bit per pixel would be 1024 or 1280 pixels, more than the 512/640
pixel clocks of a line, so the mode is not a documented one (Sock lists only the HRES widths). Both renderings
are guesses at an undocumented mode; XRoar's author tests on real GIMEs and MAME's table looks like a copy of the
CRES 1 row, so XRoar is taken with low confidence. No known software uses the mode. Ours: no difference.

## render.gfx_bpr160_cres0.*
accurate: xroar
confidence: low

Same cause and evidence as `render.gfx_bpr128_cres0.*` (HRES 7, 160 bytes per row).

## render.txt32_noattr.line100_hash
accurate: mame
confidence: medium

### GIME text font, character $15, font row 4

The XRoar and MAME GIME fonts are the same table shifted by one row (XRoar `font_gime[c*12 + row + 1]` with row
12 wrapping to 0, `tcc1014.c:1381-1384`; MAME `hires_font[c][line_in_row]`, `gime.cpp:1757`), and a byte-by-byte
comparison of all 128 x 12 rows finds exactly one difference: character $15 (o with a slash), font row 4, XRoar
$5C (`font-gime.c:25`) and MAME $54 (`gime.cpp:2032`). Display line 100 at LPR 3 is font row 4, and the four
text checks that fail have a $15 byte on that row.

Evidence: VCC's independent CoCo 3 font (`cc3font.h`, character 21) also has $54. $54 gives the clean diagonal
(.#..##. / .#.#.#. / .##..#.) used by the capital slashed O at $18 in all three fonts (`0x38, 0x4c, 0x54, 0x54,
0x54, 0x64, 0x38`); $5C adds a stray pixel. Two independent tables against one, plus the glyph's own shape.
Ours copies XRoar's table (`font_gime.h:54`), so ours: bug (one font byte).

## render.txt40_attr.line100_hash
accurate: mame
confidence: medium

Same cause and evidence as `render.txt32_noattr.line100_hash` (character $15, font row 4).

## render.txt80_noattr.line100_hash
accurate: mame
confidence: medium

Same cause and evidence as `render.txt32_noattr.line100_hash` (character $15, font row 4).

## render.txt80_attr.line100_hash
accurate: mame
confidence: medium

Same cause and evidence as `render.txt32_noattr.line100_hash` (character $15, font row 4).

## random.video.*
accurate: xroar
confidence: medium

Every split in this area is `field_ticks` (FS fall to FS fall): XRoar has 263 lines and ours and MAME 262, the
`video.lines_per_field_*` cause, with the same evidence and confidence. The random $FF98 writes keep H50 = 0
(the suite is NTSC-only), so no 50 Hz field is involved.

## random.irq.*
accurate: xroar
confidence: low

The splits in this area have two causes, both favouring XRoar:

1. An EI source enabled while its input is high (76 of the 94 runs with a split, found by replaying each run's
   writes): the `irq.enable_pending_*` cause.
2. The first HBORD after reset in runs that advance one line (the other 18 runs): MAME raises HBORD from its HS callback
   912 ticks after reset (`gime.cpp:1258-1261`), XRoar at its right-border point 1673 ticks after reset
   (`tcc1014.c:1186-1188`, the HBORD point measured by scope in b24f5281), so after one line MAME already shows
   HBORD (16) in $FF92/$FF93 and XRoar does not. Within a line, XRoar's HBORD position is the measured one.

Checks where ours differs from XRoar here are baselined. Their reasons come from replaying the suite with a scratch
copy of ours (never committed): ours patched to XRoar's d9bba8a3 interrupt logic (edge-detected IL, TMR latch on
enable, no TMR re-latch on a status read, enable write keeping still-enabled pending bits) clears 247 of the 343
`random.irq` FAILs, ours patched to raise HBORD on every line clears 70, 12 clear with either patch and 38 need
both.

## random.mmu.6.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.31.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.34.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.36.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.41.decode_FE*
accurate: xroar
confidence: high

MC3 clear after a slot 7/15 write: MAME's bank 8 ($FE00 page) is stale. Same cause and evidence as `mmu.mc3_fe00_clr_*`.

## random.mmu.42.decode_FE*
accurate: xroar
confidence: high

MC3 clear after a slot 7/15 write: MAME's bank 8 ($FE00 page) is stale. Same cause and evidence as `mmu.mc3_fe00_clr_*`.

## random.mmu.45.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.48.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.52.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.62.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.74.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.79.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.89.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.127.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.136.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.142.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.143.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.145.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.161.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.166.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.170.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.180.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.181.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.

## random.mmu.189.decode_FE*
accurate: xroar
confidence: low

MC3 set with ROM mapped: MAME's adapter reports S = 7 for the $FE00 page. Same cause and evidence as `mmu.mc3_fe00_set_*`.
