# SNES 현재 성능과 다음 검수 대상

> 2026-10-02 최신 결과: [실기·릴리즈 판정](SNES_DEVICE_RESULTS_2026-10-02.md).
> 마리오 월드 저장 장면은 340MHz에서 일반 렌더 60.13 FPS, 젤다는 전 프레임 렌더 57.24 FPS였다.
> 벌크 대기 계산은 릴리즈 기본값 1로 반영한다. 아래는 2026-10-01 조회 당시의 기록이다.

정리일: 2026-10-01. 현재 소스와 저장소에 남은 실기 측정 기록을 대조했다.
이번 작업은 조회와 문서 정리만 수행했으며, 코드 수정·빌드·플래시·새 측정은 하지 않았다.
아래 FPS는 과거의 해당 빌드와 장면에서 얻은 결과다. 현재 작업 트리나 현재 기기의 성능을 새로 확인한 수치는 아니다.

**범용 SNES 코어의 젤다 3는 실제 그려진 50 FPS를 넘긴 기록이 있다.**
대기 루프 최적화 A/B에서 **51.03 drawn FPS**, 별도 기본 빌드 확인에서 **52.65 drawn FPS**를 기록했다.
부팅 후 1200프레임에 저장한 초기 장면의 결과이며, 모든 플레이 구간이 50 FPS를 유지한다는 뜻은 아니다.
현재 검수에서 우선할 대상은 범용 코어의 이벤트 스케줄러와 CPU 호출 경계다.
PPU와 Gaussian 보간을 다시 주요 목표로 삼을 근거는 없다.

## 젤다 50프레임 기록과 적용 범위

여기서 젤다는 `A Link to the Past` ROM을 실행하는 **범용 SNES 코어**다.
별도 `zelda3` 네이티브 포트의 FPS와 구분한다.
`emulated FPS`는 에뮬레이션 진행량, `drawn FPS`는 화면에 그려진 프레임 수다.
프레임 스킵이 있으므로 둘은 같지 않다.

| 기록 | 장면과 조건 | Emulated FPS | Drawn FPS | 해석 |
|---|---|---:|---:|---|
| 대기 루프 bake A/B | 콜드 부팅 1200프레임의 저장 장면, 동일 상태에서 off/on | 60.9 → 60.9 | 25.90 → **51.03** | 화면 프레임 **+97.0%** |
| 기본 옵션 빌드 확인 | 위 측정 장면, 기본 bake 활성화 및 상태 복원 확인 | 32 kHz 비교표의 16 kHz arm에서 61.00 | **52.65** | 추가 최적화 플래그 없이 50 FPS 초과 확인 |
| 별도 플레이 저장 장면의 bake A/B | 앞선 빗속 플레이 계열 기록, 위 초기 장면과 다름 | 이 표에는 미기재 | 16.44 → 19.15 | 장면에 따라 효과와 절대 FPS가 달라짐 |
| ROM fetch page 확장 A/B | 별도 저장 장면, page cache 확장 전후 | 60.9 → 61.0 | 20.4 → 21.5 | 앞선 51.03과 같은 장면의 연속 비교가 아님 |

첫 세 행의 근거는 [SNES_WAIT_LOOP_BAKE.md](SNES_WAIT_LOOP_BAKE.md)의 Hardware results,
A wider sample, Default 절이다. 마지막 행은
[SNES_CARTRIDGE_PROFILES_0814.md](SNES_CARTRIDGE_PROFILES_0814.md)의 첫 결과표다.
51.03과 52.65는 서로 다른 측정 창의 기록이므로 그 차이를 별도 개선량으로 계산하지 않는다.

초기 문서의 “젤다 40 → 51 FPS”, “59.40 FPS”, “57.2 FPS”는 당시 측정 조건과 지표를 따라 읽어야 한다.
그 숫자를 현재의 drawn FPS 또는 전 구간 성능으로 재사용하지 않는다.
특히 LINE_CACHE는 한때 활성화되어 있었으나 실제 플레이 재측정에서 역효과가 확인되어 현재 기본값은 0이다.

## 현재 소스의 기본 옵션

소스 기준은 상위 저장소 `bd60143b`, `external/sm`은 `5b77015292343a11591e7244c2a926b24c41b090`이다.
상위 저장소에는 작업 중인 다른 시스템 및 공통 코드 변경이 있으므로 이 식별자가 전체 작업 트리를 고정하지는 않는다.
실제로 기기에 올라간 펌웨어·SD 코어의 버전은 이번 조회에서 확인하지 않았다.

| 기능 | 현재 기본값 | 상태 |
|---|---|---|
| `SNES_THUMB2_CPU`, `SNES_THUMB2_SPC` | 1 / 1 | 65816 및 SPC700 어셈블리 실행 경로 |
| `SNES_SPIN_BAKE` | 1 | ROM에서 NMI 대기 루프를 찾아 span 단위로 실행 |
| `SNES_SPC_IDLE_SKIP` | 1 | N-SPC 타이머 대기를 묶어 처리 |
| `SNES_BUS_IN_ITCM`, `SNES_WRAP_ITCM` | 1 / 1 | 버스 함수와 opcode wrapper의 ITCM 배치 |
| `SNES_ROMPAGE_FOLD`, `SNES_DSP_FASTPATH` | 1 / 1 | 비거듭제곱 크기 ROM과 DSP-1 보드의 fetch page cache 지원 |
| `SNES_PPU_VIRGIN_Z`, `SNES_PPU_BLEND_LUT`, `SNES_PPU_OPAQUE_TILE` | 1 / 1 / 1 | 이미 검증된 PPU 개선 |
| `SNES_SKIP_SPRITE_EVAL_ON_SKIP` | 1 | 스킵 프레임의 sprite 평가 생략 |
| `SNES_STRETCH_FOLLOW` | 1 | gap-free 모드, 공급 속도에 따른 재생 속도·음정 변화 허용 |
| `SNES_AUDIO_RATE` | 미지정 시 16000 Hz | DSP 출력을 평균해 266개 mono 샘플로 제출 |
| `SNES_LINE_CACHE`, `SNES_ROMCACHE`, `SNES_ROMPAGE_LOW` | 0 / 0 / 0 | 각각 별도 실기 결과에 따라 비활성화 |
| `SNES_SPIN_SKIP` | 0 | runtime learner의 비용 때문에 비활성화 |
| `SNES_DSP_MONO` | 0 | 샘플을 버리는 경로 대신 기존 평균 downmix 사용 |
| `SNES_SMW_HLE`, `SNES_NSPC_HLE`, `RCSMW` | 0 / 0 / 0 | 범용 기본 빌드의 경로가 아님 |
| `SNES_DEVICE_PROFILE`, `SNES_LOAD_DIAG` | 0 / 0 | 성능 판정용 최종 A/B에서는 계측 비활성화 |

기준 파일: [Makefile](../Makefile), [Makefile.common](../Makefile.common),
[main_snes.c](../Core/Src/porting/snes/main_snes.c).
`SNES_ROMCACHE=0`은 어셈블리의 inline ROM cache 경로를 끄는 설정이다.
`snes_cpuRead()` 내부의 fetch page cache 확장까지 꺼졌다는 뜻은 아니다.

## 완료된 개선과 검증 범위

대기 루프 bake는 2,075개 카트리지에서 같은 로더와 인식기로 조사했고, 413개에 루프를 설치했다.
해당 413개는 M7 rig에서 off/on의 상태·오디오 해시가 모두 같았다.
이는 300프레임 창의 동일성 검증이며 전체 게임 플레이의 완전한 호환성 보장은 아니다.
7개 카트리지의 실기 A/B에서는 회귀가 기록되지 않았다.
상세 자료와 재현 명령은 [SNES_ROM_SURVEY.md](SNES_ROM_SURVEY.md)에 있다.

fetch page cache 확장 후 별도 실기 장면에서 Super Metroid는
47.5 → 60.9 emulated FPS, 11.9 → 22.4 drawn FPS를 기록했다.
Pilotwings는 44.9 → 59.1 emulated FPS, 11.2 → 17.6 drawn FPS였다.
14개 카트리지의 1200프레임 rig 결과는 상태·오디오 해시가 같았다.
근거는 [SNES_CARTRIDGE_PROFILES_0814.md](SNES_CARTRIDGE_PROFILES_0814.md)다.

호환성의 후속 상태도 구분해야 한다. DSP-1과 Cx4 HLE는 현재 코어에 포함되어 있다.
Cx4의 Mega Man X2/X3는 실기 부팅·플레이가 확인됐으며, DSP-2/3/4는 DSP-1로 잘못 처리하지 않도록 거부한다.
이 결과는 젤다 성능 개선과 별개다. 근거는 [SNES_COMPATIBILITY.md](SNES_COMPATIBILITY.md)다.

## 다음 최적화 검수 우선순위

### 범용 코어의 스케줄러와 CPU 호출 경계

`main_snes.c`의 `run_frame_events()` → `run_dots()` → `run_one_opcode()`가 범용 코어의 실행 경로다.
이미 이벤트 사이를 묶어 진행하고 DMA 활성 상태 확인도 옮겨 놓았으므로,
“dot 단위 루프를 처음 묶는다”는 제안은 현재 코드를 반영하지 않는다.
남은 후보는 opcode마다 반복하는 사이클·위치 갱신과 어셈블리 진입·복귀 비용을 줄이는 것이다.
정적 코드 검수에서 확인한 후보이며 구현과 개선량 측정은 없다.

Kart의 과거 프로파일에는 scheduler residue 18.9%, 약 126 cycles/opcode가 기록되어 있다.
다만 residue는 직접 측정한 단일 함수 비용이 아니며, 같은 문서는 계측 빌드의 wrapper 배치가
약 14%의 비용을 더했던 문제도 설명한다. **18.9% 전체를 줄일 수 있는 비용으로 확정하거나 FPS로 환산하지 않는다.**
현재 wrapper는 ITCM section을 직접 지정한다.
다음 실험은 수정된 계측 경로로 병목을 확인하고 profiler OFF 빌드로 이득을 판정해야 한다.

여러 opcode를 한 어셈블리 진입에서 실행하는 방식은 검토 후보지만,
DMA 시작, H/V IRQ, NMI, APU 포트 동기화, 다음 PPU 이벤트의 실행 순서를 보존해야 한다.
65816 어셈블리 자체는 이미 적용되어 있으므로 단순히 “CPU를 어셈블리로 바꾸자”는 단계는 끝났다.

### 오디오 출력 변환의 작은 후보

`dsp_getSamples()`는 534개 stereo 샘플을 266개 mono 샘플로 바꾸면서
`double` 위치 누적, 구간별 합산 및 나눗셈을 수행한다.
고정 출력 크기에 맞춘 변환은 검토할 수 있으나 비용이 따로 측정되지 않았고, 큰 이득의 근거도 없다.
기존 구간 경계, 음수 나눗셈의 절삭, 평균 필터를 그대로 보존해야 한다.
단순히 샘플을 절반 버리는 `SNES_DSP_MONO`와는 구분한다.

### 네이티브 포트의 후속 측정

Super Metroid 네이티브 포트의 과거 PC sample에서
`snes_handle_pos_stuff` 27.8%와 `snes_run_line` 26.6%가 기록됐다.
armed H-timer 때문에 불필요하게 dot loop를 타는 경로는 이미 수정되어 `SNES_LINE_HIRQ=1`이 기본이다.
상태 동일성 gate는 통과했지만 도달 가능한 장면이 이미 모든 프레임을 그리는 63–64 FPS였으므로,
무거운 장면에서의 개선량은 아직 없다.
이 **54.4%는 범용 SNES 코어의 비용이 아니다**.
범용 코어는 `snes_run_line()`을 실행 루프로 사용하지 않는다.

## 다시 제안하기 전에 확인할 종료된 경로

- PPU의 SIMD pixel pair, coarse skip, tile memo·prefetch·pipeline 및 line cache는 실기에서 무효 또는 역효과가 기록됐다. 젤다 특정 장면의 렌더 전체 삭제 상한 +3.15 emulated FPS를 일반적인 회수 가능 이득으로 사용하지 않는다.
- Gaussian 4탭의 `SMLAD` 최적화는 현재 범용 코어의 대상이 아니다. `GNW_SNES_CORE` 빌드에서는 이미 2점 linear 보간을 실행한다.
- DSP idle voice hoist와 skip은 drawn FPS로 다시 측정해도 유의미한 이득이 없거나 역효과였다. sparse echo FIR는 젤다 측정 창에 8개 tap이 모두 활성화되어 생략할 작업이 없었다.
- HDMA는 Kart 측정에서 프레임의 0.3%였다. 타이머 묶기 실험은 해시가 같아도 명령 수가 증가해 되돌렸다.
- `SNES_ROMPAGE_LOW=1`은 일부 HiROM의 약 1% 이득보다 cache 불가 카트리지의 6.2% 손해가 컸다.
- 32 kHz 오디오는 같은 젤다 저장 장면에서 52.65 → 21.2 drawn FPS, 상수 조정 후에도 19.9였다. 기본 16 kHz를 유지한다.
- static recompilation, runtime spin learner, 범용 N-SPC HLE의 기본 활성화는 기존 성능·호환성 결과로 종료된 경로다.

각 실험의 근거는 [OPTIMIZATION_LEDGER.md](OPTIMIZATION_LEDGER.md),
[SNES_WAIT_LOOP_BAKE.md](SNES_WAIT_LOOP_BAKE.md),
[SNES_CARTRIDGE_PROFILES_0814.md](SNES_CARTRIDGE_PROFILES_0814.md)에 있다.

## 다음 측정에서 남길 자료

같은 ROM·같은 저장 상태·같은 클록과 화면 배율에서 emulated FPS와 drawn FPS를 함께 기록한다.
상태 복원 성공, 실행 중인 core와 flashed arm의 일치, A/B 바이너리 차이도 확인한다.
프레임 스킵 비율이 가변적인 장면은 같은 세션에 arm별 5–7회 표본을 남기고,
최종 성능은 profiler OFF로 판정한다. audio underrun과 재생 속도도 같이 기록한다.
정확성 검증은 실제 변경 경로가 실행되는 창에서 상태·영상·오디오를 비교한다.

기존 [SNES_NEXT_SESSION.md](SNES_NEXT_SESSION.md)는 실험의 경과와 철회 기록이다.
이 문서를 현재 상태의 시작점으로 읽고, 세부 근거가 필요할 때 기존 문서를 따라간다.
