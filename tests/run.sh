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
# Video-app AVI demuxer (pure FILE* logic — the read path the prefetch rd=/pf=
# HUD accounting sits on). Compiled only where the video source is present.
if [ -f "Core/Src/porting/video/avi.c" ]; then
    $CC $FLAGS -ICore/Inc/porting/video \
        tests/test_avi.c Core/Src/porting/video/avi.c        -o /tmp/mtest/test_avi
fi
# rg_clock.c is compiled as an SD-card build here (-DSD_CARD=1) so the alarm /
# GIF / photo / picker logic is all present; test_clock_sd0.c below builds the
# same source as a flash build (-DSD_CARD=0) to prove the media compile-out.
$CC -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=1 -Itests/clock_stubs \
    tests/test_clock_alarm.c                             -o /tmp/mtest/test_clock_alarm
$CC -O2 -Wall -std=gnu11 -Itests/clock_stubs -ICore/Src/porting/lib/gifdec \
    tests/test_clock_gif.c                               -o /tmp/mtest/test_clock_gif
$CC -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=1 -Itests/clock_stubs \
    tests/test_clock_more.c                              -o /tmp/mtest/test_clock_more
$CC -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=0 -Itests/clock_stubs \
    tests/test_clock_sd0.c                               -o /tmp/mtest/test_clock_sd0
# All-state alarm PURE logic (next-alarm epoch math, wake-cause table, tone
# presets) — rg_alarm.c compiled with -DRG_ALARM_HOST so no HAL is needed.
$CC -O2 -Wall -Wextra -std=gnu11 -DRG_ALARM_HOST -ICore/Inc/retro-go \
    tests/test_alarm.c                                   -o /tmp/mtest/test_alarm
# MP3-alarm module: overlay SIZE linker symbols faked with --defsym (small, so the
# staging memset stays in-bounds); -no-pie keeps those absolute symbols addressable.
$CC -O2 -Wall -Wextra -std=gnu11 -no-pie -Itests/clock_stubs \
    -Wl,--defsym=_OVERLAY_MUSIC_BSS_SIZE=64 -Wl,--defsym=_OVERLAY_MUSIC_SIZE=64 \
    tests/test_clock_mp3.c                               -o /tmp/mtest/test_clock_mp3
mkdir -p /tmp/favtest
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/fav_stubs \
    -DFAVORITES_FILE='"/tmp/favtest/favorites.txt"' \
    -DFAVORITES_TMP='"/tmp/favtest/favorites.new"' \
    tests/test_favorites.c Core/Src/retro-go/rg_favorites.c -o /tmp/mtest/test_favorites
# rg_storage.c compiled twice: FatFs backend (SD_CARD=1) and FrogFS backend (SD_CARD=0)
$CC -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=1 -Itests/storage_stubs -ICore/Inc/retro-go \
    tests/test_storage.c Core/Src/retro-go/rg_storage.c \
    tests/storage_stubs/stubs.c tests/storage_stubs/fake_fatfs.c tests/storage_stubs/posix_dir.c \
    -o /tmp/mtest/test_storage_sd1
$CC -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=0 -Itests/storage_stubs -ICore/Inc/retro-go \
    tests/test_storage.c Core/Src/retro-go/rg_storage.c \
    tests/storage_stubs/stubs.c tests/storage_stubs/fake_frogfs.c \
    -o /tmp/mtest/test_storage_sd0
# clock photo-album loader: BMP header parse + BGR->565 + row flip + rejection
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/album_stubs -ICore/Inc/retro-go \
    tests/test_album.c tests/album_stubs/album_stub_impl.c Core/Src/retro-go/rg_clock_album.c \
    -o /tmp/mtest/test_album
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
[ -x /tmp/mtest/test_avi ] && { /tmp/mtest/test_avi || rc=1; }
/tmp/mtest/test_clock_alarm || rc=1
/tmp/mtest/test_clock_gif   || rc=1
/tmp/mtest/test_clock_more  || rc=1
/tmp/mtest/test_clock_sd0   || rc=1
/tmp/mtest/test_alarm       || rc=1
/tmp/mtest/test_clock_mp3   || rc=1
/tmp/mtest/test_favorites   || rc=1
/tmp/mtest/test_storage_sd1 || rc=1
/tmp/mtest/test_storage_sd0 || rc=1
/tmp/mtest/test_album      || rc=1

# === merge-hygiene guard (red-green) ==================================
# A merge that interleaves OUR logo additions with upstream's can silently
# duplicate a RG_LOGO_* enumerator (→ compile error) or a logo blob (→ link
# error). That is exactly what a stray upstream Lynx-logo re-add did once,
# and it only blew up 5 minutes into the ARM CI. Catch it HERE, on the host,
# so following upstream stays "pretty" and never confuses a downstream picker.
echo "=== merge hygiene: duplicate logo enums / blobs ==="
dup_enum=$(grep -oE 'RG_LOGO_[A-Z0-9_]+' Core/Inc/retro-go/bitmaps.h | sort | uniq -d)
dup_blob=$(grep -oE '^const retro_logo_image[[:space:]]+[a-z0-9_]+[[:space:]]+LOGO_DATA' \
           Core/Src/retro-go/rg_logos.c | awk '{print $3}' | sort | uniq -d)
if [ -n "$dup_enum" ] || [ -n "$dup_blob" ]; then
    echo "FAIL duplicate logo — enum:[$dup_enum] blob:[$dup_blob]"; rc=1
else
    echo "OK no duplicate logo enums/blobs"
fi

exit $rc
