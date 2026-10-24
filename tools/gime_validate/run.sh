#!/bin/bash
# Build and run the host-side GIME validation suite.
# usage: tools/gime_validate/run.sh [--seed N] [--update-baseline] [--selftest]
#
# XROAR_SRC (default ~/proyectos/coco-emu-other-projects-source/xroar) is an
# XRoar source tree; its tcc1014.c is built unmodified as the XRoar oracle.
# A missing tree prints a NOTICE and that model's checks are SKIPPED.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_validate
mkdir -p "$OUT"
XROAR_SRC=${XROAR_SRC:-$HOME/proyectos/coco-emu-other-projects-source/xroar}

DEFS=()
OBJS=()
ARGS=()
if [ -f "$XROAR_SRC/src/tcc1014/tcc1014.c" ]; then
    XI=(-I"$XROAR_SRC" -I"$XROAR_SRC/src" -I"$XROAR_SRC/portalib" -I"$D")
    for src in "$D/xroar_gime.c" "$XROAR_SRC/src/events.c" \
               "$XROAR_SRC/src/tcc1014/font-gime.c" "$XROAR_SRC/portalib/delegate.c"; do
        gcc -std=gnu11 -O2 -w "${XI[@]}" -c "$src" -o "$OUT/xg_$(basename "$src" .c).o"
    done
    # One object holding the XRoar GIME (both variants are chosen at runtime).
    ld -r -o "$OUT/xroar_gime.o" "$OUT"/xg_*.o
    DEFS=(-DHAVE_XROAR)
    OBJS=("$OUT/xroar_gime.o" "$D/model_xroar.cpp")
    ARGS=(--xroar-commit "$(git -C "$XROAR_SRC" log -1 --format=%h 2>/dev/null || echo unknown)")
else
    echo "NOTICE: xroar source not found at $XROAR_SRC; skipping"
fi

g++ -std=gnu++17 -O2 -Wall -Wno-unused-function "${DEFS[@]}" -o "$OUT/gime_validate" \
    "$D/gime_validate.cpp" "$D/compare.cpp" "$D/scenario.cpp" "$D/model_ours.cpp" \
    "$D/scn_reset_regs.cpp" "${OBJS[@]}"
COMMIT=$(git -C "$D" rev-parse --short HEAD 2>/dev/null || echo unknown)
"$OUT/gime_validate" --verdicts "$D/verdicts.md" --baseline "$D/known_failures.txt" \
    --json "$OUT/results.json" --ours-commit "$COMMIT" "${ARGS[@]}" "$@"
