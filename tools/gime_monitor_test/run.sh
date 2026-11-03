#!/bin/bash
# Build and run the host test of the GIME monitor colour tables.
# usage: tools/gime_monitor_test/run.sh
set -e
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/gime_monitor_test
mkdir -p "$OUT"
g++ -std=gnu++11 -O2 -Wall -Wextra "$D/monitor_test.cpp" -o "$OUT/monitor_test"
"$OUT/monitor_test"
