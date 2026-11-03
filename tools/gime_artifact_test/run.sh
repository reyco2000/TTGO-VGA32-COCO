#!/bin/bash
# Build and run the host test of the PMODE 4 artifact colours.
# usage: tools/gime_artifact_test/run.sh
set -e
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_artifact_test
mkdir -p "$OUT"
python3 "$D/golden.py" | cmp -s - "$D/golden.h" || { echo "golden.h is stale: python3 golden.py > golden.h"; exit 1; }
g++ -std=gnu++11 -O2 -Wall -Wextra "$D/artifact_test.cpp" -o "$OUT/artifact_test"
"$OUT/artifact_test"
