#!/bin/bash
# bench.sh — SNES-style budget table for the SegaCD host boot harness.
#
# Methodology (b2 SNES probe pattern): for each component, run a variant with
# that component stubbed; the wall-clock drop IS that component's cost. Print
# one BUDGET ms=<n> line per run, then assemble a table:
#
#   A = baseline              (everything: main + sub + z80/audio + vdp + skel)
#   B = -audio                (z80_run + SN76489_run + ym2612_run stubbed)
#   C = -audio -vdp           (+ gwenesis_vdp_render_line stubbed)
#   D = -audio -vdp -sub      (+ segacd_run_sub stubbed)  -> MAIN + skeleton
#   E = baseline + main_idle  (SCD_MAIN_IDLE_SKIP=1)      -> idle-skip preview
#
# Cost derivation:
#   audio_cost  = A - B
#   vdp_cost    = B - C
#   sub_cost    = C - D
#   main+skel   = D
#   main_save   = A - E
#
# NOTE: SCD_SUB_IDLE_SKIP was removed — the $36a9 spin is a bidirectional
# handshake (sub must ack doorbell via L2 ISR before main re-pulses).
# Skipping sub execution prevents the handshake from completing.
#
# Histogram (b7) further subdivides MAIN into idle ($fe26 spin, 88%) and real
# work (12%), and SUB into idle ($36a9 spin, 13%) and ISR work (87%).
#
# Usage: bash tools/segacd_harness/bench.sh [frames]
#   default 900 frames (mode-8 stall window per handoff)
set -uo pipefail
BIOS=/tmp/scd/bios_CD_U.bin
CUE=$(ls /tmp/scd/*.cue 2>/dev/null | head -1)
FRAMES=${1:-900}
BIN=${BIN:-/tmp/boot_bench}   # bench.sh wants the un-instrumented build

if [ ! -x "$BIN" ]; then
    echo "FATAL: $BIN missing. Run build_bench.sh first." >&2
    exit 1
fi
if [ ! -f "$BIOS" ] || [ -z "$CUE" ]; then
    echo "FATAL: BIOS ($BIOS) or CUE ($CUE) missing." >&2
    exit 1
fi

# Pin to a single core + nice to dampen scheduler noise. Drop privileges if
# taskset isn't available (BSD / WSL).
PIN=""
if command -v taskset >/dev/null 2>&1; then PIN="taskset -c 1"; fi

# Warm the page cache once so the first run isn't penalised for I/O.
$PIN "$BIN" "$BIOS" "$CUE" 50 >/dev/null 2>&1 || true

run() {  # run <label> <env assignments...>
    local label="$1"; shift
    local samples=()
    # 7 samples, take MIN. Min is the right summary statistic for "best-case
    # wall clock" — it filters out scheduler interference (which only ever
    # *adds* time). Median-of-3 was too noisy on this aarch64 host.
    for _n in 1 2 3 4 5 6 7; do
        local ms
        ms=$(env "$@" SCD_QUIET=1 $PIN "$BIN" "$BIOS" "$CUE" "$FRAMES" 2>/dev/null \
            | grep -a '^BUDGET ' | sed -n 's/.*ms=\([0-9]*\).*/\1/p')
        samples+=("$ms")
    done
    local min
    min=$(printf '%s\n' "${samples[@]}" | sort -n | head -1)
    local hits=""
    # capture idle_hits too (so we can sanity-check the idle-skip variant)
    local last_hits
    last_hits=$(env "$@" SCD_QUIET=1 $PIN "$BIN" "$BIOS" "$CUE" "$FRAMES" 2>/dev/null \
        | grep -a '^BUDGET ' | sed -n 's/^BUDGET ms=[0-9]* frames=[0-9]* mspf=[0-9.]* idle_hits=\([0-9]*\).*/\1/p')
    printf 'BENCH %-20s min=%-5s idle_hits=%s  samples=[%s]\n' \
        "$label" "$min" "$last_hits" "$(IFS=,; echo "${samples[*]}")"
}

echo "=== bench: $FRAMES frames, min of 7, host=$(uname -m) ==="
run A_baseline
run B_no_audio        SCD_SKIP_AUDIO=1
run C_no_audio_vdp    SCD_SKIP_AUDIO=1 SCD_SKIP_VDP=1
run D_no_av_sub       SCD_SKIP_AUDIO=1 SCD_SKIP_VDP=1 SCD_SKIP_SUB=1
run E_main_idle_skip  SCD_MAIN_IDLE_SKIP=1
run F_no_ym           SCD_SKIP_YM=1
run G_no_psg          SCD_SKIP_PSG=1
run H_no_z80          SCD_SKIP_Z80=1
run I_no_planeB       SCD_SKIP_PLANEB=1
run J_no_planeA       SCD_SKIP_PLANEA=1
run K_no_sprites      SCD_SKIP_SPRITES=1
run L_no_vdp_sub      SCD_SKIP_PLANEB=1 SCD_SKIP_PLANEA=1 SCD_SKIP_SPRITES=1
echo "=== bench done ==="
