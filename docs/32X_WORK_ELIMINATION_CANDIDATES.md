# 32X: 반복 자체를 제거하는 후보 검토

2026-10-08. 사용자가 요청한 방향은 spin-skip처럼 불필요한 작업 자체를 제거하는
범용 방식이다. 소스·보존 ELF·공개 원본 검토 후 **S01의 BT/S 경로를 isolated 후보로
구현하고 census·CPU 차등검증·clean rig·실기 A/B/A를 완료했다.** 최종 single-commit
후보는 Doom 실기에서 **+1.99796%**였다. 한 triplet의 결과이며 정식 설치판은 원래대로 복원했다.

선행 기록: [실패·보류 접근 목록](32X_FAILED_APPROACHES.md).
기준 core: PCM-r2 / PicoDrive `10a5d460`.

## S01: 이미 접은 SDRAM spin 안의 호스트 반복을 상수 횟수 계산으로 제거

### 실제 확인한 코드

깨끗한 PCM worktree의 `cpu/sh2/mame/sh2pico.c`에는 다음 세 helper 내부 반복이 남아 있다.
번호는 2026-10-08 읽은 파일 기준이다.

| 형식 | 소스 위치 | 현재 helper 내부 작업 |
| --- | --- | --- |
| MOV.W + TST + backward BT/BF | line 1266 | `while (icount >= 5)`에서 같은 주소를 읽고 T와 destination을 갱신 |
| CMP/EQ + MOV.L + BT/S + AND delay slot | line 1370 | 같은 주소를 읽고 mask/compare를 매번 반복 |
| TST R0 + BFS + GBR MOV.W delay slot | line 1647 | 같은 주소를 읽고 zero 조건을 매번 검사 |

게스트 opcode dispatch는 이미 줄였지만, host helper는 반복 횟수에 비례해 일한다.
이것이 단순히 C 소스에만 남은 루프인지도 확인했다. 완료된 Thumb 실험의 **baseline**
ELF에서 `md32x__sh2_execute_interpreter`를 역어셈블했다.

- `0x6010..0x604e`: SDRAM `ldrh`, register store, TST, `icount -= 5`, backward branch.
- `0x5f4e..0x5f80`: SDRAM `ldr`, halfword swap, AND/CMP, register/T store, backward branch.
- `0x5370..0x53a6`: SDRAM `ldrh`, R0 store, zero test, `icount -= 5`, backward branch.

보존 근거: `build/32x-fixed-thumb-v4-20261007/device-arms/baseline/gw_retro_go.elf`와
`build/32x-spin-invariant-review-20261008/baseline-interpreter.disasm`.
컴파일러가 모든 반복을 이미 제거했다고 가정할 수 없다.

### 바꾸려는 전제와 범용성

한 helper 호출 안에서 **일반 guest SDRAM의 같은 주소**를 직접 읽고,
guest memory write·다른 CPU 동기화·DMA 처리·IRQ 전달이 그 사이에 없다면,
같은 값을 반복해서 다시 읽는 것은 중복 작업이다. `RW/RL`의 GNW 직접 SDRAM 경로와
`run_sh2`/`p32x_sync_other_sh2` 호출 구조에서 이 후보를 검토할 근거를 확인했다.

그 경로에만 한정하여 값과 조건을 한 번 확인하고,
완전히 들어맞는 반복 수 `k = icount / 5`의 사이클을 한꺼번에 청구한다.
PC/PPC/EA/T/delay와 마지막 destination 및 잔여 cycle은 기존 helper와 같게 남긴다.
다음 CPU 실행·event 경계를 넘어 시간을 건너뛰지 않는다.

BT/S는 초기 destination으로 첫 compare를 하기 때문에 **첫 1~2회는 별도로 계산**해야 한다.
이를 무시하고 즉시 마지막 값을 대입하면 early exit와 cycle 청구가 달라진다.
적은 budget에서는 원래와 같이 아무 반복도 접지 않는다.

게임 이름·ROM CRC·고정 guest PC를 추가하지 않는다. 기존 opcode 형식을 동일하게 처리한다.
새 native code cache나 모든 load마다 호출하는 감지 hook도 필요하지 않은 설계다.
아래 검증 범위에서는 원래 helper와 일치했다. 모든 게임·장치 상태의 증명은 아니다.

### 기존 실패와 다른 점

- **H07/H08**은 CPU를 scheduler idle 상태로 park하거나 read마다 감지하여 비용·음원 문제가 났다.
  S01은 기존 실행 구간과 시간 흐름을 유지하면서 helper 안의 중복 host 반복을 제거한다.
- **G01/G02/G03**은 새 interpreter/block dispatch 비용 문제였다.
  S01은 현재 성공 경로 안에서 실제 반복을 제거하며 별도 진입 경로를 추가하지 않는 후보다.
- 이미 `countdown_fold.h`와 BRA-self/countdown fold에 산술 일괄 청구가 있다.
  그 성공을 새 발견으로 주장하지 않는다. 위 **세 memory-poll helper의 host while 제거**는
  조사한 완료 기록에서 동일한 A/B를 찾지 못했고 현재 baseline 기계 코드에도 남아 있다.

### 먼저 확인할 것 / 진행을 멈출 조건

1. 원래 guard 중 느슨한 주소 mask를 그대로 신뢰하지 않는다. 정확한 직접 SDRAM region,
   유효한 메모리와 alias 조건을 검증해야 한다. MMIO·poll FIFO·read callback·다른 CPU를
   실행시키는 read는 제외한다. 관련 compile-time 경로도 구분한다.
2. 초기/탈출/잔여 budget, signed load와 mask, register alias, T/EA/PC 및 delay-slot 순서를
   기존 interpreter/helper와 대조한다. 화면뿐 아니라 audio와 cycle 흐름도 보존한다.
3. profiler와 분리해 실제 helper 호출 수 및 내부 반복 수를 집계한다.
   게스트 opcode histogram은 이미 접힌 내부 반복을 세지 못하므로 그 비율로 상한을 추정하지 않는다.
4. 전체 절감의 상한이 미미하면 그 결과만 남기고 종료한다.
   충분한 제거량이 있을 때만 isolated 후보·동일 구간 clean rig·메모리 fit 검증으로 진행한다.

### 10-08 직접 검증: 실제 제거 기회와 첫 후보

GLM 전송은 tmux 권한 거부로 시작되지 않았고, 사용자가 Codex의 직접 실행으로 변경했다.
로컬 GCC 14.2.1 / QEMU 10.0.13을 사용했다. 기존 GCC 15.2.1로 만든 공통 객체와 ROM
객체를 재사용하되, 비교 양쪽 CPU·harness·링크는 같은 로컬 도구로 만들었다.
따라서 이전 compiler의 기준 숫자와 새 candidate 숫자를 직접 비교하지 않는다.

- census: Doom f900–1499의 master BT/S helper 59,871호출, 9,569,584 read/iteration,
  그중 보수적으로 첫 2회씩 남겨도 9,450,132회 제거 기회. 호출 내부 값 변경 0회,
  callback 경로 0회, 최대 468반복. After Burner f540–719에서는 세 poll 형식 모두 0호출.
- 첫 후보: 기존 첫 비교·reload와 다음 비교의 early exit를 보존하고 나머지 cycle을
  `% 5`로 청구한다. 게임 이름·CRC·고정 PC를 추가하지 않는다.
- 합성 차등검증: 기존 **전체 fastloop helper**와 후보를 비교한 65,536상태에서
  6,016 B 전체 SH2 context 및 rejection cache 일치. 두 CPU, 16 destination, 비교 방향,
  cache-through alias, mask/초기 값, cycle remainder/zero/negative budget, 잘못된 코드·주소·alias 포함.
  helper 입력 범위의 검증이며 모든 IRQ/장치 상태의 증명은 아니다.
- 첫 clean rig: Doom 9,561,760 → 9,143,305 host insn/frame, **−4.37634%**;
  After Burner 11,810,067 → 11,823,544, **+0.11411%**.
  양쪽 warmup/end 화면 dump와 전체 구간 audio hash·sample count·energy 일치.
  새 profiler/guest dispatch counter는 제외했고 실기 FPS는 미측정이다.
- 첫 후보 CPU text +268 B, data/bss 추가 0 B. 기존 while의 중복 fallback이 같은
  inline-read 빌드에 남아 있어 compact 후보에서 이를 제거한다. 이 구조 변경은 실제 중복
  코드 제거이며 게임별 튜닝이나 PC 변경이 아니다. After Burner 증가의 개별 원인은 미계측이다.

증거: [census](../build/32x-poll-census-20261008/run-local-000001/result.json),
[첫 후보 proof/state](../build/32x-poll-fold-20261008/run-000001/state.json),
[Doom clean 비교](../build/32x-poll-fold-20261008/run-000001/doom-comparison.json),
[After Burner clean 비교](../build/32x-poll-fold-20261008/run-000001/after-burner-comparison.json).
실기 compiler로 전체 ELF/ITCM budget을 확인하고 실기 A/B/A까지 통과하기 전에는 설치판을 바꾸지 않는다.

### 이전 compact 후보: 같은 루프에서 첫 반복 뒤 남은 반복을 계산

첫 후보에 추가한 별도 block을 제거했다. 기존 while의 첫 반복과 기존 early-return을
그대로 사용한 뒤, inline SDRAM read 빌드에서 나머지 안정된 반복을 계산하고 반환한다.
`GNW_NO_INLINE_DATA_READ` 빌드는 원래 read callback 경로의 while을 그대로 유지한다.
기존 주소·opcode guard를 그대로 사용하며 새로운 전역 hook/cache를 추가하지 않는다.

| 작업량 | 동일 로컬 baseline host insn/frame | compact 후보 | 변화 |
| --- | ---: | ---: | ---: |
| Doom f900–1499, 600프레임 | 9,561,760 | 9,139,712 | **−4.41392%** |
| After Burner f540–719, 180프레임 | 11,810,067 | 11,818,851 | **+0.07438%** |

변경 없는 baseline 측정은 첫 후보의 완료 결과를 재사용했다. ROM/window/defs/compiler/
CPU source/harness/common object origin을 비교하고 보존 ELF 해시를 확인했다.
compact 후보만 새로 실행했다. 두 작업량 모두 기존 warmup/end framebuffer RLE dump와
전체 audio hash·sample count·energy가 일치했고, 65,536개 합성 전체 context/rejection-cache
차등검증도 다시 통과했다. CPU object text **29,267 → 29,139 B (−128 B)**,
data/bss 추가 0 B. 이는 로컬 compiler의 object 수치이며 실기 전체 linker budget 검증이 아니다.

**판정: 연산량 감소가 있는 범용 opcode 후보, 미배포/실기 미검증.**
After Burner의 작은 증가 원인은 분리 계측하지 않았으며 모든 게임의 개선을 주장하지 않는다.
현재 주 작업트리의 core는 `5cc10e1a`로 측정한 PCM-r2의 `10a5d460` source와 다르다.
따라서 오래된 core를 임의로 수정/배포하지 않고 검증한 source의 패치와 결과를 보존한다.

[보존 패치](../tools/32x_research/patches/s01-stable-sdram-bts.patch),
[이전 compact report](../build/32x-poll-fold-20261008/run-000002/report.json),
[runner](../tools/32x_research/run_poll_fold.py),
[전체 helper proof](../tools/32x_research/poll_fold_proof.h).
동일 census/proof/clean 측정은 다시 실행하지 않는다. 다음 검증은 실제 배포 compiler의
정상 firmware 전체 fit과 같은 입력·구간의 실기 A/B/A이며, 완료 근거를 재사용한다.

### 실기 준비에서 확인한 compiler/공간 차이

사용자가 USB 권한을 변경하고 전원을 켠 뒤 SWD 연결과 정상 설치판 bank2 해시를 확인했다.
정상 firmware의 실제 timeout setter 명령과 live config magic/version을 확인한 후
RAM의 유휴 종료 값만 600 → 0으로 바꿔 현재 전원을 유지했다. 아직 flash/reset은 하지 않았다.

실기 GCC 15.2.1로 compact 후보를 빌드한 결과, 기준 ITCM/BSS end `0xffd8`에서
`0x10018`로 증가해 **24 B 초과**했다. 같은 소스의 GCC 14 object 감소를 GCC 15의
전체 firmware fit으로 간주할 수 없음을 확인했다. 최초 linker 오류에서 멈췄고 해당 arm은
플래시하지 않았다. [실패 기록](../build/32x-poll-fold-20261008/device-000001/device-build-result.json).

변경한 전제: 기존 compact 형태는 첫 비교/다음 비교에서 dest/T/icount를 단계별로
갱신했다. 직접 SDRAM 경로에서 지역 변수로 두 비교와 budget을 계산하고 **최종 context를
한 번만 저장**하는 동일 변환으로 중복 store 경로를 제거한다. opcode/PC/게임별 조건,
handler의 XIP 이동, 새 dispatch hook을 추가하지 않는다. 실제 GCC 15를 사용하며
전체 helper 차등검증·clean 비교·full link를 통과하기 전에는 기기에 올리지 않는다.

### 최종 single-commit 후보: 실제 GCC 15 검증 및 실기 완료

두 비교와 잔여 budget을 지역 변수에서 계산하고 dest/T/icount를 한 번 저장하는 형태로
중복 store를 제거했다. 앞선 24 B 링크 초과의 전제를 바꾼 것이며, 게임별 조건이나
handler 배치 변경 없이 기존 직접 SDRAM BT/S helper 안의 반복만 제거한다.
GCC 15.2.1에서 65,536개 전체 context/rejection-cache 차등검증을 통과했다.

| 작업량 | GCC 15 baseline host insn/frame | single-commit 후보 | 변화 |
| --- | ---: | ---: | ---: |
| Doom f900–1499, 600프레임 | 9,478,729 | 9,209,143 | **−2.84412%** |
| After Burner f540–719, 180프레임 | 12,027,728 | 12,027,733 | **+0.00004157%** |

이 표는 양쪽을 동일 실제 compiler로 비교한 완료 결과다. 이전 GCC 14의 −4.41%나
−128 B와 섞지 않는다. 두 작업량 모두 warmup/end 화면과 전체 audio signature가 일치했다.
실기 benchmark 전체 ELF 링크도 성공했다. ITCM/BSS end는 기준 `0xffd8`, 후보
`0x10000`으로 후보 여유 **0 B**, RAM_EMU BSS end는 `0x240ffff4`로 여유 **12 B**다.
benchmark hook을 제외한 정식 firmware 전체 링크·공간 검증은 완료하지 않았다.

실기 한 A/B/A: **30.05711 → 30.65917 → 30.06012 FPS**, 앞뒤 A 평균 대비
**+1.99796%**, A 드리프트 **+0.01002%**. 세 arm 모두 340 MHz, cold boot,
warmup 900/측정 600 emulated·600 drawn 프레임, 측정 중 입력 0, volume 0이었다.
시작 전 동일 B 입력으로 레벨에 진입했다. 저장한 화면을 직접 확인하여 Doom 레벨 내부와
HUD를 확인했으며 menu 화면이 아니다. 입력 없는 구간으로 이동 중 성능까지 주장하지 않는다.
종료 화면·raw audio buffer 해시는 세 arm 모두 같고 audio 생성 호출·sample 수도 같다.
실기 비교는 endpoint 검증이며 전체 스트림 정확성·통계적 유의성·모든 게임 개선은 주장하지 않는다.

원래 bank2/core/CONFIG/off.sav를 복원하고 해시 readback 및 정상 부팅을 확인했다.
복원 후 RAM 유휴 종료 값 0을 다시 적용했다. 현재 켜진 세션의 자동 종료를 막는 설정이며
SD CONFIG는 원래 값 그대로다. 후보는 패치/빌드로 보존했고 정식 설치판에 상주하지 않는다.
완료 통지에서는 결과를 읽고 기록했으며 새 benchmark나 재실행은 시작하지 않았다.

[최종 패치](../tools/32x_research/patches/s01-stable-sdram-bts-single-commit.patch),
[통합 보고](../build/32x-poll-fold-20261008/report.json),
[GCC 15 Doom 비교](../build/32x-poll-fold-20261008/run-000003/doom-comparison.json),
[실기 완료](../build/32x-poll-fold-20261008/device-000002/device-000001/result.json),
[보존 화면](../build/32x-poll-fold-20261008/device-000002/device-000001/round-000001/candidate/capture/screen.png).
동일 census/proof/clean/실기 비교는 완료 근거를 재사용한다.

## S02: 알려진 짧은 형식 밖의 idle loop를 자동 분석해 제거

공개 [mGBA `memory.c`](https://github.com/mgba-emu/mgba/blob/master/src/gba/memory.c)의
`GBASetActiveRegion`과 `_analyzeForIdleLoop`는 재진입한 branch target에서 register 상태를
확인하고, instruction을 분석하여 store·불안정한 주소·상수로 볼 수 없는 IO read를
거절한 뒤 idle loop를 찾는다. 이후 `GBAHalt`를 사용한다.
단순한 “같은 PC를 다시 봤으니 생략”보다 강한 조건을 검사하는 실제 구현이다.

이를 SH2에 적용하려면 알려진 opcode 형식을 늘리는 대신 제한된 read-only loop의
상태/주소 의존성을 분석하는 방식이 후보가 된다. 대기 중 값의 writer와 IRQ까지
고려해야 한다. GBA용 검사를 그대로 32X의 두 SH2와 68000에 이식하면 안 된다.

공개 [MAME `diexec.h`](https://github.com/mamedev/mame/blob/master/src/emu/diexec.h)와
[`diexec.cpp`](https://github.com/mamedev/mame/blob/master/src/emu/diexec.cpp)는
`spin_until_trigger` 및 trigger에서 resume하는 실행 관리 방식을 제공한다.
MAME driver의 고정 PC/주소 speedup 목록을 복사하는 것은 사용자의 범용 요구에 맞지 않는다.
재사용할 부분은 wait/wake 메커니즘이다.

S02는 H07/H08과 가까운 계열이다. **read마다 감지하는 비용을 피하는 branch 진입 분석과
완전한 writer/IRQ wake 조건이 구체화되기 전에는 재실험하지 않는다.**
그 검사/cache 자체의 RAM·ITCM 비용과 실제 새 loop coverage도 미확인이다.

## 이번 검토의 결론

우선순위는 S01이다. 실제 baseline에 남아 있는 host 반복과 정확히 바꿀 전제를 찾았다.
최종 single-commit 후보는 실제 GCC 15 clean rig에서 Doom 전체 명령 수 **−2.84412%**,
After Burner **+0.00004157%**, 실기 Doom 한 A/B/A에서 **+1.99796% FPS**였다.
범용 반복 제거로 작은 실제 이득을 확인했으며 모든 게임의 이득이나 대폭 개선을 주장하지 않는다.
S02는 공개 구현의 참고 방식이며 곧바로 실행할 후보가 아니다.
측정용 arm을 일시 실행한 뒤 정상 firmware를 복원했다. 정식 배포는 하지 않았다.

## 후속 공간 확보: S03 정상 firmware 링크 완료

S01에 Z80의 중복 SZ_BIT 256 B 표 제거를 더한 별도 정상 clone을 빌드했다.
실제 GCC 15.2.1 전체 링크 결과 ITCM 여유 **256 B**, RAM_EMU 여유 **528 B**다.
Z80 실행 함수 text는 17,892 B로 그대로이며 정상 ELF에는 benchmark 상태/완료 marker가 없다.
1,056,768개 BIT 플래그 조합이 일치했다. S03의 게임 rig 확인은 사용자 요청으로
GLM 5.3 세션 1.1에 Enter 포함 위임했다. S01의 완료 기준을 재사용하며 후보만 비교한다.
현재 기기에 새 후보를 설치하거나 재측정하지 않았다.
[정상 링크 검증](../build/32x-space-reclaim-20261008/run-000002/normal-link-validation.json),
[표 제거 패치](../tools/32x_research/patches/s03-cz80-bit-table.patch).

## 최신 공간 확보 완료: S04

S03에서 확보한 256 B에 더해, 기존 22 KiB DTCM 예약의 빈 꼬리로 네 Z80 flag 표
1,024 B를 옮겼다. 정상 firmware 전체 링크 결과 **ITCM 여유 1,280 B**,
**RAM_EMU 여유 528 B**다. DTCM 실제 heap limit까지 76 B 남으며 예약량을 늘리지 않았다.
실행 opcode·ITCM text 배치·32개 MD32X object가 그대로이고 주소 literal만 바뀌었다.
S04의 실기 성능은 아직 미검증이며 후보를 기기에 설치하지 않았다.
[공간 통합 보고](../build/32x-space-reclaim-20261008/report.json),
[GLM 완료 결과](../build/32x-space-reclaim-20261008/glm-s04-dtcm-000001/result.json),
[Codex 저장 증거 대조](../build/32x-space-reclaim-20261008/glm-s04-dtcm-000001/review.json),
[보존 패치](../tools/32x_research/patches/s04-z80-flags-dtcm.patch).
