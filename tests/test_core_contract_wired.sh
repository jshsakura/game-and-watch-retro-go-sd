#!/bin/bash
# Every emulator core owes the shared machinery four calls. They are easy to
# forget because forgetting one does not crash: the core runs, and one whole
# feature is simply absent.
#
#   odroid_system_init()       assigns currentApp.id. Without it the per-app
#                              config slot is the launcher's, so every core
#                              option silently reads somebody else's bytes.
#                              (32X shipped that way; see
#                              tests/test_per_app_settings_wired.sh.)
#   odroid_system_emu_init()   registers the savestate callbacks. Without it
#                              save and load do nothing at all -- no error, no
#                              message. Super Metroid shipped three releases
#                              like this.
#   common_emu_frame_loop()    pacing, frameskip, speedup and the FPS counter.
#                              Super Metroid had none of them for the same
#                              three releases.
#   audio start                the SAI DMA never starts, so a core that submits
#                              audio into a stopped ring can busy-wait on a
#                              counter that never advances. There is more than
#                              one entry point for this, so the accepted names
#                              are DERIVED from Core/Src/gw_audio.c -- every
#                              function there that calls HAL_SAI_Transmit_DMA --
#                              rather than hardcoded. A hardcoded name made this
#                              gate's first run accuse pkmini, which starts the
#                              DMA through audio_start_playing_full_length().
#
# No unit test can catch a missing call: the functions are all correct. This
# gate reads the source and asserts each core makes them.
#
# Exemptions live in tests/core_contract_exempt.txt with a reason per line,
# because two of these files are apps rather than emulators and two meet the
# contract in a sibling translation unit.
#
# Exits 0 (pass) or 1 (fail). Skips loudly if it finds no cores.
set -u
cd "$(dirname "$0")/.."

EXEMPT=tests/core_contract_exempt.txt
fails=0
checked=0
seen=""

exempt_for() {   # $1 = core dir name, $2 = contract name
  [ -f "$EXEMPT" ] || return 1
  awk -F'\t' -v c="$1" -v k="$2" '
    /^[[:space:]]*#/ { next }
    NF < 3           { next }
    $1 == c { n = split($2, a, ","); for (i = 1; i <= n; i++) if (a[i] == k) { print $3; found = 1 } }
    END { exit(found ? 0 : 1) }
  ' "$EXEMPT"
}

# The names that actually start the SAI DMA, read out of gw_audio.c. Derived,
# not listed: the check must come from the thing it protects.
AUDIO_STARTERS=$(awk '
  # Function definitions at column 0. Cut at the paren FIRST, then drop the
  # return type: the other order leaves the trailing brace.
  /^[a-zA-Z_].*\(/ { fn = $0; sub(/\(.*/, "", fn); sub(/.*[ \t*]/, "", fn); next }
  { body[fn] = body[fn] "\n" $0 }
  END {
    # Direct: the function that touches the DMA. Then one level of indirection,
    # because audio_start_playing() is a two-line wrapper around
    # audio_start_playing_full_length() and a core may legitimately call
    # either. Deriving both beats listing either.
    for (f in body) if (body[f] ~ /HAL_SAI_Transmit_DMA/) { direct[f] = 1; print f }
    for (f in body) for (d in direct) if (f != d && body[f] ~ d "[ \t]*\\(") print f
  }
' Core/Src/gw_audio.c 2>/dev/null | grep -E "^[a-z_][a-z0-9_]*$" | sort -u)
if [ -z "$AUDIO_STARTERS" ]; then
  echo "SKIP could not derive the audio-start entry points from Core/Src/gw_audio.c"
  AUDIO_STARTERS="audio_start_playing"
fi

starts_audio() {   # $1 = core directory
  for fn in $AUDIO_STARTERS; do
    grep -rqE "^[^*/]*\b$fn[[:space:]]*\(" "$1" 2>/dev/null && return 0
  done
  return 1
}

for f in Core/Src/porting/*/main_*.c; do
  [ -e "$f" ] || continue
  core=$(basename "$(dirname "$f")")
  # One entry per core directory: several have more than one main_*.c.
  case " $seen " in *" $core "*) continue ;; esac
  seen="$seen $core"
  checked=$((checked + 1))

  core_fails=0
  skipped=0
  for pair in "odroid_system_init:system_init" \
              "odroid_system_emu_init:emu_init" \
              "common_emu_frame_loop:frame_loop" \
              "audio:audio"; do
    fn=${pair%%:*}; key=${pair##*:}
    # Look across the core's whole directory: several cores split the call
    # into a sibling TU, which is a layout choice, not a missing call.
    if [ "$key" = "audio" ]; then
      starts_audio "$(dirname "$f")" && continue
      what="any audio start ($(echo $AUDIO_STARTERS | tr '\n' '/' | sed 's|/$||'))"
    else
      grep -rqE "^[^*/]*\b$fn[[:space:]]*\(" "$(dirname "$f")" 2>/dev/null && continue
      what="$fn()"
    fi
    if reason=$(exempt_for "$core" "$key"); then
      echo "  SKIP $core has no $what -- $reason"
      skipped=$((skipped + 1))
      continue
    fi
    echo "  FAIL $core never calls $what"
    core_fails=$((core_fails + 1))
  done
  if [ "$core_fails" -eq 0 ]; then
    if [ "$skipped" -gt 0 ]; then
      echo "  OK   $core wires the rest of the core contract"
    else
      echo "  OK   $core wires the core contract"
    fi
  fi
  fails=$((fails + core_fails))
done

if [ "$checked" -eq 0 ]; then
  echo "SKIP no Core/Src/porting/*/main_*.c found"
  exit 0
fi
echo "  checked $checked core(s)"
if [ "$fails" -gt 0 ]; then
  echo "       A missing call here does not crash. It removes a feature and"
  echo "       says nothing: no savestates, no pacing, or a silent audio ring."
  echo "       Add the call, or add an exemption with a reason that is true."
  exit 1
fi
exit 0
