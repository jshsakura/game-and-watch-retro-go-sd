# 32X `msh2` 공략 가능성 적대 검수

검수일: 2026-07-21  
대상: `gnw-32x`, `explore/32x-feasibility`, main `14b78d42`, picodrive `41258a36`

## 결론 먼저

`msh2`는 가장 큰 **관측 버킷**인 것은 맞지만, 아직 “ROM XIP를 고치면 회수되는 59.4%”는 아니다.
원래 메모의 핵심 판정법인 `cycles / dispatched guest insn` 하나로 XIP 원인을 즉시 확증한다는
주장은 **REFUTED**다. 그 비율은 명령 수와 명령당 평균비용을 산술적으로 분리할 뿐, 평균비용의
원인이 ROM인지 opcode mix·data/MMIO·fastloop·IRQ인지 가르지 못한다.

반대로 “RAM이 없으므로 맞아도 아무것도 못 한다”도 너무 강하다. 일반 ROM 캐시는 사실상 막혀
있지만, 현재 `GNW_FETCH_SD`는 주석과 달리 인라인 fast path가 전혀 없는 `RW()` 별칭이다. 과거
QEMU에서 +3.9%라 폐기한 실험은 XIP/cache stall을 모델링하지 못하며, 같은 이유로 ITCM 이동의
기기 +30%를 예측하지 못했다. 저장공간 없이 할 수 있는 **device-only inline fetch A/B**는 아직
안 끝난 축이다.

따라서 판정은 **전면 착수 NO / 2-build 제한 프로브 YES**다. 먼저 무프로브 device A/B로
inline fetch의 wall 이득을 재고, 별도 count-only 실행으로 ROM 지역·PC 집중도를 센다. 이 둘이
양성이 아니면 캐시·대형 디스패처 개조는 하지 않는다.

## 먼저 바로잡아야 할 분모 두 개

1. 보고된 59.4%는 현재 덤프 코드상 `pct_of_pico`다
   (`md32x_profile.c:254-280`). 즉 전체 wall frame의 59.4%가 아니라
   `PicoFrame()` 내부의 59.4%다. 실제 wall 비중은

   ```text
   f_msh2_wall = 0.594 * (pico_cycles / loop_total_cycles)
   ```

   이다. outer `pico/total`이 없으면 “전체 프레임 59.4%”로 환산할 수 없다.

2. 새 cycles/insn 빌드는 무오염 관측기가 아니다. `GNW_SH2_INSN_TICK`은 매 dispatch마다
   RAM의 64-bit 카운터를 증가시키며(`sh2pico.c:129-137, 646-650`), 그 비용은 그대로
   `pp_msh2/pp_ssh2` 시간에 포함된다. master가 dispatch를 더 많이 하면 master 버킷을 더 많이
   부풀린다. 기존 59.4/17.1 분포가 counter 이전 빌드에서 나왔다면 그 분포 자체를 폐기할 이유는
   없지만, **새 ratio run의 phase share와 합쳐 쓰면 안 된다.**

3. `refcount_leaks=0`은 중첩 scope가 닫혔다는 필요조건일 뿐 profiler가 공짜라는 뜻은 아니다.
   `pprof_start/end`는 SH2 slice마다 DWT read와 refcount bookkeeping을 실행한다. msh2 1위라는
   큰 순위는 쉽게 뒤집히지 않겠지만 59.4/17.1의 소수점까지 성능 분배로 쓰려면 profiler
   off/on wall intrusion과 `pico_total(outside) ≈ frame_total(pprof)`를 같이 통과해야 한다.

## (1) 3.5배 원인을 `cycles/guest-insn`으로 가를 수 있는가

### 판정: **REFUTED** — workload count 대 평균비용 분해까지만 가능, XIP 귀속은 불가능

산술적으로는 유용하다.

```text
C_m / C_s = (D_m / D_s) * ((C_m / D_m) / (C_s / D_s))
```

여기서 `D`는 실제 guest instruction 수가 아니라 현재 카운터가 세는 **dispatcher 진입 횟수**다.
따라서 3.5배 중 얼마가 dispatch 횟수 차이이고 얼마가 dispatch당 평균비용 차이인지는 볼 수 있다.
하지만 후자를 곧 ROM XIP라고 부를 수는 없다.

반례는 다음과 같다.

- master와 slave의 opcode mix가 다르다. `MAC_L`, memory op, branch/delay, exception은 같은 한
  dispatch라도 host 비용이 다르다. 16-way computed goto 뒤에도 `op0000()` 등의 하위 switch가
  남아 있다(`sh2.c:1858+`).
- opcode fetch 외의 data read/write 지역이 다르다. ROM, SDRAM, CS0 sysreg/comm/PWM은 비용과
  side effect가 모두 다르다.
- fastloop는 한 번 dispatch된 branch 안에서 여러 guest cycle/반복을 소모한다
  (`sh2pico.c:307-575`). 카운터는 이를 1로 센다. 코어별 fastloop hit mix가 다르면 ratio가
  interpreter CPI가 아니라 helper 비용을 반영한다.
- IRQ/slice 경계 비용은 `pp_msh2` 안에 들어갈 수 있지만 dispatched count와 같은 비율로 늘지 않는다.
- 두 코어 모두 ROM을 비슷하게 쓰면 ratio가 같아도 XIP는 공통 병목일 수 있다. 반대로 master
  ratio가 높아도 memory-heavy opcode나 MMIO가 원인일 수 있다.

필요한 최소 측정은 세 실행으로 분리해야 한다.

1. **phase-only**: per-insn counter 없이 현 DWT pprof만 켜서 `pico/total`, `msh2`, `ssh2`를 잰다.
   같은 workload의 profiler-off wall과 비교해 계측 침입도 함께 보고한다.
2. **count-only**: 사이클 판정에 쓰지 않는 별도 빌드에서 코어별 dispatch 수, opcode-fetch 지역
   (ROM/SDRAM/BIOS/other), 전체 read/write 지역과 opcode class를 센다. 가능하면 함수 로컬 32-bit
   카운터를 slice 끝에 64-bit global로 합쳐 매 dispatch 64-bit 메모리 RMW를 없앤다.
3. **causal A/B**: profiler를 끈 동일 세이브/입력에서 fetch 경로만 바꿔 wall fps 또는 coarse
   DWT와 framebuffer/state를 비교한다. XIP 귀속은 이 개입이 있어야 성립한다.

QEMU PC histogram은 “어느 주소가 몇 번 실행됐나”를 찾는 데는 쓸 수 있다. 다만 그 count로
기기 사이클 절감을 주장하면 안 된다.

## (2) XIP가 맞다면 현 예산 안에서 취할 수 있는 수가 있는가

### 판정: **일반 hot cache는 REFUTED, 무저장 fetch 단축은 SURVIVES**

### 일반 캐시

- RAM_EMU 696B와 AHB 856B로는 data와 tag/refill code를 가진 유효한 ROM cache를 만들 수 없다.
- ITCM 4,688B는 이론상 4KB 단일 window 하나가 한계다. 4KB를 쓰면 정렬·분기·메타데이터와 향후
  code growth에 688B만 남는다.
- 현재 SH2 read map의 한 entry는 `SH2_READ_SHIFT=25`, 즉 32MB 단위다. 4KB만 기존 map으로
  갈아끼울 수 없다. 매 opcode/data read에 별도 range check를 넣어야 하므로 miss 경로에도 비용을
  부과한다.
- ROM은 M7에서 data load로 읽혀 D-cache의 도움을 받을 수 있다. 이미 cache-hot인 4KB를 ITCM에
  복사하면 물리 XIP stall이 거의 줄지 않을 수도 있다. 단순 “주소가 ROM” count는 miss 비용의
  증거가 아니다.

따라서 동적 page cache는 NO-GO다. 예외는 **post-fastloop 현재 Doom**의 hot access가 한 4KB
window에 극단적으로 집중되고, 그 window를 시작 시 한 번 복사한 device A/B가 큰 이득을 보일
때뿐이다. 과거 `0x02036f36`의 6-byte countdown loop 집중도는 근거가 될 수 없다. 그 루프는 이미
fastloop로 제거됐으므로, 반드시 현재 빌드의 잔여 histogram을 새로 봐야 한다.

### 예산 없이 가능한 것

1. **의도됐지만 실제로 꺼진 opcode-fetch inline path**

   `sh2pico.c:61-66` 주석은 SDRAM direct access로 cross-TU call을 없앤다고 쓰지만 실제 정의는

   ```c
   #define GNW_FETCH_SD(sh2, addr) ((UINT32)(UINT16)RW(sh2, addr))
   ```

   이다. 모든 opcode fetch가 `p32x_sh2_read16()`을 호출한다. 해당 callee는 RAM_EMU에 있고
   (`STM32H7B0VBTx_SDCARD.ld:556`), SDRAM early return도 이미 있지만 call 자체는 남는다.
   과거 direct macro는 QEMU에서 +3.9%라 폐기됐으나 QEMU는 ITCM/XIP stall 판정기가 아니다.
   동일 변경을 **profiler-off 기기 A/B**하는 것은 아직 유효하고 수백 byte 이하라 ITCM 예산 안이다.

2. **ROM direct-map fast path**

   ROM map은 master/slave 모두 `MAP_MEMORY(Pico.rom)`으로 대칭이다
   (`memory.c:2418-2434`). 표준 비뱅크 ROM에 한해 generic map lookup을 생략할 수 있다. 다만
   SSF2/bank handler와 byte order를 우회하면 조용히 깨지므로 우선 SDRAM inline을 재게이트하고,
   ROM variant는 map semantics를 그대로 인라인하거나 non-banked 조건을 명시해야 한다. 이것은
   map/call overhead만 없애며 OSPI access latency 자체는 남는다.

3. **잔여 fastloop/PC specialization**

   이 프로젝트에서 가장 큰 실현 이득은 이미 generic cache가 아니라 게임의 poll/countdown loop를
   cycle-exact하게 접은 데서 나왔다. 현재 Doom 잔여 PC histogram에 또 하나의 side-effect-free 또는
   event-modelable loop가 있으면 수십~수백 byte로 처리할 수 있다. 없으면 이 수는 없다.

4. **OSPI 전역 튜닝**

   현재 OSPI는 quad memory-mapped linear burst지만 wrap은 꺼져 있다(`main.c:953-958`,
   `gw_flash.c:512-543`). cache-line refill microbenchmark 후보는 되지만 모든 코어와 flash chip에
   영향을 주는 resident 변경이므로 32X 전용 저위험 수가 아니다. 가장 뒤에 둔다.

## (3) `msh2` 59.4% 안의 다른 회수 성분

### 판정: **SURVIVES**, 단 아직 분량 미확정

다음 비용은 실제 코드에 남아 있다.

- 매 opcode의 external `p32x_sh2_read16()` call과 주소 분류.
- 16-way top-nibble computed goto 뒤 opcode-group 하위 switch.
- data read/write의 map lookup 및 CS0 handler/poll bookkeeping. SDRAM read8/16/32 direct path는 이미
  있으므로 이 부분을 미개척 100%로 보면 안 된다 (`memory.c:1965-2022`).
- branch/delay/IRQ bookkeeping과 fastloop reject prefilter.
- 아직 모델 가능한 잔여 poll/countdown loop가 있다면 반복 dispatch 전체.
- 실제 ROM line fill/stall. 이것은 위 항목과 별개이며 cache-miss/locality 증거가 필요하다.

반면 이미 banked된 것은 threaded dispatch, lazy-T, 여러 Doom/VR/Kolibri/Tempo/Metal Head fastloop,
SDRAM read early return, interpreter 전체 ITCM 이동이다. 59.4%를 “처음 보는 원시 인터프리터”처럼
다시 최적화할 수는 없다.

회수 목표의 크기도 냉정하게 봐야 한다. Doom 19.5→24.5fps는 wall time을 20.41% 줄여야 한다.
59.4%를 전체 wall share로 잘못 최대로 잡아도 master 내부를 **34.4%** 줄여야 한다. 실제로는
`pct_of_pico`이므로:

| `PicoFrame / wall` | 실제 msh2 wall share | +5fps에 필요한 msh2 내부 절감 |
|---:|---:|---:|
| 100% (비현실적 상한) | 59.4% | 34.4% |
| 84% | 49.9% | 40.9% |
| 80% | 47.5% | 43.0% |
| 70% | 41.6% | 49.1% |

따라서 call/map 몇 사이클을 깎는 실험은 해볼 가치가 있지만 그것 하나가 +5fps 경로일 가능성은
낮다. +5를 노리려면 XIP miss 또는 잔여 loop처럼 master 비용의 35~50%를 지우는 성분이 실제로
나와야 한다.

## (4) SNES APU wire보다 기대값이 높은가

### 판정: **지금은 SNES APU wire가 SURVIVES; 32X 전면투자는 REFUTED**

32X는 이론 ceiling이 크지만 실현확률이 낮다. 59.4%의 분모가 PicoFrame이고, ROM attribution도
미확정이며, 일반 캐시는 메모리 예산으로 막힌다. 게다가 19.5→24.5는 20.41% wall 절감이라
필요 절감폭 자체가 크다.

SNES exact Zelda APU 쪽은 +5에 필요한 wall 절감이 11.04%이고, exact player와 LLE oracle이라는
구현 자산이 이미 있다. 아직 device 3-ledger로 exclusive APU share와 wire cost를 재야 GO를
확정할 수 있지만, “무엇을 대체할지”는 32X XIP보다 구체적이다. 따라서 엔지니어링 주력의 현재
기대값은 SNES가 높다.

다만 32X에는 비용이 작은 옵션가치가 있다. 아래 두 A/B만 먼저 하는 것은 합리적이다.

1. profiler-off baseline vs 실제 SDRAM inline fetch, 동일 Doom 구간.
2. phase-only와 count-only를 분리해 `pico/wall`, core dispatch 수, fetch/read region, 잔여 PC
   concentration을 산출.

진행 게이트는 다음처럼 잡는다.

- inline fetch가 device wall에서 최소 약 5% 또는 약 1fps를 직접 회수하면 2차 ROM direct-map과
  ITCM 소규모 dispatch 개선을 계속한다.
- 아니면 current hot 4KB window가 master ROM traffic의 압도적 다수를 차지하고 static-copy A/B가
  양성이어야 캐시를 계속한다.
- 둘 다 아니면 32X는 보류하고 SNES APU device 판정으로 돌아간다.

## 최종 한 줄

**32X `msh2`를 지금 파도 되나: 전면 착수는 안 된다. 다만 “cycles/insn만 보고 XIP 캐시”가 아니라, 미실측인 device inline-fetch A/B와 분리 계측 두 번까지만 지금 파는 것은 YES다.**
