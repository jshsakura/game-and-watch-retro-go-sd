# SNES device DWT frame-ledger design: adversarial review

검수 대상은 구현 전 제안인 `65816 / PPU / APU / present+DMA2D / audio pacing wait / rest` 6분할이다.

## 결론

**목적은 survives, 제안된 6분할은 refuted다.** 이 구조를 그대로 구현하면 세 가지 오류가 생긴다.

1. PPU와 APU는 독립된 top-level phase가 아니라 `run_frame_events()` 안에서 CPU/DMA와 교차 실행한다. 특히 `$2140-$2143` 접근의 `cpu_runOpcode -> snes_readBBus/RtlApuWrite -> snes_catchupApu`는 명시적인 중첩이다.
2. DMA2D는 `snes_pcm_submit()`과 동시에 진행한다. DMA lifetime과 CPU audio 시간을 둘 다 합계에 넣으면 같은 wall interval을 두 번 센다.
3. audio pacing은 `cpumon_sleep()`의 `WFI`다. Cortex-M7 sleep은 processor clock을 멈추므로 DWT CYCCNT만으로는 sleep의 wall time을 측정할 수 없다. DWT wait delta에는 주로 wakeup/ISR/loop overhead만 보일 수 있다.

따라서 하나의 6-bucket 합계가 아니라 다음 **세 ledger**가 필요하다.

- foreground active-cycle ledger: DWT, 합이 맞는 배타적 CPU 실행시간
- wall/deadline ledger: sleep 중에도 가는 timer, 실제 frame time과 audio deadline
- async side channels: DMA2D lifetime, audio DMA tick/miss; 합계에 더하지 않고 overlap 해석에만 사용

## 코드에서 확인된 중첩

- `run_frame_events()`는 한 프레임 동안 `snes_handle_pos_stuff()`, CPU/DMA, PPU scanline render와 APU catch-up을 교차 실행한다.
- `ppu_runLine()`은 `snes_handle_pos_stuff()`에서 호출되므로 PPU는 `run_frame_events()`의 child다.
- CPU의 B-bus read는 `snes_readBBus()`에서, write는 `RtlApuWrite()`에서 `snes_catchupApu()`를 부른다. 따라서 opcode 전체 timer와 catch-up timer를 단순 합하면 APU가 CPU에도 한 번, APU에도 한 번 잡힌다.
- DMA도 B-bus를 접근할 수 있으므로 APU child는 CPU뿐 아니라 DMA scope에서도 열릴 수 있다.
- 프레임 끝의 `snes_catchupApu()`와 `snes_pcm_submit()` 안의 `apu_cycle()` top-up은 CPU opcode 바깥의 APU 작업이다. 둘 다 APU wire 회수 상한에 들어가야 한다.
- `present_frame()`은 cache clean과 DMA2D start 뒤 즉시 돌아오고, audio 생성 뒤 `present_frame_wait()`가 남은 tail만 기다린다. DMA2D의 전체 lifetime은 foreground bucket이 아니다.

## 권장 측정 모델

### Ledger A: top-level foreground active cycles

한 frame iteration의 맨 앞에서 DWT를 한 번 기준 잡고, 다음 경계의 누적값 차분만 기록한다. 중간에 CYCCNT를 다시 clear하지 않는다.

1. frame-control: `common_emu_frame_loop`, cadence/skip 결정
2. input/front-end: gamepad, menu/turbo, callback 준비
3. emulation outer: `run_frame_events` 전체
4. present kick: cache clean, DMA2D configure/start 또는 CPU scaler/copy
5. PCM submit outer: LLE/HLE audio 생성, volume conversion, DMA half-buffer copy
6. present tail: DMA2D poll tail, overlay, `lcd_swap`
7. pacing active: wait loop와 그 사이 깨어서 실행한 foreground/ISR cycle
8. loop remainder: watchdog, profiler book-keeping 등
9. active total

여기서 `active total ~= 1..8의 합`이라는 identity를 매 frame 검사한다. drawn/skip, scaling OFF/FIT/FULL, actual cadence와 auto-skip 결과를 반드시 따로 모은다.

Ledger A의 `emulation outer`와 `PCM submit outer`는 child를 포함하는 top-level 합계용 값이다. 아래 Ledger B의 PPU/APU 값을 같은 합계에 다시 더하지 않는다. Ledger B는 각 outer를 내부적으로 재분류하는 별도 계층이다.

### Ledger B: `run_frame_events` 내부의 exclusive attribution

APU wire 판정에 필요한 최소 분할은 다음이면 충분하다.

- PPU inclusive cycles
- APU LLE exclusive cycles
  - `snes_catchupApu -> apu_run`
  - `snes_pcm_submit`의 `apu_cycle` top-up
  - sample extraction 중 wire가 대체할 부분
- core remainder = emulation outer - PPU - in-frame APU

`core remainder`는 65816 interpreter뿐 아니라 DMA, event scheduler, spin bookkeeping을 포함한다. 이를 **65816**이라고 이름 붙이면 안 된다. 정말 65816만 필요하면 CPU/DMA/event를 추가로 나눠야 하지만, per-opcode DWT bracket은 probe 비용이 커지므로 이번 APU GO/NO-GO에는 권하지 않는다.

CPU timer를 꼭 둘 경우에는 nested APU 진입 때 CPU timer를 pause하고 복귀 때 resume하는 stack-based exclusive profiler가 필요하다. 단순 `cpu_inclusive + apu_inclusive` 합은 금지한다. stack depth, underflow/overflow, frame-end nonzero depth를 오류 카운터로 남긴다.

### Ledger C: wall/deadline와 async side channels

sleep 중에도 동작한다고 기기에서 확인한 free-running peripheral timer를 쓴다. 현재 `HAL_GetTick()`은 SysTick 기반이므로 sleep-safe라고 가정할 수 없다. WFI sanity test에서 계속 진행함이 증명된 경우에만 긴 window의 보조 wall clock으로 쓰고, 1 ms 해상도 때문에 per-frame percentile 용도로는 쓰지 않는다. 적합한 timer가 없다면 audio DMA edge count와 외부 GPIO/logic-analyzer 측정을 함께 써야 한다.

기록할 값:

- frame wall start/end, pacing wall start/end
- pacing 진입 전 `dma_counter - last_dma`
- pacing 중 WFI 횟수와 wakeup 횟수
- frame당 audio DMA tick delta
- deadline을 이미 놓쳐 wait 없이 통과한 frame 수와 놓친 period 수
- emulated frames/s, actual drawn frames/s, audio underrun/stale-half count
- DMA2D start/completion wall timestamp와 `present_frame_wait`의 tail

DMA2D lifetime은 audio와 겹치는 side channel이다. active ledger에 넣는 값은 launch CPU cost와 실제 poll tail뿐이다. DMA2D bus contention은 시간 subtraction으로 분리할 수 없으므로 drawn/skip 또는 DMA2D on/off의 별도 A/B로만 경계를 잡는다.

## 항목별 판정

### (1) 6분할의 타당성

**판정: refuted.**

빠진 항목은 DMA/event scheduler, frame-control/input, PCM submit의 비-APU 변환·복사, IRQ, DMA2D overlap이다. CPU/APU는 물리적으로 동시에 실행하지 않지만 call stack상 중첩되므로 inclusive timer를 합하면 이중계상한다. `present+DMA2D`도 async lifetime을 포함하는 순간 audio와 이중계상한다.

위의 3-ledger 구조라면 목적은 달성할 수 있다. APU 착수 판정만을 위해서는 65816 순수값보다 `APU exclusive`와 `non-APU foreground remainder`가 더 안전하고 충분하다.

### (2) DWT 측정의 중첩·재진입·인터럽트 함정

**판정: “DWT면 자동으로 disjoint”는 refuted. active-cycle 계측기로서 DWT 자체는 survives.**

32X 기록은 두 종류의 실패를 이미 보여준다.

- profiler define이 object에 실제 적용되지 않은 stale build가 배포되어 call site가 한 번도 컴파일되지 않았다.
- mid-frame draw/FM/PWM child가 CPU/sound parent에서 pause되지 않아 같은 시간이 두 bucket에 잡혔고, early return으로 pprof scope가 새기도 했다.

SNES도 같은 위험을 가진다. 방지 조건은 다음과 같다.

- profiling flag 변경 시 clean/reproducible build를 강제한다.
- `nm`, map 또는 disassembly로 profiler call site와 출력 symbol이 실제 binary에 있음을 gate한다.
- boot log와 결과 파일에 firmware commit, profiler version, ROM SHA, clock, scaling, HLE/LLE, cadence를 기록한다.
- DWT는 frame 시작 한 번만 기준을 잡고 unsigned subtraction을 쓴다. nested helper가 CYCCNT를 clear하지 못하게 한다. 기존 `SNES_LOAD_DIAG`와 새 profiler는 상호배타 또는 하나로 통합한다.
- scope enter/exit 수, 최대 depth, frame-end depth, underflow를 출력하고 하나라도 어긋나면 결과 전체를 FAIL 처리한다.
- `outer total - sum(exclusive foreground)` residual을 매 frame 검사한다.

인터럽트는 더 까다롭다. SysTick, SAI, LTDC, TIM1 등이 어느 bucket 도중이든 들어와 그 bucket의 DWT delta를 부풀린다. SAI는 pacing과, LTDC/DMA2D는 present와 상관되어 있어 단순 평균으로 완전히 사라지지 않는다. 인터럽트를 끄면 audio pacing과 실제 기기 동작 자체가 바뀌므로 금지한다.

정확한 exclusive attribution이 필요하면 관련 ISR의 entry/exit를 재진입 안전하게 별도 `irq` bucket으로 빼야 한다. 그렇게 하지 않으면 모든 foreground 값은 `IRQ-inclusive`라고 표시하고, 여러 window의 분산 및 `irq` 상한을 함께 보고해야 한다. DWT `EXCCNT`는 exception entry/exit overhead만으로 handler body 전체의 대체물이 아니다.

### (3) audio pacing wait의 올바른 측정과 해석

**판정: DWT로 wait를 직접 재는 설계는 refuted. wall timer + DMA deadline ledger는 survives.**

`cpumon_sleep()`은 `__WFI()`를 실행한다. ST의 Cortex-M7 programming manual은 sleep mode가 processor clock을 멈춘다고 명시한다. DWT CYCCNT는 processor cycle counter이므로, pacing 전후 DWT 차이를 wall wait로 간주하면 안 된다. standalone device에서 다음 sanity test를 먼저 통과시켜야 한다.

```text
known wall interval의 busy-NOP: DWT / SystemCoreClock ~= wall time
같은 interval의 WFI: wall timer는 같고 DWT는 sleep을 제외하는지 확인
debugger 연결/미연결 둘 다 기록하고 실제 profile은 미연결로 수행
```

wait가 크다는 뜻은 그 **장면의 평균**에서 다음 audio deadline 전에 일이 끝나 slack이 있다는 뜻이다. 이때 연산 절감은 wait로 바뀌어 emulated fps가 60을 넘지 않는다. 하지만 다음을 뜻하지는 않는다.

- p95/p99 heavy frame도 안전하다.
- 최적화가 무가치하다. deadline miss, audio stale period와 auto-skip을 줄일 수 있다.
- APU가 천장이 아니다. 평균 wait와 heavy-tail compute bound가 한 run에 공존할 수 있다.

반대로 wait가 0이라는 것도 반드시 한 period만큼 overrun했다는 뜻은 아니다. pacing 진입 전에 DMA counter가 몇 번 진행했는지를 봐야 한다. 이 loop는 이미 진행한 tick이 있으면 즉시 통과하며, LLE에서는 놓친 audio period를 복구하지 않는다. 따라서 판정 단위는 평균 wait가 아니라 다음이다.

- `work_active`와 frame period의 p50/p95/p99
- wait가 필요했던 frame 비율
- pacing 진입 전 0/1/2+ deadline advance 분포
- emulated/drawn fps와 audio underrun 분포

### (4) profiler 자체 비용

**판정: 현재처럼 세밀한 per-opcode/per-DSP probe를 재사용하면 refuted. 낮은 빈도의 계층형 profiler는 조건부 survives.**

오늘 관측된 baseline 대비 0.38--8.37% probe 증가는 +10 fps 목표와 같은 크기다. hash 동일성은 에뮬레이터 결과가 같다는 뜻이지, cache·deadline·frameskip이 교란되지 않았다는 뜻은 아니다. 큰 overhead를 사후에 상수처럼 빼는 것도 안전하지 않다.

권장 순서:

1. Phase A는 frame당 수십 회 이하의 top-level boundary read만 넣는다.
2. APU는 먼저 call count만 센다. 고빈도라면 모든 SPC opcode와 DSP tick을 bracket하지 않는다.
3. APU total이 필요하면 `snes_catchupApu`와 PCM top-up을 coarse scope로 재되 call당 DWT read/accumulator cost를 device microbench로 측정한다.
4. profiler-off baseline, profiler-on, 동일 probe topology의 null/control을 같은 deterministic trace로 비교한다.
5. profiler가 wall fps, deadline-miss 분포, actual draw/skip 분포 중 하나라도 1% 이상 바꾸면 attribution run을 무효화하고 sampling 또는 더 coarse한 scope로 낮춘다.
6. 결과 dump는 측정 window가 끝나고 audio/emulation을 정지한 뒤 한 번만 한다. sample buffer의 RAM placement와 크기도 build map으로 gate한다.

최종 성능 A/B는 반드시 profiler를 완전히 끈 binary로 다시 한다.

### (5) 이 측정 하나로 APU 착수 여부를 판정할 수 있는가

**판정: 단독 GO 판정은 refuted. NO-GO 상한 gate로는 survives.**

40.3 fps에서 50.3 fps에는 compute-bound라는 전제에서도 전체 wall cost 19.88% 절감이 필요하다. 따라서 다음 식의 상한이 19.88%보다 작으면 APU 하나로 +10은 즉시 NO-GO다.

```text
recoverable upper = APU LLE exclusive
                  + wire가 제거하는 CPU-side port/catchup 비용
                  - exact wire 자체 실행비용
```

반대로 APU share가 20%를 넘었다고 바로 GO는 아니다. LLE 비용은 zero-cost로 사라지지 않고 wire player, command translation, audio render와 state synchronization으로 바뀐다. 또 `$2140-$2143`의 값과 관측 순서를 보존해야 한다.

GO에 추가로 필요한 증거:

- exact Zelda gameplay save/input trace의 여러 장면: field, dungeon, battle, menu, 음악 전환
- baseline의 CPU/PPU/APU machine-state hash와 APU port read/write transcript
- PCM/audio hash 또는 “exact”가 허용하는 명시적 오차 규약
- wire 전환·fallback·save/load 후 동일성
- profiler-off LLE vs wire device A/B의 emulated fps, drawn fps, p95/p99, deadline miss와 underrun
- wire의 code/RAM 비용과 unsupported song/effect fallback 비율

측정 하나는 후보를 죽이는 데는 충분할 수 있지만, 후보를 채택하는 데는 실제 wire A/B가 필요하다.

## 실행 전 필수 gate

1. 현재 `9b4a49ca` cadence 결함이 있는 상태로 측정하지 않는다. baseline은 cadence Auto로 고정하고 actual draw/skip을 기록한다.
2. WFI wall-time sanity test를 먼저 한다.
3. profiler binary가 실제로 계측 call site를 포함하는지 `nm/map/disassembly`로 확인한다.
4. active ledger 합계와 nesting/IRQ 오류 gate를 먼저 통과시킨다.
5. profiler-off/on intrusion이 1% 미만인지 확인한다.
6. 동일 Zelda ROM SHA, save-state, scripted input, clock, scaling, volume로 최소 3개 반복 window를 잰다.
7. 보고서는 평균만이 아니라 p50/p90/p95/p99, deadline miss, drawn/skip 분리를 포함한다.

## 최종 한 줄

**이 6분할 그대로는 재면 안 된다. DWT active cycles, sleep-safe wall/deadline, async DMA side channel의 3-ledger로 고치고 nested/IRQ/overhead gate를 넣으면 측정 착수는 GO다.**
