# 32X offline native block proof

Before selecting or rebuilding an optimization, read
[the failed and deferred approach list](../../docs/32X_FAILED_APPROACHES.md).
It includes the generic arithmetic span, all five short-AOT variants, and the
fixed-register Thumb candidate rejected after a 15.21453% hardware FPS loss.
The next-step suggestions below are historical notes, not an automatic work queue.

These tools do not connect to the device, acquire its lock, flash firmware,
or change the Sega CD workspace. New output belongs under the repository's
`build/` directory. ROM input paths may be symlinks left by the disk migration.

## What is implemented

`native_blocks.py` decodes a bounded subset of one-cycle SH2 register operations
and emits separate C functions for the Cortex-M7 compiler. Register numbers and
immediates are constants; operations use local scalar values. The emitted
functions do not replay `opXXXX` interpreter handlers. Each function checks its
entry PC, available cycles, pending delay slot and IRQ test before execution.
A decline leaves the complete SH2 context unchanged so a caller can fall back.

Accepted instructions include moves, addition/subtraction with carry/borrow,
logical operations, comparisons, register extension/swaps, selected shifts and
rotates, T-bit operations, and DT. DT also updates the interpreter's
`no_polling` state. SHAR is excluded because the current interpreter can consume
multiple SHAR instructions inside its handler.

Memory operations, branches, delay-slot execution, actual IRQ delivery and
variable-cycle operations are excluded. A deployable sidecar loader, ROM/core
identity guard, runtime lookup index and writable-code invalidation are not
implemented. The proof is a first stage of an offline translator, not a
replacement 32X core.

## Reproduce the finite proof

Choose a fresh output directory; existing results are not overwritten.

```sh
python3 tools/32x_research/run_native_proof.py \
  --output build/32x-native-proof-new \
  --rom afterburner=/path/to/afterburner.32x \
  --rom doom=/path/to/doom.32x
```

ROM arguments are optional. Without ROMs, the generator still tests opcode
families and deterministic mixed blocks. With ROMs, it also picks static runs
of accepted opcodes. These runs may be data rather than executable code;
reachability and runtime hotness are deliberately unproven. Their instructions
are relocated into a synthetic ROM for the isolated semantic proof.

The Docker builder compiles the current real SH2 interpreter, the generated
functions and the existing M7 bare-metal runtime. Unexpected bus accesses and
IRQ delivery trap rather than return invented values. QEMU on `mps2-an500`
runs once with a fixed timeout. There is no status polling.

The checker compares all 6,016 bytes of the SH2 context across edge and random
register states, both T-bit values and master/slave identities. It also checks
insufficient budgets, wrong PCs, pending delay slots and IRQ tests decline
without mutation. Measurements include entry guards and caller loops.

`result.json` contains comparisons and QEMU instruction-cost samples;
`inputs.json`, `generated.h`, the ELF, map and disassembly remain available.
QEMU omits physical flash/cache latency: its reductions are not device FPS or
whole-game savings. ROM samples' coverage is unknown.

## Completed evidence

- `build/32x-native-proof-20260930/`: first failure caught DT's missing
  `no_polling` update. This result is preserved as a failed proof.
- `build/32x-native-proof-20260930-v2/`: 291 blocks, 13,968 complete-state
  comparisons and 2,895 unchanged declines passed. Native functions total
  23,112 text bytes, including 227 single-op semantic examples. This is not a
  game's translation working-set size or a proposed resident cache allocation.
- `code-audit.json` confirms the generated native functions contain no calls
  to other functions. Short functions use 84–156 bytes in the measured examples.

The next correctness stage is memory and branch boundaries. Physical
measurements wait until the user releases the device from Sega CD use.

## After Burner execution census and measured ALU runs

`run_guest_trace.py` reuses the existing full-game M7 rig and its core build.
It places a snapshot and a small dispatch observer under a fresh `build/`
directory. Runs split at timeslice boundaries, PC discontinuities, delay slots
and 32 operations; they are dynamic dispatch runs rather than canonical basic
blocks. Full opcode sequences distinguish modified code at the same PC.
The observer counts ROM, SDRAM, data-array and other execution separately,
reports all lost dispatches and verifies the recorded run totals match.
Existing fastloop folds remain enabled, so their internal work is excluded.

```sh
python3 tools/32x_research/run_guest_trace.py \
  --rom /path/to/afterburner.32x \
  --output build/32x-ab-trace-new
python3 tools/32x_research/run_native_proof.py \
  --output build/32x-ab-native-new \
  --trace-candidates build/32x-ab-trace-new/alu-candidates.json
```

The census runs QEMU without `icount` to collect paths quickly. Discard its rig
timing fields. The second command uses the existing M7 `icount` meter to compare
interpreter and generated code for the top 32 observed ALU subsequences, with
the existing full-state correctness gates. Those measurements use synthetic
register states and relocated instructions; traced SDRAM code uses the
interpreter's SDRAM fetch route. They do not integrate native blocks into the
running game. Framebuffer dumps establish which scene the census actually
reached. Coverage alone is not a whole-game speedup estimate.

### Verified Stage 1 flight, 2026-09-30

Use the existing native host driver to find input sequences first, then replay
them in the M7 rig. This session initially wasted cold-boot runs guessing a
single START timing. Those results are preserved as title-only evidence.
The working sequence is START for six frames, released for six frames, during
frames 0–119; no input afterward. Both host and M7 reached the flight HUD.
The census runner defaults to this sequence and frames 540–719.

- `build/32x-ab-verified-entry-trace-20260930/result.json`: 67,369,293
  observed dispatches, no losses; 4,707 distinct dispatch runs. All observed
  code executed from SDRAM. Supported ALU instructions comprise 46.78% of
  dispatches, but supported non-delay subsequences of at least two operations
  cover only 17.27%. The top 32 cover 16.06%.
- Longer SDRAM runs matching ROM bytes account for 88.53% of dispatches.
  Matching bytes do not prove copying origin or future stability. No opcode
  changes at observed PCs were seen in this finite window.
- `build/32x-ab-flight-native-20260930/result.json`: the top 32 observed ALU
  subsequences reduce isolated M7 `icount` cost by 57.90–86.25%. The complete
  proof includes 291 blocks, 13,968 state comparisons, 1,963 unchanged declines
  and zero generated function calls. This is not whole-game speed or FPS.
- The five-op read/test/countdown loop at `0x060038ee` accounts for 15.73%
  of dispatches. Its read address and scheduling dependencies are still
  unknown; do not blindly skip it. Repeated load/store and arithmetic runs at
  `0x060069xx`/`0x060068xx` are concrete targets for generic memory translation.

Screens at frames 539 and 719 show carrier departure and early Stage 1 flight,
score zero, without measured input. This is not combat or later-stage coverage.
The rig selects RGB555; these final PNGs use the existing host RGB555 decoder
instead of the legacy RLE utility's RGB565 interpretation.

## Generic countdown wait fold

`countdown_fold.h` is an offline candidate for the five-op pattern
MOV.B displacement/Rn to R0; TST R0,R0; BF exit; DT counter; BF back.
It recognizes instructions rather than a game name or fixed PC. For a zero
SDRAM flag, it computes complete seven-cycle iterations within the current
interpreter budget, leaving an ordinary instruction tail. Delay/IRQ tests,
changing address/register aliases and counter exhaustion prevent folding.
It preserves R0/counter/T/PC/ppc/ea/no_polling and the consumed cycles. No
guest memory writes, IRQ suppression or frameskip are added.

The targeted address diagnostic found the After Burner flag at `0x06003af1`:
2,138,940 reads, 2,138,850 zero and 90 nonzero (1/2/3). Host instruction-store
tracing found `0x06003776` producing those values and `0x06003946` clearing
them. The producer's path reaches RTE at `0x0600373a`; its exact interrupt
source is not established. The texture kernels read SDRAM and primarily
write framebuffer overwrite aliases. Zero source bytes preserve destination
bytes in that mode, which a future native memory backend must implement.

```sh
python3 tools/32x_research/run_guest_trace.py --rom /path/to/afterburner.32x \
  --output build/32x-idle-baseline-new --performance-only
python3 tools/32x_research/run_guest_trace.py --rom /path/to/afterburner.32x \
  --output build/32x-idle-candidate-new --performance-only --countdown-fold
```

Performance mode disables the dispatch observer and uses the existing M7
`icount` meter over frames 540–719. Completed safe-candidate evidence is
`build/32x-countdown-proof-20260930/full-game-summary.json`:

- Entire frame cost: 25,732,614 → 22,454,234 host instructions/frame,
  **12.7402% reduction**.
- SH2 dispatches: 374,273 → 316,207/frame, **15.5143% reduction**.
- Matching ROM/input/source except the candidate interpreter; screen bytes
  at frames 539/719, sparse frame checksums and complete sound signatures match.
- `countdown_proof.c`: 19,872 full 6,016-byte context comparisons, 1,512 cases
  actually folded, eight unchanged declines. The isolated reference uses the
  real interpreter and the exact GNW SDRAM byte-read fixture; other bus and
  IRQ execution trap. The full-game run exercises the real bus and IRQ delivery.

These are full-game M7 instruction costs for early flight, not device FPS.
Other games may receive only guard overhead and require an additional gate.
The normal core/firmware is unchanged; the candidate resides in evidence
snapshots. Next: generic SDRAM/framebuffer native memory blocks, matched Doom
comparison, and physical A/B/A when the device is available.

## Native framebuffer spans and wide multiplication (2026-09-30)

`pixel_native.h` replaces up to four consecutive eight-op texture/pixel
patterns with one native execution (maximum 32 guest operations). It checks
current SDRAM code, cycle budget, IRQ/delay conditions and every effective
address before writing. It preserves framebuffer overwrite-mode zero bytes,
SDRAM byte order, carry flags, all affected registers, EA, PC/PPC and cycles.
The interpreter hook is restricted to its existing 0x6 opcode dispatch.

`native_math.py` replaces DMULS, DMULU and MAC.L software partial products
with C wide products that the M7 compiler can implement natively. It retains
actual bus reads, register alias order, accumulator/saturation behavior and
cycles. This does not replace whole guest calculation routines. In particular,
the frequent opcode `201f` is **MULS.W**, not MAC.L; its surrounding loop
remains a target for whole-block translation.

Completed evidence: `build/32x-ab-native-combined-perf-20260930/summary.json`.
Matched After Burner frames 540–719, same input and no frameskip:

- Baseline: 25,732,614 M7 host instructions/frame.
- Countdown + pixel native: 21,395,054 (pixel adds 4.7171% over countdown).
- Countdown + pixel + wide math: 21,175,016; **17.7114% total reduction**.
- Wide math adds 1.0285% over the pixel candidate.
- Native pixel proof: 2,048 complete actual game contexts and touched memory
  comparisons with the actual interpreter/bus, plus 12 unchanged declines.
- Wide math proof: 49,152 complete 6,016-byte context comparisons with actual
  original handlers and SDRAM bus reads, including signed edge values,
  register aliases and both saturation modes.
- The proof-free combined replay matches raw frame539/frame719 bytes, sparse
  frame checksums and the entire sound signatures. Only copied `sh2.c` and
  `sh2pico.c` change; normal firmware and hardware remain untouched.

Reproduce in a fresh disk directory:

```sh
python3 tools/32x_research/run_guest_trace.py --rom /path/to/afterburner.32x \
  --output build/32x-native-new --performance-only --countdown-fold \
  --pixel-native --native-math
```

`--pixel-proof` enables bounded live shadow comparisons; `--math-proof`
enables arithmetic comparisons before the frame meter. Proof builds are
separate from the retained proof-free performance build.

These are M7 instruction costs, not physical FPS. The 50% whole-frame cost
reduction target is unmet: another 39.2383% of the remaining measured cost
would need to disappear. Other titles, combat and real hardware are untested.
The next structural target is full hot arithmetic/control loops, including
MULS/STS MACL and memory/branches, rather than only their individual operators.

## Generic native control, PCM arithmetic and ROM selection (2026-10-01)

Completed evidence: `build/32x-native-final-20261001/summary.json`.
Same ROM/input/frame window, no frameskip, existing Cortex-M7 Thumb/O2
QEMU icount rig; all new artifacts reside on disk under build/.

| Measured scene | Original instructions/frame | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| After Burner early flight, frames540–719 | 25,732,614 | 16,597,935 | 35.4984% |
| After Burner later action, frames1620–1799 | 25,518,532 | 18,118,390 | 28.9991% |
| Doom rendered level/menu, frames540–719 | 8,115,783 | 8,115,783 | 0%, identical baseline |
| Clean prepared After Burner core, frames540–719 | 25,732,614 | 16,764,794 | 34.8500% |

Each comparison matches complete endpoint raw screens, sparse frame checksums
and complete audio hash/energy signatures. Later action is rendered flight
with explosions; input after frame119 is zero, so do not call it confirmed
interactive combat. Doom is a menu over the rendered level, not gameplay FPS.
Clean prepared code uses existing gnw_direct fetch/delay tracking and excludes
rig proof/reporting helpers. Its cost is reported separately from research code.

Implemented generic guarded code patterns: MULS/DMULS control and parameter
interpolation blocks; chained callback/loop execution; bounded inactive
zero-result iterations; pixel tails; word lookup spans; PCM scale/limit tail.
Patterns check current guest code, cycle budget, IRQ/delay conditions and bus
addresses; preserve clock, flags, PC/PPC, EA, registers and touched memory.
Callback arithmetic appears to be PCM mixing/interpolation from unique ROM
code matches and sample-ring stores, an inference from static code rather
than live peripheral tracing. Actual ring/MMIO stores remain interpreted.

Proof build: `build/32x-ab-auto-full-proof-20260930/result.json`.
Control, pixel, lookup, chain and PCM tail each receive 2,048 bounded live
full-context shadow checks per applicable kind through the real interpreter;
pixel/lookup also compare touched memory. Synthetic control tests: 4,480
full contexts and 7,808 unchanged declines; chain: 863/161; PCM tail: 896/3,200.
Previously retained countdown proof: 19,872 full contexts; wide arithmetic:
49,152. Context size is 6,016 bytes. These checks cover signed edges, branches,
budgets and meaningful unsafe-state guards; they are bounded evidence, not
exhaustive game-state verification.

Blanket native hooks slowed Doom by 3.9650%. Moving hooks inside opcode
handlers also failed: Doom +6.68% cost and slower After Burner. Both are
rejected. `native_selection.py` scans ROM opcode patterns without title names,
fixed PCs or ROM-hash whitelist; `--auto-native` prepares the matching native
variant or the exact baseline. The completed Doom fallback has identical cost
and output. Static pattern presence is eligibility, not execution coverage.
This is offline core selection; device loader selection is not connected.

Prepare a clean source snapshot in a fresh disk directory:

```sh
python3 tools/32x_research/prepare_native_bank.py --rom /path/to/game.32x \
  --output build/32x-prepared-new
python3 tools/32x_research/run_guest_trace.py --rom /path/to/game.32x \
  --output build/32x-replay-new --performance-only \
  --core-source build/32x-prepared-new/picodrive
```

Production compile gate: native CPU object text17,552B vs baseline12,808B
(+4,744B); rodata157 vs29B, bss140 vs68B. This exceeds the previously recorded
3,144B ITCM static margin if all CPU text is live. The compact-code forced
inlining experiment increases text to18,112B and is rejected. Full firmware
bank links and actual map/veneer accounting are mandatory before deployment;
object compilation alone does not establish fit. Root firmware/core/linker
remain unchanged and the Sega CD device has not been accessed.

The 50% whole-frame reduction and physical60FPS targets remain unmet. Early
flight research code still needs22.4822% reduction of its remaining cost to
halve the original. Next: exact production memory placement/bank links and
variant loader integration, more active game coverage, and hardware A/B once
the device is released. Remaining SH2 cost also warrants local validation reuse
within pure native chains; persistent RAM-code caches require proper writer
invalidation and must not assume copied guest code stays immutable.

## Native hardware first triplet and audio completion repair (2026-10-01)

Latest upstream bd60143b merged after preserving old local work in stash and
binary patches (`build/32x-device-20261001`). Restored32x instrumentation and
renderer changes; obsolete SegaCD changes remain preserved in stash. Private
firmware snapshots keep native out-of-line helpers in RAM_EMU and interpreter
in ITCM; bank link gates passed, candidate ITCM ends0xfdf8 (520B margin).

After Burner full-frame cold-boot input0..119 START6on/6off, warmup540,
window180,340MHz,fullscreen0,tear_guard0. Endpoint forced drawn in both arms.
Original A:9.99556FPS, candidate B:11.64370FPS, original A:9.99056FPS;
+16.5179% drawn/emuFPS, drift-0.04995%. All180 measured frames drawn.
Endpoint screen and last synthesized audio bytes match; identical guest clock,
input, samples and completion frame. One triplet is descriptive evidence only.
Result: `build/32x-device-20261001/jobs/job-y3sa31kw/round-000001/comparison.json`.

The completed test stopped the CPU while circular audio DMA remained running,
causing loud repeated audio. User reported this; emergency SAI disable stopped
output, then original current bank/core/XIP/selector were readback-verified,
restored and started. CONFIG/off.sav remained byte-identical; idle timeout600
restored in RAM, original settings remain saved on SD. Future benchmark source
now calls audio_stop_playing after end timestamp and before breakpoint; the
comparison md32x_snd is separate from the cleared DMA buffer. This new silent
completion path has not yet received a hardware validation; do not rerun old arms.

UI audit: RGB565 double buffering means LUT8 full-palette/CLUT upload issues
are inapplicable. Existing border/frame-skip and fullscreen row mapping tests
pass. Original installed32x core tag8c533dca differs from current resident tag
bd5a5da6, so a normal boot refuses it with an installation-warning dialog.
Matched normal UI arm is built at arms/ui but has not been installed or audited
live. Menu/pause/thumbnail/scaling/tearing live checks remain pending. Original
installation state was preserved rather than silently retagging incompatible code.


## 2026-10-01 실기 최종 정리

- 조용한 종료를 적용한 A/B/A: 10.1078 → 11.5778 → 10.1186 FPS, 후보 +14.4819%, 기준 드리프트 +0.1068%. 한 번의 triplet 결과이며 60 FPS 달성은 아니다.
- 동일 180 프레임, 화면 및 오디오 결과 일치. 세 arm 모두 종료 시 오디오 DMA 정지 확인.
- RGB565 이중 버퍼이므로 LUT8 팔레트/CLUT 업로드 수정은 해당하지 않는다. 메뉴 재도색 중 게임 배경이 검게 변하는 문제를 고쳤고 실기 화면에서 확인했다.
- 공통 스케일링 옵션을 32X 렌더링에 연결하는 수정은 양쪽 bank 빌드 성공까지 확인했다. 실기 스케일링, PAUSE, 저장 썸네일 및 나머지 화면 점검은 미완료다.
- 사용자 요청에 따라 추가 실행을 종료했다. 원래 bank2/core/XIP/선택 파일/CONFIG를 복원하고 해시 확인 후 bank2를 시작했다. off.sav도 원본과 일치한다. 원래 CONFIG의 자동 꺼짐 설정도 복원했다. 최적화 펌웨어는 상시 설치하지 않았다.
- 측정: `build/32x-device-20261001/jobs-quiet/job-jmdc2ory/round-000001/comparison.json`
- 복원: `build/32x-device-20261001/restoration-final-ui/result.json`
- 메뉴 확인: `build/32x-device-20261001/ui-live-fixed/held-menu.png`
