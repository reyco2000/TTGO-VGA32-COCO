#!/bin/bash
# Build and run the host-side GIME validation suite.
# usage: tools/gime_validate/run.sh [--seed N] [--update-baseline] [--selftest]
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_validate
mkdir -p "$OUT"
g++ -std=gnu++17 -O2 -Wall -Wno-unused-function -o "$OUT/gime_validate" \
    "$D/gime_validate.cpp" "$D/compare.cpp" "$D/scenario.cpp" "$D/model_ours.cpp" \
    "$D/scn_reset_regs.cpp"
COMMIT=$(git -C "$D" rev-parse --short HEAD 2>/dev/null || echo unknown)
"$OUT/gime_validate" --verdicts "$D/verdicts.md" --baseline "$D/known_failures.txt" \
    --json "$OUT/results.json" --ours-commit "$COMMIT" "$@"
