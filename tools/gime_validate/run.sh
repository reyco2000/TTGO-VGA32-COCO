#!/bin/bash
# Build and run the host-side GIME validation suite.
# usage: tools/gime_validate/run.sh [--seed N] [--random N] [--update-baseline] [--selftest]
# Writes ${TMPDIR:-/tmp}/gime_validate/gime_validation_report.md (not committed) and prints its path.
#
# XROAR_SRC (default ~/proyectos/coco-emu-other-projects-source/xroar) is an
# XRoar source tree; its tcc1014.c is built unmodified as the XRoar oracle.
# MAME_SRC (default ~/proyectos/coco-emu-other-projects-source/mame) is a MAME
# source tree; its src/mame/trs/gime.cpp and src/devices/video/mc6847.cpp are
# built unmodified against mame_shim/ as the MAME oracle, twice: as is (1986
# GIME) and from a copy whose line 99 is rewritten to GIME_TYPE_1987 1.
# A missing tree or a failed oracle build prints a NOTICE and that model's
# checks are SKIPPED.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_validate
mkdir -p "$OUT"
XROAR_SRC=${XROAR_SRC:-$HOME/proyectos/coco-emu-other-projects-source/xroar}
MAME_SRC=${MAME_SRC:-$HOME/proyectos/coco-emu-other-projects-source/mame}

# Mirror drift check (ruling R5). model_ours.cpp runs the GIME part of
# machine_run_scanline_coco3() from a copy of src/core/machine.cpp, in two
# blocks marked "// MIRROR-BEGIN <name>" / "// MIRROR-END <name>". Each is
# compared with the machine.cpp range found by anchor text (not line numbers):
#   timer:    "tcc1014_tick_scanline(&m->gime);" through
#             "mc6821_ca1_transition(&m->pia0, true);"
#   vertical: "TCC1014* g = &m->gime;" through the "}" after
#             "m->scanline = 0;"
# Normalisation, both sides: "//" comments, indentation, trailing blanks and
# empty lines are dropped. machine.cpp side: "m->scanline" -> "scanline_",
# "&m->gime" -> "&g_", "mc6821_ca1_transition(&m->pia0, false);" -> @HS_FALL,
# "mc6821_ca1_transition(&m->pia0, true);" dropped (no HS rise event),
# "mc6821_cb1_transition(&m->pia0, false|true);" -> @FS_FALL|@FS_RISE, and
# the render + HAL blocks (the "{" before
# "PERF_PROBE_SCOPE(PROBE_RENDER_SCANLINE);" through the "}" closing the
# PROBE_HAL_SCANLINE block) -> @RENDER. model_ours.cpp side: the
# "ev_.push_back({ EvType::X, now_ });" lines -> @X, except TIMER, which is
# dropped with its "for (; fires_ < g_.timer_fires; fires_++)" line (event
# bookkeeping), "have_line_ = false;" and "line_note_.clear();" dropped, and
# the lines from "// MIRROR-RENDER-BEGIN" to "// MIRROR-RENDER-END" ->
# @RENDER (raw_output forced false, line capture). Any other difference fails
# the build. MACHINE_CPP overrides the machine.cpp path (to test the check).
MACHINE_CPP=${MACHINE_CPP:-$D/../../src/core/machine.cpp}
if ! python3 -I - "$D/model_ours.cpp" "$MACHINE_CPP" <<'PY'
import difflib, re, sys

def norm(lines):
    out = []
    for l in lines:
        l = re.sub(r"//.*", "", l).strip()
        if l:
            out.append(l)
    return out

def find(lines, text, start=0):
    for i in range(start, len(lines)):
        if text in lines[i]:
            return i
    sys.exit("mirror check: anchor not found in machine.cpp: %r" % text)

def ours_block(lines, name):
    b = [i for i, l in enumerate(lines) if l.strip() == "// MIRROR-BEGIN " + name]
    e = [i for i, l in enumerate(lines) if l.strip() == "// MIRROR-END " + name]
    if len(b) != 1 or len(e) != 1 or e[0] < b[0]:
        sys.exit("mirror check: model_ours.cpp needs one MIRROR-BEGIN/END %s pair" % name)
    blk, out, skip = lines[b[0] + 1:e[0]], [], False
    for l in blk:
        t = l.strip()
        if t == "// MIRROR-RENDER-BEGIN":
            out.append("@RENDER"); skip = True; continue
        if t == "// MIRROR-RENDER-END":
            skip = False; continue
        if not skip:
            out.append(l)
    res = []
    for l in norm(out):
        m = re.fullmatch(r"ev_\.push_back\(\{ EvType::(\w+), now_ \}\);", l)
        if m:
            if m.group(1) != "TIMER":
                res.append("@" + m.group(1))
            continue
        if l in ("for (; fires_ < g_.timer_fires; fires_++)", "have_line_ = false;",
                 "line_note_.clear();"):
            continue
        res.append(l)
    return res

def machine_norm(blk, render):
    n = norm(blk)
    if render:
        n = replace_render(n)
    res = []
    for l in n:
        l = l.replace("m->scanline", "scanline_").replace("&m->gime", "&g_")
        if l == "mc6821_ca1_transition(&m->pia0, true);":
            continue
        l = {"mc6821_ca1_transition(&m->pia0, false);": "@HS_FALL",
             "mc6821_cb1_transition(&m->pia0, false);": "@FS_FALL",
             "mc6821_cb1_transition(&m->pia0, true);": "@FS_RISE"}.get(l, l)
        res.append(l)
    return res

def replace_render(n):
    # render + HAL: two brace blocks starting at the "{" before PROBE_RENDER
    try:
        i = n.index("PERF_PROBE_SCOPE(PROBE_RENDER_SCANLINE);") - 1
    except ValueError:
        sys.exit("mirror check: PERF_PROBE_SCOPE(PROBE_RENDER_SCANLINE) not found in machine.cpp")
    if n[i] != "{":
        sys.exit("mirror check: no '{' before PROBE_RENDER_SCANLINE in machine.cpp")
    j, depth, blocks = i, 0, 0
    while j < len(n):
        depth += n[j].count("{") - n[j].count("}")
        if depth == 0:
            blocks += 1
            if blocks == 2:
                break
        j += 1
    if blocks != 2 or "PERF_PROBE_SCOPE(PROBE_HAL_SCANLINE);" not in n[i:j + 1]:
        sys.exit("mirror check: render/HAL blocks not found as expected in machine.cpp")
    return n[:i] + ["@RENDER"] + n[j + 1:]

ours = open(sys.argv[1]).read().split("\n")
mach = open(sys.argv[2]).read().split("\n")
a = find(mach, "tcc1014_tick_scanline(&m->gime);")
b = find(mach, "mc6821_ca1_transition(&m->pia0, true);", a)
c = find(mach, "TCC1014* g = &m->gime;", b)
d = find(mach, "m->scanline = 0;", c)
if d + 1 >= len(mach) or mach[d + 1].strip() != "}":
    sys.exit("mirror check: no '}' after 'm->scanline = 0;' in machine.cpp")
bad = False
for name, rng in (("timer", (a, b)), ("vertical", (c, d + 1))):
    want = machine_norm(mach[rng[0]:rng[1] + 1], name == "vertical")
    got = ours_block(ours, name)
    if got != want:
        bad = True
        print("MIRROR DRIFT: model_ours.cpp block '%s' no longer matches machine.cpp:%d-%d"
              % (name, rng[0] + 1, rng[1] + 1))
        for l in difflib.unified_diff(want, got, "machine.cpp (normalised)",
                                      "model_ours.cpp (normalised)", lineterm="", n=2):
            print("  " + l)
sys.exit(1 if bad else 0)
PY
then
    echo "build failed: model_ours.cpp's machine.cpp mirror has drifted; update it to match machine.cpp"
    exit 2
fi

DEFS=()
OBJS=()
ARGS=()
if [ -f "$XROAR_SRC/src/tcc1014/tcc1014.c" ]; then
    XI=(-I"$XROAR_SRC" -I"$XROAR_SRC/src" -I"$XROAR_SRC/portalib" -I"$D")
    XSRCS=("$D/xroar_gime.c" "$XROAR_SRC/src/events.c"
           "$XROAR_SRC/src/tcc1014/font-gime.c" "$XROAR_SRC/portalib/delegate.c")
    XOBJS=("$OUT/xg_xroar_gime.o" "$OUT/xg_events.o" "$OUT/xg_font-gime.o" "$OUT/xg_delegate.o")
    # One object holding the XRoar GIME (both variants are chosen at runtime).
    # A failed build skips the oracle instead of failing the run.
    # (errexit is off inside an if condition, so each step checks itself.)
    build_xroar() {
        for i in "${!XSRCS[@]}"; do
            gcc -std=gnu11 -O2 -w "${XI[@]}" -c "${XSRCS[$i]}" -o "${XOBJS[$i]}" || return 1
        done
        ld -r -o "$OUT/xroar_gime.o" "${XOBJS[@]}" || return 1
    }
    if build_xroar; then
        DEFS=(-DHAVE_XROAR)
        OBJS=("$OUT/xroar_gime.o" "$D/model_xroar.cpp")
        ARGS=(--xroar-commit "$(git -C "$XROAR_SRC" log -1 --format=%h 2>/dev/null || echo unknown)")
    else
        echo "NOTICE: xroar oracle build failed; skipping"
    fi
else
    echo "NOTICE: xroar source not found at $XROAR_SRC; skipping"
fi

MG=$MAME_SRC/src/mame/trs/gime.cpp
if [ -f "$MG" ] && [ -f "$MAME_SRC/src/devices/video/mc6847.cpp" ]; then
    # The only edit to MAME: the 1987 copy of gime.cpp, line 99 0 -> 1.
    # The copy must differ from the source in that one line and nowhere else.
    build_mame() {
        mkdir -p "$OUT/mame87" || return 1
        sed '99s/^#define GIME_TYPE_1987          0$/#define GIME_TYPE_1987          1/' \
            "$MG" > "$OUT/mame87/gime.cpp" || return 1
        if [ "$(diff "$MG" "$OUT/mame87/gime.cpp" | grep -c '^[<>]')" != 2 ] ||
           ! sed -n 99p "$OUT/mame87/gime.cpp" | grep -qx '#define GIME_TYPE_1987          1'; then
            echo "gime.cpp:99 is not '#define GIME_TYPE_1987          0'"
            return 1
        fi
        # mame_shim/ comes before $MAME_SRC/src/devices, so its machine/ram.h
        # and bus/coco/cococart.h replace MAME's; no emu/ directory is used.
        local MI=(-I"$D" -I"$D/mame_shim" -I"$MAME_SRC/src/mame/trs" -I"$MAME_SRC/src/devices")
        g++ -std=gnu++17 -O2 -w -DMAME_GIME_NS=mame86 "${MI[@]}" \
            -c "$D/mame_gime.cpp" -o "$OUT/mame86.o" || return 1
        g++ -std=gnu++17 -O2 -w -DMAME_GIME_NS=mame87 -I"$OUT/mame87" "${MI[@]}" \
            -c "$D/mame_gime.cpp" -o "$OUT/mame87.o" || return 1
    }
    if build_mame; then
        DEFS+=(-DHAVE_MAME)
        OBJS+=("$OUT/mame86.o" "$OUT/mame87.o" "$D/model_mame.cpp")
        ARGS+=(--mame-commit "$(git -C "$MAME_SRC" log -1 --format=%h 2>/dev/null || echo unknown)")
    else
        echo "NOTICE: mame oracle build failed; skipping"
    fi
else
    echo "NOTICE: mame source not found at $MAME_SRC; skipping"
fi

g++ -std=gnu++17 -O2 -Wall -Wno-unused-function -I"$D/stub" "${DEFS[@]}" -o "$OUT/gime_validate" \
    "$D/gime_validate.cpp" "$D/compare.cpp" "$D/scenario.cpp" "$D/model_ours.cpp" \
    "$D/scn_reset_regs.cpp" "$D/scn_mmu.cpp" "$D/scn_timer.cpp" "$D/scn_irq.cpp" "$D/scn_video.cpp" "$D/scn_render.cpp" "$D/scn_random.cpp" "${OBJS[@]}"
COMMIT=$(git -C "$D" rev-parse --short HEAD 2>/dev/null || echo unknown)
REPORT=$OUT/gime_validation_report.md
rm -f "$OUT/results.json"
# The runner's exit code is the suite's result; the report is written either way.
set +e
"$OUT/gime_validate" --verdicts "$D/verdicts.md" --baseline "$D/known_failures.txt" \
    --json "$OUT/results.json" --ours-commit "$COMMIT" "${ARGS[@]}" "$@"
RC=$?
set -e
# --selftest writes no results.json, so there is nothing to report on.
if [ -f "$OUT/results.json" ]; then
    if python3 -I "$D/make_report.py" "$OUT/results.json" "$D/verdicts.md" "$REPORT"; then
        echo "report: $REPORT"
    else
        echo "make_report.py failed"
        [ "$RC" -eq 0 ] && RC=2
    fi
fi
exit "$RC"
