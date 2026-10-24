#!/bin/bash
# Build and run the host-side GIME validation suite.
# usage: tools/gime_validate/run.sh [--seed N]
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_validate
mkdir -p "$OUT"
g++ -std=gnu++17 -O2 -Wall -Wno-unused-function -o "$OUT/gime_validate" \
    "$D/gime_validate.cpp" "$D/scenario.cpp" "$D/model_ours.cpp" "$D/scn_reset_regs.cpp"
"$OUT/gime_validate" "$@"
