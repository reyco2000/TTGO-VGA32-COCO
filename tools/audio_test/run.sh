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
# sound.cpp includes "../hal/hal.h": build it from a copy beside a stub HAL.
mkdir -p "$OUT/tree/core" "$OUT/tree/hal"
cp "$D/../../src/core/sound.cpp" "$D/../../src/core/sound.h" "$OUT/tree/core/"
cp "$D/stub/hal/hal.h" "$OUT/tree/hal/"
g++ -std=gnu++11 -O2 -Wall -Wextra -I"$D/stub" -I"$OUT/tree" "$D/sound_test.cpp" "$OUT/tree/core/sound.cpp" -o "$OUT/sound_test"
"$OUT/sound_test"
