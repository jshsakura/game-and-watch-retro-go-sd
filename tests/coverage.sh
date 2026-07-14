#!/usr/bin/env bash
# Host line/branch coverage for the modules listed MEASURED in
# tests/coverage_scope.txt, gathered by recompiling the exact same sources
# tests/run.sh already tests — same #includes, same stub dirs, same -D flags
# — with --coverage instead of tests/run.sh's -O2, then running the same
# binaries and reducing gcov's output through tests/coverage_report.py.
#
# Why not just add --coverage to tests/run.sh: coverage wants -O0 (line
# attribution gets fuzzy once the optimizer merges/hoists lines, and this
# script's aggregation assumes one gcno per translation unit per binary).
# tests/run.sh stays the CI correctness gate at -O2; this is a separate,
# slower, instrumented rebuild of the same inputs, for measurement only.
#
# CI's host-tests job (.github/workflows/package.yml) checks out WITHOUT
# submodules and has no ARM toolchain. This script needs neither for anything
# in tests/run.sh except external/firmware_update, which that job fetches on
# its own for exactly this test. If it's missing here, skip with a message —
# never fail the coverage run over an absent submodule.
set -u
cd "$(dirname "$0")/.."
CC="${CC:-gcc}"
COV="--coverage -O0 -g -std=gnu11 -w"
BUILD=/tmp/covbuild
GCOV_OUT="$BUILD/_gcov"
rc=0

rm -rf "$BUILD"
mkdir -p "$BUILD" "$GCOV_OUT" /tmp/mtest /tmp/favtest

# build_and_run NAME BINNAME -- <compiler args...>
# Compiles into $BUILD/NAME/BINNAME with $COV, runs it from that directory (so
# its .gcno/.gcda land next to it), and does not treat a nonzero test exit as
# fatal to the coverage run — a red test still ran the lines it ran.
build_and_run() {
    local name="$1" bin="$2"; shift 2
    [ "$1" = "--" ] && shift
    local dir="$BUILD/$name"
    mkdir -p "$dir"
    if ! $CC $COV "$@" -o "$dir/$bin" 2>"$dir/build.log"; then
        echo "SKIP  $name: build failed — see $dir/build.log"
        rc=1
        return 1
    fi
    ( cd "$dir" && "./$bin" >run.log 2>&1 )
    local ec=$?
    [ $ec -ne 0 ] && echo "note  $name: test binary exited $ec (coverage collected up to that point)"
    return 0
}

echo "=== compiling instrumented builds ==="

# --- rg_alarm.c: test_alarm.c #includes it, -DRG_ALARM_HOST compiles out the
# firmware half (see rg_alarm.h) so no HAL stubbing is needed. ---
build_and_run alarm test_alarm \
    -- -DRG_ALARM_HOST -ICore/Inc/retro-go tests/test_alarm.c

# --- rg_clock.c: three binaries exercise three different configurations
# (alarm/GIF/photo picker at SD_CARD=1, the flash-build compile-out at
# SD_CARD=0); union their coverage in the report, don't just take one. ---
build_and_run clock_alarm test_clock_alarm \
    -- -DSD_CARD=1 -Itests/clock_stubs tests/test_clock_alarm.c
build_and_run clock_more test_clock_more \
    -- -DSD_CARD=1 -Itests/clock_stubs tests/test_clock_more.c
build_and_run clock_sd0 test_clock_sd0 \
    -- -DSD_CARD=0 -Itests/clock_stubs tests/test_clock_sd0.c

# --- rg_clock_gif.c + gifdec.c: needs a real GIF fixture at a path the stub
# hardcodes (tests/clock_stubs redirects "/clock/gif/bg.gif" to
# /tmp/mtest/bg.gif — not parameterised, so it must land exactly there). ---
python3 - <<'PYEOF'
from PIL import Image, ImageDraw
frames = []
for i in range(8):
    im = Image.new('RGB', (320, 240), (10 + i*5, 20, 60))
    d = ImageDraw.Draw(im)
    d.rectangle([20 + i*30, 60, 120 + i*30, 180], fill=(255, 160 - i*10, 40))
    frames.append(im.convert('P', palette=Image.ADAPTIVE, colors=128))
frames[0].save('/tmp/mtest/bg.gif', save_all=True, append_images=frames[1:], duration=100, loop=0)
PYEOF
build_and_run clock_gif test_clock_gif \
    -- -Itests/clock_stubs -ICore/Src/porting/lib/gifdec tests/test_clock_gif.c

# --- rg_clock_alarm_mp3.c: test_clock_mp3.c #includes it; overlay SIZE
# linker symbols are faked with --defsym, -no-pie keeps them addressable. ---
build_and_run clock_mp3 test_clock_mp3 \
    -- -no-pie -Itests/clock_stubs \
       -Wl,--defsym=_OVERLAY_MUSIC_BSS_SIZE=64 -Wl,--defsym=_OVERLAY_MUSIC_SIZE=64 \
       tests/test_clock_mp3.c

# --- rg_favorites.c ---
build_and_run favorites test_favorites \
    -- -Itests/fav_stubs \
       -DFAVORITES_FILE='"/tmp/favtest/favorites.txt"' \
       -DFAVORITES_TMP='"/tmp/favtest/favorites.new"' \
       tests/test_favorites.c Core/Src/retro-go/rg_favorites.c

# --- rg_storage.c, both backends ---
build_and_run storage_sd1 test_storage_sd1 \
    -- -DSD_CARD=1 -Itests/storage_stubs -ICore/Inc/retro-go \
       tests/test_storage.c Core/Src/retro-go/rg_storage.c \
       tests/storage_stubs/stubs.c tests/storage_stubs/fake_fatfs.c tests/storage_stubs/posix_dir.c
build_and_run storage_sd0 test_storage_sd0 \
    -- -DSD_CARD=0 -Itests/storage_stubs -ICore/Inc/retro-go \
       tests/test_storage.c Core/Src/retro-go/rg_storage.c \
       tests/storage_stubs/stubs.c tests/storage_stubs/fake_frogfs.c

# --- rg_clock_album.c ---
build_and_run album test_album \
    -- -Itests/album_stubs -ICore/Inc/retro-go \
       tests/test_album.c tests/album_stubs/album_stub_impl.c Core/Src/retro-go/rg_clock_album.c

# --- video: avi.c / video_decode.c / video_audio.c / video_play.c -- only
# present on branches that carry the video player. Mirrors tests/run.sh's
# compile commands exactly (same sources, same stub dirs, same -D flags). ---
if [ -f Core/Src/porting/video/avi.c ]; then
    build_and_run avi test_avi \
        -- -ICore/Inc/porting/video tests/test_avi.c Core/Src/porting/video/avi.c

    build_and_run video_decode test_video_decode \
        -- -Itests/video_stubs -ICore/Inc/porting/video -ICore/Src/porting/lib -ICore/Inc/porting/music \
           tests/test_video_decode.c Core/Src/porting/video/video_decode.c

    build_and_run video_audio test_video_audio \
        -- -Itests/video_stubs -ICore/Inc/porting/video -ICore/Inc/porting/music \
           tests/test_video_audio.c Core/Src/porting/music/music_minimp3.c

    build_and_run video_play test_video_play \
        -- -Itests/video_stubs -ICore/Inc/porting/video -ICore/Src/porting/lib -ICore/Inc/porting/music \
           tests/test_video_play.c Core/Src/porting/video/avi.c \
           Core/Src/porting/video/video_decode.c Core/Src/porting/video/video_audio.c \
           Core/Src/porting/music/music_minimp3.c

    # video_audio/video_play's servo tests need the same real MP3 fixture
    # tests/run.sh generates with ffmpeg; skip quietly (not fail) without it.
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=45" \
            -ar 44100 -ac 1 -b:a 128k /tmp/mtest/video_audio_test.mp3
    else
        echo "ffmpeg not found -- video_audio/video_play servo tests will SKIP their MP3-dependent checks"
    fi
else
    echo "SKIP  avi.c / video_decode.c / video_audio.c / video_play.c: not present on this branch"
fi

# --- external/firmware_update/Core/Src/tar.c: needs that one submodule. CI's
# host-tests job fetches exactly this one for exactly this test; if it is not
# here, skip rather than fail — this script does not manage submodules. ---
FW_TAR=external/firmware_update/Core/Src/tar.c
FF_SRC=external/firmware_update/Core/Src/porting/lib/FatFs
if [ -f "$FW_TAR" ] && [ -f "$FF_SRC/ff.c" ]; then
    dir="$BUILD/fw_tar"
    ffcopy="$dir/fatfs"
    mkdir -p "$dir" "$ffcopy"
    cp "$FF_SRC"/ff.c "$FF_SRC"/ff.h "$FF_SRC"/ffconf.h "$FF_SRC"/ffunicode.c "$FF_SRC"/diskio.h "$ffcopy"/
    sed -i 's/^#define FF_USE_MKFS\t*0/#define FF_USE_MKFS\t1/' "$ffcopy/ffconf.h"
    # FatFs itself is vendored third-party (see coverage_scope.txt EXCLUDED) —
    # build it plain, no --coverage, matching tests/run.sh's own "lint only
    # our code" stance on it.
    gcc -O1 -g -std=gnu11 -w -I"$ffcopy" -c "$ffcopy/ff.c"        -o "$dir/ff.o"
    gcc -O1 -g -std=gnu11 -w -I"$ffcopy" -c "$ffcopy/ffunicode.c" -o "$dir/ffunicode.o"
    build_and_run fw_tar test_fw_tar \
        -- -I"$ffcopy" -Itests/fw_tar_stubs -Iexternal/firmware_update/Core/Inc \
           tests/test_fw_tar.c tests/fw_tar_stubs/ram_diskio.c "$FW_TAR" \
           "$dir/ff.o" "$dir/ffunicode.o"
else
    echo "SKIP  external/firmware_update/Core/Src/tar.c: submodule not checked out here"
    echo "      run: git submodule update --init --depth 1 external/firmware_update"
fi

# --- Core/Src/gw_flash_alloc.c: the ring every core's ROM goes through. It was
# inside the blanket "Core/Src/gw_ = hardware bring-up" exclusion, and it is not:
# it is an allocator, pure logic, and it holed Super Metroid's ROM. Measured now.
build_and_run flash_alloc test_flash_alloc \
    -- -Itests/flash_alloc_stubs -ICore/Inc \
       tests/test_flash_alloc.c tests/flash_alloc_stubs/flash_stubs.c \
       Core/Src/gw_flash_alloc.c

# --- Core/Src/porting/lib/hw_jpeg_decoder.c: three binaries drive the REAL
# file through its three real entry points (JPEG_DecodeToFrameInit/ToFrame/
# ToBuffer) against a faithful ST HAL fake (tools/jpeg_harness/hal_fake/);
# union their coverage same as the multi-binary modules above. Only the
# GREEN (current-code) build is measured here — tools/jpeg_harness/run.sh's
# RED builds compile a DIFFERENT (historical, pre-fix) file, which is not
# the file this is measuring. ---
JPEG_INC="-Itools/jpeg_harness/hal_fake -ICore/Src/porting/lib"
JPEG_SRCS="Core/Src/porting/lib/hw_jpeg_decoder.c tools/jpeg_harness/hal_fake/hal_jpeg_fake.c"
build_and_run jpeg_lock test_jpeg_lock \
    -- -no-pie $JPEG_INC tools/jpeg_harness/lock_test.c $JPEG_SRCS
build_and_run jpeg_callback test_jpeg_callback \
    -- -no-pie $JPEG_INC tools/jpeg_harness/callback_test.c $JPEG_SRCS
build_and_run jpeg_floor test_jpeg_floor \
    -- -no-pie $JPEG_INC tools/jpeg_harness/floor_test.c $JPEG_SRCS
build_and_run jpeg_coverage test_jpeg_coverage \
    -- -no-pie $JPEG_INC tools/jpeg_harness/coverage_test.c $JPEG_SRCS

# --- Core/Src/porting/common.c: whole-file #include (like rg_clock.c above)
# so the file's static frame_integrator/skip_streak are reachable. ---
build_and_run common test_common \
    -- -Itests/common_stubs tests/test_common.c

# --- Core/Src/gw_malloc.c: linker's SIZEOF-as-address symbols given real
# values via --defsym, same technique as test_clock_mp3.c's overlay SIZE
# symbols above. ---
build_and_run gw_malloc test_gw_malloc \
    -- -no-pie -Itests/gw_malloc_stubs -ICore/Inc \
       -Wl,--defsym=__RAM_EMU_END__=0x21000 \
       -Wl,--defsym=__ahbram_heap_start__=0x30000 \
       -Wl,--defsym=__ahbram_audio_start__=0x30100 \
       -Wl,--defsym=__itcram_start__=0x1000 \
       -Wl,--defsym=__itcram_end__=0x1010 \
       -Wl,--defsym=__ITCMRAM_LENGTH__=0x40 \
       -Wl,--defsym=__NULLPTR_LENGTH__=0x8 \
       tests/test_gw_malloc.c Core/Src/gw_malloc.c

# --- Core/Src/porting/crc32.c ---
build_and_run crc32 test_crc32 \
    -- -Itests/crc32_stubs -ICore/Inc/porting tests/test_crc32.c Core/Src/porting/crc32.c

# --- Core/Src/porting/lib/lz4_depack.c: no stubbing needed. ---
build_and_run lz4_depack test_lz4_depack \
    -- -ICore/Src/porting/lib tests/test_lz4_depack.c Core/Src/porting/lib/lz4_depack.c

echo
echo "=== reducing gcov data ==="
n=0
while IFS= read -r -d '' gcda; do
    gdir=$(dirname "$gcda")
    if gcov -b --json-format -o "$gdir" "$gcda" >/dev/null 2>"$GCOV_OUT/gcov_err_$n.log"; then
        mv ./*.gcov.json.gz "$GCOV_OUT/" 2>/dev/null
    fi
    n=$((n + 1))
done < <(find "$BUILD" -name '*.gcda' -print0)
echo "  $n .gcda file(s) reduced"

echo
echo "=== coverage report ==="
python3 tests/coverage_report.py --scope tests/coverage_scope.txt --gcov-dir "$GCOV_OUT"
report_rc=$?

exit $(( rc || report_rc ))
