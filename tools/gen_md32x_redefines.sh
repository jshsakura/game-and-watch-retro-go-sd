#!/bin/bash
# Regenerate md32x_redefines: every global the 32X (picodrive) overlay defines
# gets an md32x__ prefix so it can NEVER alias another core's symbols — most
# importantly gwenesis's Genesis core (md), which is NOT namespaced and shares
# names like z80/m68k/VDP/ym globals. Cross-overlay aliasing = silent corruption
# (the g_snes saga: 3 dead SM releases). check_core_symbol_aliases.py enforces
# this on every link. app_main_md32x stays unrenamed (the launcher entry point).
#
# Run after adding a source file to MD32X_C_SOURCES or a global to main_md32x.c.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
CC=arm-none-eabi-gcc
PD=external/picodrive
# Same defines/arch as the real build: interpreter-only 32X (no DRC), GNW guards.
CF="-mcpu=cortex-m7 -mthumb -O1 -c -DEMU_G68K -DTABLES_FULL -D_USE_CZ80 -DNDEBUG -DGNW_32X_CORE \
    -I$PD -I$PD/pico -I$PD/cpu -I$PD/cpu/fame -I$PD/zlib -w"

# The trimmed 32X interpreter source set (matches MD32X_C_SOURCES in Makefile).
SRCS="
cpu/sh2/sh2.c cpu/sh2/mame/sh2pico.c
cpu/gwenesis68k/m68kcpu.c cpu/gwenesis68k/g68k_bus.c cpu/cz80/cz80.c
pico/32x/32x.c pico/32x/draw.c pico/32x/memory.c pico/32x/pwm.c pico/32x/sh2soc.c
pico/cart.c pico/memory.c pico/draw.c pico/sek.c pico/videoport.c
pico/media.c pico/pico.c pico/misc.c pico/patch.c pico/z80if.c
pico/eeprom.c
pico/sound/sound.c pico/sound/mix.c pico/sound/sn76496.c pico/sound/ym2612.c
pico/sound/resampler.c
"
for s in $SRCS; do
  o="$T/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
  $CC $CF "$PD/$s" -o "$o"
done

{
  arm-none-eabi-nm -g --defined-only "$T"/*.o | awk '{print $3}' | grep -v '^$'
} | sort -u | grep -v '^app_main_md32x$' | awk '{print $1" md32x__"$1}' > md32x_redefines
rm -rf "$T"
echo "md32x_redefines: $(wc -l < md32x_redefines) symbols"
