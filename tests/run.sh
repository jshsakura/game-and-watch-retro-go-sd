#!/usr/bin/env bash
# Host unit tests for the Music app's pure-logic modules (red-green)
# and the Clock app's alarm logic (fire/re-arm, window catch-up, cfg
# round-trip, tone DMA sync — stubs in tests/clock_stubs/).
set -e
cd "$(dirname "$0")/.."
INC="Core/Inc/porting/media"
SRC="Core/Src/porting/media"
CC="${CC:-gcc}"
FLAGS="-O2 -Wall -Wextra -std=c11 -I$INC"

mkdir -p /tmp/mtest
rc=0
echo "=== compile ==="
# Music-app tests only where the media module exists (feat/music-player)
if [ -d "$SRC" ]; then
    $CC $FLAGS tests/test_lyrics.c   "$SRC/media_lyrics.c" -o /tmp/mtest/test_lyrics
    $CC $FLAGS tests/test_id3.c      "$SRC/media_id3.c"    -o /tmp/mtest/test_id3
    $CC $FLAGS tests/test_ui_layout.c                        -o /tmp/mtest/test_ui_layout
    $CC $FLAGS tests/test_browser.c                          -o /tmp/mtest/test_browser
    $CC $FLAGS tests/test_color.c                            -o /tmp/mtest/test_color
else
    echo "media module not on this branch — skipping music-app tests"
fi
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/clock_stubs \
    tests/test_clock_alarm.c                             -o /tmp/mtest/test_clock_alarm
$CC -O2 -Wall -std=gnu11 -Itests/clock_stubs -ICore/Src/porting/lib/gifdec \
    tests/test_clock_gif.c                               -o /tmp/mtest/test_clock_gif
python3 - <<'PYEOF2'
from PIL import Image, ImageDraw
frames = []
for i in range(8):
    im = Image.new('RGB', (320, 240), (10 + i*5, 20, 60))
    d = ImageDraw.Draw(im)
    d.rectangle([20 + i*30, 60, 120 + i*30, 180], fill=(255, 160 - i*10, 40))
    frames.append(im.convert('P', palette=Image.ADAPTIVE, colors=128))
frames[0].save('/tmp/mtest/bg.gif', save_all=True, append_images=frames[1:], duration=100, loop=0)
PYEOF2

echo "=== run ==="
if [ -d "$SRC" ]; then
    /tmp/mtest/test_lyrics      || rc=1
    /tmp/mtest/test_id3         || rc=1
    /tmp/mtest/test_ui_layout   || rc=1
    /tmp/mtest/test_browser     || rc=1
    /tmp/mtest/test_color       || rc=1
fi
/tmp/mtest/test_clock_alarm || rc=1
/tmp/mtest/test_clock_gif   || rc=1
exit $rc
