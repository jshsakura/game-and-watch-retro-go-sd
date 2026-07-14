#!/usr/bin/env bash
# The M4A mixer HLE is only worth anything if it is WIRED — and every part of
# that wiring can silently rot while the build stays green:
#
#   - Drop -DGBA_M4A_HLE and every m4a file still compiles and links; cpu.o just
#     never calls it. The device boots, runs, and quietly interprets 27-60% more
#     guest instructions than it has to. Nothing anywhere would say a word.
#     (This repo's signature failure mode: Super Metroid never called
#     common_emu_frame_loop(); the clock never asked the idle timeout. The
#     functions were fine. See test_idle_timeout_wired.sh.)
#   - Put m4a_hle.o's 102 KB of .text in RAM and the GBA overlay stops linking;
#     put its rodata in the flash blob and the first memcmp reads a sentinel.
#     The split is a list of filenames in a linker script, and a list is easy
#     to edit wrong. (test_gba_xip_contract.sh proves cpu.o reaches no
#     sentinel; THIS test proves each m4a piece is on its intended side.)
#
# So, against the linked image:
#   (1) cpu.o (ITCM) actually reaches the hook: the ITCM image holds the
#       address of m4a_hle_execute (through its veneer) and of m4a_hook_pc.
#   (2) main.o actually scans: the overlay's code calls m4a_hle_scan_frame
#       and m4a_hle_reset.
#   (3) The six transliterations live in the blob; the glue, the variant
#       table and the six signatures live in overlay RAM.
#   (4) The variant table itself is sound: NULL-terminated, every entry's
#       code pointer in RAM, every entry's run pointer in the blob (i.e. a
#       sentinel the load-time pass will patch).
#
# Same SKIP rules as test_gba_xip_contract.sh: no toolchain or no ELF is a
# SKIP, not a FAIL — a safety net that breaks the build teaches people to
# ignore CI.
set -u
cd "$(dirname "$0")/.."

ELF="${ELF:-build/gw_retro_go.elf}"
NM="${NM:-arm-none-eabi-nm}"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"
OBJDUMP="${OBJDUMP:-arm-none-eabi-objdump}"

if ! command -v "$NM" >/dev/null 2>&1 || ! command -v "$OBJCOPY" >/dev/null 2>&1 \
   || ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo "SKIP: no arm-none-eabi toolchain on PATH — cannot inspect the linked image"
    exit 0
fi
if [ ! -f "$ELF" ]; then
    echo "SKIP: $ELF not built — run 'make release DOCKER=1 <flags>' first"
    exit 0
fi
if ! "$NM" "$ELF" | grep -q " _GBA_MAIN_CODE_END$"; then
    echo "SKIP: this ELF has no GBA core (SD_CARD=0, or the core is not on this branch)"
    exit 0
fi

python3 - "$ELF" "$NM" "$OBJCOPY" "$OBJDUMP" <<'PY'
import subprocess, struct, sys, tempfile, os

elf, nm, objcopy, objdump = sys.argv[1:5]
BASE = 0xDEC00000

syms = {}
for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

rc = 0
def fail(*lines):
    global rc
    print("FAIL: " + lines[0])
    for l in lines[1:]:
        print("      " + l)
    rc = 1

# The feature must be present at all. If the GBA core is here but the HLE is
# not, someone unwired it — that is exactly what this test exists to say.
need = ['m4a_hle_execute', 'm4a_hook_pc', 'm4a_variants', 'm4a_scan',
        '__ram_emu_gba_start__', '__ram_emu_gba_end__',
        '__xip_gba_start__', '__xip_gba_end__']
missing = [s for s in need if s not in syms]
if missing:
    print("FAIL: GBA core is in this ELF but the M4A HLE is not: missing %s"
          % ", ".join(missing))
    print("      The two m4a files left GBA_C_SOURCES, or their symbols were renamed.")
    sys.exit(1)

ram_lo, ram_hi = syms['__ram_emu_gba_start__'], syms['__ram_emu_gba_end__']
blob_len = syms['__xip_gba_end__'] - syms['__xip_gba_start__']
def in_ram(a):  return ram_lo <= a < ram_hi
def in_blob(a): return BASE <= (a & ~1) < BASE + blob_len

def section(name):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        tmp = f.name
    subprocess.run([objcopy, '-O', 'binary', '--only-section=' + name, elf, tmp], check=True)
    data = open(tmp, 'rb').read()
    os.unlink(tmp)
    return data

# --- (3) each piece on its intended side -----------------------------------
RUNS = [s for s in syms if s.startswith('m4a_run_')]
if len(RUNS) < 6:
    fail("expected the six m4a_run_* transliterations, found %d: %s"
         % (len(RUNS), ", ".join(sorted(RUNS)) or "none"))
for s in RUNS:
    if not in_blob(syms[s]):
        fail("%s is at 0x%08x — not in the flash blob." % (s, syms[s]),
             "102 KB of transliteration in RAM is how the overlay stops linking;",
             "the .xip_gba list in STM32H7B0VBTx_SDCARD.ld must claim m4a_hle.o (.text).")

SIGS = [s for s in syms if s.startswith('m4a_code_')]
if len(SIGS) < 6:
    fail("expected six m4a_code_* signature arrays, found %d" % len(SIGS))
for s in SIGS + ['m4a_variants', 'm4a_hle_execute', 'm4a_hle_scan_frame', 'm4a_hook_pc']:
    if not in_ram(syms[s]):
        fail("%s is at 0x%08x — not in overlay RAM." % (s, syms[s]),
             "The per-frame scan memcmps the signatures and cpu.o loads the glue;",
             "in the blob they are sentinels nobody patches for these readers.",
             "Check the m4a lines inside .overlay_gba in STM32H7B0VBTx_SDCARD.ld.")

# --- (1) cpu.o, in ITCM, actually reaches the hook --------------------------
# cpu.o compares every dispatched PC against m4a_hook_pc and calls
# m4a_hle_execute through a linker veneer. Both addresses must therefore
# appear as literal words inside the ITCM image. If -DGBA_M4A_HLE is dropped
# from C_DEFS_GBA, both vanish — and nothing else in the build notices.
itc = section('.overlay_gba_itc')
words = set(struct.unpack_from('<%dI' % (len(itc) // 4), itc, 0))
if syms['m4a_hook_pc'] not in words:
    fail("cpu.o's ITCM image never references m4a_hook_pc.",
         "The interpreter is not checking for the mixer: -DGBA_M4A_HLE is off,",
         "or the hook in external/gpsp cpu.cc is gone. The HLE is dead weight.")
if (syms['m4a_hle_execute'] | 1) not in words and syms['m4a_hle_execute'] not in words:
    fail("cpu.o's ITCM image holds no address for m4a_hle_execute.",
         "The PC check may exist but the call does not — same causes as above.")

# --- (2) the per-frame scan is called --------------------------------------
# main.o's frame boundary must call m4a_hle_scan_frame, and reset_gba must
# call m4a_hle_reset: a mixer nobody scans for is never found, and a stale
# hook across reset points into rewritten IWRAM.
dis = subprocess.run([objdump, '-d', '--section=.overlay_gba', elf],
                     capture_output=True, text=True).stdout
for callee, why in [('m4a_hle_scan_frame',
                     "nobody scans IWRAM, the mixer is never found, the hook never fires"),
                    ('m4a_hle_reset',
                     "a hook surviving reset_gba points into rewritten IWRAM")]:
    if ('<%s>' % callee) not in dis.replace('<%s>:' % callee, '', 1):
        fail("no call to %s anywhere in the overlay." % callee, why + ".")

# --- (4) the variant table is sound -----------------------------------------
ov = section('.overlay_gba')
def ov_word(vma):
    off = vma - syms['__ram_emu_gba_start__']
    if not (0 <= off <= len(ov) - 4):
        return None
    return struct.unpack_from('<I', ov, off)[0]

n = 0
p = syms['m4a_variants']
seen_runs = set()
while True:
    v = ov_word(p)
    if v is None:
        fail("m4a_variants ran off the end of the overlay image at 0x%08x" % p)
        break
    if v == 0:
        break
    # m4a_variant: {name, code, size, exit_off, run} — five words.
    name_p, code_p = ov_word(v), ov_word(v + 4)
    size, run_p = ov_word(v + 8), ov_word(v + 16)
    if not in_ram(v) or name_p is None:
        fail("variant table entry %d points outside overlay RAM (0x%08x)" % (n, v))
        break
    if not in_ram(code_p) or not (0 < size <= 4096):
        fail("variant %d: signature pointer 0x%08x / size %d is not sane RAM rodata"
             % (n, code_p, size),
             "m4a_identify memcmps this every scan frame; in the blob it is a sentinel.")
    if not in_blob(run_p):
        fail("variant %d: run pointer 0x%08x is not a blob sentinel." % (n, run_p),
             "Either the transliteration moved into RAM (see the size check above)",
             "or the table is corrupt. The load-time pass patches exactly the",
             "sentinel range; anything else is left pointing at nonsense.")
    seen_runs.add(run_p & ~1)
    n += 1
    p += 4
if n < 6:
    fail("the variant table holds %d entries; the census says six." % n)
if rc == 0:
    print("ok   cpu.o (ITCM) references m4a_hook_pc and calls m4a_hle_execute")
    print("ok   the overlay calls m4a_hle_scan_frame and m4a_hle_reset")
    print("ok   %d transliterations in the blob, glue + %d signatures in RAM" % (len(RUNS), len(SIGS)))
    print("ok   variant table: %d entries, every run pointer a patchable sentinel" % n)
sys.exit(rc)
PY
rc=$?

if [ $rc -eq 0 ]; then
    echo "PASS: tests/test_gba_m4a_wired.sh"
else
    echo "FAIL: tests/test_gba_m4a_wired.sh"
fi
exit $rc
