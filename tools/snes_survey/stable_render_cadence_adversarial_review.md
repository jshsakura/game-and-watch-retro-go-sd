# SNES stable render cadence adversarial review

검수 대상: `perf/zelda-stable30`의 `fcc6e129`와 `9b4a49ca`

## 결론

현재 커밋은 **NO-GO**다. `+1.6~+3.7 fps`는 production 1:1 cadence를 실행한 실측값이 아니라 always-render와 always-skip의 차이를 2로 나눈 추정값이다. 더 심각하게는 cadence가 기존 auto-skip guard의 최종 draw 결정을 뒤에서 다시 취소한다. 지속 과부하에서 기본값 30 fps(`g_render_cadence=1`)는 guard가 강제로 그리는 모든 프레임과 위상이 어긋나 화면이 무기한 갱신되지 않을 수 있다.

`fcc6e129`의 instrumentation anchor 수정 자체는 타당하고 빌드도 다시 성립한다. 그러나 현재 COREHASH/AUDIOHASH gate는 전체 에뮬레이터 상태나 deep instrumentation의 framebuffer 동일성을 증명하지 않는다.

## 최우선 반증

### 1. production cadence를 측정하지 않았다

`build_snes_cost.sh`의 source 목록에는 `main_snes.c`가 없고 frontend 대신 `rig_snes.c`가 들어간다. 생성하는 비교군도 baseline과 `-DRIG_FRAMESKIP`뿐이다. 후자는 `g_ppu_skip_render=true`를 모든 프레임에 고정한다.

따라서 커밋 메시지의 `1:1 saves`와 `+1.6/+3.3/+3.7`은 다음 산술 추정일 뿐이다.

```text
estimated cadence cost = (always-render cost + always-skip cost) / 2
```

실제 cadence의 draw/skip 횟수, auto-skip과의 충돌, 짝수/홀수 프레임 비용 차이, present 생략은 어느 것도 그 실행에서 측정되지 않았다.

### 2. 기본 30 fps가 지속 과부하에서 0 displayed fps가 될 수 있다

`common_emu_frame_loop()`는 `skip_frames==2`가 지속되면 네 번째마다 draw를 강제하고, 그 순간 내부 `skip_streak`을 0으로 되돌린다. 그러나 SNES frontend는 이 결과를 받은 뒤 cadence mask로 draw를 다시 false로 바꾼다. common layer는 실제로 그리지 않았다는 사실을 모른다.

기본 1:1 mask가 짝수 frame만 허용하고 guard의 강제 draw가 홀수 위상에 놓이면 다음 상태가 반복된다.

```text
auto:     skip skip skip FORCE-DRAW | skip skip skip FORCE-DRAW | ...
cadence:  draw skip draw skip       | draw skip draw skip       | ...
actual:   skip skip skip skip       | skip skip skip skip       | ...
```

시작 직후 몇 프레임 뒤 sustained overload에 들어가는 현재 호출 순서에서는 이 위상이 실제로 가능하다. 15 fps 설정도 modulo 4 mask와 guard의 위상이 어긋나면 같은 문제가 생긴다. 20 fps는 최악의 경우 12 emulated frames당 한 장 수준까지 내려갈 수 있다.

그러므로 이 구현은 “stable floor”가 아니다. cadence는 표시율의 상한일 뿐이고 auto가 더 낮출 수 있으며, 현재 조합은 기존 1-in-4 안전장치까지 무효화한다. 실제 에뮬레이션 속도가 약 44 fps라면 1:1의 표시율도 30이 아니라 약 22 fps다.

## 공격 지점별 판정

### (1) ALL-OFF에서 실제 회수 가능 몫 분리 / 17% 신뢰성

**판정: 해석은 refuted, 17%를 cadence 회수율로 부르는 것도 refuted. 단, 해당 단일 workload의 always-off compositor 몫이라는 좁은 뜻은 survives.**

`ppu_runLine()`은 brightness 갱신, sprite evaluation, range/time-over 관련 상태를 처리한 뒤 `g_ppu_skip_render`에서 return한다. 즉 이 관측성 작업은 render-on과 render-off 양쪽에 모두 들어가며 두 실행의 차이에서 이미 소거된다. “ALL-OFF delta에 flags 비용이 섞여서 1:1로 회수 못 했다”는 설명은 코드 구조와 반대다. delta는 같은 workload라면 skip return 뒤의 compositor 비용이다.

다만 all-off delta의 절반이 정확한 1:1 비용이라는 것은 프레임 비용이 위상과 독립일 때만 맞는다. 젤다에 2-frame 패턴이 있으면 draw-even과 draw-odd 결과가 달라질 수 있다. 올바른 분리는 다음 조건의 세 실행으로 해야 한다.

1. 동일 ROM SHA, 동일 save-state/input trace, 동일 frame window와 compile flags로 always-draw, production-cadence, always-skip을 실행한다.
2. production-cadence는 phase 0과 phase 1을 각각 측정한다.
3. 매 프레임 `cadence request`, `auto veto`, `actual draw`, `actual skip`, 충돌 횟수와 최대 연속 skip을 기록한다.
4. `C_cadence`를 직접 재고, `(C_on+C_off)/2`는 phase 대칭이 확인됐을 때만 교차검증으로 쓴다.
5. emulated fps와 displayed fps를 따로 보고한다.

42.6%와 17%는 같은 실험의 단계별 분해도 아니다.

- 42.6% 쪽: 미국판 `zelda_alttp.smc`, SHA-256 `66871d66...`, 무입력, 후반 200-frame window.
- 17% 쪽: 일본판 `Zelda no Densetsu...smc`, SHA-256 `f7fd1efc...`, `RIG_INPUT_TAP`, 301--1200 누적 차분.

수리된 rig를 목표 미국판 ROM으로 1200프레임 다시 실행한 결과는 COREHASH/AUDIOHASH가 on/off에서 같았고 다음과 같았다.

```text
render on : emu 6,349,559 + apu 533,434 insn/frame
render off: emu 5,244,371 + apu 533,433 insn/frame
delta     : 1,105,188 insn/frame
```

총비용 기준 all-off renderer share는 16.1%, 이상적인 1:1 절감은 8.0%다. 그러나 100-frame window의 all-off share는 후반 장면에서 약 18.4%부터 35.6%까지 움직였다. 따라서 17%는 “대략 그 실행의 누적 compositor share”로는 그럴듯하지만 일반적인 젤다 회수율이나 실제 cadence 실측치는 아니다.

### (2) +1.6~+3.7이 구현 결함 때문일 가능성

**판정: survives가 아니라 refuted. 구현 결함이 확인됐고, +1.6~+3.7은 구현 측정값조차 아니다.**

위의 auto-guard/cadence 위상 충돌은 release 코드의 실제 결함이다. 또한 rig가 `main_snes.c`를 링크하지 않으므로 이 결함을 검출할 수 없다. 현재 숫자가 작아서 결함을 의심하는 수준이 아니라, 숫자와 구현의 실행 경로가 아예 분리되어 있다.

추가로 다음 표현도 성립하지 않는다.

- “steady 30”: emulated loop가 60 fps일 때만 cadence 상한이 30이고, auto veto가 있으면 더 낮다.
- “floor”: 코드는 draw를 skip으로만 바꾸므로 floor가 아니라 ceiling이다.
- “input latency untouched”: 입력과 게임 상태는 매 emulated frame 진행하지만 화면에 반영되는 latency는 증가한다.
- “option-gated”: 메뉴 선택지는 있으나 기본값이 30이어서 모든 SNES 타이틀에 즉시 적용된다.

### (3) rig가 못 재는 present/LCD/DMA2D가 +10을 채울 수 있는가

**판정: 미측정이라는 말은 survives. 정확한 device 값은 판단불가. 그러나 default OFF-scaling에서 부족한 +6.3 fps를 전부 채울 가능성은 현재 근거와 상한 계산상 refuted에 가깝다.**

default OFF-scaling의 device 경로는 320x240 RGB565 약 150 KiB에 대해 cache clean을 하고 async DMA2D copy를 시작한다. 그 전송은 `snes_pcm_submit()`과 겹치며, 이후에는 남은 DMA tail만 기다린다. `lcd_swap()`도 vblank를 동기 대기하지 않고 reload를 예약한다. 따라서 cadence가 회수하는 wall time은 대략 cache-clean + HAL setup + 오디오와 겹치지 않은 DMA tail + overlay/swap이다. DMA 전체 시간을 그대로 더할 수 없다.

40.3에서 50.3 fps로 가려면 총 frame cost를 19.88% 줄여야 한다. 내부 compositor의 추정 절감 8.4%를 인정해도 평균 11.48%가 더 필요하다. present는 두 프레임 중 한 번만 생략되므로, draw frame 하나당 미측정 present 경로가 전체 baseline의 약 22.96%여야 한다.

```text
baseline 8.436 Mcycles/frame 가정
필요한 추가 평균 절감 = 0.969 Mcycles/frame
생략되는 present 1회당 필요 비용 = 1.937 Mcycles = 5.70 ms @ 340 MHz
```

rig의 CPU memcpy proxy는 81,626 instructions/frame, 전체 재실행 비용의 약 1.19%였다. instruction과 device cycle은 동일하지 않지만 +10을 채우려면 이 proxy보다 약 24배 큰 5.7 ms의 비중첩 비용이 필요하다. async DMA2D와 audio overlap 구조상 긍정적 근거가 없다. FIT/FULL의 CPU scaler는 별도이며 더 비쌀 수 있지만, 그 경우 결과는 display-mode 종속이고 default OFF 성능 주장을 뒷받침하지 않는다.

상한을 확정하는 방법은 실제 기기에서 DWT로 다음 네 구간을 같은 trace에 따로 재는 것이다.

- `run_frame_events`
- `SCB_CleanDCache_by_Addr + DMA2D setup`
- `snes_pcm_submit`
- `present_frame_wait + overlay + lcd_swap`

특히 wall-clock outer-loop와 `present_frame_wait` tail을 같이 기록해야 overlap을 이중계산하지 않는다.

### (4) 화면 절반을 버려 +3.7의 tradeoff

**판정: 저위험/유리한 trade라는 평가는 refuted.**

40.3 fps workload에서 이상적인 +3.7은 emulated throughput 약 44 fps이고 실제 표시율은 1:1이면 약 22 fps다. 화상 움직임과 시각적 입력 반응을 절반으로 줄여도 60 fps 실시간에는 멀다. 더구나 현재 구현은 1-in-4 guard를 깨고 default로 모든 SNES 게임의 표시율을 제한한다.

기능을 남길 이유가 있다면 성능 최적화가 아니라 사용자가 명시적으로 켜는 저표시율 fallback이다. 기본은 Auto여야 하고, actual-draw 기준 guard와 결합해 최소 표시율을 보장해야 한다. 현재 상태로는 ship할 트레이드가 아니다.

## fcc6e129 anchor와 gate

### Anchor 수정

**판정: survives.**

기존 `#endif\n    } else {`는 line cache 추가 뒤 두 곳에 나타나 `one()`의 유일성 전제를 깨뜨렸다. 새 anchor는 뒤의 direct-math `#if`까지 포함해 의도한 fast-path 경계에 고정하고, replacement도 그 `#if`를 보존한다. 독립적으로 PPU-only rig를 목표 미국판 ROM으로 다시 빌드/실행했고 PASS했다. 전체 release link도 성공했다.

### COREHASH 재게이트

**판정: “게이트가 다시 실행 가능해졌다”는 survives, “정확성을 충분히 재게이트했다”는 refuted.**

현재 COREHASH는 WRAM과 cart SRAM만 hash한다. CPU registers, PPU state, APU RAM/ports, DMA state와 input은 포함하지 않는다. AUDIOHASH는 생성된 PCM만 본다. `run_snes_ppu_deep.py`는 on에 대해 off/deep의 COREHASH와 AUDIOHASH만 비교하며, on과 deep의 STATEHASH는 비교하지 않는다. 따라서 잘못 꽂힌 instrumentation이 framebuffer만 바꾸거나 hash 밖의 machine state를 바꿔도 PASS할 수 있다.

필요한 보강은 다음 두 가지다.

1. 둘 다 render-on인 baseline/deep의 STATEHASH를 반드시 동일성 gate에 넣는다.
2. framebuffer를 제외한 전체 serialized machine-state hash를 만들어 on/off를 비교한다.

cadence 검증에는 실제 draw frame만 정렬해 framebuffer hash를 비교하고, audio는 모든 emulated frame에서 비교해야 한다.

## 최종 한 줄

**현재 `9b4a49ca` 그대로의 기기 A/B는 가치가 없다. 이 커밋은 접고, actual-draw guard를 보존하도록 정책을 고친 뒤 production cadence 계측과 present-tail DWT를 넣은 단 한 번의 A/B만 진단 목적으로 허용하라; default 30 fps 최적화는 NO-GO다.**
