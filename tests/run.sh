#!/usr/bin/env bash
# Host unit tests for the Music app's pure-logic modules (red-green).
set -e
cd "$(dirname "$0")/.."
INC="Core/Inc/porting/music"
SRC="Core/Src/porting/music"
CC="${CC:-gcc}"
FLAGS="-O2 -Wall -Wextra -std=c11 -I$INC"

mkdir -p /tmp/mtest
echo "=== compile ==="
$CC $FLAGS tests/test_lyrics.c   "$SRC/music_lyrics.c" -o /tmp/mtest/test_lyrics
$CC $FLAGS tests/test_id3.c      "$SRC/music_id3.c"    -o /tmp/mtest/test_id3
$CC $FLAGS tests/test_ui_layout.c                        -o /tmp/mtest/test_ui_layout
$CC $FLAGS tests/test_browser.c                          -o /tmp/mtest/test_browser
$CC $FLAGS tests/test_color.c                            -o /tmp/mtest/test_color

rc=0
echo "=== run ==="
/tmp/mtest/test_lyrics    || rc=1
/tmp/mtest/test_id3       || rc=1
/tmp/mtest/test_ui_layout || rc=1
/tmp/mtest/test_browser   || rc=1
/tmp/mtest/test_color     || rc=1
exit $rc
