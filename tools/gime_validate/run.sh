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
