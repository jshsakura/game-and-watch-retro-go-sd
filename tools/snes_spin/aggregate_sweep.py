#!/usr/bin/env python3
"""Aggregate a spin-sweep TSV: does the spin-skip lever generalize?"""
import sys
from statistics import median, mean

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/snes_spin_sweep.tsv"
rows = []
for line in open(path, encoding="utf-8", errors="replace"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 8:
        continue
    rows.append(dict(rom=p[0], status=p[1], lit=p[2], pure=p[3], io=p[4],
                     wai=p[5], site=p[6], polls=p[7]))

ok = [r for r in rows if r["status"] == "OK"]
unr = [r for r in rows if r["status"] == "UNRENDERED"]
crash = [r for r in rows if r["status"] in ("BOOT_CRASH", "LOAD_FAIL", "TIMEOUT")]

def pf(r): return float(r["pure"])

BUCKETS = [(0, 10), (10, 30), (30, 50), (50, 70), (70, 90), (90, 101)]
def hist(rs):
    out = []
    for lo, hi in BUCKETS:
        n = sum(1 for r in rs if lo <= pf(r) < hi)
        out.append((f"{lo}-{hi-1 if hi<101 else 100}%", n))
    return out

print(f"total={len(rows)}  OK(rendered)={len(ok)}  UNRENDERED={len(unr)}  CRASH/LOAD/TIMEOUT={len(crash)}")
print("\nPURE-spin%% histogram (rendered ROMs):")
for label, n in hist(ok):
    print(f"  {label:>8} {n:4d}  {'#'*n}")
print("\nPURE-spin%% histogram (UNRENDERED, boot-phase numbers, informational):")
for label, n in hist(unr):
    print(f"  {label:>8} {n:4d}  {'#'*n}")

vals = sorted(pf(r) for r in ok)
print(f"\nrendered: median={median(vals):.1f}%  mean={mean(vals):.1f}%")
ge50 = [r for r in ok if pf(r) >= 50]
lt10 = [r for r in ok if pf(r) < 10]
print(f">=50%% spin (big winners): {len(ge50)}/{len(ok)} = {100*len(ge50)/len(ok):.0f}%")
print(f"<10%% spin (hint-gate must-handle): {len(lt10)}/{len(ok)} = {100*len(lt10)/len(ok):.0f}%")

def poll_class(r):
    a = r["polls"]
    if a in ("-", ""):
        return "none"
    v = int(a, 16)
    bank, off = v >> 16, v & 0xFFFF
    if bank in (0x7E, 0x7F) or (off < 0x2000 and (bank < 0x40 or 0x80 <= bank < 0xC0)):
        return "WRAM/DP flag"
    if 0x2140 <= off <= 0x217F:
        return "APU port"
    if off >= 0x8000 or (0x40 <= bank < 0x7E) or bank >= 0xC0:
        return "ROM"
    return "other IO"

print("\ntop-site polled address class (rendered):")
from collections import Counter
c = Counter(poll_class(r) for r in ok)
for k, n in c.most_common():
    print(f"  {k:>14} {n:4d}")

apu_pollers = [r for r in ok if poll_class(r) == "APU port"]
print(f"\nAPU-port pollers (sound-HLE coupling — SPC700/HLE must still answer): {len(apu_pollers)}")
for r in apu_pollers:
    print(f"  {r['rom'][:52]:<52} pure={r['pure']}% io={r['io']}%")

print("\n<10%% spin rendered ROMs (tracker-overhead risk set):")
for r in sorted(lt10, key=pf):
    print(f"  {r['rom'][:52]:<52} pure={r['pure']}% io={r['io']}% polls={r['polls']}")

print("\nWAI users (wai>0):")
wu = [r for r in ok if float(r["wai"]) > 0]
for r in sorted(wu, key=lambda x: -float(x["wai"]))[:15]:
    print(f"  {r['rom'][:52]:<52} wai/frame={r['wai']} pure={r['pure']}%")
print(f"  ({len(wu)} rendered ROMs use WAI)")

print("\ntop-spin rendered (>=70%):")
for r in sorted(ok, key=pf, reverse=True):
    if pf(r) < 70: break
    print(f"  {r['rom'][:52]:<52} pure={r['pure']}% polls={r['polls']}")
