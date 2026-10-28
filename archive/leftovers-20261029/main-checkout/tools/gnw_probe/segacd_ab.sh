#!/usr/bin/env bash
# Sega CD WRRQ=0 A/B harness (2026-09-22 campaign).
#
#   segacd_ab.sh build                # baseline (HS=0 DT=0) + HEAD shadow (HS=1 DT=0)
#   segacd_ab.sh run  "/roms/segacd/.../game.cue"
#   segacd_ab.sh all  "/roms/segacd/.../game.cue"
#   segacd_ab.sh snapshot ARM_NAME
#   segacd_ab.sh ring    ARM_NAME     # decode the CTRL0-write ring
#   segacd_ab.sh mode8   ARM_NAME     # two-point MAINBOOT/mode-8 measurement
#   segacd_ab.sh bioscheck ARM_NAME   # compare live XIP BIOS with known images
#   segacd_ab.sh build_dt / run_dt    # DECI-timeout arm (only after trace analysis)
#   segacd_ab.sh build_int / run_int  # CDD tick/sub-CPU interleave arm
#   segacd_ab.sh build_b4  / run_b4   # interleave + 4-line sub batch
#   segacd_ab.sh build_w1  / run_w1   # interleave + canonical 1M Word-RAM
#   segacd_ab.sh build_w1d / run_w1d   # W1 + sub-68K derail tracer (HOOK_CPU)
#   segacd_ab.sh derail    ARM_NAME    # dump+decode the derail ring/shot
#
# Arms (one experimental axis each — never both on in one arm):
#   baseline:   HS=0 DT=0 INTERLEAVE=0  (device-verified burst baseline)
#   hs:         HS=1 DT=0 INTERLEAVE=0  (CTRL1.SHDREN banked HEAD shadow)
#   dt:         HS=0 DT=1 INTERLEAVE=0  (DECI auto-clear, 67250 sub cycles)
#   interleave: HS=0 DT=0 INTERLEAVE=1  (short sub slot after each CDD tick)
#
# Each arm: clean build, matching bin+xip+intflash flash, RUN_SECONDS wait,
# ONE SWD snapshot (no polling).
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ARMS=${ARMS:-/tmp/gnw_arms}
SPEED=${SPEED:-2}
RUN_SECONDS=${RUN_SECONDS:-30}
RESULTS=${RESULTS:-/tmp/segacd_ab_results.tsv}
RINGS=${RINGS:-/tmp/segacd_ab_ring}
MODE8=${MODE8:-/tmp/segacd_mode8}
ARM_TOOL=tools/gnw_probe/arm.sh
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000' -c 'stm32h7x.cpu0 configure -work-area-size 0'"

arm_name() {
  # axis label -> arm dir name; encodes both axis values (mislabeled-artifact guard)
  case "$1" in
    baseline) printf 'segacd_hs0_dt0_s%s' "$SPEED" ;;
    hs)       printf 'segacd_hs1_dt0_s%s' "$SPEED" ;;
    dt)       printf 'segacd_hs0_dt1_s%s' "$SPEED" ;;
    interleave) printf 'segacd_hs0_dt0_i1_s%s' "$SPEED" ;;
    interleave_b4) printf 'segacd_hs0_dt0_i1_b4_s%s' "$SPEED" ;;
    interleave_w1) printf 'segacd_hs0_dt0_i1_w1_s%s' "$SPEED" ;;
    interleave_w1d) printf 'segacd_hs0_dt0_i1_w1_d1_s%s' "$SPEED" ;;
    interleave_w1e) printf 'segacd_hs0_dt0_i1_w1_e1_s%s' "$SPEED" ;;
    *)        echo "unknown axis: $1" >&2; return 1 ;;
  esac
}

arm_flags() {
  case "$1" in
    baseline) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=0' ;;
    hs)       printf 'SEGACD_HEAD_SHADOW=1 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=0' ;;
    dt)       printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=1 SEGACD_CDD_INTERLEAVE=0' ;;
    interleave) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=1' ;;
    interleave_b4) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=1 SEGACD_SUB_LINE_BATCH=4' ;;
    interleave_w1) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=1 SEGACD_WORDRAM_1M_CANONICAL=1' ;;
    interleave_w1d) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=1 SEGACD_WORDRAM_1M_CANONICAL=1 SEGACD_DERAIL=1' ;;
    interleave_w1e) printf 'SEGACD_HEAD_SHADOW=0 SEGACD_DECI_TIMEOUT=0 SEGACD_CDD_INTERLEAVE=1 SEGACD_WORDRAM_1M_CANONICAL=1 SEGACD_EVLOG=1' ;;
  esac
}

build_one() {
  local axis=$1 name flags trace=1
  name=$(arm_name "$axis")
  flags=$(arm_flags "$axis")
  [[ "$axis" == interleave_w1* ]] && trace=0
  echo "[segacd-ab] build $name ($flags)"
  # Make does not encode command-line feature values into object prerequisites.
  # A clean build per arm prevents a mislabeled binary from reusing the other
  # arm's main_segacd.o.
  make clean >/dev/null
  # shellcheck disable=SC2086
  bash "$ARM_TOOL" build "$name" \
    GNW_AUTOBOOT=1 \
    SEGACD=1 \
    SEGACD_GA_TRACE="$trace" \
    SEGACD_CDD_SPEED="$SPEED" \
    $flags
  test -f "$ARMS/$name/cores/segacd.bin"
  test -f "$ARMS/$name/cores/segacd.xip"
}

snapshot_lite() {
  local name=$1 elf="$ARMS/$1/gw_retro_go.elf"
  # GA_TRACE=0 arms only carry the always-on counters; read what exists.
  local names=(scd_dbg_frame scd_boot_mode
    scd_dbg_gfx_ops scd_dbg_gfx_lines scd_dbg_gfx_mapnz
    scd_dbg_cdd_ticks scd_cdupd_pass scd_cdupd_feed
    scd_dbg_dec_calls scd_dbg_dec_wrrq
    scd_dbg_host_sub_adv scd_dbg_ctrl0_wrrq)
  local labels=() addrs=() n a cmd out i value
  for n in "${names[@]}"; do
    a=$(sym "$elf" "$n")
    if [ -z "$a" ]; then
      printf '%s\t%s\tN/A_GA_TRACE0\n' "$name" "$n" | tee -a "$RESULTS"
      continue
    fi
    labels+=("$n"); addrs+=("$a")
  done
  cmd="$OC -c init -c halt"
  for a in "${addrs[@]}"; do cmd+=" -c 'mdw $a'"; done
  cmd+=" -c resume -c shutdown"
  out=$(ssh "$HOST" "$cmd" 2>&1)
  for i in "${!labels[@]}"; do
    value=$(printf '%s\n' "$out" | grep -iE "^0x?${addrs[$i]#0x}:" | tail -1 | awk '{print $2}')
    printf '%s\t%s\t%s\n' "$name" "${labels[$i]}" "${value:-READ_ERROR}" | tee -a "$RESULTS"
  done
}

gfx_probe() {
  local name=$1 window_ms=${2:-1000} elf="$ARMS/$1/gw_retro_go.elf"
  local names=(scd_dbg_cdd_ticks scd_dbg_gfx_ops scd_dbg_gfx_lines scd_dbg_gfx_mapnz
    scd_dbg_dec_wrrq scd_dbg_host_sub_adv scd_boot_mode)
  local addrs=() labels=() n a cmd out raw="$MODE8.$name.gfx.raw"
  for n in "${names[@]}"; do
    a=$(sym "$elf" "$n")
    if [ -z "$a" ]; then echo "[gfxprobe] N/A_GA_TRACE0: $n" >&2; continue; fi
    addrs+=("$a"); labels+=("$n")
  done
  cmd="$OC -c init -c halt -c 'echo GFX_PRE'"
  for a in "${addrs[@]}"; do cmd+=" -c 'mdw $a'"; done
  cmd+=" -c resume -c 'sleep $window_ms' -c halt -c 'echo GFX_POST'"
  for a in "${addrs[@]}"; do cmd+=" -c 'mdw $a'"; done
  cmd+=" -c resume -c shutdown"
  out=$(ssh "$HOST" "$cmd" 2>&1)
  printf '%s\n' "$out" >"$raw"
  python3 - "$raw" "$window_ms" "${labels[@]}" -- "${addrs[@]}" <<'PY'
import re,sys
text=open(sys.argv[1]).read(); ms=int(sys.argv[2]); cut=sys.argv.index('--'); names=sys.argv[3:cut]
addrs=[int(x,16) for x in sys.argv[cut+1:]]
parts=re.split(r'GFX_(PRE|POST)\s*\n',text)
sec={parts[i]:parts[i+1] for i in range(1,len(parts)-1,2)}
def parse(s):
 d={}
 for m in re.finditer(r'^0x([0-9a-fA-F]+):\s+([0-9a-fA-F]{8})',s,re.M):
  d[int(m.group(1),16)]=int(m.group(2),16)
 return d
p,q=parse(sec['PRE']),parse(sec['POST'])
for n,a in zip(names,addrs): print(f'{n}\tPRE=0x{p[a]:x}\tPOST=0x{q[a]:x}\tDELTA={(q[a]-p[a])&0xffffffff}')
dt=(q[addrs[0]]-p[addrs[0]])&0xffffffff
print(f'cdd_tick_rate\t{dt*1000/ms:.2f}/s')
PY
}

sym() {
  # Do not exit awk early: with pipefail that SIGPIPEs nm and aborts the run.
  arm-none-eabi-nm "$1" | awk -v n="$2" '$3 == n && !found { print "0x" $1; found=1 }'
}

snapshot() {
  local name=$1 elf="$ARMS/$1/gw_retro_go.elf"
  local names=(
    scd_dbg_frame scd_dbg_mframe_done
    scd_dbg_cdd_ticks scd_dbg_cdd_batch_max
    scd_cdupd_pass scd_cdupd_feed scd_dbg_cdupd_read scd_dbg_dec_calls scd_dbg_dec_wrrq
    scd_dbg_host_sub scd_dbg_host_sub_adv scd_dbg_host_main scd_dbg_deliver5_n
    scd_dbg_deliver5_total scd_dbg_dtrg_dma scd_dbg_dma_fire scd_boot_mode
    scd_dbg_ctrl0_w scd_dbg_ctrl0_wrrq scd_dbg_ctrl1_w scd_dbg_ctrl1_last
    scd_dbg_head_rd scd_dbg_head_bank
    scd_dbg_main_pc scd_dbg_sub_pc
    scd_dbg_ctrl0_ring_idx scd_dbg_ctrl0_ring_n
    scd_dbg_cdcw_ring_idx scd_dbg_cdcw_ring_n
  )
  local labels=() addrs=() n a cmd out hist stop_addr ring_addr
  local cdcw_idx_addr cdcw_n_addr cdcw_idx_value cdcw_n_value

  for n in "${names[@]}"; do
    a=$(sym "$elf" "$n")
    test -n "$a" || { echo "[segacd-ab] missing trace symbol: $n" >&2; return 1; }
    labels+=("$n"); addrs+=("$a")
  done
  hist=$(sym "$elf" scd_dbg_cdd_cmd_hist)
  test -n "$hist" || { echo "[segacd-ab] missing scd_dbg_cdd_cmd_hist" >&2; return 1; }
  stop_addr=$(printf '0x%x' $((hist + 4)))
  labels+=("stop_cmd_count"); addrs+=("$stop_addr")
  ring_addr=$(sym "$elf" scd_dbg_ctrl0_ring)
  test -n "$ring_addr" || { echo "[segacd-ab] missing scd_dbg_ctrl0_ring" >&2; return 1; }
  cdcw_addr=$(sym "$elf" scd_dbg_cdcw_ring)
  test -n "$cdcw_addr" || { echo "[segacd-ab] missing scd_dbg_cdcw_ring" >&2; return 1; }
  cdcw_mid=$(printf '0x%x' $((cdcw_addr + 128)))

  cmd="$OC -c init -c halt"
  for a in "${addrs[@]}"; do cmd+=" -c 'mdw $a'"; done
  cmd+=" -c 'mdw $ring_addr 20'"
  cmd+=" -c 'mdw $cdcw_addr 10'"
  cmd+=" -c resume -c shutdown"
  out=$(ssh "$HOST" "$cmd" 2>&1)
  cdcw_idx_addr=$(sym "$elf" scd_dbg_cdcw_ring_idx)
  cdcw_n_addr=$(sym "$elf" scd_dbg_cdcw_ring_n)
  cdcw_idx_value=$(printf '%s\n' "$out" | grep -iE "^0x?${cdcw_idx_addr#0x}:" | tail -1 | awk '{print $2}')
  cdcw_n_value=$(printf '%s\n' "$out" | grep -iE "^0x?${cdcw_n_addr#0x}:" | tail -1 | awk '{print $2}')
  { printf '%s\n' "$out"
    printf '# CTRL0_RING_BASE %s\n' "$ring_addr"
    printf '# CDCW_RING_BASE %s\nCDCW_RING_IDX %s\nCDCW_RING_N %s\n' \
      "$cdcw_addr" \
      "${cdcw_idx_value:-0}" \
      "${cdcw_n_value:-0}" ; } >"$RINGS.$name.raw"

  local i value
  for i in "${!labels[@]}"; do
    value=$(printf '%s\n' "$out" | grep -iE "^0x?${addrs[$i]#0x}:" | tail -1 | awk '{print $2}')
    printf '%s\t%s\t%s\n' "$name" "${labels[$i]}" "${value:-READ_ERROR}" | tee -a "$RESULTS"
  done
}

ring_dump() {
  # Decode the CTRL0-write ring and the CDC-write ring using the nm-derived
  # bases recorded by snapshot() in the raw file (# markers).
  local name=$1 raw="$RINGS.$1.raw"
  test -f "$raw" || { echo "[ring] no raw snapshot for $name (run snapshot first)" >&2; return 1; }
  python3 - "$raw" "$name" <<'PY'
import re, struct, sys
raw_all, name = open(sys.argv[1]).read(), sys.argv[2]
ctrl0_base = cdcw_base = cdcw_idx = cdcw_n = None
lines = []
for ln in raw_all.splitlines():
    t = ln.split()
    if ln.startswith('# CTRL0_RING_BASE '): ctrl0_base = int(t[2], 16); continue
    if ln.startswith('# CDCW_RING_BASE '): cdcw_base = int(t[2], 16); continue
    if ln.startswith('CDCW_RING_IDX '): cdcw_idx = int(t[1], 16) if len(t) > 1 else None; continue
    if ln.startswith('CDCW_RING_N '): cdcw_n = int(t[1], 16) if len(t) > 1 else None; continue
    if ln.startswith('#'): continue
    lines.append(ln)
raw = chr(10).join(lines)
mem = {}
for m in re.finditer(r'^0x([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{8}\s*)+)$', raw, re.M):
    a = int(m.group(1), 16)
    mem[a] = [int(x, 16) for x in m.group(2).split()]

def read(base, n):
    out, a = [], base
    while len(out) < n:
        ws = mem.get(a)
        if not ws: break
        take = ws[:n - len(out)]
        out += take
        a += 4 * len(take)
    return out

print(f"[ring] {name} ctrl0_base={ctrl0_base and hex(ctrl0_base)} cdcw_base={cdcw_base and hex(cdcw_base)}")
if ctrl0_base is not None:
    run = read(ctrl0_base, 20)
    if len(run) >= 20:
        print("[ring] CTRL0 ring (physical slots; ring is 4 deep):")
        print("  #  val ctl1 ifc ifs | head(MM SS FF MD) | wa   cdd st3 | sub_pc    cur_lba")
        for e in range(4):
            w = run[e*5:e*5+5]
            val, ctl1, ifc, ifs = w[0]&0xff, (w[0]>>8)&0xff, (w[0]>>16)&0xff, (w[0]>>24)&0xff
            head = [w[1]&0xff, (w[1]>>8)&0xff, (w[1]>>16)&0xff, (w[1]>>24)&0xff]
            wa, cdd, st3 = w[2]&0xffff, (w[2]>>16)&0xff, (w[2]>>24)&0xff
            pc, lba = w[3], struct.unpack('<i', struct.pack('<I', w[4]))[0]
            wrrq = "WRRQ" if val & 4 else "----"
            print(f"  {e}  {val:02x}  {ctl1:02x}  {ifc:02x}  {ifs:02x} | "
                  f"{head[0]:02x}:{head[1]:02x}:{head[2]:02x} m{head[3]:02x}    | "
                  f"{wa:04x} {cdd:02x}  {st3:02x} | {pc:08x} {lba:8d}  {wrrq}")
    else:
        print(f"[ring] CTRL0 ring: only {len(run)} words readable")
if cdcw_base is not None:
    run = read(cdcw_base, 10)
    REG = {0x00:'CR0SW',0x01:'IFCTRL',0x02:'DBCL',0x03:'DBCH',0x04:'HD0',0x05:'HD1',
           0x06:'HD2',0x07:'HD3',0x08:'PTL',0x09:'PTH',0x0a:'CTRL0',0x0b:'CTRL1',
           0x0c:'PTL2',0x0d:'PTH2',0x0e:'CTRL2',0x0f:'WAL',0x10:'WAH',0x11:'DTACK',0x12:'DTRG'}
    if len(run) >= 10:
        n = cdcw_n if (cdcw_n is not None and cdcw_n < 5) else 5
        first = (cdcw_idx - n) % 5 if cdcw_idx is not None else 0
        print(f"[ring] CDC write ring ({n} kept of {cdcw_idx} total writes; chronological):")
        print("  #   reg        val  pc       pre2")
        for k in range(n):
            w = run[((first + k) % 5) * 2:((first + k) % 5) * 2 + 2]
            idx, val = w[0]&0xff, (w[0]>>8)&0xff
            pre2a, pre2b = (w[0]>>16)&0xff, (w[0]>>24)&0xff
            pc = w[1]
            r = 'LATCH' if idx == 0xfe else REG.get(idx, '?'+format(idx,'02x'))
            print(f"  {k:3d} {r:6s} <- {val:02x}  {pc:08x}  {pre2a:02x} {pre2b:02x}")
    else:
        print(f"[ring] CDC write ring: only {len(run)} words readable")
PY
}

mode8_probe() {
  # Read-only, two-point probe for the post-CDC mode-8 state machine. One
  # OpenOCD connection, one short run window, no injected IRQ or RAM writes.
  # Addresses come from the exact arm ELF; the only guest offsets below are
  # BIOS ABI locations ($d000 and MAINBOOT's A6=$97e8 workspace).
  local name=$1 window_ms=${2:-250} elf="$ARMS/$1/gw_retro_go.elf"
  local page0 wordram scd regs_off regs bank0 bank1 work cmd out raw report
  local frame done main_pc sub_pc boot state8 stamp mainstamp
  local cdd_ticks runsub runsub_ran gfx_ops gfx_lines gfx_mapnz
  local n_init n_reset n_release n_hold
  local m68k_core bios_ptr m68k_pc_addr
  test -f "$elf" || { echo "[mode8] arm not built: $name" >&2; return 1; }
  [[ "$window_ms" =~ ^[0-9]+$ ]] || { echo "[mode8] WINDOW_MS must be numeric" >&2; return 1; }

  page0=$(sym "$elf" scd_prg_page_axi)
  wordram=$(sym "$elf" scd_word_ram)
  scd=$(sym "$elf" SCD)
  frame=$(sym "$elf" scd_dbg_frame)
  done=$(sym "$elf" scd_dbg_mframe_done)
  main_pc=$(sym "$elf" scd_dbg_main_pc)
  sub_pc=$(sym "$elf" scd_dbg_sub_pc)
  boot=$(sym "$elf" scd_boot_mode)
  state8=$(sym "$elf" scd_dbg_state8_hits)
  stamp=$(sym "$elf" scd_dbg_stampwr_hits)
  mainstamp=$(sym "$elf" scd_dbg_mainstamp_hits)
  cdd_ticks=$(sym "$elf" scd_dbg_cdd_ticks)
  runsub=$(sym "$elf" scd_dbg_n_runsub)
  runsub_ran=$(sym "$elf" scd_dbg_n_runsub_ran)
  gfx_ops=$(sym "$elf" scd_dbg_gfx_ops)
  gfx_lines=$(sym "$elf" scd_dbg_gfx_lines)
  gfx_mapnz=$(sym "$elf" scd_dbg_gfx_mapnz)
  n_init=$(sym "$elf" scd_dbg_n_init)
  n_reset=$(sym "$elf" scd_dbg_n_reset)
  n_release=$(sym "$elf" scd_dbg_n_release)
  n_hold=$(sym "$elf" scd_dbg_n_hold)
  # The Sega CD overlay renames the shared gwenesis core; plain `m68k`
  # belongs to another overlay at the same VMA and is not live here.
  m68k_core=$(sym "$elf" segacd__m68k)
  bios_ptr=$(sym "$elf" segacd_bios)
  for a in "$page0" "$wordram" "$scd" "$frame" "$done" "$main_pc" \
           "$sub_pc" "$boot" "$state8" "$stamp" "$mainstamp" "$cdd_ticks" \
           "$runsub" "$runsub_ran" "$gfx_ops" "$gfx_lines" "$gfx_mapnz"; do
    test -n "$a" || { echo "[mode8] missing ELF symbol" >&2; return 1; }
  done
  for a in "$n_init" "$n_reset" "$n_release" "$n_hold"; do
    test -n "$a" || { echo "[mode8] missing lifecycle symbol" >&2; return 1; }
  done
  test -n "$m68k_core" && test -n "$bios_ptr" || {
    echo "[mode8] missing main CPU symbols" >&2; return 1;
  }
  m68k_pc_addr=$(printf '0x%x' $((m68k_core + 5204)))

  # Refuse an unexpected DWARF layout instead of silently measuring the
  # wrong structure with a stale hard-coded offset.
  regs_off=$(arm-none-eabi-readelf --debug-dump=info "$elf" 2>/dev/null |
    awk '/DW_AT_name.*s68k_regs/{seen=1} seen && !found && /DW_AT_data_member_location/{print $NF; found=1}')
  test "$regs_off" = 8232 || {
    echo "[mode8] unexpected s68k_regs offset: ${regs_off:-missing}" >&2; return 1;
  }
  regs=$(printf '0x%x' $((scd + regs_off)))
  work=$(printf '0x%x' $((page0 + 0x97e8)))
  bank0=$(printf '0x%x' $((wordram + 0x400)))
  bank1=$(printf '0x%x' $((wordram + 0x20400)))
  raw="$MODE8.$name.raw"
  report="$MODE8.$name.tsv"

  cmd="$OC -c init -c halt"
  for phase in PRE POST; do
    cmd+=" -c 'echo MODE8_${phase}'"
    for a in "$frame" "$done" "$main_pc" "$sub_pc" "$boot" \
             "$state8" "$stamp" "$mainstamp" "$cdd_ticks" "$runsub" \
             "$runsub_ran" "$gfx_ops" "$gfx_lines" "$gfx_mapnz"; do
      cmd+=" -c 'mdw $a'"
    done
    for a in "$n_init" "$n_reset" "$n_release" "$n_hold"; do cmd+=" -c 'mdw $a'"; done
    cmd+=" -c 'mdw $bios_ptr' -c 'mdw $m68k_core 5' -c 'mdw $m68k_pc_addr'"
    cmd+=" -c 'mdb 0x0000d000 64'"
    cmd+=" -c 'mdb $regs 104'"
    cmd+=" -c 'mdb $work 96'"
    cmd+=" -c 'mdb $bank0 96'"
    cmd+=" -c 'mdb $bank1 96'"
    if [[ "$phase" == PRE ]]; then
      cmd+=" -c resume -c 'sleep $window_ms' -c halt"
    fi
  done
  cmd+=" -c resume -c shutdown"
  out=$(ssh "$HOST" "$cmd" 2>&1)
  printf '%s\n' "$out" >"$raw"

  python3 - "$raw" "$report" "$name" "$window_ms" \
    "$frame" "$done" "$main_pc" "$sub_pc" "$boot" \
    "$state8" "$stamp" "$mainstamp" "$cdd_ticks" "$runsub" \
    "$runsub_ran" "$gfx_ops" "$gfx_lines" "$gfx_mapnz" "$n_init" "$n_reset" \
    "$n_release" "$n_hold" "$bios_ptr" "$m68k_core" "$m68k_pc_addr" \
    "$regs" "$work" <<'PY'
import re, sys
(raw_path, report, name, window_ms, *args) = sys.argv[1:]
keys = ('frame','done','main_pc','sub_pc','boot_mode','state8','stamp','mainstamp',
        'cdd_ticks','runsub','runsub_ran','gfx_ops','gfx_lines','gfx_mapnz',
        'n_init','n_reset','n_release','n_hold')
addrs = {k:int(v,16) for k,v in zip(keys,args[:len(keys)])}
bios_ptr_addr, m68k_core, m68k_pc_addr, regs, work = map(
    lambda x:int(x,16), args[len(keys):])
text = open(raw_path).read()
parts = re.split(r'MODE8_(PRE|POST)\s*\n', text)
sections = {parts[i]:parts[i+1] for i in range(1,len(parts)-1,2)}

def words(sec):
    out = {}
    for m in re.finditer(r'^0x([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{8}\s*)+)$', sec, re.M):
        a = int(m.group(1),16)
        for x in m.group(2).split():
            out[a] = int(x,16); a += 4
    return out

def bytes_(sec):
    out = {}
    for m in re.finditer(r'^0x([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s*)+)$', sec, re.M):
        a = int(m.group(1),16)
        for x in m.group(2).split():
            out[a] = int(x,16); a += 1
    return out

rows=[]
snap={}
for phase in ('PRE','POST'):
    if phase not in sections: raise SystemExit(f'[mode8] missing {phase} marker')
    w,b = words(sections[phase]),bytes_(sections[phase])
    vals={k:w.get(a) for k,a in addrs.items()}
    vals.update({'bios_ptr':w.get(bios_ptr_addr),
                 'main_map0_base':w.get(m68k_core),
                 'main_map0_read8':w.get(m68k_core+4),
                 'main_map0_read16':w.get(m68k_core+8),
                 'main_ctx_pc':w.get(m68k_pc_addr)})
    # Direct 68K RAM is word-pair swapped for guest byte accesses.
    vals.update({
      'd007': b.get(0xd007 ^ 1),
      'reg3': b.get(regs + 3),
      'comm0e': b.get(regs + 0x0e), 'comm0f': b.get(regs + 0x0f),
      'mb_flag3': b.get(work + (3 ^ 1)),
      'mb_ready6': b.get(work + (6 ^ 1)),
      'mb_frames': (b.get(work+5,0)<<8) | b.get(work+4,0),
    })
    snap[phase]=vals
    for k,v in vals.items(): rows.append((phase,k,'READ_ERROR' if v is None else f'0x{v:x}'))

for k in ('frame','done','state8','stamp','mainstamp','mb_frames','cdd_ticks',
          'runsub','runsub_ran','gfx_ops','gfx_lines'):
    a,z=snap['PRE'][k],snap['POST'][k]
    rows.append(('DELTA',k,'READ_ERROR' if a is None or z is None else str((z-a)&0xffffffff)))
df = (snap['POST']['frame'] - snap['PRE']['frame']) & 0xffffffff
rows.append(('DERIVED','fps',f'{df * 1000 / int(window_ms):.2f}'))
with open(report,'w') as f:
    for row in rows: f.write('\t'.join((name,*row))+'\n')
print(f'[mode8] {name}, window={window_ms}ms')
for row in rows: print('\t'.join(row))
print(f'[mode8] raw={raw_path} report={report}')
PY
}

bios_check() {
  local name=$1 elf="$ARMS/$1/gw_retro_go.elf" bios_sym out ptr
  local remote=/tmp/segacd_bios.live.bin local_dump="$MODE8.$name.bios.bin"
  test -f "$elf" || { echo "[bioscheck] arm not built: $name" >&2; return 1; }
  bios_sym=$(sym "$elf" segacd_bios)
  test -n "$bios_sym" || { echo "[bioscheck] missing segacd_bios symbol" >&2; return 1; }
  out=$(ssh "$HOST" "$OC -c init -c halt -c 'mdw $bios_sym' -c resume -c shutdown" 2>&1)
  ptr=$(printf '%s\n' "$out" | grep -iE "^0x?${bios_sym#0x}:" | tail -1 | awk '{print $2}')
  [[ "$ptr" =~ ^9[0-9a-fA-F]{7}$ ]] || {
    printf '%s\n' "$out"; echo "[bioscheck] invalid XIP pointer: ${ptr:-missing}" >&2; return 1;
  }
  ssh "$HOST" "$OC -c init -c halt -c 'dump_image $remote 0x$ptr 0x20000' -c resume -c shutdown" >/dev/null 2>&1
  scp -q "$HOST:$remote" "$local_dump"
  echo "[bioscheck] live ptr=0x$ptr file=$local_dump"
  sha256sum "$local_dump"
  local candidate
  for candidate in /tmp/bios_CD_U.bin /tmp/scd/bios_CD_U.bin /tmp/opencode/bios_probe_u.bin; do
    if test -f "$candidate"; then
      sha256sum "$candidate"
      if cmp -s "$local_dump" "$candidate"; then
        echo "[bioscheck] MATCH $candidate"
      else
        echo "[bioscheck] DIFFER $candidate"
      fi
    fi
  done
}

derail_dump() {
  local name=$1 elf="$ARMS/$1/gw_retro_go.elf"
  local ring shot cnt hits seen cmd out raw="$MODE8.$name.derail.raw"
  test -f "$elf" || { echo "[derail] arm not built: $name" >&2; return 1; }
  ring=$(sym "$elf" scd_derail_ring)
  shot=$(sym "$elf" scd_derail_shot)
  cnt=$(sym "$elf" scd_derail_count)
  hits=$(sym "$elf" scd_derail_hits)
  seen=$(sym "$elf" scd_derail_seen)
  for n in "$ring" "$shot" "$cnt"; do
    [ -n "$n" ] || { echo "[derail] missing symbol (arm built without SEGACD_DERAIL=1?)" >&2; return 1; }
  done
  # ring: 32 entries * 4 words; shot: 18 words; counters: 1 word each
  cmd="$OC -c init -c halt"
  cmd+=" -c 'mdw $ring 128'"
  cmd+=" -c 'mdw $shot 18'"
  cmd+=" -c 'mdw $cnt'"
  [ -n "$hits" ] && cmd+=" -c 'mdw $hits'"
  [ -n "$seen" ] && cmd+=" -c 'mdw $seen'"
  cmd+=" -c resume -c shutdown"
  out=$(ssh "$HOST" "$cmd" 2>&1)
  printf '%s\n' "$out" >"$raw"
  python3 - "$raw" "$ring" "$shot" "$cnt" "$hits" "$seen" <<'PY'
import re, sys
raw, ring, shot, cnt, hits, seen = sys.argv[1:7]
text = open(raw).read()
words = {}   # addr -> [words...]
for m in re.finditer(r'^0x?([0-9a-fA-F]+):\s+((?:0x[0-9a-fA-F]+|[0-9a-fA-F]{4,8}\s*)+)$', text, re.M):
    base = int(m.group(1), 16)
    vals = [int(x, 16) for x in m.group(2).split()]
    for i, v in enumerate(vals):
        words[base + 4*i] = v
def block(base, n):
    out = []
    for i in range(n):
        w = words.get(base + 4*i)
        if w is None: break
        out.append(w)
    return out
print("== derail ring (oldest first) ==")
rw = block(int(ring,16), 128)
for i in range(0, len(rw)-3, 4):
    t_pc, d0, a6, prev = rw[i:i+4]
    tag = chr(t_pc >> 24) if t_pc >> 24 else '?'
    pc = t_pc & 0xffffff
    print(f"  [{i//4:2d}] {tag} pc=${pc:06x} prev=${prev:06x} d0=${d0:08x} a6=${a6:06x}")
print("== frozen shot (first data-zone entry) ==")
sw = block(int(shot,16), 18)
if len(sw) >= 18:
    pc, prev = sw[0], sw[1]
    print(f"  pc=${pc:06x} prev={prev:06x}")
    print("  d0-d7: " + " ".join(f"{x:08x}" for x in sw[2:10]))
    print("  a0-a7: " + " ".join(f"{x:06x}" for x in sw[10:18]))
else:
    print("  (not captured)")
cw = block(int(cnt,16), 1)
print(f"count={cw[0] if cw else '?'}", end="")
if hits:
    hw = block(int(hits,16), 1)
    print(f" hits={hw[0] if hw else '?'}", end="")
if seen:
    ew = block(int(seen,16), 1)
    print(f" seen={ew[0] if ew else '?'}", end="")
print()
PY
}

run_one() {
  local axis=$1 name elf
  name=$(arm_name "$axis")
  elf="$ARMS/$name/gw_retro_go.elf"
  test -f "$elf" || { echo "[segacd-ab] arm not built: $name" >&2; return 1; }

  echo "[segacd-ab] flash $name + matching segacd.bin/segacd.xip"
  PUSH_CORE=segacd PUSH_HB=0 bash "$ARM_TOOL" flash "$name"
  echo "[segacd-ab] autoboot $name for ${RUN_SECONDS}s, then one SWD snapshot"
  sleep "$RUN_SECONDS"
  if [[ "$axis" == interleave_w1 ]]; then snapshot_lite "$name";
  elif [[ "$axis" == interleave_w1d ]]; then derail_dump "$name";
  elif [[ "$axis" == interleave_w1e ]]; then snapshot_lite "$name";
  else snapshot "$name"; fi
}

push_selector() {
  local rom=$1 basename selector
  basename=${rom##*/}
  # ROM-list display names have their extension removed by
  # emulator_add_rom_file(); gnw_autoboot_named() compares that field.
  selector="segacd:${basename%.*}"
  printf '%s\n' "$selector" >/tmp/snes_bench_index.txt
  scp -q /tmp/snes_bench_index.txt "$HOST:/tmp/"
  ssh -n "$HOST" "python3 -m gnwmanager sdpush --file /tmp/snes_bench_index.txt --dest-path '/'" \
    >/dev/null 2>&1
  echo "[segacd-ab] selector: $selector"
}

cmd=${1:-}
case "$cmd" in
  build)
    build_one baseline
    build_one hs
    ;;
  build_dt)
    build_one dt
    ;;
  build_baseline)
    build_one baseline
    ;;
  build_int)
    build_one interleave
    ;;
  build_b4)
    build_one interleave_b4
    ;;
  build_w1)
    build_one interleave_w1
    ;;
  build_w1d)
    build_one interleave_w1d
    ;;
  build_w1e)
    build_one interleave_w1e
    ;;
  run_baseline)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one baseline
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run)
    rom=${2:?usage: segacd_ab.sh run /roms/segacd/.../game.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one baseline
    run_one hs
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run_dt)
    rom=${2:?usage: segacd_ab.sh run_dt /roms/segacd/.../game.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one dt
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run_int)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one interleave
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run_b4)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one interleave_b4
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run_w1)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one interleave_w1
    echo "[segacd-ab] results: $RESULTS"
    ;;
  run_w1d)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one interleave_w1d
    ;;
  run_w1e)
    rom=${2:-/roms/segacd/orgun/orgun.cue}
    : >"$RESULTS"
    push_selector "$rom"
    run_one interleave_w1e
    echo "[segacd-ab] results: $RESULTS"
    ;;
  all)
    rom=${2:?usage: segacd_ab.sh all /roms/segacd/.../game.cue}
    build_one baseline
    build_one hs
    : >"$RESULTS"
    push_selector "$rom"
    run_one baseline
    run_one hs
    echo "[segacd-ab] results: $RESULTS"
    ;;
  snapshot)
    snapshot "${2:?usage: segacd_ab.sh snapshot ARM_NAME}"
    ;;
  ring)
    ring_dump "${2:?usage: segacd_ab.sh ring ARM_NAME}"
    ;;
  mode8)
    mode8_probe "${2:?usage: segacd_ab.sh mode8 ARM_NAME [WINDOW_MS]}" "${3:-250}"
    ;;
  derail)
    derail_dump "${2:?usage: segacd_ab.sh derail ARM_NAME}"
    ;;
  bioscheck)
    bios_check "${2:?usage: segacd_ab.sh bioscheck ARM_NAME}"
    ;;
  gfxprobe)
    gfx_probe "${2:?usage: segacd_ab.sh gfxprobe ARM_NAME [WINDOW_MS]}" "${3:-1000}"
    ;;
  *)
    sed -n '2,18p' "$0"
    exit 1
    ;;
esac
