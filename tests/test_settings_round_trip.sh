#!/bin/bash
# A per-app setting that a core SAVES but never READS BACK is invisible to
# every other check. The menu works, the toggle applies immediately, the value
# lands in the right slot of /CONFIG -- and the next time the core launches it
# is gone. To the player the setting simply does not stick, which reads like
# forgetfulness rather than a bug.
#
# Watara Supervision shipped exactly that: palette_update_cb() called
# odroid_settings_Palette_set(), and init called
# supervision_set_color_scheme(SV_COLOR_SCHEME_DEFAULT) straight over it.
# Fixed 2026-09-07. gb_tgbdual has always done it correctly
# (index_palette = odroid_settings_Palette_get()), which is what made the
# asymmetry visible once anybody counted.
#
# The rule: within one core, every odroid_settings_<X>_set() needs a matching
# _get(). The reverse is fine -- reading a value the launcher owns (cpu_oc_level)
# without writing it is normal.
#
# Only files the build actually compiles are checked. Core/Src/porting/nes/ is
# the nofrendo core, which no source list mentions (see root CLAUDE.md); a bug
# in a file that never links is not a bug.
#
# Exits 0 (pass) or 1 (fail).
set -u
cd "$(dirname "$0")/.."

# --red: take a healthy core, delete its _get, require this gate to name it.
if [ "${1:-}" = "--red" ]; then
  red=$(mktemp -d); trap 'rm -rf "$red"' EXIT
  mkdir -p "$red/tests" "$red/Core/Src/porting"
  cp -r Core/Src/porting/* "$red/Core/Src/porting/" 2>/dev/null
  cp Makefile Makefile.common "$red/" 2>/dev/null
  cp "$0" "$red/tests/"
  sed -i 's/odroid_settings_Palette_get(/odroid_settings_Palette_GONE(/' \
      "$red/Core/Src/porting/wsv/main_wsv.c" 2>/dev/null
  out=$(cd "$red" && bash "tests/$(basename "$0")" 2>&1); rc=$?
  [ "$rc" -ne 0 ] || { echo "  FAIL RED: a save-without-restore PASSED"; exit 1; }
  case "$out" in
    *"wsv saves Palette but never reads it back"*) ;;
    *) echo "  FAIL RED: it failed, but did not name the setting:"
       echo "$out" | grep -E "FAIL" | sed 's/^/         /'; exit 1 ;;
  esac
  echo "  OK   RED: a setting saved but never restored is caught, by name"
  exit 0
fi

fails=0
checked=0
for f in Core/Src/porting/*/main_*.c Core/Src/porting/*/main_*.cpp; do
  [ -e "$f" ] || continue
  # Compiled by the build? A file no source list names cannot ship a bug.
  # Strip comments first: Makefile:675 MENTIONS main_nes.c inside a comment
  # about GNW_DISABLE_COMPRESSION, and reading that as a build entry put the
  # dead nofrendo core back into this gate's scope. This tree has been bitten
  # by comment text passing for code before; grep does not know the difference
  # unless you tell it.
  base=$(basename "$f")
  if ! sed 's/#.*//' Makefile Makefile.common 2>/dev/null | grep -qF "$base"; then
    continue
  fi
  core=$(basename "$(dirname "$f")")
  dir=$(dirname "$f")
  names=$(grep -rhoE 'odroid_settings_[A-Za-z0-9_]+_set[[:space:]]*\(' "$dir" 2>/dev/null \
          | sed -E 's/odroid_settings_(.*)_set[[:space:]]*\(/\1/' | sort -u)
  [ -n "$names" ] || continue
  for n in $names; do
    checked=$((checked + 1))
    if grep -rqE "^[^*/]*odroid_settings_${n}_get[[:space:]]*\(" "$dir" 2>/dev/null; then
      echo "  OK   $core round-trips $n"
    else
      echo "  FAIL $core saves $n but never reads it back -- the setting will not stick"
      fails=$((fails + 1))
    fi
  done
done

if [ "$checked" -eq 0 ]; then
  echo "  SKIP no core writes a per-app setting"
  exit 0
fi
echo "  checked $checked setting(s) that a core writes"
if [ "$fails" -gt 0 ]; then
  echo "       Read it back where the core starts, after odroid_system_init,"
  echo "       and clamp it: the byte comes off an SD card."
  exit 1
fi
exit 0
