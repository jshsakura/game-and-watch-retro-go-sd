# SNES CPU 대기축 적대적 검토

- 검토일: 2026-07-21
- 검토 대상: `perf/snes-wai-survey` / `b0b4da9f148fa225a42128a701f246e09d39ef8c`
- 주 데이터: `tools/snes_survey/wai_summary.txt`, `rpi-genie5:/tmp/snes_wai_remote/wai.tsv`
- 보조 데이터: 2026-07-15의 기존 1200-frame START-pulse actual-read spin sweep
- 범위: 검수 전용. 구현이나 신규 corpus sweep은 하지 않았다.

## 검토 결론

"WAI는 이 관측 범위에서 좁다"는 결론은 대체로 살아남는다. 그러나 다음과 같은 더 강한 결론은 데이터에서 따라오지 않는다.

> SNES CPU dead-wait 축은 library-wide 최적화 레버가 아니며, 기존 spin-skip이 남은 기회를 이미 가져갔으므로 이 축은 소진됐다.

최종 판정은 **refuted**다. 축 전체를 닫으면 안 된다.

| 공격 지점 | 판정 | 요약 |
|---|---|---|
| 동일수치 클러스터 | 판단불가 | 공통 바이너리 드라이버는 대체로 아니지만, START 대기라고 특정할 수도 없다. 반올림, 동일 게임 변형, 공통 프로토콜, 부팅 교착이 섞여 있다. |
| APU 0-10% 구간 기각 | refuted | 얕은 구간의 corpus 총질량이 40%+ 꼬리보다 크다. 실제 host 이익은 미측정이지만 무가치로 버릴 근거는 없다. |
| 도트가중을 의사결정 척도로 사용 | refuted | emulated-time 점유율이지 recoverable host time이 아니다. 과대·과소 방향은 device A/B 없이는 정할 수 없다. |
| 대기축 소진 단정 | refuted | 측정 제약, 분류 오탐, 미검증 표본, 기존 spin-skip의 IO 제외 및 배포 범위 때문에 과대주장이다. |

## 1. 동일수치 클러스터

### 판정: 판단불가

관측된 동일값은 원시 정밀값이 아니다. 하네스가 모든 비율을 소수점 한 자리(`%.1f`)로 출력하므로 같은 0.1% bucket에 들어간 값이다. 따라서 "소수점까지 동일" 자체에는 강한 식별력이 없다.

클러스터도 독립적인 무관 게임 집합이 아니다.

- HV 97.0%의 세 DBZ ROM은 같은 게임의 지역판·명칭 변형이다.
- APU 96.6%의 Yoshi Island 두 ROM도 같은 게임 계열이다.
- Eric Cantona Football Challenge와 Striker는 최종 `lit=55114`까지 같고, 기존 actual-read 조사에서도 미러 뱅크의 거의 같은 코드 위치에서 `$4212`를 폴링했다. 같은 엔진 또는 리브랜딩 계열로 보는 편이 자연스럽다.

반면 APU 96.6% 여섯 ROM은 모두 최종 framebuffer가 `lit=0`이었다. 기존 actual-read 자료에서는 서로 다른 PC들이 공통으로 `$2140` 계열을 폴링했다.

| ROM/계열 | actual-read 관측 예 | 해석 |
|---|---|---|
| Human Grand Prix III | `$80:f6d9`, `$2140` | APU IO spin |
| Batman: Revenge of the Joker | `$00:8021`, `$2140` | APU IO spin |
| Masoukishin | `$00:8e20`, `$2140` | APU IO spin |
| Yoshi Island 두 ROM | `$00:8440`, `$2140` | 동일 게임 계열 |
| Rendering Ranger R2 | `$36:8537`, `$2143` | 다른 코드 위치, 같은 APU 프로토콜 계열 |

이는 공통 바이너리 드라이버/BIOS 루틴보다는 여러 게임이 같은 APU handshake 프로토콜에서 멈춘 모양에 가깝다. 따라서 driver HLE의 증거는 아니지만, 같은 MMIO 이벤트를 기다리는 generic event-aware skip 가능성까지 부정하지는 않는다.

"입력 없는 타이틀 화면에서 START를 기다린다"는 더 구체적인 원인도 현재 자료로는 입증되지 않는다. 다음 상태가 모두 같은 heavy tail을 만들 수 있다.

- 정상 타이틀/attract loop
- APU 부팅 handshake 교착
- 비지원 enhancement chip 또는 코어 호환성 실패
- 입력이 있어야 진행되는 초기 상태
- 분류기의 false positive

HV 양성 캘리브로 사용한 BS-X BIOS도 보조 actual-read 조사에서는 주된 IO poll이 대상 밖인 `$4213`이었다. 캘리브 자체가 `$4210/$4211/$4212` 검출의 강한 양성 대조군이라고 보기 어렵다.

### 판별에 필요한 자료

- 실제 read hook으로 확인한 MMIO 주소
- loop anchor PC, period, 디코드된 명령열
- 시간 구간별 점유율과 loop 진입/이탈 frame
- 최종 framebuffer가 아니라 실행 중 render 여부의 누적 기록
- scripted START와 gameplay savestate에서의 동일 계측

## 2. APU의 넓고 얕은 분포

### 판정: refuted

0-10% 구간을 "얕아서 무가치"로 버리면 corpus 전체 기회량을 잘못 읽는다.

| APU 구간 | ROM 수 | 구간 평균 | 비율 합계 | 전체 APU 질량 중 비중 |
|---|---:|---:|---:|---:|
| 0-10% | 1,159 | 3.706% | 4,295.5%p | 32.3% |
| 10-20% | 239 | 14.062% | 3,360.9%p | 25.2% |
| 20-40% | 84 | 25.963% | 2,180.9%p | 16.4% |
| 40%+ | 45 | 77.358% | 3,481.1%p | 26.1% |

0-10% 구간의 합계는 40%+ heavy tail보다 크다. 전체 2,100개 OK 표본에 균등 가중하면 이 구간만의 이상적 제거 상한은 평균 약 2.05%p다.

최종 framebuffer가 `lit>0`인 1,675개만 보면 경향은 더 강하다.

- 0-10% 구간: APU 질량의 39.6%
- 10-20% 구간: 31.1%
- 20-40% 구간: 20.2%
- 40%+ 구간: 9.1%

따라서 정상적으로 화면을 낸 표본에서는 heavy tail보다 넓고 얕은 구간이 더 중요한 후보군이다.

다만 이 수치는 최적화의 가치가 아니라 탐색할 가치만 입증한다. 3-5%를 회수하려고 항상 켜는 detector가 더 비싸면 순손실이다. low-overhead read-path specialization 또는 device-side A/B가 필요하다.

## 3. 도트가중의 적합성

### 판정: refuted as decision metric

도트가중은 "에뮬레이트된 master-dot 중 이 상태가 차지한 비율"에는 맞다. 그러나 "M7 host cycle 중 제거 가능한 비율"과 같지 않다.

WAI continuation은 `cpu_runOpcode()`에서 1 CPU cycle을 반환하는 early-return이다. 한 호출은 싸지만 6 master dots마다 다시 호출된다. 데이터에서도 척도 선택에 따라 40% threshold를 넘는 수가 달라진다.

| WAI 척도 | 양수 ROM | 20%+ | 40%+ | corpus 평균 |
|---|---:|---:|---:|---:|
| dot share | 109 | 87 | 69 | 2.481% |
| dispatch share | 113 | 91 | 85 | 3.480% |

dot 기준 40% 미만이지만 dispatch 기준 40% 이상인 ROM이 17개다. 따라서 dot weighting은 적어도 WAI를 일관되게 부풀린 척도가 아니며, 오히려 이 threshold에서는 더 좁게 보이게 했다.

APU poll은 비용 구조가 더 복잡하다. `$2140-$2143` read마다 `snes_catchupApu()`가 호출된다. CPU poll loop를 건너뛰어도 SPC700/DSP의 필수 작업은 남지만, 잘게 쪼개진 catch-up 호출을 묶는 이익은 생길 수 있다. dot share만으로는 양쪽을 분리할 수 없다.

필요한 의사결정 척도는 다음과 같다.

1. WAI early-return path와 event-loop skeleton의 DWT self-time
2. HV/APU poll opcode와 `snes_catchupApu()`의 DWT self-time
3. 구간별 대표 ROM에서 event-aware skip OFF/ON device A/B
4. 정확성 gate를 통과한 뒤의 순 cycle 절감과 detector overhead

이 자료가 없으므로 dot share가 host 비용을 과대평가하는지 과소평가하는지의 방향은 판단불가다.

## 4. "대기축 소진" 결론

### 판정: refuted

### 4.1 실험 범위가 library-wide gameplay 결론을 지지하지 않는다

- 기본 실행은 600 frames다.
- 앞 120 frames를 버리므로 집계 창은 480 frames뿐이다.
- 모든 frame에서 controller state는 0이다.
- 181개 crash를 제외한다.
- OK 2,100개 중 425개는 마지막 framebuffer가 `lit=0`이다.

따라서 전체 2,281개 중 crash 또는 최종 blank인 표본은 606개, 26.6%다. `lit=0`이 실행 내내 미렌더였다는 뜻은 아니지만, 적어도 정상 gameplay를 관측했다는 증거도 아니다.

40% threshold는 WAI/HV heavy-tail의 집중도를 설명하는 데에는 유용하다. 실제로 WAI와 HV의 전체 측정 질량 중 40%+ 표본이 각각 87.7%, 86.8%를 차지한다. 하지만 같은 threshold를 APU의 library aggregate value에 적용하면 넓고 얕은 질량을 버리게 된다.

### 4.2 HV/APU는 기존 spin-skip과 중복되지 않는다

현재 `spin_skip.c`는 `$21xx`, `$42xx` 등 IO-classified read가 한 번이라도 있으면 `io_seq`를 증가시키고 adoption을 거부한다. 즉 HV/APU poll loop는 기존 exact-replay spin-skip이 의도적으로 처리하지 않는 영역이다.

배포 범위도 library-wide가 아니다. 현재 whitelist에는 다음 두 항목만 있고 실제 ON은 Super Mario World 하나다.

- Super Mario World: ON
- The Legend of Zelda: OFF
- 나머지 ROM: default OFF

따라서 "기존 spin-skip이 branch-to-self를 이미 가져갔다"는 말은 알고리즘의 존재와 corpus 배포 완료를 혼동한다.

### 4.3 OTHER 전체를 작업루프 아티팩트로 기각할 수 없다

WAI survey의 OTHER detector는 단순 PC 반복만 보고 register recurrence, write-free, actual read address를 검사하지 않는다. Doom과 Street Fighter Zero 2 같은 top 사례가 false positive/작업루프일 가능성은 높다. 하지만 top 10만으로 1,518개의 40%+ 표본 전체를 설명할 수는 없다.

저장소에 이미 존재하던 2026-07-15의 별도 spin sweep은 다음과 달랐다.

- 1200 frames
- 200-frame warmup
- 주기적인 START pulse
- actual CPU read hook
- 동일 PC 재방문 시 모든 register/flag 동일성 확인
- write-free와 IO read 여부 분리

그 조사의 rendered-status 1,792개 결과는 다음과 같다.

| 척도 | 결과 |
|---|---:|
| PURE-spin 평균 | 44.75% of opcodes |
| PURE-spin 중앙값 | 52.13% |
| PURE-spin 50%+ | 941 / 1,792 |
| IO-spin 평균 | 9.19% of opcodes |
| top poll이 APU port | 332 / 1,792 |
| top poll이 HV register | 139 / 1,792 |

두 조사에서 이름이 매칭되고 보조 조사상 rendered-status였던 1,645개를 비교하면 다음과 같다.

- WAI survey OTHER와 actual probe PURE-spin의 Pearson correlation: 0.613
- `OTHER>=40%`: 1,213개
- 그중 `PURE-spin>=50%`: 815개

보조 조사는 더 오래된 코어와 2,497개 library를 사용했으므로 현재 2,281개 결과를 대체하지 않는다. 그러나 "OTHER는 거의 전부 작업루프 아티팩트" 및 "대기축 소진"과 정면으로 충돌하는 기존 증거다. 축을 닫기 전에 두 결과를 같은 코어·같은 ROM set·같은 입력 프로토콜로 화해시켜야 한다.

## 계측상 추가 결함

### HV/APU classifier가 실제 operand를 검증하지 않는다

`classify_loop()`는 instruction boundary나 addressing mode를 디코드하지 않는다. `[min_pc, max_pc + 4]` 바이트창에서 다음 두 바이트쌍을 어디서든 찾는다.

- HV: `10/11/12 42`
- APU: `40/41/42/43 21`

따라서 다음이 모두 false positive가 될 수 있다.

- 다른 명령의 immediate/data byte
- 인접한 두 명령에 걸친 우연한 byte pair
- inline table/data
- 실제 loop 밖에 추가된 `max_pc + 4` 영역

실제로 WAI survey에서 HV 96.5%였던 Ogre Battle은 보조 actual-read 조사에서 PURE-spin 66.84%, 전체 IO-spin 0.59%였다. 두 조사의 입력과 기간이 다르므로 단일 사례로 확정 오탐이라고 단정할 수는 없지만, 현 classifier의 분류 정확도가 검증되지 않았음을 보여준다.

### 출력에 원인 추적 정보가 없다

현재 raw TSV에는 group 비율만 있고 다음이 없다.

- loop PC와 period
- 실제 또는 추정 operand 주소
- loop byte signature
- frame별 점유율
- 분류별 top-N site

따라서 동일값 클러스터가 같은 코드인지, 같은 프로토콜의 다른 코드인지, 우연한 오탐인지 사후 판별할 수 없다.

## 닫기 전에 필요한 최소 gate

축 전체를 닫으려면 최소한 다음을 충족해야 한다.

1. 실제 CPU read hook 또는 정확한 65816 decoder로 HV/APU를 재분류한다.
2. 최종 blank/crash/비지원 chip 표본을 정상 rendered gameplay 표본과 분리한다.
3. scripted input과 대표 gameplay savestate를 사용해 title-only bias를 제거한다.
4. 0-10%, 10-20%, 20-40%, 40%+에서 각각 대표 ROM을 추출한다.
5. 각 대표 ROM에서 device DWT 기준 OFF/ON 순 cycle 절감을 잰다.
6. 기존 PURE-spin 조사와 OTHER 결과의 불일치를 같은 코어와 입력 조건에서 해소한다.
7. generic detector overhead를 포함한 breakeven을 정하고 corpus 기대값을 다시 계산한다.

## 최종 결정

**이 축은 닫으면 안 된다.**

단, **WAI 단독 최적화는 library-wide 레버로 NO-GO이며 닫아도 된다.** 2,100개 중 94.8%가 0%, 40%+는 3.3%뿐이고 corpus 평균도 dot 기준 2.481%다. continuation이 1-cycle early-return이라 dot share 전부가 host 절감으로 바뀌지도 않는다. 다시 열 조건은 특정 WAI-heavy 타이틀을 별도로 최적화할 필요가 생기고 device self-time이 실제 병목으로 확인되는 경우뿐이다.

열어 둘 것은 WAI가 아니라 APU의 넓고 얕은 질량, spin-skip이 의도적으로 제외하는 HV/APU IO event-wait, 그리고 실제 배포되지 않은 PURE-spin corpus 기회다.

## 후속 APU gate 결과 (2026-07-21)

검토 뒤 기존 `spin_probe.c`의 actual CPU read hook으로 26종을 1200 frames(200-frame warmup + START pulse) 재측정하고, 대표 6종을 device-shaped M7 rig에서 비용 분해했다. 이는 신규 최적화 구현 결과가 아니라 다음 구현을 시작할지 정하는 gate다.

### 실제 APU signal 재현

- actual-read 재현: 17/26, 65.4%
- 1% threshold 기준 classifier 대 actual-read: TP 16, FP 7, FN 2, TN 1
- precision 69.6%, recall 88.9%
- Ogre Battle의 HV 96.5%는 actual IO 0.16%로 false positive가 재확인됐다.

정적 classifier의 개별 ROM 수치는 신뢰할 수 없지만, 얕은 구간에 실제 APU poll signal이 존재한다는 결론은 살아남았다.

### M7 비용 상한

300-frame 창에서 poll이 발현하지 않은 Art of Fighting 한 종을 제외한 5종의 회수가능 상한은 다음과 같다.

| ROM | actual IO | CPU poll 상한 | `catchup_self` | 합계 상한 |
|---|---:|---:|---:|---:|
| Super Tennis World Circuit | 99.94% | 44.85% | 16.94% | 61.8% |
| Super Rugby | 99.97% | 44.03% | 16.74% | 60.8% |
| Iron Commando | 36.41% | 15.9% | 5.32% | 21.2% |
| Oda Nobunaga | 16.62% | 6.5% | 9.71% | 16.2% |
| Muscle Bomber | 19.96% | 3.8% | 3.02% | 6.8% |

`catchup_self`는 SPC700/DSP의 필수 실행 비용을 제외한 `snes_catchupApu()` 호출·부기 자체의 비용이다. 대표군에서 frame cost의 3.02-16.94%였고, dot/opcode share에는 나타나지 않던 비용이다. 다섯 종 합계 상한 평균은 약 33.4%다.

모든 계측 build는 기존 결과와 STATEHASH/AUDIOHASH/COREHASH가 일치했다. 단, 위 숫자는 제거 가능한 비용의 **상한**이지 아직 구현으로 실현된 절감률이 아니다.

### 후속 판정

**APU는 GO다.** 다음 순서는 위험이 낮은 `snes_catchupApu()` 호출 batching prototype을 먼저 A/B하고, 실제 순절감이 확인된 뒤에만 CPU poll event-aware skip으로 확장한다. WAI는 이 결과와 무관하게 NO-GO 상태를 유지한다.
