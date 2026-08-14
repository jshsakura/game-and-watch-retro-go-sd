#!/usr/bin/env bash
# Host unit tests for the Music app's pure-logic modules (red-green)
# and the Clock app's alarm logic (fire/re-arm, window catch-up, cfg
# round-trip, tone DMA sync — stubs in tests/clock_stubs/).
set -e
cd "$(dirname "$0")/.."
INC="Core/Inc/porting/music"
SRC="Core/Src/porting/music"
CC="${CC:-gcc}"
FLAGS="-O2 -Wall -Wextra -std=c11 -I$INC"

# Nothing stale: every binary the run phase executes must come from THIS run's
# compile phase. /tmp/mtest survives between runs, so without this a test whose
# compile broke would silently re-run yesterday's binary and pass.
rm -rf /tmp/mtest
mkdir -p /tmp/mtest

# A failing check must NAME itself. This suite prints ~1900 lines in CI, and an
# `exit 1` with no summary once hid a real failure for a day: the CPS-1 romset
# gate said "STALE:", not "FAIL", so every grep anyone tried came back clean and
# the job looked like an infrastructure flake. A red gate nobody can read is a
# gate people learn to ignore. So: collect the names, print them at the end.
#
# Use `fail` and not a bare `rc=1` -- and use it as `cmd || fail NAME` rather
# than running `cmd` bare, because `set -e` above would otherwise abort the
# whole suite at the first failing harness and skip every check after it.
rc=0
FAILED=""
REACHED_END=0
fail() {
    rc=1
    FAILED="${FAILED}  - ${1}
"
}

# The summary must print however the suite ends. `cmd || fail NAME` keeps a
# failing CHECK from aborting the run, but a failing COMPILE is not wrapped
# (there are 40 of them, and wrapping each one invites a stale binary from a
# previous run being executed as if it were fresh). `set -e` therefore still
# kills the suite mid-file on a compile error -- and when it did, the summary
# below never ran, so the output ended on a raw cc1 error and named nothing.
# An EXIT trap prints it either way, and says plainly when the run stopped
# early instead of letting a truncated log look like a clean one.
summary() {
    if [ "$rc" != 0 ] || [ "$REACHED_END" = 0 ]; then
        echo
        echo "=== FAILED CHECKS ==="
        printf '%s' "$FAILED"
        [ "$REACHED_END" = 0 ] && echo "  - the suite STOPPED EARLY (a compile or a harness aborted it); the last error above is the cause, and every check after it never ran"
        echo "=== $(printf '%s' "$FAILED" | grep -c . ) named; everything else above passed or skipped ==="
    fi
}
trap summary EXIT

# Music-app tests run only where the music module exists (it arrived on
# feat/music-player and is on testbed/main now). Decided ONCE, here, because
# the compile block and the run block below both need the answer and a second
# copy of the condition is a second thing to forget.
#
# The three states are deliberate. This guard used to be a bare `-d` on
# Core/Src/porting/media, and when the module was renamed media_* -> music_*
# the directory stopped existing, so all five tests silently stopped running
# and the suite still said PASSED. Missing-because-absent and
# missing-because-moved look identical to `-d` and are not the same thing:
# only the first is a legitimate skip.
MUSIC_TESTS=0
if [ -f "$SRC/music_lyrics.c" ] && [ -f "$SRC/music_id3.c" ]; then
    MUSIC_TESTS=1
elif [ -d "$SRC" ] || [ -d "Core/Src/porting/media" ]; then
    # The module is here, but not under the names this file compiles. That is
    # rot, not absence -- say so and go red rather than skipping five tests.
    fail "music-app tests: module present but sources are not $SRC/music_{lyrics,id3}.c"
else
    echo "music module not on this branch — skipping music-app tests"
fi

echo "=== compile ==="
if [ "$MUSIC_TESTS" = 1 ]; then
    $CC $FLAGS tests/test_lyrics.c   "$SRC/music_lyrics.c" -o /tmp/mtest/test_lyrics
    $CC $FLAGS tests/test_id3.c      "$SRC/music_id3.c"    -o /tmp/mtest/test_id3
    $CC $FLAGS tests/test_ui_layout.c                        -o /tmp/mtest/test_ui_layout
    $CC $FLAGS tests/test_browser.c                          -o /tmp/mtest/test_browser
    $CC $FLAGS tests/test_color.c                            -o /tmp/mtest/test_color
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
        -ICore/Src/porting/lib -I$INC \
        tests/test_video_decode.c Core/Src/porting/video/video_decode.c \
        -o /tmp/mtest/test_video_decode

    # video_audio.c: the trim_step() clock-drift servo (see this dir's
    # CLAUDE.md, "Nothing synchronises the two clocks" -- the shipped "fine
    # for 4 minutes, then permanent stutter" bug). #includes video_audio.c
    # directly for its static servo state, same pattern rg_clock.c's tests use.
    $CC -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
        -I$INC \
        tests/test_video_audio.c $SRC/music_minimp3.c \
        -o /tmp/mtest/test_video_audio

    # video_play.c: the pf_step()/pf_fetch()/pf_reset() prefetch state machine
    # (jitter buffer + the audio-ring gate that jammed shut during the drift
    # bug). #includes video_play.c directly for its statics; links the REAL
    # avi.c/video_decode.c/video_audio.c it actually drives, not a reimplementation.
    $CC -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
        -ICore/Src/porting/lib -I$INC \
        -DRESUME_HOST_STDIO -DRESUME_PATH='"/tmp/mtest/video_resume_play.txt"' \
        tests/test_video_play.c Core/Src/porting/video/avi.c \
        Core/Src/porting/video/video_decode.c Core/Src/porting/video/video_audio.c \
        Core/Src/porting/video/video_resume.c \
        $SRC/music_minimp3.c \
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
# TamaPoke sound: links the REAL tamapoke_audio.cpp (the file the firmware
# compiles), because the frame loop calls fill() unconditionally and plays back
# whatever it returns -- nothing downstream can tell silence from a stuck effect.
# -x c++ because the source is C++ (enum Sfx : uint8_t).
$CC -O2 -Wall -std=gnu11 -x c++ -ICore/Inc/porting/tamapoke \
    tests/test_tamapoke_audio.c Core/Src/porting/tamapoke/tamapoke_audio.cpp \
    -lstdc++ -o /tmp/mtest/test_tamapoke_audio

# Video resume positions: pure FILE* logic, so the real video_resume.c links here.
# The store path is overridden to /tmp; on the device it is /data/video_resume.txt.
$CC -O2 -Wall -Wextra -std=gnu11 -ICore/Inc/porting/video \
    -DRESUME_HOST_STDIO -DRESUME_PATH='"/tmp/mtest/video_resume.txt"' \
    tests/test_video_resume.c Core/Src/porting/video/video_resume.c \
    -o /tmp/mtest/test_video_resume

mkdir -p /tmp/favtest
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/fav_stubs -ICore/Inc/retro-go -ICore/Inc -ICore/Inc/porting -Iretro-go-stm32/components/odroid \
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
# Super Metroid savestate: links the REAL external/sm/src/snes/ppu.c, with the
# device's defines. The PPU keeps its screen-enable registers twice — unpacked in
# layer[], packed in screenEnabled[] — and only the packed copies are what the
# compositor reads. They live past the end of the serialized range, so a load left
# them at whatever they were: zero, on the launcher's resume path, which boots the
# core and loads second. No layers enabled means every line comes out as backdrop:
# a black screen, at full framerate, on almost no CPU. Skipped where the submodule
# is not checked out (CI's host-tests job has no submodules) — honestly, and loudly.
if [ -f external/sm/src/snes/ppu.c ]; then
    $CC -O1 -g -std=gnu11 -w -DTARGET_GNW -DPPU_RGB565 \
        -iquote external/sm/src -iquote tests/sm_stubs \
        tests/test_sm_ppu_saveload.c tests/sm_stubs/sm_stubs.c external/sm/src/snes/ppu.c \
        -o /tmp/mtest/test_sm_ppu_saveload
else
    echo "SKIP  external/sm is not checked out — sm savestate test not built"
fi

# rc_dispatch heap-allocation gate: the REAL rc_dispatch.c (whole-file #include
# in the test, with malloc redirected to a counter). The old hash design
# OOM-crashed the device because the per-bank tables totaled ~85 KB on a 81 KB
# DTCM heap. This test fails if anyone adds a single malloc back. RED-verified:
# the counter mechanism catches the old code's per-bank allocations.
if [ -f external/sm/src/snes/rc_dispatch.c ]; then
    $CC -O2 -Wall -Wextra -std=c11 -Iexternal/sm/src/snes \
        tests/test_rc_dispatch_heap.c -o /tmp/mtest/test_rc_dispatch_heap
else
    echo "SKIP  external/sm is not checked out — rc_dispatch heap test not built"
fi

# System-grid layout: the REAL rg_system_grid_layout.c, which is dependency-free
# precisely so this links it instead of re-deriving its rules.
$CC -O2 -Wall -Wextra -std=gnu11 -ICore/Inc/retro-go \
    tests/test_system_grid.c Core/Src/retro-go/rg_system_grid_layout.c \
    -o /tmp/mtest/test_system_grid

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
    echo "FAIL test FatFs config drifted from the firmware's beyond FF_USE_MKFS"
    fail "test FatFs config drifted from the firmware's ffconf.h"
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
if [ "$MUSIC_TESTS" = 1 ]; then
    /tmp/mtest/test_lyrics      || fail test_lyrics
    /tmp/mtest/test_id3         || fail test_id3
    /tmp/mtest/test_ui_layout   || fail test_ui_layout
    /tmp/mtest/test_browser     || fail test_browser
    /tmp/mtest/test_color       || fail test_color
fi
[ -x /tmp/mtest/test_avi ]          && { /tmp/mtest/test_avi          || fail test_avi; }
[ -x /tmp/mtest/test_video_decode ] && { /tmp/mtest/test_video_decode || fail test_video_decode; }
[ -x /tmp/mtest/test_video_audio ]  && { /tmp/mtest/test_video_audio  || fail test_video_audio; }
[ -x /tmp/mtest/test_video_play ]   && { /tmp/mtest/test_video_play   || fail test_video_play; }
/tmp/mtest/test_clock_alarm || fail test_clock_alarm
/tmp/mtest/test_clock_gif   || fail test_clock_gif
/tmp/mtest/test_clock_more  || fail test_clock_more
/tmp/mtest/test_clock_sd0   || fail test_clock_sd0
/tmp/mtest/test_alarm       || fail test_alarm
/tmp/mtest/test_clock_mp3   || fail test_clock_mp3
/tmp/mtest/test_favorites   || fail test_favorites
/tmp/mtest/test_storage_sd1 || fail test_storage_sd1
/tmp/mtest/test_storage_sd0 || fail test_storage_sd0
/tmp/mtest/test_album      || fail test_album
/tmp/mtest/test_fw_tar     || fail test_fw_tar
/tmp/mtest/test_system_grid || fail test_system_grid
[ -x /tmp/mtest/test_sm_ppu_saveload ] && { /tmp/mtest/test_sm_ppu_saveload || fail test_sm_ppu_saveload; }
[ -x /tmp/mtest/test_rc_dispatch_heap ] && { /tmp/mtest/test_rc_dispatch_heap || fail test_rc_dispatch_heap; }

# === colour tab icons: stored bbox must match its array and fit its box ====
# gui_draw_color_icon() indexes data[] by bw*bh and blits at (ox,oy) inside the
# width x height footprint. A generator change that desyncs those reads past the
# end of the array, on a screen nobody looks at twice.
echo "=== colour icon bbox invariants ==="
python3 - <<'PYEOF3' || fail "colour icon bbox invariants"
import re, sys
# The colour icons live in rg_logos_fork.c since the 0722 split; the name
# headers stay in rg_logos.c. Read both, or this check silently passes on
# an empty set.
src = (open('Core/Src/retro-go/rg_logos.c').read()
       + open('Core/Src/retro-go/rg_logos_fork.c').read())
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
           Core/Src/retro-go/rg_logos.c Core/Src/retro-go/rg_logos_fork.c \
           | awk '{print $3}' | sort | uniq -d)
if [ -n "$dup_enum" ] || [ -n "$dup_blob" ]; then
    echo "FAIL duplicate logo — enum:[$dup_enum] blob:[$dup_blob]"
    fail "duplicate logo enum/blob (merge hygiene)"
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
/tmp/mtest/test_common || fail test_common

echo "=== md32x_border_clear.c: border-row clear survives frame-skip (32X overlay-close flicker) ==="
# common.c's generic clear_frames mechanism decrements once per LOOP
# ITERATION regardless of whether that iteration draws+swaps -- under 32X's
# frame-skip pacing, two skipped iterations right after menu close can both
# land on the same still-active physical buffer, leaving the other one's
# border rows stuck with the overlay's status-bar remnant forever (visible
# as the top/bottom bands flickering every other frame). Whole-file #include
# (tests/test_md32x_border_clear.c) reuses tests/common_stubs/gw_lcd.h's
# declarations with a 2-buffer fake.
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/common_stubs \
    tests/test_md32x_border_clear.c                      -o /tmp/mtest/test_md32x_border_clear
/tmp/mtest/test_md32x_border_clear || fail test_md32x_border_clear

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
/tmp/mtest/test_gw_malloc || fail test_gw_malloc

echo "=== crc32.c: known-vector pins ==="
$CC -O2 -Wall -Wextra -std=gnu11 -Itests/crc32_stubs -ICore/Inc/porting \
    tests/test_crc32.c Core/Src/porting/crc32.c            -o /tmp/mtest/test_crc32
/tmp/mtest/test_crc32 || fail test_crc32

echo "=== lz4_depack.c: round-trip + boundary cases ==="
$CC -O2 -Wall -Wextra -std=gnu11 -ICore/Src/porting/lib \
    tests/test_lz4_depack.c Core/Src/porting/lib/lz4_depack.c -o /tmp/mtest/test_lz4_depack
/tmp/mtest/test_lz4_depack || fail test_lz4_depack

# (The CPS-1 romset-table gate lived here until the CPS-1 core was removed
# from this branch (6a6c9729): the generated cps1_romset.c it checked no
# longer exists, so the gate could only fail. Generator + JSON live on in
# feat/cps1-container with the rest of the core.)

# === launcher: a dotless name (CPS-1 folder rom) must not 4GB-memcpy =========
# remove_extension() assumed every rom name had an extension; a CPS-1 game is a
# folder with no dot, so strrchr()==NULL made the copy length ~4 GB and the
# device bus-faulted while merely listing the system. Compiles the REAL function
# under ASan; RED-verified against the pre-fix version in git history.
echo "=== launcher: remove_extension on a dotless name ==="
bash tests/test_remove_extension.sh || fail tests/test_remove_extension.sh

# === safety nets must not be the thing that breaks the build =========
# Two CI jobs went red once not from a real defect but from the safety nets
# themselves: check_core_symbol_aliases.py crashing when nm wasn't on PATH,
# and this file's sm block dying when external/sm wasn't checked out. Both
# pinned below so that regresses instead of silently recurring.
echo "=== check_core_symbol_aliases: nm-missing / alias / clean-tree pins ==="
bash tests/test_check_core_symbol_aliases.sh || fail tests/test_check_core_symbol_aliases.sh

echo "=== tests/run.sh: sm parity SKIPS (not fails) without external/sm ==="
bash tests/test_sm_skip_guard.sh || fail tests/test_sm_skip_guard.sh

# /bios/logo.bin is indexed positionally by the RG_LOGO_* enum, and only the
# LINK ORDER of the LOGO_DATA structs backs that up -- a thing no compiler
# checks and which two upstream merges have quietly broken. The 0727 break was
# not cosmetic: an 18px header slot was served a 40px pad and the (then
# unclipped) blit wrote past the framebuffer, killing the launcher on the grid
# home. Runs the real gate against the real 0727 link order and the real
# shipped-broken header, so case (b) is RED against the actual bug.
echo "=== check_logo_index_alignment: enum vs link order, RED against the 0727 header ==="
bash tests/test_check_logo_index_alignment.sh || fail tests/test_check_logo_index_alignment.sh

# The release tar is rolled from sd_content/ wholesale, so a previous build's
# leftovers ship: 305 sprite files once, a deleted core's 1 MB payload later.
echo "=== check_sd_content_fresh: stale build output must block packaging ==="
bash tests/test_check_sd_content_fresh.sh || fail tests/test_check_sd_content_fresh.sh

# A C++ overlay whose .init_array leaks into the resident array makes
# __libc_init_array() call into RAM_EMU before main() -- before the LCD exists.
# Black screen, no BSOD, no rescue, OFW still boots, so it looks like anything
# but firmware. Shipped in 1501 and 1610 (TamaPoke) and in C64/Frodo before it.
echo "=== check_resident_init_array: no boot ctor may point into overlay RAM ==="
bash tests/test_check_resident_init_array.sh || fail tests/test_check_resident_init_array.sh

# .sdcard_logo is staging, not memory: its symbol addresses are extflash LOAD
# addresses objcopy'd out to /bios/logo.bin. rg_get_logo() returned one of them
# for the favorites wordmark and entering that tab faulted at 0x004c1d5e.
echo "=== tamapoke: the sprite packer never invents a palette index ==="
python3 tests/test_tamapoke_repack.py || fail tests/test_tamapoke_repack.py

echo "=== tamapoke: packer and blit agree on the transparent palette index ==="
bash tests/test_tamapoke_sprite_transparency.sh || fail tests/test_tamapoke_sprite_transparency.sh

# The save was in the ROOT of the card (every other homebrew asks the launcher for
# the path), and two of the launcher's five exit hooks were NULL, so quitting to
# the menu or holding POWER lost the pet. Both are call-site faults: the save code
# itself was fine, which is why only a wiring test can see them.
bash tests/test_tamapoke_save_wired.sh || fail tests/test_tamapoke_save_wired.sh

# ACT_* (behaviour states) and PMD_* (the ids a sprite pack is indexed by) are two
# enums, and the animation call takes the second. The code passed the first, so a
# sleeping pet walked sideways and feeding played walk-right. Both enums start at 0,
# so ACT_IDLE was right and it read as odd animation choices, not as a bug.
bash tests/test_tamapoke_pmd_actions.sh || fail tests/test_tamapoke_pmd_actions.sh

# The converter and the firmware disagreed about the thumbnail record for every
# release and neither side could see it: both were self-consistent. verify_assets_dat.py
# reads a built container from the FIRMWARE's side; this proves it actually rejects
# things (a truncated record, an overlapping one, a pack over the 124 KB slot, a pack
# with no PMD_IDLE, an out-of-range action id, an index that misses payload).
python3 tests/test_tamapoke_verify_assets.py || fail tests/test_tamapoke_verify_assets.py

# common_emu_input_loop() ACTS on the PAUSE shortcuts and arms an overlay; drawing it
# is a separate call every core makes between its render and lcd_swap(). TamaPoke
# called the first and not the second, so volume and brightness changed with nothing
# on screen -- reported as "the shortcut overlay is broken".
bash tests/test_ingame_overlay_wired.sh || fail tests/test_ingame_overlay_wired.sh

# The same flip, from the other side: g_common_drawn_frames counts lcd_swap(),
# so a core that skips its render and flips anyway reports every frame as drawn.
# Three do flip anyway on purpose and call lcd_swap_stale() instead. Neither that
# mistake nor the one it replaced (counting the guard's DECISION, which put VB in
# the ledger at 9 drawn fps while it presented 36) produces a compile error or a
# visible glitch -- only a number that looks like a measurement. So: a census.
bash tests/test_lcd_swap_audited.sh || fail tests/test_lcd_swap_audited.sh

# tools/gnw_probe/appid.sh labels a measurement window by reading APPID values
# out of the header instead of carrying a copy. Two properties with no symptom:
# it must not guess a value the header numbers implicitly (that is how a script
# comes to disagree with the compiler), and it must not fail its caller -- the
# first version exited 1 on an absent name, which under the probe scripts'
# `set -e -o pipefail` would kill a device run over a label.
bash tests/test_appid_value.sh || fail tests/test_appid_value.sh

/tmp/mtest/test_tamapoke_audio || fail tests/test_tamapoke_audio.c
/tmp/mtest/test_video_resume  || fail tests/test_video_resume.c

echo "=== check_no_resident_logo_refs: no unreadable logo address in flash ==="
bash tests/test_check_no_resident_logo_refs.sh || fail tests/test_check_no_resident_logo_refs.sh

# GBA: the flash-XIP split is a contract the compiler cannot check. cpu.o runs
# from ITCM, which the sentinel pass does not scan, so nothing cpu.o references
# may live in the blob — move one file across that line in the linker script and
# the build stays green while the device faults on the first frame. Counts the
# sentinels in the linked image. Skips (loudly) without an ELF or a toolchain,
# which is the host-tests CI job's situation.
echo "=== gba: nothing cpu.o references may live in the XIP blob ==="
bash tests/test_gba_xip_contract.sh || fail tests/test_gba_xip_contract.sh

# GBA: the M4A mixer HLE is wired — cpu.o checks the hook, main.o scans for the
# mixer, the six transliterations sit in the blob and their signatures in RAM.
# Drop -DGBA_M4A_HLE and everything still builds and boots, just 27-60% slower;
# this is the only thing in the tree that would say so. Same SKIP rules as the
# XIP contract above. RED-verified: a relink without the define fails it.
echo "=== gba: the M4A mixer HLE is wired, and each piece is on its side ==="
bash tests/test_gba_m4a_wired.sh || fail tests/test_gba_m4a_wired.sh

# GBA: the output low-pass — passband, stopband, bypass and stability, against
# tones, compiling the real gba_audio_filter.c. RED-verified: flipping a
# coefficient sign fails it.
echo "=== gba: the output low-pass does what its header promises ==="
$CC -O2 -Wall -Wextra -std=gnu11 \
    tests/test_gba_audio_filter.c Core/Src/porting/gba/gba_audio_filter.c \
    -lm -o /tmp/mtest/test_gba_audio_filter
/tmp/mtest/test_gba_audio_filter || fail test_gba_audio_filter

# GBA: load_gamepak() on a memory-mapped cart. Runs gpSP's real load path on the
# host BECAUSE the host traps what QEMU does not: an XIP build has no
# gamepak_buffers, and the code that read the cart through them scanned a megabyte
# from address 0. On an mps2-an500 that is mapped and the scan shrugged; on the
# device the first 64 KB is ITCM and the next byte is a bus fault, which is how
# Pokemon Ruby died. Page zero is unmapped here, so it is a SIGSEGV instead.
echo "=== gba: load_gamepak reads an XIP cart through the cart, not through NULL ==="
bash tools/gba_harness/run.sh || fail tools/gba_harness/run.sh

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
/tmp/mtest/test_sm_state_header || fail test_sm_state_header

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
    SM_SAVELOAD=1 bash tools/sm_harness/device_run.sh "$SM_ROM" 300 \
        || fail "sm: ppu cache invalidation on load (device_run.sh SM_SAVELOAD)"
    SM_SAVELOAD=1 SM_COLD_LOAD=1 bash tools/sm_harness/device_run.sh "$SM_ROM" 300 \
        || fail "sm: ppu cache invalidation on cold load (device_run.sh SM_COLD_LOAD)"
else
    echo "SKIP  needs external/sm checked out AND a ROM at \$SM_ROM (CI's host-tests job has neither)"
fi

echo "=== jpeg: hw_jpeg_decoder.c lock/callback/floor-to-4 regressions ==="
# Three real bugs shipped in hw_jpeg_decoder.c in three consecutive releases, each
# found only after the device died: a stuck HAL lock, HAL's end-of-input callback
# misread as an error, and HAL flooring InDataLength to a multiple of 4 (dropping
# the JPEG's trailing EOI). All three were pure state/arithmetic — no peripheral
# needed to reproduce them on a host. See tools/jpeg_harness/run.sh.
bash tools/jpeg_harness/run.sh || fail tools/jpeg_harness/run.sh

echo "=== idle power off: one setting, one rule, and every idle loop asks it ==="
bash tests/test_idle_timeout_wired.sh || fail tests/test_idle_timeout_wired.sh

echo "=== boot rescue: a bricked boot must end somewhere a person can act ==="
# A bad firmware hung the device dark with the power button dead — firmware
# reads that button, so a hang can only be escaped by a flat battery. The
# rescue counter/screen/power-off only work if every hook is CALLED (wiring),
# and the counter logic itself runs here against a fake backup register
# (compiling the real gw_boot_rescue.c, per the flash-cache lesson).
bash tests/test_boot_rescue_wired.sh || fail tests/test_boot_rescue_wired.sh
BR_DIR=/tmp/mtest/boot_rescue
rm -rf "$BR_DIR"; mkdir -p "$BR_DIR"
$CC -O1 -g -std=gnu11 -Wall $SAN -DBOOT_RESCUE_HOST_TEST \
    -Itests/boot_rescue_stubs -ICore/Inc \
    tests/test_boot_rescue.c Core/Src/gw_boot_rescue.c \
    -o "$BR_DIR/test_boot_rescue" || fail "compile test_boot_rescue"
"$BR_DIR/test_boot_rescue" || fail test_boot_rescue

echo "=== SNES audio stretcher: a slow frame must not become a silent gap ==="
# The core makes 266 samples per EMULATED frame; the DMA eats a buffer every
# 16.625 ms of REAL time. They only balance at 60 fps, and below it the old
# path zero-filled -- an audible click on every slow frame. This links the
# real module (not a copy of it) and checks the rate it settles at, that it
# never runs dry, and that it stays transparent at full speed.
$CC -O1 -g -std=gnu11 -Wall $SAN \
    tests/test_snes_audio_stretch.c Core/Src/porting/snes/snes_audio_stretch.c \
    -lm -o "$BR_DIR/test_snes_audio_stretch" || fail "compile test_snes_audio_stretch"
"$BR_DIR/test_snes_audio_stretch" || fail test_snes_audio_stretch

# Pacing-block integration test: compiles the ACTUAL pacing block from
# main_snes.c against the real stretcher, drives it at 20-60 fps, and checks
# that at most one emit lands per DMA period. RED (HEAD, buggy catch-up emit)
# must fail; GREEN (working tree, fix) must pass. A regression that
# reintroduces the extra emits will fail here on the host, not on the device.
# The harness script handles its own extraction, compilation, and RED/GREEN
# logic — it exits non-zero on any mismatch.
bash tests/test_snes_audio_pacing.sh || fail test_snes_audio_pacing

# ISR-pull architecture test: compares main-loop emit (current) vs ISR pull
# (new architecture) at 20-60 fps. The current architecture has a half-buffer
# resonance bug at integer-ratio fps (30/28/25): one DMA half is never written
# and plays stale content forever. The ISR-pull architecture fills every period
# regardless of main-loop timing. MODE_ISR_PULL must pass; MODE_MAIN_LOOP must
# show the resonance/stale bug. This is the RED/GREEN proof that the ISR-pull
# architecture overcomes the pacing fix's limitation.
$CC -O1 -g -std=gnu11 -Wall $SAN \
    tests/test_snes_audio_isr_pull.c Core/Src/porting/snes/snes_audio_stretch.c \
    -o "$BR_DIR/test_snes_audio_isr_pull" || fail "compile test_snes_audio_isr_pull"
"$BR_DIR/test_snes_audio_isr_pull" || fail test_snes_audio_isr_pull

echo "=== update guard: a truncated update file must not reach the flasher ==="
# The bootloader validates nothing; the firmware is the gate. The validator
# runs here against synthetic images in the release script's real layout,
# with truncation — the classic brick — as the headline case.
$CC -O1 -g -std=gnu11 -Wall $SAN -DUPDATE_GUARD_HOST_TEST \
    -ICore/Inc \
    tests/test_update_guard.c Core/Src/gw_update_guard.c \
    -o "$BR_DIR/test_update_guard" || fail "compile test_update_guard"
"$BR_DIR/test_update_guard" || fail test_update_guard

# Everything from here to EOF is re-run standalone by tests/test_sm_skip_guard.sh,
# which is why nothing that is expensive or self-contained belongs below this line.
# ---------------------------------------------------------------- flash cache --
# The ring every core's ROM goes through. It had no tests, and it holed Super
# Metroid's ROM: the launcher caches the ROM and hands the core its address, the
# core then caches its code blob, and once the ring has come round that write
# lands on the ROM being read. New Game was a black screen; Continue was fine.
#
# RED first, and against the real thing: the same test runs over the allocator as
# it was BEFORE the fix (git show), where it must fail. A test that has never
# failed proves nothing.
echo "=== flash cache: a write must not land on a file being read ==="
FA_DIR=/tmp/mtest/flash_alloc
rm -rf "$FA_DIR"; mkdir -p "$FA_DIR/saves"
FA_PREFIX_REV=e51ed278          # the allocator as it was, before the live set

$CC -O1 -g -std=gnu11 -Wall $SAN -Itests/flash_alloc_stubs -ICore/Inc \
    tests/test_flash_alloc.c tests/flash_alloc_stubs/flash_stubs.c \
    Core/Src/gw_flash_alloc.c -o "$FA_DIR/test_flash_alloc" \
    || fail "compile test_flash_alloc"

if git cat-file -e "$FA_PREFIX_REV:Core/Src/gw_flash_alloc.c" 2>/dev/null; then
    git show "$FA_PREFIX_REV:Core/Src/gw_flash_alloc.c" > "$FA_DIR/prefix.c"
    echo 'void flash_alloc_forget_live_files(void) {}' > "$FA_DIR/shim.c"
    $CC -O1 -g -std=gnu11 -w $SAN -Itests/flash_alloc_stubs -ICore/Inc \
        tests/test_flash_alloc.c tests/flash_alloc_stubs/flash_stubs.c \
        "$FA_DIR/prefix.c" "$FA_DIR/shim.c" -o "$FA_DIR/test_prefix" \
        || fail "compile test_flash_alloc against the pre-fix allocator"
    if ( cd "$FA_DIR" && ./test_prefix > /dev/null 2>&1 ); then
        echo "FAIL the pre-fix allocator passed - this test cannot see the bug it is for"
        fail "flash cache RED check: the pre-fix allocator passed"
    else
        echo "OK  the pre-fix allocator fails it, as the shipped bug did"
    fi
else
    # A shallow clone has no history to check against. Skip, and say so: a safety
    # net that fails the build when it cannot run teaches people to ignore CI.
    echo "SKIP no $FA_PREFIX_REV in this clone (shallow?) - RED check not run"
fi

( cd "$FA_DIR" && ./test_flash_alloc ) || fail test_flash_alloc

# CPS-1's OWN real shape (wof + wofj pooled, tools/cps1_romsets.json says 16
# distinct chips + the XIP blob = 17) exceeded MAX_LIVE_FILES=16 the moment
# subfolder pooling shipped -- one boot, no reboot in between, same disease as
# the ROM/blob test above but sized to CPS-1's actual numbers instead of two.
echo "=== flash cache: CPS-1's real wof+wofj pool (17 live files) must fit MAX_LIVE_FILES ==="
FAC_DIR=/tmp/mtest/flash_alloc_cps1
rm -rf "$FAC_DIR"; mkdir -p "$FAC_DIR/saves"
FAC_PREFIX_REV=4ddbe382        # MAX_LIVE_FILES=16, before the cps1-shaped bump

$CC -O1 -g -std=gnu11 -w $SAN -Itests/flash_alloc_stubs -ICore/Inc \
    tests/test_flash_alloc_cps1_pool.c tests/flash_alloc_stubs/flash_stubs.c \
    Core/Src/gw_flash_alloc.c -o "$FAC_DIR/test_flash_alloc_cps1_pool" || rc=1

if git cat-file -e "$FAC_PREFIX_REV:Core/Src/gw_flash_alloc.c" 2>/dev/null; then
    git show "$FAC_PREFIX_REV:Core/Src/gw_flash_alloc.c" > "$FAC_DIR/prefix.c"
    $CC -O1 -g -std=gnu11 -w $SAN -Itests/flash_alloc_stubs -ICore/Inc \
        tests/test_flash_alloc_cps1_pool.c tests/flash_alloc_stubs/flash_stubs.c \
        "$FAC_DIR/prefix.c" -o "$FAC_DIR/test_prefix" || rc=1
    if ( cd "$FAC_DIR" && ./test_prefix > /dev/null 2>&1 ); then
        echo "FAIL MAX_LIVE_FILES=16 passed this - it cannot see the bug it is for"
        rc=1
    else
        echo "OK  MAX_LIVE_FILES=16 fails it: the 17th (unprotected) file gets corrupted"
    fi
else
    echo "SKIP no $FAC_PREFIX_REV in this clone (shallow?) - RED check not run"
fi

( cd "$FAC_DIR" && ./test_flash_alloc_cps1_pool ) || rc=1

# external/sm's dma_doDma drains a whole A->B channel per call instead of one
# byte per dma_cycle() — a host-CPU win for VRAM/CGRAM/OAM uploads that must not
# change what a DMA transfers. Link the REAL dma.c and compare its transfer
# sequence against an independent per-byte reference over a spread of configs.
echo "=== sm dma: bulk A->B transfer matches per-byte reference ==="
DMA_DIR=/tmp/mtest/dma_bulk
rm -rf "$DMA_DIR"; mkdir -p "$DMA_DIR"
if [ -f external/sm/src/snes/dma.c ]; then
    $CC -O1 -g -std=gnu11 -Wall $SAN -Iexternal/sm/src/snes -Iexternal/sm \
        tests/test_dma_bulk_equiv.c external/sm/src/snes/dma.c \
        -o "$DMA_DIR/test_dma_bulk" || rc=1
    ( cd "$DMA_DIR" && ./test_dma_bulk ) || rc=1
else
    # A submodule that was not checked out (e.g. a partial clone) cannot be
    # tested. Skip and say so rather than fail the build over a missing source.
    echo "SKIP external/sm not checked out — dma transfer test not run"
fi

echo "=== sm: device source set is symbol-complete ==="
# tests/test_sm_skip_guard.sh re-runs everything from the marker above to EOF as
# a standalone script, so the reporting helpers must also exist in that slice.
# In a full run `fail` is already defined and this is a no-op.
type fail >/dev/null 2>&1 || { rc=0; FAILED=""; fail() { rc=1; FAILED="${FAILED}  - ${1}
"; }; }
# The main sm harness compiles all of external/sm, including sm_cpu_infra.c, which
# defines and sets g_snes. The device does not compile that file. That gap let
# three builds ship in which the linker bound sm's SNES bus to Super Mario World's
# g_snes — the harness ran 4,000 clean frames while the device died on its first
# register read. Link exactly what the device links, and nothing else.
#
# CI's host-tests job checks out the repo without submodules, so external/sm is an
# empty directory there. Skipping is honest; pretending to have checked is not.
if [ -f external/sm/src/sm_rtl.c ]; then
    bash tools/sm_harness/device_parity.sh \
        || fail tools/sm_harness/device_parity.sh
else
    echo "SKIP  external/sm is not checked out (no submodules in this job)"
fi

# === i18n: the generator must see every lang_t string field ==================
# lang_t is indexed BY POSITION in /lang/*.bin. A field the generator fails to
# parse is not a missing string — it shifts every index after it, and the .bin
# ships one language's labels in another's slots. That is exactly what happened:
# rg_i18n_lang.h had `// ... an older /lang/*.bin` — a "/*" inside a LINE comment
# — and the block-comment stripper, being a regex, took it as an opener. It sat
# harmless for months because no "*/" followed it. The day someone added a block
# comment lower down, it ate 50 fields; the build stayed green and the tool
# printed "0 missing". Nothing but this check would have caught it.
echo "=== i18n: every lang_t field reaches the generator ==="
python3 - <<'PYEOF4' || fail "i18n: lang_t fields reaching the generator"
import importlib.util, re, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location('g', 'tools/gen_i18n_bin.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
hdr = Path('Core/Inc/retro-go/rg_i18n_lang.h')
declared = re.findall(r'^\s*const\s+char\s*\*\s*s_(\w+)\s*;', hdr.read_text(encoding='utf-8'), re.M)
try:
    parsed = g.parse_header_field_order(hdr)
except SystemExit as e:
    print(f"FAIL generator refuses the header: {e}"); sys.exit(1)
if len(parsed) != len(declared):
    print(f"FAIL generator sees {len(parsed)} of {len(declared)} lang_t fields"); sys.exit(1)
# And the trap itself: no "/*" may hide inside a "//" comment in the header.
for i, line in enumerate(hdr.read_text(encoding='utf-8').splitlines(), 1):
    if re.search(r'//.*/\*', line):
        print(f"FAIL {hdr}:{i} has '/*' inside a '//' comment — this is the landmine"); sys.exit(1)
print(f"OK  all {len(declared)} lang_t string fields parse; no '/*' hidden in a '//' comment")
PYEOF4

# === system grid: wired into retro_loop, and owning no loop of its own ======
# Both halves of this matter, and neither is a unit test:
#  - A screen that runs its own while(1) has to re-ask every rule the launcher
#    loop already asks (idle power-off, watchdog, the due-alarm poll). The clock
#    app did exactly that, forgot odroid_idle_timeout_expired(), and sat lit for
#    ever at any setting. The grid is a MODE of retro_loop(), so it inherits them
#    — and this guard is what keeps it that way.
#  - A perfectly correct screen nobody calls is the other half of the same bug.
echo "=== system grid wiring ==="
grid_bad=0
if grep -qE 'odroid_input_read_gamepad|while *\( *(1|true) *\)' Core/Src/retro-go/rg_system_grid.c; then
    echo "FAIL rg_system_grid.c grew a loop/input read of its own — it must stay a"
    echo "     mode of retro_loop(), or it has to re-ask idle-timeout + watchdog itself"
    grid_bad=1
fi
for sym in rg_system_grid_open rg_system_grid_close rg_system_grid_commit \
           rg_system_grid_step rg_system_grid_is_open; do
    grep -q "$sym" Core/Src/retro-go/rg_main.c || {
        echo "FAIL retro_loop() never calls $sym — the grid is unreachable"; grid_bad=1; }
done
grep -q 'rg_system_grid_draw' Core/Src/retro-go/gui.c || {
    echo "FAIL gui_redraw_callback() never draws the grid"; grid_bad=1; }
# A fresh boot lands on the grid; quitting a game lands back in the ROM list you
# were browsing. Both halves are the same one-line condition, so pin it.
grep -q 'retro_loop(boot_mode != BOOT_MODE_HOT)' Core/Src/retro-go/rg_main.c || {
    echo "FAIL a fresh boot no longer opens the grid — or quitting a game now does"
    grid_bad=1; }
# B is the back key now; ROM info/delete must still be reachable from the A menu.
grep -q 'emulator_show_file_info' Core/Src/retro-go/rg_emulators.c || {
    echo "FAIL ROM info/delete became unreachable when B stopped opening it"; grid_bad=1; }
if [ "$grid_bad" = 0 ]; then
    echo "OK  grid is a mode of retro_loop(), reachable, drawn; ROM info still reachable"
else
    fail "system grid wiring"
fi

REACHED_END=1
exit $rc
