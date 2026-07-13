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

    # video_decode.c: jpeg_dims() SOF-marker walk (reached only through the
    # public video_decode_slot(), it's static) + the g_scratch slot-layout
    # pin. Only hardware seam stubbed is the HW JPEG peripheral itself.
    $CC -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
        -ICore/Src/porting/lib -ICore/Inc/porting/music \
        tests/test_video_decode.c Core/Src/porting/video/video_decode.c \
        -o /tmp/mtest/test_video_decode

    # video_audio.c: the trim_step() clock-drift servo (see this dir's
    # CLAUDE.md, "Nothing synchronises the two clocks" -- the shipped "fine
    # for 4 minutes, then permanent stutter" bug). #includes video_audio.c
    # directly for its static servo state, same pattern rg_clock.c's tests use.
    $CC -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
        -ICore/Inc/porting/music \
        tests/test_video_audio.c Core/Src/porting/music/music_minimp3.c \
        -o /tmp/mtest/test_video_audio

    # video_play.c: the pf_step()/pf_fetch()/pf_reset() prefetch state machine
    # (jitter buffer + the audio-ring gate that jammed shut during the drift
    # bug). #includes video_play.c directly for its statics; links the REAL
    # avi.c/video_decode.c/video_audio.c it actually drives, not a reimplementation.
    $CC -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
        -ICore/Src/porting/lib -ICore/Inc/porting/music \
        tests/test_video_play.c Core/Src/porting/video/avi.c \
        Core/Src/porting/video/video_decode.c Core/Src/porting/video/video_audio.c \
        Core/Src/porting/music/music_minimp3.c \
        -o /tmp/mtest/test_video_play

    # Real MP3 fixture the trim_step()/audio-ring-gate tests above decode
    # through the REAL minimp3 (servo behaviour depends on real frame sizes,
    # not synthetic bytes -- see test_video_audio.c's header comment for why).
    # CI's host-tests job runs on ubuntu-latest, which ships ffmpeg; if it's
    # missing anyway, both tests print their own SKIP and still exit 0.
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=45" \
            -ar 44100 -ac 1 -b:a 128k /tmp/mtest/video_audio_test.mp3
    else
        echo "ffmpeg not found -- video_audio/video_play servo tests will SKIP their MP3-dependent checks"
    fi
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

# === firmware-update tar extractor ====================================
# The REAL external/firmware_update/Core/Src/tar.c against the REAL FatFs, on a
# RAM disk, under ASan+UBSan. This is the code that unpacks retro-go_update.bin
# onto the SD at boot; a silent failure here flashes a truncated image into
# internal flash. f_mkfs is needed to format the test volume, so the FatFs
# sources are copied and FF_USE_MKFS is flipped on — every other knob must match
# the firmware's ffconf.h, and the diff below proves it.
FF_SRC="external/firmware_update/Core/Src/porting/lib/FatFs"
FF_COPY="/tmp/mtest/fw_fatfs"
if [ ! -f "$FF_SRC/ff.c" ]; then
    echo "FAIL $FF_SRC is missing."
    echo "     run: git submodule update --init --depth 1 external/firmware_update"
    exit 1
fi
rm -rf "$FF_COPY" && mkdir -p "$FF_COPY"
cp "$FF_SRC"/ff.c "$FF_SRC"/ff.h "$FF_SRC"/ffconf.h "$FF_SRC"/ffunicode.c "$FF_SRC"/diskio.h "$FF_COPY"/
sed -i 's/^#define FF_USE_MKFS\t*0/#define FF_USE_MKFS\t1/' "$FF_COPY/ffconf.h"
if ! diff -q <(grep -v FF_USE_MKFS "$FF_SRC/ffconf.h") <(grep -v FF_USE_MKFS "$FF_COPY/ffconf.h") >/dev/null; then
    echo "FAIL test FatFs config drifted from the firmware's beyond FF_USE_MKFS"; rc=1
fi
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
# ChaN's FatFs is vendored third-party: build it warning-free, lint only our code.
$CC -O1 -g -std=gnu11 -w $SAN -I"$FF_COPY" -c "$FF_COPY/ff.c"        -o /tmp/mtest/fw_ff.o
$CC -O1 -g -std=gnu11 -w $SAN -I"$FF_COPY" -c "$FF_COPY/ffunicode.c" -o /tmp/mtest/fw_ffunicode.o
$CC -O1 -g -std=gnu11 -Wall -Wextra $SAN \
    -I"$FF_COPY" -Itests/fw_tar_stubs -Iexternal/firmware_update/Core/Inc \
    tests/test_fw_tar.c tests/fw_tar_stubs/ram_diskio.c \
    external/firmware_update/Core/Src/tar.c /tmp/mtest/fw_ff.o /tmp/mtest/fw_ffunicode.o \
    -o /tmp/mtest/test_fw_tar
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
[ -x /tmp/mtest/test_avi ]          && { /tmp/mtest/test_avi          || rc=1; }
[ -x /tmp/mtest/test_video_decode ] && { /tmp/mtest/test_video_decode || rc=1; }
[ -x /tmp/mtest/test_video_audio ]  && { /tmp/mtest/test_video_audio  || rc=1; }
[ -x /tmp/mtest/test_video_play ]   && { /tmp/mtest/test_video_play   || rc=1; }
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
/tmp/mtest/test_fw_tar     || rc=1

# === colour tab icons: stored bbox must match its array and fit its box ====
# gui_draw_color_icon() indexes data[] by bw*bh and blits at (ox,oy) inside the
# width x height footprint. A generator change that desyncs those reads past the
# end of the array, on a screen nobody looks at twice.
echo "=== colour icon bbox invariants ==="
python3 - <<'PYEOF3' || rc=1
import re, sys
src = open('Core/Src/retro-go/rg_logos.c').read()
structs = re.findall(
    r'const color_icon_t (cicon_\w+) = \{ (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), \w+, (\w+) \};', src)
if not structs:
    print("FAIL no cicon_* structs parsed — did the generator format change?"); sys.exit(1)
bad = 0
for var, w, h, ox, oy, bw, bh, dname in structs:
    w, h, ox, oy, bw, bh = map(int, (w, h, ox, oy, bw, bh))
    m = re.search(r'static const uint8_t %s\[(\d+)\] = ' % dname, src)
    n = int(m.group(1))
    if n != (bw * bh + 1) // 2:
        print(f"FAIL {var}: data[{n}] but bw*bh={bw*bh} needs {(bw*bh+1)//2}"); bad += 1
    if ox + bw > w or oy + bh > h:
        print(f"FAIL {var}: bbox {bw}x{bh}+{ox}+{oy} escapes the {w}x{h} footprint"); bad += 1
print(f"OK  {len(structs)} colour icons: data length and bbox both consistent" if not bad else "")
sys.exit(1 if bad else 0)
PYEOF3

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

echo "=== common.c: frame integrator clamp / skip_frames thresholds / speedup table / sound_sync ==="
# common_emu_frame_loop() is the shared per-core pacing loop; the Super Metroid
# port never called it at all (root CLAUDE.md), which starved it of pacing,
# frameskip and a speedup toggle. That's a different bug (a call site, not
# this logic), but the pacing math itself had no test before this one. Whole-
# file #include (tests/test_common.c) so the file's static frame_integrator/
# skip_streak are reachable; stubs in tests/common_stubs/ cover only the
# hardware/menu seams this logic never touches.
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/common_stubs \
    tests/test_common.c                                  -o /tmp/mtest/test_common
/tmp/mtest/test_common || rc=1

echo "=== gw_malloc.c: itc/ahb/ram bump allocators (alignment, ITC bounds, over-large fails) ==="
# The itc_malloc/ahb_malloc/ram_malloc/ram_calloc bump allocators behind "RAM
# priority = emulators first" (root CLAUDE.md). Linker symbols
# (__ITCMRAM_LENGTH__ etc.) are the SIZEOF-as-address trick gw_malloc.c reads
# by taking their &address, not their value -- given real values here with
# -Wl,--defsym, same technique test_clock_mp3.c already uses for the overlay
# SIZE symbols. ram_start is a real writable global, set directly by the test.
$CC -O2 -Wall -Wextra -std=gnu11 -no-pie \
    -Itests/gw_malloc_stubs -ICore/Inc \
    -Wl,--defsym=__RAM_EMU_END__=0x21000 \
    -Wl,--defsym=__ahbram_heap_start__=0x30000 \
    -Wl,--defsym=__ahbram_audio_start__=0x30100 \
    -Wl,--defsym=__itcram_start__=0x1000 \
    -Wl,--defsym=__itcram_end__=0x1010 \
    -Wl,--defsym=__ITCMRAM_LENGTH__=0x40 \
    -Wl,--defsym=__NULLPTR_LENGTH__=0x8 \
    tests/test_gw_malloc.c Core/Src/gw_malloc.c           -o /tmp/mtest/test_gw_malloc
/tmp/mtest/test_gw_malloc || rc=1

echo "=== crc32.c: known-vector pins ==="
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/crc32_stubs -ICore/Inc/porting \
    tests/test_crc32.c Core/Src/porting/crc32.c            -o /tmp/mtest/test_crc32
/tmp/mtest/test_crc32 || rc=1

echo "=== lz4_depack.c: round-trip + boundary cases ==="
$CC -O2 -Wall -Wextra -std=gnu11 -ICore/Src/porting/lib \
    tests/test_lz4_depack.c Core/Src/porting/lib/lz4_depack.c -o /tmp/mtest/test_lz4_depack
/tmp/mtest/test_lz4_depack || rc=1

# === safety nets must not be the thing that breaks the build =========
# Two CI jobs went red once not from a real defect but from the safety nets
# themselves: check_core_symbol_aliases.py crashing when nm wasn't on PATH,
# and this file's sm block dying when external/sm wasn't checked out. Both
# pinned below so that regresses instead of silently recurring.
echo "=== check_core_symbol_aliases: nm-missing / alias / clean-tree pins ==="
bash tests/test_check_core_symbol_aliases.sh || rc=1

echo "=== tests/run.sh: sm parity SKIPS (not fails) without external/sm ==="
bash tests/test_sm_skip_guard.sh || rc=1

echo "=== sm: savestate header refuses a foreign or truncated file ==="
# sm_system_SaveState/LoadState (main_sm.c) stamp every file with a magic/
# version/length header and refuse to load one that doesn't match — a
# savestate is a raw dump of live structs, and a file from a build whose
# structs have since moved would otherwise still open, still read to the
# end, and quietly restore nonsense (a black screen, no clue why). The
# check itself lives in sm_state_header.h, factored out of main_sm.c so it
# links here without the SNES core or the G&W HAL main_sm.c sits on top of.
#
# This block sits BEFORE "sm: device source set is symbol-complete" on
# purpose: test_sm_skip_guard.sh re-runs everything from that marker to EOF
# as its own standalone script to check the SKIP-without-external/sm path,
# and it expects that slice to run clean. Anything placed after the marker
# becomes part of what it exercises (and, since it has no $CC of its own,
# ${CC:-gcc} rather than $CC matters there too — belt and suspenders).
mkdir -p /tmp/mtest
${CC:-gcc} -O2 -Wall -Wextra -std=c11 -ICore/Inc/porting/sm \
    tests/test_sm_state_header.c -o /tmp/mtest/test_sm_state_header
/tmp/mtest/test_sm_state_header || rc=1

echo "=== sm: a load must invalidate the PPU's derived caches, not just restore state ==="
# ppu_saveload() (external/sm/src/snes/ppu.c) does not serialise palette565 or
# brightnessMult — they're caches derived from cgram+brightness, not state, and
# they live past where the save/load stream stops (offsetof(Ppu,
# pixelbuffer_placeholder)). A load restores cgram underneath them; unless
# something invalidates them, the screen renders the loaded scene in the
# colours of the one that was showing before the load.
#
# Runs the device's actual source set (tools/sm_harness/device_run.sh, built
# with -DTARGET_GNW same as the parity check above) with SM_SAVELOAD=1, which
# deliberately poisons those caches between save and load — see device_main.c
# for why it does that instead of driving the game elsewhere with input, which
# is what an earlier version of this harness tried and which did not catch
# this bug. SM_COLD_LOAD additionally covers the "PPU has rendered nothing
# yet" shape of the same bug (load right after boot, before any frame primed
# the caches).
#
# Needs both external/sm AND a local ROM fixture — CI's host-tests job has
# neither (no submodules, and this ROM is not something we can ship/commit).
SM_ROM="${SM_ROM:-/home/ubuntu/app/jupyterLab/notebooks/game-and-what/backend/data/library/public/_data/Super Metroid (Japan, USA) (En,Ja).sfc}"
if [ -f external/sm/src/sm_rtl.c ] && [ -f "$SM_ROM" ]; then
    SM_SAVELOAD=1 bash tools/sm_harness/device_run.sh "$SM_ROM" 300
    rc=$(( rc || $? ))
    SM_SAVELOAD=1 SM_COLD_LOAD=1 bash tools/sm_harness/device_run.sh "$SM_ROM" 300
    rc=$(( rc || $? ))
else
    echo "SKIP  needs external/sm checked out AND a ROM at \$SM_ROM (CI's host-tests job has neither)"
fi

echo "=== sm: device source set is symbol-complete ==="
# The main sm harness compiles all of external/sm, including sm_cpu_infra.c, which
# defines and sets g_snes. The device does not compile that file. That gap let
# three builds ship in which the linker bound sm's SNES bus to Super Mario World's
# g_snes — the harness ran 4,000 clean frames while the device died on its first
# register read. Link exactly what the device links, and nothing else.
#
# CI's host-tests job checks out the repo without submodules, so external/sm is an
# empty directory there. Skipping is honest; pretending to have checked is not.
if [ -f external/sm/src/sm_rtl.c ]; then
    bash tools/sm_harness/device_parity.sh
    rc=$(( rc || $? ))
else
    echo "SKIP  external/sm is not checked out (no submodules in this job)"
fi

echo "=== jpeg: hw_jpeg_decoder.c lock/callback/floor-to-4 regressions ==="
# Three real bugs shipped in hw_jpeg_decoder.c in three consecutive releases, each
# found only after the device died: a stuck HAL lock, HAL's end-of-input callback
# misread as an error, and HAL flooring InDataLength to a multiple of 4 (dropping
# the JPEG's trailing EOI). All three were pure state/arithmetic — no peripheral
# needed to reproduce them on a host. See tools/jpeg_harness/run.sh.
bash tools/jpeg_harness/run.sh
rc=$(( rc || $? ))

exit $rc
