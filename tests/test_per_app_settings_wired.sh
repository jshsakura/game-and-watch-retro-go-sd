#!/bin/bash
# odroid_settings_*_get()/set() index persistent_config.app[] by
# odroid_system_get_app()->id, and that id is assigned inside
# odroid_system_init(). A core that reads one of those accessors BEFORE it
# calls odroid_system_init reads app[0] -- the launcher's slot -- and gets
# whatever the launcher happens to have there. The menu still shows a value,
# the setting still persists to the right slot, and nothing engages.
#
# That is not hypothetical: the 32X screen-tear option shipped that way in
# ca44eccf and was dead in every build until 2026-09-07. No unit test could
# have caught it, because both functions were correct and only their order was
# wrong. This gate asserts the order instead.
#
# Scope, and why it is narrow: only uses inside the SAME function that calls
# odroid_system_init, and only above that call. A submenu callback defined
# earlier in the file is textually before the init call but runs only when the
# player opens the menu, which is long after. Line order is not execution
# order, and a gate that confuses the two cries wolf on three healthy cores.
#
# Exits 0 (pass) or 1 (fail). Skips loudly rather than failing when it cannot
# find what it needs -- a safety net must not be the thing that breaks a build.
set -u
cd "$(dirname "$0")/.."

# --red: build the broken arrangement this gate exists to catch, run ourselves
# against it, and require a failure. A gate that has never failed proves
# nothing, and the knowledge of what "broken" looks like belongs next to the
# gate rather than in a one-off block in run.sh.
if [ "${1:-}" = "--red" ]; then
  red=$(mktemp -d)
  trap 'rm -rf "$red"' EXIT
  mkdir -p "$red/tests" "$red/Core/Src/porting"
  cp -r Core/Src/porting/md32x "$red/Core/Src/porting/" 2>/dev/null
  cp "$0" "$red/tests/$(basename "$0")"
  # Put the settings reads back above odroid_system_init, which is where they
  # sat while the 32X tear guard shipped dead.
  f="$red/Core/Src/porting/md32x/main_md32x.c"
  python3 - "$f" <<'PYEOF'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
reads = [l for l in s.split('\n') if 'odroid_settings_ScreenTearFix_get()' in l
         or 'odroid_settings_DisplayScaling_get()' in l]
if reads:
    for l in reads:
        s = s.replace(l + '\n', '', 1)
    anchor = '  odroid_gamepad_state_t joystick;\n'
    s = s.replace(anchor, anchor + '\n'.join(reads) + '\n', 1)
io.open(p, 'w', encoding='utf-8').write(s)
PYEOF
  out=$(cd "$red" && bash "tests/$(basename "$0")" 2>&1); rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  FAIL RED: the pre-fix arrangement PASSED, so this gate cannot see it"
    exit 1
  fi
  # Failing is not enough: it has to fail for the right reason. A fixture that
  # merely lost a file also exits non-zero and would let a blind gate through.
  case "$out" in
    *"reads a per-app setting before its own odroid_system_init"*) ;;
    *) echo "  FAIL RED: it failed, but not on the read order:"
       echo "$out" | sed 's/^/         /'
       exit 1 ;;
  esac
  echo "  OK   RED: a read above odroid_system_init is caught, by name"
  exit 0
fi

ACCESSORS='odroid_settings_(Region|Palette|DisplayScaling|DisplayFilter|DisplayOverscan|SpriteLimit|ScreenTearFix)_(get|set)'
fails=0
checked=0

for f in Core/Src/porting/*/main_*.c; do
  grep -qE "$ACCESSORS" "$f" || continue
  core=$(basename "$(dirname "$f")")
  grep -q 'odroid_system_init' "$f" || { echo "  SKIP $core uses a per-app accessor but never calls odroid_system_init"; continue; }

  bad=$(awk -v pat="$ACCESSORS" '
    # Track the last line that opened a function body at column 0.
    /^[A-Za-z_].*\(.*\)[[:space:]]*$/ { pend = NR; pendtxt = $0 }
    /^\{/                             { if (pend == NR - 1) { fstart = pend; fname = pendtxt } }
    /^[A-Za-z_].*\(.*\)[[:space:]]*\{[[:space:]]*$/ { fstart = NR; fname = $0 }

    /^[[:space:]]*\*/   { next }
    /^[[:space:]]*\/\// { next }
    /extern[[:space:]]/ { next }

    $0 ~ pat && $0 ~ /\(/ { use[++n] = NR ":" fstart ":" $0 }
    /odroid_system_init\(/ && $0 !~ /^[[:space:]]*[*\/]/ { initline = NR; initfn = fstart }

    END {
      if (initline == 0) exit 0
      for (i = 1; i <= n; i++) {
        split(use[i], p, ":")
        if (p[2] == initfn && p[1] + 0 < initline) {
          sub(/^[0-9]+:[0-9]+:/, "", use[i])
          printf "       line %s:%s\n", p[1], use[i]
        }
      }
      printf "INITLINE %d\n", initline
    }
  ' "$f")

  initline=$(echo "$bad" | sed -n 's/^INITLINE //p')
  offenders=$(echo "$bad" | grep '^       line ' || true)
  [ -n "$initline" ] || { echo "  SKIP $core: could not locate the odroid_system_init call"; continue; }
  checked=$((checked + 1))

  if [ -n "$offenders" ]; then
    echo "  FAIL $f reads a per-app setting before its own odroid_system_init (line $initline):"
    echo "$offenders"
    echo "       app[] is indexed by currentApp.id, and that call is what sets it."
    fails=$((fails + 1))
  else
    echo "  OK   $core reads its per-app settings after odroid_system_init"
  fi
done

if [ "$checked" -eq 0 ]; then
  echo "SKIP no core uses a per-app settings accessor -- nothing to check"
  exit 0
fi
echo "  checked $checked core(s)"
[ "$fails" -eq 0 ] || exit 1
exit 0
