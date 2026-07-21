#!/usr/bin/env bash
# The GBA core's flash-XIP split is a contract, and it is one the compiler cannot
# check. This asserts it against the linked image.
#
# The contract (STM32H7B0VBTx_SDCARD.ld, ".xip_gba"):
#
#   cpu.o — gpSP's ARM7 interpreter — runs from ITCM. The sentinel pass in
#   main_gba.c walks the RAM overlay and rewrites every 0xDEC0xxxx word to where
#   the blob really landed. It does NOT walk ITCM. Therefore nothing cpu.o
#   references may live in the blob: an unrewritten sentinel is a jump, or a load,
#   to 0xDEC0xxxx, and the device dies on the first frame.
#
# Today that holds because nm says cpu.o calls only gba_memory.o, main.o, cheats.o
# and savestate.o, and reads no rodata but its own — so the linker script keeps
# exactly those in RAM. But the split is a list of filenames in a linker script.
# Add a source file to the wrong list, or let gpSP grow a call from cpu.cc into
# video.cc, and the build stays green while the device stops booting. Nothing else
# in the tree would say a word.
#
# So: count the sentinels. In the linked ELF there are exactly three places they
# can be, and each has a number that must not change.
#
#   .overlay_gba_itc            0   — cpu.o, in ITCM, never scanned
#   main_gba.o's window         1   — open_gba_bios_rom, relocated by hand
#                                     (gba_xip_ptr(); the window is skipped because
#                                     GBA_CODE_BASE is a literal inside it)
#   the rest of .overlay_gba   >0   — patched at load; zero would mean the pass
#                                     has quietly become a no-op
#
# Needs a linked ELF and the cross toolchain. The host-tests CI job has neither
# ("Cheap, no toolchain: plain gcc" — .github/workflows/package.yml), so this
# SKIPS there rather than failing the build. A safety net that breaks the build
# teaches people to ignore CI; see test_check_core_symbol_aliases.sh, same lesson.
set -u
cd "$(dirname "$0")/.."

ELF="${ELF:-build/gw_retro_go.elf}"
NM="${NM:-arm-none-eabi-nm}"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"

if ! command -v "$NM" >/dev/null 2>&1 || ! command -v "$OBJCOPY" >/dev/null 2>&1; then
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

python3 - "$ELF" "$NM" "$OBJCOPY" <<'PY'
import subprocess, struct, sys, tempfile, os

elf, nm, objcopy = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = 0xDEC00000

syms = {}
for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

need = ['_GBA_MAIN_CODE_START', '_GBA_MAIN_CODE_END',
        '__ram_emu_gba_start__', '__xip_gba_start__', '__xip_gba_end__',
        'gba__open_gba_bios_rom']
missing = [s for s in need if s not in syms]
if missing:
    print("FAIL: the linked image is missing %s" % ", ".join(missing))
    sys.exit(1)

blob_len = syms['__xip_gba_end__'] - syms['__xip_gba_start__']
start, end = syms['_GBA_MAIN_CODE_START'], syms['_GBA_MAIN_CODE_END']
ov_vma = syms['__ram_emu_gba_start__']
bios = syms['gba__open_gba_bios_rom']

def section(name):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        tmp = f.name
    subprocess.run([objcopy, '-O', 'binary', '--only-section=' + name, elf, tmp], check=True)
    data = open(tmp, 'rb').read()
    os.unlink(tmp)
    return data

def sentinels(buf, vma):
    """Every word in buf that points into the blob, as (address, value)."""
    out = []
    for i in range(0, len(buf) - 3, 4):
        v = struct.unpack_from('<I', buf, i)[0]
        if BASE <= (v & ~1) < BASE + blob_len:
            out.append((vma + i, v))
    return out

rc = 0

# (1) ITCM: the pass cannot reach here, so there must be nothing here to reach.
itc = sentinels(section('.overlay_gba_itc'), 0)
if itc:
    print("FAIL: %d sentinel word(s) in .overlay_gba_itc (cpu.o, ITCM)." % len(itc))
    print("      The sentinel pass does not scan ITCM, so these stay pointing at")
    print("      the blob's link address and the device faults on the first frame.")
    print("      Something cpu.o references was moved into the blob — check the")
    print("      .xip_gba / .rodata_gba lists against: nm -u build/gba/cpu.o")
    for a, v in itc[:8]:
        print("        ITCM+0x%04x = 0x%08x" % (a, v))
    rc = 1
else:
    print("ok   .overlay_gba_itc: 0 sentinels (cpu.o references nothing in the blob)")

# (2) and (3): the overlay, split at main_gba.o's skipped window.
ov = sentinels(section('.overlay_gba'), ov_vma)
skipped = [(a, v) for a, v in ov if start <= a < end]
scanned = [(a, v) for a, v in ov if not (start <= a < end)]

if len(skipped) != 1 or skipped[0][1] != bios:
    print("FAIL: main_gba.o's window must hold exactly one sentinel, the BIOS.")
    print("      It is skipped by the pass (GBA_CODE_BASE is a literal in it), so")
    print("      anything else in there is never relocated. Relocate it by hand with")
    print("      gba_xip_ptr(), or keep it out of the blob.")
    print("      expected: 1 word == 0x%08x (gba__open_gba_bios_rom)" % bios)
    print("      found:    %d word(s)" % len(skipped))
    for a, v in skipped:
        print("        0x%08x = 0x%08x%s" % (a, v, "" if v == bios else "   <-- unhandled"))
    rc = 1
else:
    print("ok   main_gba.o window: 1 sentinel, and it is open_gba_bios_rom")

if not scanned:
    print("FAIL: the scanned range of .overlay_gba holds no sentinels at all.")
    print("      Either nothing in RAM calls into the blob any more (so the blob is")
    print("      dead weight), or the layout moved and the pass is now a no-op.")
    rc = 1
else:
    print("ok   .overlay_gba scanned range: %d sentinels, all patched at load" % len(scanned))

sys.exit(rc)
PY
rc=$?

if [ $rc -eq 0 ]; then
    echo "PASS: tests/test_gba_xip_contract.sh"
else
    echo "FAIL: tests/test_gba_xip_contract.sh"
fi
exit $rc
