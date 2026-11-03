#!/bin/bash
# Build and run the host-side tests of the sound path.
# usage: tools/audio_test/run.sh
set -e
D=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/audio_test
mkdir -p "$OUT"
g++ -std=gnu++11 -O2 -Wall -Wextra "$D/capture_test.cpp" -o "$OUT/capture_test"
"$OUT/capture_test"
g++ -std=gnu++11 -O2 -Wall -Wextra "$D/feed_test.cpp" -o "$OUT/feed_test"
"$OUT/feed_test"
