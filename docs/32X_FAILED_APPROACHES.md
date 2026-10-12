# 32X 실패·보류 접근 목록 — 같은 실험 반복 방지

갱신: 2026-10-08. 사용자 요청: **범용으로 연산량 자체를 크게 줄여야 하며,
게임별 수동 최적화로 대체하지 않는다. 실패한 접근을 매번 다시 실험하지 않는다.**

32X 후보를 제안·구현·빌드·측정하기 전에 이 목록을 먼저 읽는다.
현재 유지할 기준은 `testbed-full-20261005-pcm-r2`, PicoDrive `10a5d460`이다.
마지막 Thumb 후보는 실기 성능 저하로 기각했고, 기존 펌웨어·코어·설정·저장 파일을
복원 및 readback 검증했다. **추가 실험은 예약돼 있지 않다.**

## 판정과 재시도 규칙

- **기각**: 해당 구현·조건에서 비용 증가, 실기 성능 저하 또는 정확성 문제가 확인됨.
- **효과 미미 / 미채택**: 작은 이득이나 일부 장면만의 이득. 범용 대폭 절감의 후보로 재포장하지 않는다.
- **보류 / 미실시 / 측정 실패**: 성능 결론이 없음. 이를 0%나 느림으로 기록하지 않는다.
- 기존 성공 패치와 수정된 초기 버그는 실패 접근 전체와 구분한다.
  아래 기록은 모든 범용 최적화가 불가능하다는 증명이 아니다.

같은 계열을 다시 검토하려면 해당 항목 ID와 **기존 실패 전제가 무엇 때문에 바뀌었는지**를
먼저 적는다. 이름 변경, 블록 수 조절, 진입 위치 조절, 컴파일 플래그 교체,
다른 공개 프로젝트에서 같은 아이디어를 발견한 것만으로 새 가설이 되지 않는다.
새 실행 구조·저장 공간·검증 방식 등이 기존 비용을 해결한다는 구체적 근거가 필요하다.
동일 입력·소스의 완료 결과는 재사용하고, 이전 `state.json`의 `next_hypothesis`나
오래된 인계의 “다음 후보”를 자동 실행 목록으로 취급하지 않는다.

숫자는 각 당시 기준과 비교한 값이다. **QEMU 명령 수 변화는 음수가 절감,
실기 FPS 변화는 음수가 성능 저하**다. 서로 다른 기준·창의 절대값을 섞지 않는다.
진단 카운터 포함/제외 결과도 별도로 다룬다.

## 최근 범용 후보: 같은 계열로 묶어 기각·미채택

| ID | 접근 / 시기 | 확인된 결과와 판정 | 같은 접근을 다시 제안하지 않기 위한 기준 |
| --- | --- | --- | --- |
| G01 | 고정 레지스터 Thumb SH2 실행기, v1–v4 / 10-07~08 | v4 clean rig는 After Burner 명령 수 −1.12193%, Doom −1.85656%. **실기 Doom 30.06313 → 25.48853 → 30.06163 FPS, −15.21453%: 기각.** A 드리프트 −0.00501%, 600 emu/600 drawn 및 endpoint 화면·음원 일치 | 중복 C fallback과 ITCM/XIP 배치 문제를 해결할 새로운 근거 없이 opcode 지원 추가나 작은 변형을 반복하지 않는다. 개별 저하 원인은 미계측이다 |
| G02 | 프로파일에서 자동 생성한 짧은 native C/AOT 블록, 공유 모듈 / 10-07 | 진입 전략 5종 모두 두 작업량에 공통 절감 없음. 아래 상세 표 참조. **기각** | 작은 블록의 lookup·live-code guard·fallback 비용을 해결하지 않은 블록 수/진입 위치 변경은 같은 실험이다 |
| G03 | ROM/PC 지정 없는 범용 순수 산술 span / 10-02 | After Burner +1.59321%, Doom +1.71523% 명령 수. 상태·화면·음원 일치했지만 **기각** | 진단에서 단일 명령 진입이 약 50~60%. 짧은 span의 재디코딩·lookahead 비용을 그대로 두고 재실험하지 않는다 |
| G04 | 직전 텍스처 lookup 주소의 결과 재사용 / 10-07 | After Burner f1200–1799: 3,927,098 lookup 중 연속 동일 주소 **0회**. 해당 창에서 기회 없음 | 부팅 구간의 20,459회 반복을 후기 구간의 이득으로 사용하지 않는다. 다른 접근에는 그 구간의 실제 재사용 증거가 필요하다 |
| G05 | PWM 좌우 raw 값이 같을 때 변환 결과 재사용 / 10-07 | 화면·음원 일치. 전체 명령 수 절감 Doom 0.00907%, After Burner 0.00162%, Kolibri 0.00218%. **효과 미미, 미채택** | 샘플 동일 비율이 높다는 것만으로 큰 이득을 주장하지 않는다. 전체 비용 절감은 이미 측정했다 |
| G06 | lookup 코드/목적지 범위 guard를 span 밖으로 이동 / 10-07 | After Burner 절감 0.36833%, Doom 증가 0.02319%, Kolibri 증가 0.02373%. **미채택, 실기 FPS 미측정** | 준비된 arm은 큰 범용 개선의 증거가 아니다. 소폭 단일 장면 개선을 다시 “획기적 후보”로 올리지 않는다 |
| G07 | PCM span 검사를 공통 dispatch에 추가, v1 / 10-04~05 | 호출 0회인 Doom +2.2796%, After Burner +2.4639% 명령 수. **그 진입 방식 기각** | 비대상 경로의 hook 비용이 사라졌는지 먼저 확인한다. 이후 수정된 PCM v3/PCM-r2 성공분을 실패로 분류하지 않는다 |
| G08 | native 패턴 hook을 모든 ROM에 적용 / 09-30~10-01 | Doom +3.9650%. opcode handler 안으로 이동한 변형도 Doom +6.68% 및 After Burner 악화. **그 hook 방식 기각** | 패턴의 미적용 비용을 실제로 제거하지 않은 hook 위치 변경은 새 접근이 아니다 |
| G09 | native helper 강제 인라인으로 compact화 / 10-01 | CPU text 17,552 → 18,112 B로 오히려 증가. 당시 baseline 12,808 B. **크기 목표 실패** | object text만 보고 전체 firmware 적합성을 가정하지 않는다. 강제 인라인을 공간 해결책으로 반복하지 않는다 |
| G10 | 빈 타일 가변 처리 대신 고정 8칸 처리 / 09-30 | 유효 실기 triplet −1.13991%, −0.79396%. 세 번째는 드리프트 초과로 제외. **기각, 기존 가변 처리 유지** | 같은 고정 묶음 재실험을 새 최적화로 취급하지 않는다 |
| G11 | SH2 top-nibble static label table / 09-30 | 정지 Doom +0.53901% FPS였으나 이동 구간 −0.27600%. **공통 이득 확인 못해 미채택** | 정지 장면 성공만 인용하지 않는다. 일반 computed-goto 전환을 미시도처럼 제안하지 않는다 |

최근 원본:
[Thumb 최종 보고](../build/32x-fixed-thumb-v4-20261007/report.json),
[실기 comparison](../build/32x-fixed-thumb-v4-20261007/device-000004/round-000001/comparison.json),
[AOT 보고](../build/32x-aot-blocks-20261007/report.json),
[산술 span](../build/32x-generic-span-20261002/summary.json),
[텍스처 census](../build/32x-texture-reuse-20261007/run-000002/result.json),
[PWM A/B](../build/32x-pwm-mono-reuse-20261007/ab-000001/result.json),
[lookup 결론](../build/32x-lookup-range-20261007/conclusion.json),
[PCM 상태/배포 기록](../build/32x-continue-20261004/state.json),
[native hook·크기 기록](../tools/32x_research/README.md),
[09-30 최종 후보](../build/32x-device-20260930/final-candidates-summary.json).

### G02: 이미 비교한 AOT 변형 5종

아래는 **QEMU 전체 프레임 host 명령 수 변화**다. 모두 endpoint 화면·음원 및
게스트 평균 실행량 gate를 통과했지만 공통 비용 절감에는 실패했다. 실기 실행은 하지 않았다.
게스트 dispatch 카운터가 포함된 arm끼리의 비교다.

| 변형 | After Burner | Doom |
| --- | ---: | ---: |
| 매 eligible 명령 진입, 128 blocks | +2.93232% | +5.86654% |
| control-flow 경계 진입, 128 blocks | +3.86834% | +8.75418% |
| entry-opcode gate, 128 blocks | −0.40281% | +0.45107% |
| timeslice 진입 + 연속 chain, 128 blocks | +0.18773% | +0.01275% |
| entry-opcode gate, 상위 32 blocks | +1.36211% | +1.76117% |

### G01: 이미 비교한 Thumb 변형과 실기 제약

| 변형 | 비교 조건 | After Burner 명령 수 | Doom 명령 수 |
| --- | --- | ---: | ---: |
| v1: 기본 고정 레지스터 실행기 | 이전 core 기준, guest 카운터 포함 | +2.71003% | +7.77256% |
| v2: C fallback의 중복 fetch 제거 | 이전 core 기준, guest 카운터 포함 | +0.71482% | +4.83573% |
| v3: 최신 PCM 기준 + 일반 branch | guest 카운터 제외 | −0.67661% | +0.02747% |
| v4: 실제 bus callback을 쓰는 store 추가 | guest 카운터 제외 | −1.12193% | −1.85656% |

v4 proof는 6,016 B 전체 SH2 context 121,159건, supported opcode encoding 50,022개를
비교했다. 합성 IRQ/write callback proof와 실제 게임 rig 검증이며 모든 상태의 증명은 아니다.
실기 arm 최초 링크는 ITCM **2,408 B 초과**. C handler section 6개를 XIP로 이동한 뒤
링크됐고, candidate ITCM 여유 240 B, benchmark RAM_EMU 여유 12 B였다.
이 배치에서 Doom 실기 FPS가 15.21453% 하락했다. C 전환과 XIP 이동은 원인 후보이며,
각 비용을 분리 측정하지 않았으므로 원인을 확정하지 않는다.
실기 한 triplet의 기술적 비교이며 전체 스트림/모든 게임 정확성이나 통계적 유의성은 주장하지 않는다.

## 예전 기각 접근: 이름을 바꿔 다시 제안하지 않는다

이 표의 숫자는 07~09월의 **당시** 기준·장면이다. 최신 PCM-r2의 성능 수치가 아니다.
로컬 원본이 없는 과거 항목은 보존 문서의 기록으로 표시하며 원본 재검증을 주장하지 않는다.

| ID | 접근 | 당시 결과 / 기각·제약 근거 | 보존 근거 |
| --- | --- | --- | --- |
| H01 | 핸드컴파일 SH2 block tier (`bt/bt2/bt4`) | Doom 명령 수 +6.6% / +5.1% / +1.1%, 해시 일치. 비용 증가 | [기존 실패 기록 §1.1](../GNW_32X_PERF_FINDINGS.md#11-핸드컴파일-블록티어-rig_blocktier-2026-09-10-warmup20-시절-기준-base8813384) |
| H02 | 대형 단일 C AOT loop trace, 원래 opcode handler 재호출 | hook만 +4.4%; NO_FASTLOOPS의 동일 기준에서 활성 +20.8%; 코드 +23,312 B. 정확성 수정 후에도 기각 | [기존 실패 기록 §1.2](../GNW_32X_PERF_FINDINGS.md#12-aot-루프트레이스-rig_looptrace-2026-09-1011--사후-검증까지-완료한-본격-시도) |
| H03 | 범용 per-op dispatch folder | 12,560,257 → 12,797,823 명령 수. 재dispatch로 비용 증가 | [기존 실패 기록 §2](../GNW_32X_PERF_FINDINGS.md#2-핵심-교훈--같은-것을-다시-들이대지-않기-위한-규칙) |
| H04 | guest data load 인라인 | checksum 동일, 명령 수 +3.6% | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H05 | `delay/test_irq/ppc`를 interpreter local로 유지 | 명령 수 +3.14%; register spill 확인. 고정 Thumb 전체 설계와 별개로 이미 시험한 C local 추가 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H06 | NOP를 second-level switch 전에 별도 처리 | rig 0.10% 수준, 미미. loop iteration 비용은 남음 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H07 | Doom SDRAM handshake poll parking | gameplay 실기 15.89 → 15.61 FPS, −1.8%; detect 비용. 원형은 lockup, safety-net 변형도 느림 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H08 | 초기 SH2 idle-skip | 당시 실기 이득 0%, Doom gunshot 음원 훼손. 이것이 이후 정확한 opcode countdown fold의 실패를 뜻하지는 않음 | [기존 optimization ledger / 32X](OPTIMIZATION_LEDGER.md#sega-32x) |
| H09 | Z80 backward-jump마다 idle fold / slice마다 fold / 매 8번째 line sync | 실기 −6.9%(hook만 −6.1%) / 116,694 slices에 fold 0회 / 이득 0% | [기존 do-not-retry 표](32X_NEXT_SESSION.md#do-not-re-try-all-measured-on-hardware-in-the-gameplay-anchor) |
| H10 | MD FM 또는 Z80를 꺼서 32X 작업 제거 | rig 감소는 있었으나 **둘 다 audio hash 변화**. 실제 사용 중인 칩 제거라 정확성 실패 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H11 | `-O3`, `-Os`, `-funroll-loops`로 해결 | 전체 -O3 rig −1.45%였으나 실기 −2.21~−2.48%; ITCM-only도 저하. -Os rig +9.2%, O3+unroll +0.13%. 별도 과거 실기 cz80 -Os −3.9%, ym2612 -Os −2.4%, sh2pico -O3 −1.1% | [실기 반전 기록 §10](../GNW_32X_PERF_FINDINGS.md#10-디바이스dwtfps-측정-체계-전환-과업-2026-09-14--o3-부호-반전-입증-채택-철회), [과거 flags 표](32X_NEXT_SESSION.md#do-not-re-try-all-measured-on-hardware-in-the-gameplay-anchor) |
| H12 | SH2 register context를 DTCM으로 이동 | 94.0 → 92.1 cycles/insn은 scene noise 내; shared heap 12 KB 소모. 원복 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H13 | 작은 `chan_render`를 ITCM으로 이동 / interpreter 함수 순서 변경 | chan_render 효과 0%. 중간 삽입·재배치 arm −11.6% FPS, 원인은 미확정 | [기존 do-not-retry 표](32X_NEXT_SESSION.md#do-not-re-try-all-measured-on-hardware-in-the-gameplay-anchor) |
| H14 | MD blank nametable row cache, 초기 4/16 slots | rig frame −1.82%였으나 실기 24.01 → 23.66 FPS, −1.46%. 16 slots는 비용 증가 및 당시 공간 초과 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H15 | compositor solid-run detector 제거 | rig 절감 0.48%였으나 실기 25.46 → 24.71 FPS, −2.95%. detector 유지 | [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |
| H16 | SAT 동일값 skip / sprite DIRTY gate / SYNC_NEXT 억제로 MD 무변화 프레임 생략 | 각 단계 rig 약 0~0.18%. 당시 RGB555 compose-in-place가 매 프레임 MD 재렌더를 요구. MD layer 제거는 화면 해시 변경 | [기존 VDP 기록 §8](../GNW_32X_PERF_FINDINGS.md#8-md-vdp-draw-과업-결과-2026-09-13--구조적-상한-입증-코드-미반영) |
| H17 | 작은 실행 RAM cache만으로 기존 DRC 재도입 | 당시 hot set 872 blocks 추정 native 117~234 KiB vs 실행 공간 약 21 KiB. 18.4 KiB tcache 재검토도 해결 못함 | [DRC 재검토](32X_CLOSED.md#2026-08-27-the-dynarec-re-trial-closes-on-a-hardware-fact-not-a-policy-argument) |
| H18 | DMA2D로 기존 compositor/tile renderer 교체 | 이 part에는 필요한 colour-key 기능 없음. CLUT pass 등 별도 pipeline 가능성과 구분 | [기존 optimization ledger / 32X](OPTIMIZATION_LEDGER.md#sega-32x) |
| H19 | PicoDrive 기존 ARM assembly/DRC를 그대로 활성화 | A32 emitter이며 Cortex-M7은 Thumb. 바로 사용할 backend가 아님 | [기존 optimization ledger](OPTIMIZATION_LEDGER.md#sega-32x), [공개 reference 감사](../build/32x-public-reference-20261007/research.json) |
| H20 | OSPI 136 MHz (`PLLQ=5`) | 당시 console hang. 연산량 감소도 아님 | [기존 do-not-retry 표](32X_NEXT_SESSION.md#do-not-re-try-all-measured-on-hardware-in-the-gameplay-anchor) |
| H21 | 44.1 → 22.05 kHz만으로 해결 | 당시 +1.3% 수준, 청취 품질 문제로 미채택. 정확성 유지한 큰 범용 절감의 대안이 아님 | [과거 audio 기록](32X_NEXT_SESSION.md#2026-08-26-the-release-ships-44100--22050-without-the-stretcher-is-the-sound-the-user-already-complained-about) |

## 성능 실패와 구분할 보류·미실시 기록

| ID | 항목 | 현재 판단 |
| --- | --- | --- |
| U01 | 16 B name-row cache, 09-30 재설계 | 정지 A drift +0.70423%, 이동 −2.12530%로 각각 사전 한계 0.5% 초과. **측정 무효, 성능 결론 없음**. H14와 별도 구현. [최종 기록](../build/32x-device-20260930/final-candidates-summary.json) |
| U02 | empty scheduler 진단 / 10-06, 후속 완료 확인 10-09 | 초기 exit127 뒤 완료된 run-000003에서 Doom/After Burner/Kolibri 측정 **948,217 steps, 제거 가능 subdivision 0회**. 기존 event cap이 이미 STEP_N 이하라 추가 gate에 제거 기회 없음. **해당 후보 기각**, scheduling 변경·실기 실행 없음. [최종 결론](../build/32x-empty-scheduler-20261006/run-000003/conclusion.json) |
| U03 | LTO | 과거 기록은 기대치/링크 구조 검토만 있고 **미실시**. 성능 실패로 주장하지 않는다. [기존 §9.3](../GNW_32X_PERF_FINDINGS.md#93-빌드-플래그-ab-1800f-warmup1200--전-레인-해시-4종gate3-정확) |
| U04 | 전체 SH2→Thumb compiler / SD native sidecar / XIP translation | 완전 구현·실기 이득 없음. G02는 제한된 read/ALU 블록 prototype이며 full recompiler 실패의 증명이 아니다. writable SDRAM code 검증/무효화, code/index footprint, guard 비용과 실제 XIP 시간은 해결할 전제 |
| U05 | 공개 프로젝트 교체 / 10-07 조사 | Copy-and-Patch/PochiVM, MicroPython Thumb emitter, wasm3, Blink, Ymir, SaturnRecomp, PicoDrive 조사 완료. 바로 쓸 SH2→M7 core나 측정된 큰 개선 없음. **reference 연구이지 속도 비교 결과가 아님**. [research](../build/32x-public-reference-20261007/research.json) |
| U06 | 과거 잔여 fold 확장 “8% 불가” 계산 | 특정 Doom 창·당시 구현에서 잔여 cluster 합계 약 7.9%라는 상한 분석. **미구현**이며 다른 ROM·새 구조·최신 core의 불가능 증명으로 사용하지 않는다. [기존 §3](../GNW_32X_PERF_FINDINGS.md#3-상한-입증-왜-sh2-fold로는-8이-안-나오는가) |
| U07 | D32XR ROM 교체 | 게임별 엔진/ROM 변경이라 이번 범용 요구 밖. 과거 41 FPS 주장에는 움직이는 장면 확인 문제가 있었고 공식 build는 별도 작업량이었다. emulator의 범용 연산량 절감으로 인용하지 않는다. [기존 closed ledger](32X_CLOSED.md#ledger-of-closed-axes--all-closed-by-measurement-not-by-opinion) |

공개 reference를 재사용할 때도 **컴파일 속도 개선 ≠ 실행 연산량 감소**다.
Thumb opcode emitter, stencil stitching, tail-call threading은 이미 조사·후보에 반영한 아이디어다.
새 프로젝트 이름만으로 G01/G02를 새 실험으로 소개하지 않는다.

## 재발 방지: 실험 준비·해석에서 이미 발생한 오류

| ID | 오류 | 다음 작업에서 확인할 내용 |
| --- | --- | --- |
| E01 | Doom 이름 파일이 하나라고 가정, 10-08 attempt 2 중단 | SD에 `doom.32x`와 `둠 (Doom).32x`가 함께 있었다. ROM hash로 검증한 뒤 선택. 두 파일은 같은 SHA256 `0bfa9a1a9341bca5b68f29e0e94e334d9d2f781f84d02d60f51a8be24f12f11c`였다. 이름 개수로 실패시키지 않는다 |
| E02 | CONFIG CRC를 앞 552 B만 계산, 10-08 attempt 3 중단 | firmware는 **556 B 전체 구조, 마지막 CRC 4 B를 0으로** 놓고 계산. `zlib.crc32(config[:552] + bytes(4))`가 저장 CRC와 비교할 값. CONFIG 손상이 아니라 runner 검사 오류였다 |
| E03 | ITCM 두 interpreter 경로를 동시에 넣어 링크 초과 | G01 최초 +2,408 B. 함수/section 배치를 full ELF/map에서 먼저 확인. C helper 하나에 section을 붙였다고 outline handler까지 XIP가 되는 것은 아니다 |
| E04 | 손실된 PC histogram으로 기회/불가능 판정 | 예전 table은 최대 45% dispatch 유실. `dropped=0`과 측정 창을 확인. boot histogram을 gameplay hotness로 쓰지 않는다. [10-04 인계](32X_HANDOFF_20261004.md#다시-밟지-말-것) |
| E05 | off hook 또는 folding 0회를 기준과 동일한 비용으로 간주 | H02 비활성 hook +4.4%, G07 미적용 hook +2%대. 진정한 기준은 hook이 compile-out된 arm. 인라인으로 interpreter 전체가 커진 과거 fold 0회 +14%도 기록됨 |
| E06 | 완료 breakpoint에서 circular audio DMA를 그대로 둠 | 10-01 종료 후 반복 음원 발생. 완료 시간 기록 후 `audio_stop_playing` 적용, DMA 정지 검증. 최근 실기는 volume 0이며 오래된 handoff의 “음소거 금지/volume 1” 지시를 현재 조건으로 복사하지 않는다 |
| E07 | rig 이득을 실기 FPS 이득으로 간주 / 한 장면만 인용 | G01, H11, H14, H15에서 부호 반전 확인. 실기 A/B/A, 동일 작업량, A drift, 실제 화면 확인이 필요. 측정 실패·menu·정지 장면·움직임을 구분 |
| E08 | build 성공만으로 적합성 판단 / 다른 ELF·core를 섞음 | resident + `32x.bin` + `32x.xip`의 같은 arm 정체성을 검증. immutable arm manifest와 전체 linker budget을 확인. benchmark의 여유와 normal firmware 여유를 혼동하지 않는다 |

E01/E02 중단은 플래시/성능 측정 전에 발생했다. [attempt 2](../build/32x-fixed-thumb-v4-20261007/device-000002/result.json),
[attempt 3](../build/32x-fixed-thumb-v4-20261007/device-000003/result.json).
해결된 준비 오류를 후보의 성능 실패나 추가 성능 샘플로 세지 않는다.

## 유지할 성공분 / 다음 세션의 출발점

- PCM-r2 및 기존 검증된 native fold/가변 blank-run 개선은 유지한다.
  기존 성공 루틴을 이번 요구의 새 “범용 대폭 개선”으로 다시 소개하지 않는다.
- 최신 실기 기각은 G01이다. 기기는 원래 정상 firmware로 복원됐다.
  후속 S01 single-commit은 Doom 한 A/B/A에서 **+1.99796%**였으며 아래 완료 기록을 우선한다.
  과거 인계의 설치 버전·볼륨·다음 후보보다 이 기록과 해당 완료 JSON이 우선한다.
- 새 근거가 없다면 이 목록의 결론을 재사용하고 실험을 시작하지 않는다.
  새 근거가 있다면 먼저 해당 실패 ID, 바뀐 전제, 예상 전체 비용/공간, 정확성 검증 범위를 남긴다.
- `build/` 링크는 로컬 상세 증거이며 배포 문서에 포함되지 않을 수 있다.
  핵심 수치·조건·판정은 이 문서에 남겼다. 원본을 못 읽으면 이를 재검증했다고 주장하지 않는다.

## 후속 검토: S01 완료 근거와 S02 보류

2026-10-08 사용자가 spin-skip 같은 연산 제거 방향을 다시 요청했다.
[반복 제거 후보 검토](32X_WORK_ELIMINATION_CANDIDATES.md)에 S01/S02를 기록했다.
S01은 최신 baseline ELF에서도 세 memory-poll helper의 **host 내부 반복**이 남아 있음을
확인한 후보다. H07/H08의 scheduler parking과 달리 기존 시간 구간을 유지하는 변환을 검토한다.
이후 사용자 요청으로 Codex가 직접 isolated census·후보·proof·clean rig를 수행했다.
Doom f900–1499에서 9,569,584 내부 반복 중 9,450,132회 제거 기회를 확인했다.
첫 후보는 65,536 전체 context 차등검증을 통과했고 clean rig Doom **−4.37634%**,
After Burner **+0.11411%** host 명령 수였다. CPU text +268 B의 중복 fallback을
실제로 제거하는 compact 변경만 후속 검증한다. 새로운 게임별 경로·hook은 추가하지 않는다.
이는 **범용 opcode 변환의 부분 작업량 이득이며 실기/모든 게임의 성공이나 정식 채택이 아니다.**
새 실기 실행은 하지 않았다. 완료 근거는 위 후보 문서에 연결했다.
compact 변환은 원래 while의 첫 반복을 재사용해 중복 block을 없앴고, CPU text를
기준 대비 **128 B 줄였다**. 최종 clean rig는 Doom **−4.41392%**, After Burner
**+0.07438%** host 명령 수, endpoint 화면·전체 audio signature 일치 및 65,536상태
차등검증 통과였다. **실기 미검증·미배포**로 보존한다. 동일 census/proof/clean을 반복하지 않는다.
[이전 compact 보고](../build/32x-poll-fold-20261008/run-000002/report.json),
[보존 패치](../tools/32x_research/patches/s01-stable-sdram-bts.patch).

이후 실제 GCC 15 compact 전체 링크는 ITCM **24 B 초과**로 실패했다. 해당 arm은
플래시하지 않았다. 바뀐 전제는 첫/두 번째 비교의 중복 context store를 없애고
지역 변수 계산 뒤 한 번만 commit하는 것이다. 최종 single-commit은 기존 section 배치를
유지하며 benchmark 전체 링크에 성공했다(ITCM/BSS end `0x10000`, 여유 0 B).
실제 GCC 15의 65,536 전체 helper 차등검증 및 clean 화면/audio 비교를 통과했고,
host 명령 수는 Doom **−2.84412%**, After Burner **+0.00004157%**였다.
이 최종 수치를 이전 GCC 14 compact의 −4.41%/−128 B와 섞지 않는다.

**최신 실기 완료:** Doom 600 emulated/600 drawn, 340 MHz, A/B/A
**30.05711 → 30.65917 → 30.06012 FPS**, 이득 **+1.99796%**, A 드리프트 **+0.01002%**.
세 arm의 endpoint 화면·raw audio 해시 및 호출/sample 수가 일치했다. 보존 화면에서
Doom 레벨 내부를 확인했다. 한 triplet·입력 없는 구간이며 모든 게임/전체 스트림/
이동 중 정확성·성능이나 통계적 유의성은 주장하지 않는다. 정식 firmware 전체 fit은 미검증이다.
정상 firmware/core/config/save를 복원·readback하고 정상 부팅을 확인했다.
복원 후 RAM timeout 0을 적용하여 현재 켜진 세션을 유지한다. 후보는 미배포 상태다.
같은 census/proof/clean/실기 비교를 자동 재실행하지 말고 이 완료 근거를 재사용한다.
[최종 통합 보고](../build/32x-poll-fold-20261008/report.json),
[실기 완료](../build/32x-poll-fold-20261008/device-000002/device-000001/result.json),
[최종 패치](../tools/32x_research/patches/s01-stable-sdram-bts-single-commit.patch).

S02의 일반 idle 분석은 H07/H08과 가까우므로 감지 비용/wake 조건을 해결할 근거가 선행돼야 한다.

## S03: 불필요한 Z80 BIT 전용 표 제거 — 공간 확보 검증

2026-10-08 사용자 요청으로 ITCM 중복 데이터를 조사했다. PCM-r2의 `SZ_BIT[256]`은
`cz80_opCB.c` 두 곳과 `cz80_opXYCB.c` 한 곳에서만 사용되며 세 index 모두
`value & (1 << bit)`이다. 입력은 0 또는 단일 비트이므로 기존 `SZP`의 parity flag와
BIT 전용 표의 zero/parity flag가 일치한다. 이 제한된 입력에서 기존 SZP를 공유하고
SZ_BIT 배열·초기화를 제거하면 256 B BSS를 제거할 수 있다. 일반 SZ_BIT 입력 전체가
SZP와 같다는 주장은 하지 않는다. 입력 범위와 undocumented flag/carry 보존을 검증한다.

G09/H11/H13/G01과 대조: 강제 inline·최적화 flag·코드 재배치·handler의 XIP 이동이
아니라 이미 있던 lookup의 중복 데이터 제거다. Z80 실행이나 음원 기능을 끄지 않는다.
새로운 공통 dispatch hook과 추가 실행 연산도 넣지 않는다. S01 완료 성능은 재사용하며
S03의 속도 이득을 주장하지 않는다. 별도 정상 firmware clone에서 실제 GCC 15 전체 링크와
ITCM/RAM 여유를 확인하고 결과를 기록한다. 이번 작업은 기기 실행 없이 수행한다.

S03 준비 기록: legacy CZ80 주석의 비 UTF-8 인코딩 때문에 첫 준비가 종료됐다.
소스 bytes/Latin-1 처리로 원래 인코딩·CRLF를 보존하도록 수정했으며 성능 실패가 아니다.
원래 C 표 초기화로 1,056,768개 BIT flag/carry/indexed-address 조합을 비교해 일치했다.
정상 전체 링크는 별도 clone에서 진행한다. 사용자가 GLM 5.3 세션 1.1에 측정을 위임하여,
기존 S01 완료 기준을 재사용하고 S03 candidate만 Doom/After Burner 각 1회 clean rig
비교하도록 Enter 포함 전송했다. 기기 접근과 진행 폴링은 위임 범위에 없다.

S03 정상 링크 완료: 실제 GCC 15.2.1, S01 포함 정상 firmware의 ITCM/BSS end
`0xff00`로 **256 B 여유**, RAM_EMU BSS end `0x240ffdf0`로 **528 B 여유**다.
`Cz80_Exec` text는 기준·후보 **17,892 B**로 동일하고 32X object의 SZ_BIT는 제거됐다.
benchmark 상태/완료 marker는 정상 ELF에 없다. 초기 최종 validator가 다른 overlay의
동명 SZ_BIT까지 검사한 오류는 object별 검사로 고쳤으며 기존 링크를 재사용했다.
기기 변경 없음, 새 속도 개선 주장 없음. [정상 링크 검증](../build/32x-space-reclaim-20261008/run-000002/normal-link-validation.json),
[표 제거 패치](../tools/32x_research/patches/s03-cz80-bit-table.patch).

## S04: 기존 DTCM 예약 범위의 빈 공간 활용 — GLM 후속 검증

사용자가 256 B보다 넉넉한 여유를 요청했다. 완료 S03 정상 map 기준 DTCM BSS는
`0x20014700..0x20019ab0`(21,424 B), 기존 예약은 22 KiB다. 예약 끝은 `0x20019f00`,
실제 heap limit은 `0x20019efc`이므로 stack/redzone과 겹치지 않는 빈 범위는 **1,100 B**다.
현재 ITCM의 Z80 flag 배열 SZ/SZP/SZHV_inc/SZHV_dec는 각 256 B, 총 1,024 B다.
이 데이터를 기존 DTCM BSS 끝에 먼저 claim하면 예약량·heap start/limit·실행 코드의
배치를 바꾸지 않고 ITCM 여유를 256 → **1,280 B**로 늘릴 수 있다는 가설이다.
새 DTCM BSS end 예상 `0x20019eb0`, 실제 heap limit까지 76 B 남는다.

G01/H12/H13과 대조: handler를 XIP로 옮기거나 SH2 context/코드 순서를 변경하지 않는다.
데이터 주소만 기존 zero-wait DTCM의 미사용 예약 안에서 바꾸며 추가 heap 소비 없음.
기존 실패를 무효화하는 새 속도 주장은 하지 않는다. 주소 변경이 실제 성능에 미치는
영향은 미검증이다. 새 계산/공통 hook/게임별 조건 없음. ITCM 목표 ≥1 KiB이며
RAM_EMU 여유 528 B 유지·DTCM/stack 겹침 없음·전체 링크/초기화/비교를 GLM에 위임한다.
기기 조작은 이번 위임에 없다. S03 비교가 실패하면 S04를 시작하지 않는다.

S03 GLM clean rig 완료 및 Codex 저장 증거 대조: Doom **9,209,143 → 9,209,143**
host insn/frame(0%), After Burner **12,027,733 → 12,027,734**(+1 명령/frame,
+0.00000831%). 두 workload 모두 GATE3, warmup/end framebuffer RLE와 전체
audio signature 일치. 양쪽 ELF 해시 및 CZ80 외 모든 object 동일성을 확인했다.
기존 S01 완료 기준을 재사용했으며 후보만 각 1회 실행했다. 실기 속도 입증은 아니다.
후보 inputs.json은 원래 기준의 복사본이므로 후보 ELF identity에는 result/review의
해시를 사용한다. S04는 이미 위임된 별도 배치 검증으로 완료 통지를 기다린다.
[GLM 결과](../build/32x-space-reclaim-20261008/glm-rig-000001/result.json),
[저장 증거 대조](../build/32x-space-reclaim-20261008/glm-rig-000001/review.json).

S04 GLM 완료 및 Codex 대조: 정상 전체 링크 통과, ITCM/BSS end **0xfb00**,
ITCM 여유 **1,280 B (1.25 KiB)**로 목표 ≥1 KiB 달성. RAM_EMU 여유 **528 B** 유지.
DTCM BSS end **0x20019eb0**, 실제 heap limit **0x20019efc**까지 **76 B** 남는다.
기존 22 KiB reserve·heap·stack/redzone 불변이다. 32개 MD32X object 해시 동일,
ITCM text 배치 및 실행 opcode 불변(재검사에서 literal word 주소 61곳만 변경),
실제 map에서 네 256 B Z80 표의 DTCM 이동을 확인했다. 원래 초기화가 전체 DTCM
BSS를 zero하고 이후 Cz80_Init가 표 값을 채우는 경로를 보존한다.
S04 실기 속도는 미검증·기기 설치 없음. S01의 +2%를 새 배치의 실기 결과로 복사하지 않는다.
DTCM 예약 꼬리는 거의 소진했으므로 같은 이동을 다시 공간 확보 후보로 반복하지 않는다.
기존 완료 S03 rig를 재사용했고 새 동일-ROM rig·실기 실행은 시작하지 않았다.
[통합 공간 보고](../build/32x-space-reclaim-20261008/report.json),
[S04 GLM 결과](../build/32x-space-reclaim-20261008/glm-s04-dtcm-000001/result.json),
[검증](../build/32x-space-reclaim-20261008/glm-s04-dtcm-000001/review.json),
[링커 패치](../tools/32x_research/patches/s04-z80-flags-dtcm.patch).


S01/S03/S04 결합 실기 완료(2026-10-08 사용자 명시 실행 요청): 같은 PCM-r2 A와
Doom coldboot warmup900/window600, 340MHz, volume0 raw audio 합성을 유지한 1 triplet에서
**30.05711 → 30.70153 → 30.06012 FPS**, 결합 이득 **+2.13887%**, A drift **+0.01002%**.
각600 emulated/600 drawn, endpoint screen/raw audio 일치. Codex가 저장 raw record의
카운터/시간, 실제 ELF/core/XIP/device readback과 backup/restore 해시를 대조했고
캡처에서 총/HUD가 있는 Doom 레벨 내부를 확인했다. 한triplet·측정창 입력없음·endpoint
검증이며 모든게임/이동중/전체스트림/통계적 유의성은 미검증. S04 단독 이득이 아니다.
정상ITCM1280B/RAM_EMU528B/DTCM heap margin76B 유지, benchmark DTCM margin은68B.
기존 정상 firmware/core/CONFIG/save/selector 복원·readback·normalboot/keepawake 완료,
새 정상 firmware 영구설치 없음. 완료 증거를 재사용하고 동일 비교를 자동 반복하지 않는다.
GLM 준비 validator의 map 캡처순서 및 타코어 동명SZ_BIT 검사 오류는 성능실패와 구분한다.
[실기 결과](../build/32x-space-reclaim-20261008/glm-device-000001/result.json),
[독립 저장증거 대조](../build/32x-space-reclaim-20261008/glm-device-000001/completed-review.json).


## 2026-10-09: 32X 하드웨어 공통 처리 정적 감사

S01/S03/S04 완료 소스·GCC15 정상 ELF에서 **S05 VDP autofill**, **S06 DMA batch copy**,
**S07 동일 그리기 호출 안의 원본 줄 재사용**을 분리했다. 모두 정적 후보이며 구현/새 빌드/
rig/실기 실행과 성능 수치 없음. [검토 기록](32X_HARDWARE_WORK_AUDIT_20261009.md),
[소스/ELF/기계코드 근거](../build/32x-specific-audit-20261009/report.json).
S05/S06은 실제 기계 코드의 반복 주소 계산/indirect dispatch를 확인했으나 현재 작업량의
빈도는 미측정이다. S07은32X line table 주소가 같은 행에 한정한 조건부 재사용 가설로,
중복 원본·MD 배경 조건의 발생률을 먼저 확인해야 한다. H14/H16의 MD frame cache가 아니다.

재사용할 닫힌 근거 추가: 기존 L8/LTDC 전환 조사에서32X palette indices256/256,
MD non-background rows80..223 및 양방향 pixel priority 문제를 확인했다.
색 손실/게임별 예외/우선순위 누락으로 우회하지 않는다.
[기존 CLUT 조사](32X_NEXT_SESSION.md#the-big-one-the-lcd-can-do-the-palette-lookup-in-hardware).
U02는 초기 runner 실패만 보던 목록을 후속 완료 결과로 바로잡았다. 같은 빈 스케줄러
후보를 미실시로 다시 실행하지 않는다. PWM FIFO park는 H08의 wake 후 절감0.4%/
실기이득 없음·음원손상 이력을 유지하며 새 후보로 세지 않는다.

## 2026-10-11: S05/S06/S07 작업 빈도 census (고정 창, QEMU) (1차 무효, r2 유효)

> **무효화(2026-10-11 재검토)**: 이 census 결과는 전부 무효다. (1) baseline harness
> (harness_sha256 5e99879b)를 복원하지 못해 baseline fb와 불일치(AB 전 항목 상이)인
> 채, 새 harness 3변형끼리만 비교하고 '계측 투명성 증명'으로 기록했다. 비교 대상이
> baseline과 같은 프로그램인지 확인되지 않았다. (2) 측정이 머지된 최적화 기준
> 트리가 아니라 미커밋 변경이 쌓인 testbed 작업 트리에서 수행됐다. 아래 수치는
> 어떤 판정 근거로도 사용 금지. 재측정은 별도 브랜치/워크트리에서 머지된 SH-2
> 루프 패치 기준 baseline을 실제 복원하고 fb 해시가 baseline과 일치할 때만
> 유효하다.

10-09 감사의 "빈도 미측정"을 닫았다. doom[900,1500]/after-burner[540,720] 고정 창,
부팅/측정 창 분리, 진단 수치(성능 아님). S01 baseline 오브젝트 재사용 + census 계측은
`build/32x-hw-census-20261011/`(candidate 트리 RIG_HW_CENSUS 패치, 러너
run_census_rig.py). 검증은 3변형 통제 사슬: harness-control==pd-control==census가
시그니처 라인·fb 덤프 완전 동일, GATE3+프레임수 일치로 계측 투명성 증명.
baseline harness 소스(harness_sha256 5e99879b)가 어느 워크트리·git 히스토리에도
없어 baseline 로그 직접비교는 불가하고, snd 해시·fb 체크포인트 차이는 통제실험으로
harness 드리프트임을 분리했다(doom f99/f299는 baseline과 동일, AB는 체크포인트
타이밍 드리프트로 전 항목 상이, 사슬 내 3변형은 동일).

- **S05 VDP autofill**: doom 측정 창 0회(부팅 512회, 전부 len=256). AB 측정 창
  20,160회(평균 112회/프레임, 전부 len=256·무-wrap 행, 3,245,760 halfword/180프레임
  ≈ 18,032 halfword/프레임), 부팅 42,401회. 실작업은 AB류 장면에만 존재.
- **S06 DMA batch copy**: 두 게임 전 구간 0회 호출, SH2 DMA memcpy 경로 자체를
  게임이 사용 안 함. 이 두 장면 기준 이득 0.
- **S07 동일 원본 줄 재사용**: 측정 창 same-src 0회(doom 부팅 중 669회뿐). 재사용
  가설이 두 장면에서 발생하지 않음. ppmode는 전 구간 NONE(H40·MD레이어 없음·scan
  없음). 참고로 solid 행 비율 doom 창 7.1%(9,585/134,624), AB 창 43.1%
  (17,444/40,544), 이는 H15 solid-run 축의 정보이지 S07 근거가 아니다.

판정(1차): **무효, baseline 불일치**. 원문 판정(S05 후보 유지, S06·S07 기각)은 철회.
빈도 수치를 clean 성능 수치·판정 근거로 인용 금지.
[결과](../build/32x-hw-census-20261011/result.json),
[doom 로그](../build/32x-hw-census-20261011/doom-census/qemu.log),
[AB 로그](../build/32x-hw-census-20261011/after-burner-census/qemu.log).

### 2026-10-11 r2: 머지된 SH-2 루프 패치 기준 재측정 (유효)

무효화 조건대로 재측정했다. baseline 정의: 별도 브랜치/워크트리
`perf/32x-hw-census-r2-20261011` = `docs/32x-handoff-20261011`(3d6e8c4f) + 서브모듈
picodrive@10a5d460 + `patches/apply.sh` 적용(picodrive-32x-sh2-loop, PR#58 리베이스본).
harness는 wt-32x-perf 커밋 b44fce72 사본에 최소 호환 패치(`-URIG_SH2_COUNT` 환경용
inert 더미 심볼 + GATE3 sh2 게이트 `#ifdef RIG_SH2_COUNT` 보호 + inputs.json 표기에서
재구성한 pad 스크립트 doom B f600..607/720..727/840..847, AB START6/6 f0..119)
유실된 원본 harness(5e99879b)는 전역 탐색으로도 부재 확인.

검증 사슬(r2): ① baseline==census 시그니처 라인·fb RLE 덤프·GATE3·프레임수 전부
일치(양 게임). ② **r2 baseline이 run-000003 원본 baseline과 출력 동일**: snd 해시·에너지·
fb 체크포인트 해시(doom f99=9cd2510f f299=22ee77a6 f1499=79f25316, AB f99=1ea3ca5a
f299=fcb7bda1 f719=f3025331) 전 항목 완전 일치. fb 전체 내용은 체크포인트 해시로
동일성 확인(이 해시는 RLE와 같은 프레임버퍼를 덮는다). ③ 패치 포함 증명: probe
컴파일 쌍(패치 트리 SH2_ILIM 컴파일 성공 / 역패치 트리 `#error` 실패) + nm fastloop
심볼(약증거, 핀에도 일부 존재). 한계 명시: b44fce72 harness는 `RIG_FB_DUMP=1`을
단일 프레임 덤프로 해석해 run-000003의 f899/f1499 전체 RLE 라인 재현은 아니며,
run-000003 비교는 체크포인트 해시+오디오 해시 기준이다.

r2 수치(진단, 성능 아님; doom[900,1500] 600프레임, AB[540,720] 180프레임):

- **S05 VDP autofill**: doom 측정 창 0회(부팅 512회, 전부 len=256 무-wrap). AB 측정 창
  20,428회(≈113.5회/프레임, 전부 len≤256 무-wrap, 그중 len≤128 182회), 3,253,920
  halfword/180프레임 ≈ 18,077 halfword/프레임, 부팅 52,741회. 실작업은 AB류 장면에만
  존재.
- **S06 DMA batch copy**: 두 게임 부팅+측정 창 전구간 0회 호출, SH2 DMA memcpy 경로
  자체를 게임이 사용 안 함. 이 두 장면 기준 이득 0.
- **S07 동일 원본 줄 재사용**: 측정 창 same-src 0회(doom 부팅 중 669회뿐, AB 부팅도
  0회). ppmode 전 구간 NONE(H40·MD레이어 없음·scan 없음). solid 행 비율 doom 창
  5.0%(6,681/134,624), AB 창 39.3%(15,930/40,544), H15 solid-run 축 정보이지 S07
  근거가 아니다.

판정(r2, 유효): **S05 = AB류 장면 한정 실작업 후보 유지**(구현·실기는 별도 예약
없음), **S06·S07 = 이 두 장면 기준 기각**. 1차 무효절과 판정은 우연히 일치하나
근거는 r2 것만 유효.
[결과](../build/32x-hw-census-r2-20261011/result.json),
[doom 로그](../build/32x-hw-census-r2-20261011/doom-census/qemu.log),
[AB 로그](../build/32x-hw-census-r2-20261011/after-burner-census/qemu.log).
