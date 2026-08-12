#!/usr/bin/env python3
"""Turn a spin sweep into the shortlist a baked table would actually carry.

    rank_candidates.py <sweep.tsv> [--min-prize 0.3]

The sweep's `pure%` is the share of executed opcodes that are replayable spin.
It is not a prize on its own, for two reasons this session measured on hardware:

  * The whole 65816 interpreter is worth **+2.5 emulated fps** (frozen against a
    running baseline). Replaying a share of its opcodes cannot be worth more
    than that share of 2.5, so prize ~= 2.5 * pure%.

  * The loop is paced by the audio DMA at 60.15 fps and **everything saturates
    at 59.7-60.0** — freezing both engines reaches 60.03. A ROM already at the
    cap converts its prize into nothing. Super Mario World spins heavily and
    gains zero for exactly this reason.

So a candidate needs a high pure% AND enough work per frame to be under the cap.
`ops/frame` is the proxy for the second: the reference points are Zelda 3 (rain,
57 fps on the device) and anything materially above it.

The output groups by wait-loop signature rather than by ROM, because that is
what a table stores -- one entry per distinct loop, covering every cart that
shares it. The GBA M4A work folded 347 carts into 6 variants the same way.
"""
import sys
import collections

ZELDA_OPS = 13220.0     # opcodes/frame on the device's reference scene, ~57 fps
INTERP_FPS = 2.5        # ablation: the whole interpreter, emulated fps
CAP_FPS = 60.15


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    min_prize = 0.3
    if "--min-prize" in sys.argv:
        min_prize = float(sys.argv[sys.argv.index("--min-prize") + 1])

    rows = [l.rstrip("\n").split("\t") for l in open(path, encoding="utf-8")]
    ok = [r for r in rows if len(r) > 8 and r[1] == "OK"]
    status = collections.Counter(r[1] for r in rows if len(r) > 1)

    def f(x):
        try:
            return float(x)
        except ValueError:
            return 0.0

    cands = []
    for r in ok:
        name, pure, site, polls, ops = r[0], f(r[3]), r[6], r[7], f(r[8])
        prize = INTERP_FPS * pure / 100.0
        # Heavier than the reference scene means it is under the cap with room
        # for the prize to land; at or below it the cap eats the gain.
        under_cap = ops >= ZELDA_OPS
        cands.append((name, pure, ops, prize, under_cap, site, polls))

    print(f"{path}: {len(rows)} rows  {dict(status)}")
    print(f"prize = {INTERP_FPS} fps x pure%   (interpreter ablation, hardware)")
    print(f"under-cap proxy: ops/frame >= {ZELDA_OPS:.0f} (the device's reference scene)\n")

    live = [c for c in cands if c[4] and c[3] >= min_prize]
    print(f"pure% >= {100*min_prize/INTERP_FPS:.0f}%  AND under the cap: "
          f"{len(live)} of {len(ok)} playable ROMs\n")

    by_sig = collections.defaultdict(list)
    for c in live:
        by_sig[(c[5], c[6])].append(c)

    print(f"{'wait loop':>22}  {'polls':>9}  {'ROMs':>5}  {'median prize':>12}")
    print("-" * 58)
    for (site, polls), group in sorted(by_sig.items(), key=lambda kv: -len(kv[1]))[:20]:
        pr = sorted(g[3] for g in group)
        med = pr[len(pr) // 2]
        print(f"{site:>22}  {'$' + polls:>9}  {len(group):>5}  {med:>9.2f} fps")

    covered = sum(len(g) for g in sorted(by_sig.values(), key=len, reverse=True)[:10])
    print(f"\ntop 10 signatures cover {covered} of {len(live)} candidates "
          f"({100.0*covered/max(1,len(live)):.0f}%)")

    print("\nbest individual candidates:")
    for c in sorted(live, key=lambda c: -c[3])[:12]:
        print(f"  {c[3]:5.2f} fps  pure={c[1]:5.1f}%  ops/f={c[2]:8.0f}  {c[5]:>12}  {c[0][:52]}")


if __name__ == "__main__":
    main()
