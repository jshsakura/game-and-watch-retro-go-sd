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
