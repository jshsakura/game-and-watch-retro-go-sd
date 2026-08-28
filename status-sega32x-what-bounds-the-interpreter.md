# Status: Sega 32X — what bounds the interpreter

 living document. Last update: 2026-08-27 — the dynarec axis closes (measured).

## The accounting that closed everything else

Anchors: 80,464 guest SH-2 insns/frame, 340MHz OC2, attract-class workload
(all published 32X fps numbers are the attract demo; gameplay runs ~70% of
attract per the 0817 record — real gameplay number still unmeasured).

| axis | verdict | evidence |
|---|---|---|
| SH-2 instruction count | **issue-bound** | addi experiment: adding one host ALU insn costs 0.84-1.03 cyc/insn (independent 0.84-0.99, dependent chain 1.03) — dual-issue absorbs only partially; throughput-limited, not latency |
| interpreter skeleton | no lever | skeleton ablation -1.22%; ppc store = wash; icount tail is load-bearing (mid-slice observers in 32x.c/memory.c) |
| guest memory traffic | 15.5-25% of frame | reads 87,086/f at 13.8cyc hit-path (dblrd), writes 11,766/f at 50.6cyc full path (dblwr), cold-line share 7.4cyc |
| dispatch | ≤16.2 cyc/insn upper bound, ~8 est. | dbra: one volatile indirect branch = -13.7% |
| OC clock ladder | closed at 340 | 312 < 340 > 348 > 353 (353 soaks hang, 348 is a regression) |
| audio | closed | production rate 55% of real time; audio is a dependent variable of speed |
| flicker | measured | swap race 1.10/frame (2-stage LTDC pipeline, phase walk); guard eliminates it at -26.2%; panel slowdown to 35Hz eliminates it at 0 fps cost (beam 119.4us/line trails the ~98us/line writer) |

## 2026-08-27: the dynarec axis closes — and why

Re-trial chain: the addi verdict (issue-bound) said removing ~55 interpreter
insns/guest-insn at ~0.9cyc each could buy 1.4-2.2x — IF the translated code
and its metadata fit the machine. Phase-0 (memory audit) and phase-0.5 (block
trace of the actual workload, 6.96M block runs over 540 frames, provisional
attract-class sample) answered the "if":

**The closure reason is not "thrash is bad" (arguable policy). It is that the
hot working set is an order of magnitude larger than this chip's executable
memory (hardware fact):**

    hot working set   872 blocks x 17.2 insn = 14,998 guest insns
    translated        @ 8B/insn = 117 KB
                      @12B/insn = 176 KB
                      @16B/insn = 234 KB
    executable memory the device can offer:
                      ITCM free 3.2 KB + Cz80_Exec eviction 17.5 KB ~= 21 KB
    -> shortfall      5.6x to 11.2x. No byte-per-insn estimate changes the verdict.

    even 90% coverage (179 blocks) = 36 KB — does not fit in 21 KB.
    what fits in 21 KB is ~104 blocks — between the 50% point (36) and the
    90% point (179).

And the reason no cache policy rescues this (measured, cache-sim on the trace):
the cache pressure is **hot-vs-hot, not cold-tail-vs-hot**. The remaining 10%
of executions (~700k runs) is spread over ~1,500 blocks at ~460 runs/block —
every threshold (K=8/32/128) lets them all in (ever_hot = 1242/995/872 blocks).
The distribution is smooth, not bimodal. The 90% CDF hid this — which is the
trap, registered below.

### Three times wrong on this axis (kept on purpose)

1. "No memory, impossible" — refuted by the hot-set measurement: the phase-0
   framing (64KB DTCM ceiling) missed ITCM and the Cz80 eviction lever.
2. "Payoff is a 42fps ceiling" — an extrapolation error: scaling the -1.22%
   result of removing 13% of instructions to removing 90% of them.
3. "Issue-bound, so it works" — correct (addi), but I never checked whether
   the lever FITS the machine (executable-memory constraint).

Lesson: **"how much does this lever earn" and "does this lever fit this
machine" are separate questions, and both must be answered before an axis
opens or closes.** Each time only one was answered.

### What stays alive from this investigation

The addi measurement (0.84-1.03 cyc per added host instruction = issue-bound)
is valid and valuable independent of the dynarec verdict. It means **on this
machine, instruction count genuinely is time** — it is the working basis for
any future interpreter optimization. Not deleted with the axis.

### Registered trap (65): execution-weighted CDF underestimates the working set

That "90% of runs = 179 blocks" does NOT mean "the tail is cold". A tail of
1,500 blocks at 460 runs each enters any cache and beats any threshold. To
ask for the working set, run a cache simulation, not a CDF.

### red-viper (Virtual Boy V810) — same axis, honest note

The VB dynarec is gated off for the same family of reasons (its drc_core.h
notes A32 codegen vs Thumb-only M7), and the working-set problem precedes the
A32 problem there too. BUT: the VB working set was **never measured**. The
honest entry is "must be re-measured the same way", not a copied conclusion.

### Provenance

Closure data: block trace of the attract-class provisional sample
(678,909B v3 savestate resume, 600 frames, 6.96M runs, 1,684 entry PCs,
100% ROM), LRU/threshold offline sim (blk_sim.py), metadata itemization from
compiler.c calloc sites (structs 36/24/48B ARM32). Full tables in
/tmp/gnw_bench_results.txt [BLOCK TRACE PROVISIONAL R2] and
[DRC PHASE-0 REDO]. The authoritative gameplay sample remains uncaptured;
the closure rests on the hardware fact (working set vs 21KB), which the
attract sample — the most favorable class — already exceeds by 5.6x.
