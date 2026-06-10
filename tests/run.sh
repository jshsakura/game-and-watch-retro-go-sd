#!/usr/bin/env bash
# Host unit tests for the Music app's pure-logic modules (red-green).
set -e
cd "$(dirname "$0")/.."
INC="Core/Inc/porting/media"
SRC="Core/Src/porting/media"
CC="${CC:-gcc}"
FLAGS="-O2 -Wall -Wextra -std=c11 -I$INC"

mkdir -p /tmp/mtest
$CC $FLAGS tests/test_lyrics.c "$SRC/media_lyrics.c" -o /tmp/mtest/test_lyrics
$CC $FLAGS tests/test_id3.c    "$SRC/media_id3.c"    -o /tmp/mtest/test_id3

rc=0
/tmp/mtest/test_lyrics || rc=1
/tmp/mtest/test_id3    || rc=1
exit $rc
