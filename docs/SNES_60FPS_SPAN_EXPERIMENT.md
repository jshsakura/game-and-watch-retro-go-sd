# SNES 젤다 60 FPS를 위한 대기 루프 구간 계산 실험

> 최신 실기·릴리즈 판정: [2026-10-02 실기 결과](SNES_DEVICE_RESULTS_2026-10-02.md).
> 아래의 실기 대기 문구는 당시 기록이며, 최신 상태는 위 문서를 따른다.

> 추가 검증 완료: [9개 게임·12조건 하네스 정리](SNES_HARNESS_READY_2026-10-01.md).
> 기본 gate에서 마리오 월드 10.86%, 드래곤즈 매직 23.84% 비용 감소를 확인했다.
> 아래는 첫 실험 기록이며 최신 비교의 분모·구간은 위 문서를 따른다.

2026-10-01. 젤다의 기존 51.03 / 52.65 drawn FPS 장면에서 추가 개선을 찾기 위해
`perf/snes-60fps-span` 워크트리를 만들고 첫 후보를 구현했다.
이 첫 실험 당시에는 **정확성 검증을 통과한 명령 수 감소 후보**였다. 이후 SMW의 저장 장면은 60 FPS에 도달했고 젤다는 미달했다. 상단 실기 기록을 따른다.

## 작업 위치와 변경

- 워크트리: `/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60`
- 상위 브랜치: `perf/snes-60fps-span`, 기준 `bd60143bb14feb1c3b049136dd7b23b1429c1109`
- `external/sm` 브랜치: `perf/snes-bulk-wait`, 기준 `5b77015292343a11591e7244c2a926b24c41b090`
- 코어 후보 commit: `e33537c06618e88786f518221787a1e033ba303b`
- 실험 옵션: `SNES_SPIN_BAKE=1 SNES_BAKE_BULK=1`
- 당시 `SNES_BAKE_BULK` 기본값: **0**. 2026-10-02 실기 검증을 마친 뒤 릴리즈 기본값은 **1**로 변경했다.

기존 bake는 `LDA dp / BEQ`의 인터프리터 호출을 생략하면서도 명령마다 조건 검사와
`cpuCyclesLeft`, `hPos`, `apuDotsAccum` 갱신을 반복한다.
후보는 실제 WRAM 대기 값이 0이고 DMA·IRQ·CPU 상태 guard가 통과한 경우,
다음 이벤트 전까지 들어가는 **완전한 반복만** 횟수로 계산한다.
부분 opcode는 기존 경로가 처리한다. 매 새 span에서 실제 값을 다시 읽기 때문에
NMI 뒤의 메모리 변화를 무시하는 과거 blind replay와 다르다.
새 필드를 CPU/SNES/DSP 저장 구조에 추가하지 않았다.

## 검증 결과

### 원본 소스와의 차등 검증

[tests/test_snes_bake_bulk.sh](../tests/test_snes_bake_bulk.sh)는 기준 코어 commit의
원본 `spin_bake.c`를 별도 namespace로 컴파일해 후보와 비교한다.
원본을 후보 코드의 bulk OFF branch로 대체한 검사가 아니다.

**109,886개 span이 일치했고 ASan·UBSan 실행도 통과했다.**
검사 대상은 CPU 전체 상태, PC, 남은 사이클, SNES 시간·APU 누적량,
WRAM, DMA 상태·호출 횟수와 bake 반복 수다.
0부터 1364까지의 모든 budget, LDA/BEQ 양쪽 PC, 부분 opcode,
bank mirror·잘못된 bank·width·direct page, counter wrap,
DMA의 대기 값 변경과 IRQ 발생, span 사이의 이벤트를 포함했다.
이 검사는 span 구현의 동일성 근거이며 실기 영상·음질 검증을 대신하지 않는다.

### M7 QEMU의 실제 ROM 실행

각 arm은 콜드 부팅 1200프레임, 입력 없음, 저장 상태 없음, 200프레임 창 6개다.
기본 gate와 강제 활성화 진단은 서로 다른 조건이므로 비교표를 분리했다.
각 조건에서 A 1회 / B 1회를 실행했으며, 모든 창의 framebuffer·audio hash와
최종 상태·오디오 hash가 일치했다. 측정값은 emulation과 PCM top-up의 합산 명령 수다.

| 조건 | A 명령 수 / frame | B 명령 수 / frame | 감소 | 판정 |
|---|---:|---:|---:|---|
| 젤다 기본 gate | 5,007,322 | 4,985,293 | **0.4399%** | PASS, 부팅 중 gate가 대부분 parked |
| 젤다 gate 강제 활성화 진단 | 4,931,398 | 4,826,319 | **2.1308%** | PASS, 활성 경로의 비용 분리 |
| 동키콩 gate 강제 활성화 진단 | 6,194,444 | 6,194,444 | **0.0000%** | PASS, loop match 없음 |

젤다 두 조건의 최종 hash는 `STATEHASH=1d0d959d AUDIOHASH=c70e2bdf`다.
강제 활성화의 양 arm은 같은 **871,862 laps**를 처리했다.
동키콩은 `STATEHASH=d8132a15 AUDIOHASH=677e8092`이며 모든 창에서 명령 수까지 같았다.

강제 활성화는 `RIG_BAKE_KEEP_ARMED`를 **양쪽 arm에 동일하게** 적용한다.
ROM의 처리 결과는 같지만 shipping gate 정책은 아니므로, 이 2.13%를 기본 빌드의
장면 전체 개선량 또는 실기 FPS 증가로 보고하지 않는다.
이 진단을 추가한 이유는 기본 gate가 180프레임 창에서 낮은 lap 빈도를 보고
parked 상태로 들어가 후보의 활성 경로를 충분히 측정하지 못했기 때문이다.

강제 활성화한 젤다의 801–1000프레임 창은 합산 명령 수 **4.0265% 감소**였다.
하지만 마지막 300프레임의 EMU p99는 **6,410,600 → 6,410,560**으로 거의 그대로였다.
즉 이 후보는 기다리는 프레임을 줄이며, 이 콜드 부팅 창의 가장 무거운 프레임을
크게 줄이지는 않는다. **이 후보 하나로 지속 60 drawn FPS를 달성한다고 판단할 근거는 없다.**

## 빌드와 산출물

컴파일러는 builder `sylverb/retro-go-sd-builder:v1.5`의 Arm GNU Toolchain 15.2.Rel1,
`arm-none-eabi-gcc 15.2.1 20251203`이다.
`-O2 -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16`과
`-ffunction-sections -fdata-sections -ffp-contract=off`를 사용했다.
어셈블리는 기기의 기본값과 같은 `-DSNES_T2_NO_ROMCACHE`다.
QEMU는 10.0.13이며 `mps2-an500`, `-icount shift=0,align=off,sleep=off`다.

코어 후보 옵션 외에는 동일한 소스로 A/B를 빌드했다.
`spin_bake_run_span`의 object text는 **328 → 412 bytes**, 진단 ELF 전체 text는
202,764 → 202,828 bytes였다. 실기 firmware link 및 ITCM 여유는 아직 확인하지 않았다.

runner는 현재 `cart.c`가 참조하는 `cx4_hle.c`를 포함하도록 맞췄고,
container에서 빌드 후 host QEMU에서 실행할 수 있도록 `RIG_BUILD_ONLY=1`을 추가했다.
실행은 foreground의 프로세스 종료를 기다렸으며 진행 상태를 조회하지 않았다.
완료한 경우에만 로그를 읽었다.

원본 로그·JSON 요약·소스 patch·ROM/ELF/소스 해시와 옵션은
[실험 산출물](../tools/snes_bulk_wait/results/20261001-span-fold/manifest.json)에 보존했다.
ROM은 외부 입력 경로에 유지하며 이 결과 디렉터리에 복제하지 않는다.
로컬 ELF는 `build/snes-bulk-evidence/m7-*/rig_snes.elf`에 있다.

재현용 entry point:

```sh
bash tests/test_snes_bake_bulk.sh
python3 tools/snes_bulk_wait/compare.py BASE_LOG CANDIDATE_LOG
```

[run_case.py](../tools/snes_bulk_wait/run_case.py)는 ROM loader로 빌드한 1200프레임 진단 ELF를
한 번 실행하고 input·binary hash와 완료 결과를 저장한다.
출력 디렉터리가 이미 있으면 덮어쓰거나 자동 재실행하지 않는다.
각 실행의 종료를 받는 방식이며 ITL/GLM 작업을 시작하거나 폴링하지 않는다.

## 다음 단계와 실기 제한

**다음 판정은 기존 52.65 FPS 저장 장면의 실기 A/B다.**
그 저장 파일과 ROM 신원은 이번 rig 실행에서 확보하지 않았고,
이번 콜드 부팅 장면이 그 장면과 같다고 가정하지 않았다.
기기는 사용자의 32X 작업에 할당되어 있어 종료 통보 전까지 플래시·reset하지 않는다.

실기에서는 같은 장면·클록·화면 배율·음원 설정을 고정하고,
profiler OFF로 A→B→A, 가변 draw 비율이면 arm별 5–7회 표본을 비교해야 한다.
drawn FPS, emulated FPS, 프레임 지연 분포와 audio underrun을 같이 남긴다.
명령 수 2.13% 감소를 52.65→60 FPS로 환산하지 않는다.

현재 `gnw-ab-bench`의
[실기 프로토콜](../../game-and-watch-retro-go-sd/.agents/skills/gnw-ab-bench/references/protocol.md)은
기존 `drawn_ab.sh` / `bench.sh`의 반복 SWD 읽기를 무폴링 계약에 맞지 않는 경로로 지정한다.
기기 확보 후에도 검증된 완료 이벤트 방식이 필요하며, 기존 폴링 도구를 그대로 실행하지 않는다.

실기에서 이득이 확인되더라도 무거운 프레임이 그대로라면, 다음 큰 검수 대상은
`run_dots`와 65816 진입·복귀 경계다. gap-free 오디오의 주기 탐색 비용도
별도 arm에서 검토할 수 있지만 이 실험에는 섞지 않았다.
