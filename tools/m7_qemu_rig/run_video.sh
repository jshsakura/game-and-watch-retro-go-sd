#!/bin/bash
# MJPEG-AVI video player on QEMU's Cortex-M7 (mps2-an500): reproduce the
# progressive-slowdown bug deterministically by making the two clocks that
# cause it (video SysTick pacing vs. audio-PLL SAI drain) SEPARATE and visible.
#
#   bash tools/m7_qemu_rig/run_video.sh [AUDIO_PPM] [FRAMES] [extra -D...]
#
# AUDIO_PPM > 0 = the audio consume clock runs that many ppm SLOWER than the
# demuxer fills (ring-filling direction). 0 = the flat control. Links the REAL
# firmware TUs (video_play/avi/video_decode/video_audio + real minimp3) and
# drives the real video_play() loop; prints a per-frame ledger.
set -euo pipefail
cd "$(dirname "$0")/../.."

PPM="${1:-0}"
FRAMES="${2:-6000}"
EXTRA="${3:-}"

RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
# RESUME_HOST_STDIO: video_resume.c picks its delete/rename primitives by
# storage backend, and with SD_CARD=0 that is LittleFS, whose header this rig
# has no business carrying. The file already provides the host-stdio arm -- the
# same one tests/run.sh uses -- and only the primitives differ; which resume
# lines survive and when the store is dropped is the same code either way.
DEF="-DSD_CARD=0 -DRESUME_HOST_STDIO -DAUDIO_PPM=$PPM -DRIG_FRAMES=$FRAMES $EXTRA"
INC="-Itests/video_stubs -ICore/Inc/porting/video -ICore/Src/porting/lib -ICore/Inc/porting/music"

# Embedded MP3: one short CBR 48 kHz mono clip. Its frames (all 384 bytes,
# 1152 samples) are the AVI's audio chunks, cycled in order. Generated once.
if [ ! -f "$OUT/tone48.mp3" ]; then
    ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=8:sample_rate=48000" \
        -ac 1 -b:a 128k -write_xing 0 -id3v2_version 0 "$OUT/tone48.mp3"
fi
# objcopy derives _binary_tone48_mp3_{start,end} from the input basename, so run
# it from inside $OUT where the file is literally named tone48.mp3.
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm tone48.mp3 tone48_mp3.o)

SRCS_C="Core/Src/porting/video/video_play.c \
        Core/Src/porting/video/avi.c \
        Core/Src/porting/video/video_decode.c \
        Core/Src/porting/video/video_audio.c \
        Core/Src/porting/video/video_resume.c \
        Core/Src/porting/music/music_minimp3.c \
        $RIG/rig_runtime.c $RIG/rig_video.c"

OBJS=""
for s in $SRCS_C; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/tone48_mp3.o"

WRAP="-Wl,--wrap=fopen,--wrap=fread,--wrap=fseek,--wrap=ftell,--wrap=rewind,--wrap=fclose,--wrap=setvbuf"

$CC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    $WRAP $OBJS -lm -o "$OUT/rig_video.elf"

arm-none-eabi-size "$OUT/rig_video.elf"

# align=off: don't slave to wall clock; sleep=off: don't idle. icount shift=0.
timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_video.elf"
