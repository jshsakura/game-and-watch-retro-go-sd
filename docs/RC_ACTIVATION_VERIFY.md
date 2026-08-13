# SNES RC (Static Recompilation) 실기 활성화 및 검증 리포트 (수정본)

본 문서는 GLM 세션에서 수정 중인 rc 해시 테이블 BSS 이전 패치가 완료된 후, 실기에 플래시하기 전에 반드시 확인해야 할 "호스트 기반 검증 게이트(Recipe)"를 담은 읽기 전용 리포트입니다. 유저의 "헛플래시"를 방지하기 위해 정직하게 판정하는 것이 목적입니다.

## 1. 활성화 조건 분석 (RC=1 조건)
실기(SMW 구동 시)에서 `rc=1`로 동작하기 위해선 다음의 조건을 모두 만족해야 합니다.
1. **타이틀 해시 및 XIP 헤더 검증**:
   - `odroid_overlay_cache_file_in_flash_relocate()`로 불러온 `rc_smw.xip`의 헤더(오프셋 0) 매직 넘버가 `0x4D534352` (RCSM)이어야 합니다.
2. **ROM 코드영역 FNV-1a 해시 일치 (가장 중요)**:
   - 기존 1차 실패 원인은 유저 덤프의 코드 영역과 RC가 컴파일될 당시 기준 ROM의 코드가 달라 활성화가 거부된 것입니다.
   - 현재 빌드에 내장된 (기준) `RC_CODE_HASH`는 **`0x5a04e964`** (`generated/rc_smw/rc_sites.inc` 77466번 라인)입니다.
   - 유저의 실기 로그에 기록된 `sum64k=00675F41`는 Retro-Go 특유의 ROM 64KB 블록 체크섬으로, 일반적으로 순정 SMW(미국판)를 가리킵니다. 유저가 플레이하는 SMW의 코드 영역이 FNV-1a 기준 `0x5a04e964`와 일치해야만 `rc=1`로 켜집니다.

## 2. DTCM OOM 재발 방지 검증 방법
GLM 세션의 BSS 정적 배열 이전 작업이 완료되면 다음의 도구를 통해 DTCM 힙 소모가 없어졌음을 호스트 단계에서 증명해야 합니다.
- **Harness 예산 검증**: 
  ```sh
  tools/gnw_hw_harness/run.sh
  ```
  이 명령을 통해 `RAM_EMU` (741,376 바이트) 안에 오버레이 + BSS + 85KB의 정적 해시 배열이 정확히 안착했는지 검증합니다. DTCM 힙 여유(약 8,216B)가 OOM 없이 안전하게 남는지(`FAIL`이 아닌 `PASS`) 확인합니다.
- **Malloc 0건 증명 (RED/GREEN 테스트)**: 
  ```sh
  tools/gnw_hw_harness/run_tests.sh
  ```
  `tests/test_rc_dispatch_heap.c` 테스트가 통과되어, `rc_dispatch_init()` 호출 시 `malloc()`이 단 한 번도 발생하지 않음(0건)을 증명해야 합니다.

## 3. −42% 성능 유지 검증 레시피 (O(1) 속도)
동적 메모리가 정적 BSS 공간으로 이동해도 메모리 액세스 타임(RAM_EMU 구간)과 Open-addressing 룩업 효율이 떨어지지 않고 리그 기준 −42%를 달성하는지 확인해야 합니다.
- **QEMU M7 Rig 프로파일링**:
  GLM 커밋 후, 호스트(QEMU)에서 다음 스크립트를 교차 실행합니다. (SMW ROM이 `roms/smw.smc` 등으로 존재한다고 가정할 시)
  ```sh
  # 기본 Spin-skip 모드 프레임당 인스트럭션 측정
  bash tools/m7_qemu_rig/run_snes_spin.sh roms/smw.smc 120
  
  # RC 모드 프레임당 인스트럭션 측정 (-42% 감소 여부 비교)
  bash tools/m7_qemu_rig/run_snes_rc.sh roms/smw.smc 120
  ```
  `run_snes_rc.sh`의 결과값(insn/frame)이 `run_snes_spin.sh` 대비 약 -42% 향상에 가까운 수치라면 정적 할당 변경으로 인한 성능 손실이 없음을 증명합니다.

## 4. 실기 플래시 GO/REVIEW 판정 체크리스트
GLM 패치가 도달한 시점에 아래 4가지 항목이 모두 **GREEN**일 경우에만 유저 실기 플래시를 지시(GO)합니다.

- [ ] **(a) 활성화 보장**: 유저가 보유한 SMW ROM의 코드 영역 해시가 `0x5a04e964`인가? (불일치 시 rc 활성화 안 됨)
- [ ] **(b) DTCM OOM 방지**: `tools/gnw_hw_harness/run.sh` 및 `run_tests.sh`를 통과하여 `malloc`이 완전히 배제되고 `RAM_EMU` 예산을 통과했는가?
- [ ] **(c) 성능 검증 (-42%)**: `tools/m7_qemu_rig` 리그 비교에서 여전히 프레임 당 명령어 수가 큰 폭(-42% 수준)으로 단축되는가?
- [ ] **(d) 진단 로깅**: `profile2 diag` 화면 또는 터미널 로그에서 `rc_smw: activated`와 해시가 정상 출력되는가?

**⚠️ 주의사항**: 위 4가지 중 단 하나라도 붉은 불(REVIEW)이 켜진다면 절대 실기에 플래시해서는 안 되며, 로그와 해시 불일치 원인부터 선행 해결해야 합니다.
