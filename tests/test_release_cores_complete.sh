#!/bin/bash
# The firmware names the core files it will open. The release has to contain
# them. When it does not, nothing fails at build time: the card is short a
# file, the launcher lists the system anyway, and picking a game raises the
# firmware's integrity dialog -- which is what happened on 2026-09-05 with a
# stale staging directory, and cost a device round trip to diagnose.
#
# The required list is DERIVED from the source: every "/cores/..." string in
# Core/. Deriving beats listing, because a new core added to
# rg_emulators.c's emu_dispatch_t table appears here on its own.
#
# Deliberate absences live in tests/release_cores_exempt.txt with a reason.
# There is exactly one today: the PICO-8 engine is third-party and the firmware
# ships a stub screen that tells the player how to install it.
#
# Checks sd_content/ (what `make release` rolls into the tar). Skips loudly
# when there is no build to look at -- this gate reports on an artefact, and
# absence of an artefact is not a failure.
#
# Exits 0 (pass) or 1 (fail).
set -u
cd "$(dirname "$0")/.."

# --red: hide one core file from a good build and require this gate to name it.
if [ "${1:-}" = "--red" ]; then
  [ -d sd_content/cores ] || { echo "  SKIP RED: no build to break"; exit 0; }
  victim=$(ls sd_content/cores/*.bin 2>/dev/null | head -1)
  [ -n "$victim" ] || { echo "  SKIP RED: no core .bin to hide"; exit 0; }
  red=$(mktemp -d); trap 'rm -rf "$red"' EXIT
  mkdir -p "$red/tests" "$red/Core"
  cp -r Core/Src Core/Inc "$red/Core/" 2>/dev/null
  cp -r sd_content "$red/" 2>/dev/null
  cp "$0" tests/release_cores_exempt.txt "$red/tests/" 2>/dev/null
  rm -f "$red/sd_content/cores/$(basename "$victim")"
  out=$(cd "$red" && bash "tests/$(basename "$0")" 2>&1); rc=$?
  [ "$rc" -ne 0 ] || { echo "  FAIL RED: a release missing $(basename "$victim") PASSED"; exit 1; }
  case "$out" in
    *"$(basename "$victim") is opened by the firmware but this build did not produce it"*) ;;
    *) echo "  FAIL RED: it failed, but did not name the missing file:"
       echo "$out" | grep -E "FAIL" | sed 's/^/         /'; exit 1 ;;
  esac
  echo "  OK   RED: a release short one core file is caught, by name"
  exit 0
fi

CORES_DIR=sd_content/cores
EXEMPT=tests/release_cores_exempt.txt

[ -d "$CORES_DIR" ] || { echo "SKIP no $CORES_DIR -- nothing built to check"; exit 0; }

need=$(grep -rhoE '"/cores/[A-Za-z0-9_./-]+"' Core/Src Core/Inc 2>/dev/null \
       | tr -d '"' | sort -u)
[ -n "$need" ] || { echo "SKIP found no /cores/ references in the source"; exit 0; }

missing=0
checked=0
for path in $need; do
  checked=$((checked + 1))
  f="sd_content$path"
  [ -f "$f" ] && continue
  reason=""
  if [ -f "$EXEMPT" ]; then
    reason=$(awk -F'\t' -v p="$path" '
      /^[[:space:]]*#/ { next }
      NF >= 2 && $1 == p { print $2; found = 1 }
      END { exit(found ? 0 : 1) }' "$EXEMPT") || reason=""
  fi
  if [ -n "$reason" ]; then
    echo "  SKIP $path is not shipped -- $reason"
    continue
  fi
  echo "  FAIL $path is opened by the firmware but this build did not produce it"
  missing=$((missing + 1))
done

echo "  checked $checked core file(s) named in the source against $CORES_DIR"
if [ "$missing" -gt 0 ]; then
  echo "       A card short one of these does not fail the build. The launcher"
  echo "       lists the system, and picking a game raises the integrity dialog."
  echo "       Add it to create_sd_data, or exempt it with a reason that is true."
  exit 1
fi
echo "  OK   every core file the firmware opens is in the release"
exit 0
