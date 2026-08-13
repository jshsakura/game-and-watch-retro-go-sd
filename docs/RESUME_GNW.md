# RESUME — Game & Watch (STM32H7B0VB) / SNES rc feasibility

> 기종: **Nintendo Game & Watch** (STM32H7B0VBT6, Cortex-M7 @480MHz, 1MB internal flash,
> 128KB DTCM, 512KB SRAM, 16MB external QSPI flash, SD-card variant). 타겟 빌드
> `GNW_TARGET=mario|zelda`, `SD_CARD=1`, `INTFLASH_BANK=2`.
>
> 이 문서는 rc(SNES 65816→C 정적 재컴파일러) 타당성 조사의 **이어하기 지점**. 세부 기록은
> `MEMORY.md`(rc 프로브 전문)와 `CLAUDE.md`(프로젝트 전반)가 단일 진실 공급원.

---

## 0. 한눈에 (현재 상태)

| 축 | 상태 | 결과 |
|----|------|------|
| **insn-count** (QEMU M7) | ✅ **CLOSED** | SMW 120f: 인터프리터 8.20M → rc 4.63M ARMv7-M insn/frame = **1.77× 절감**, STATEHASH/프레임버퍼/오디오 **bit-identical**. 480MHz 예산(60fps=8.0M cyc/frame) 대비 rc ≈ 58% → **60fps headroom 충분**. |
| **절대 실기 fps** (온디바이스 DWT) | ⏳ **빌드 검증 완료, 실측 남음** | rc_probe 리그(Stage 1/2/3)가 `RC_PROBE=1` 빌드에 완전히 배선됨. **실기에서 GAME+TIME 홀드 부팅 → LCD/printf 읽기**만 남음 (하드웨어 의존). |

**결론(현재):** rc는 알고리즘/insn 축에서 타당. 남은 유일한 미검증 = 실리콘의 캐시/대기상태 비용이
4.63M insn을 예산 내에 retire시키는가. 그걸 재는 리그가 이미 빌드에 들어있다.

---

## 1. 다음에 할 일 (이어하기 지점)

### 온디바이스 DWT 실측 (하드웨어 필요)

1. **빌드** (docker gcc 15.2 권장, 로컬 13.2는 플래시 부풀어 초과):
   ```
   rm -rf build    # RC_PROBE 토글 시 필수 (C_DEFS 의존성 추적 갭)
   make release DOCKER=1 RC_PROBE=1 COVERFLOW=0 CHEAT_CODES=0 \
       SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
       INTFLASH_BANK=2 ZH_CN=0 ZH_TW=0 KO_KR=0 JA_JP=0
   ```
   산출: `build/gw_retro_go_intflash.bin`(≈253KB, 256K 뱅크 적합), `sd_content/roms/homebrew/rcprobe.xip`(2048B).

2. **SD 준비**: `rcprobe.xip`를 SD의 `/roms/homebrew/`에 복사 (sm.xip와 같은 폴더).

3. **플래시 + 부팅**: `gnwmanager`로 intflash.bin 기기 기록. **GAME+TIME 홀드 상태로 부팅**.

4. **결과 판독** (LCD 텍스트 + printf 시리얼 로그):
   - **Stage 1**: 맵 룩업 cyc/lookup — DTCM vs AHB vs flash × hash/bsearch × N(4355/8371). PRIMARY 축 = 배치. flash가 DTCM 대비 10× 이상 느리면 → 32MB 평탄 맵은 기각, DTCM MPH 필수(제약 A 확정).
   - **Stage 2**: XIP 간접호출 cyc/op (COLD=I/D캐시 무효화 후 / WARM=핫) + 정합성. **정합성 FAIL = DOOM류 베니어 부패 신호** (rc 호출 빈도에서 위험이 현현하는지). COLD가 예산의 상당 부분을 잡아먹으면 I-cache 압력이 병목.
   - **Stage 3**: 결합 per-opcode 디스패치 (DTCM-맵 + RAM-stub 간접호출, 하한). Stage 2 실측치를 더하면 실제 per-opcode 비용 근사.

5. **판정 기준**:
   - Stage 2 정합성 PASS + COLD cyc/op이 합리적(수십~수백 cyc) → **rc 실기 타당 확정**.
   - 정합성 FAIL 또는 COLD가 수천 cyc → DOOM류 위험 현현 → 원인 규명(ABFSR/캐시 프로파일) 필요.

---

## 2. 빌드/검증 함정 (다시 밟지 말 것)

| 함정 | 증상 | 해결 |
|------|------|------|
| **`make docker` 폐기됨** | `make docker RC_PROBE=1 ...`가 릴리스 출력(플래그 무시) | `make release DOCKER=1 <플래그>` 사용 (Makefile.common:2060 경고) |
| **stale build/ 디렉토리** | RC_PROBE 토글 후 링크 에러(undefined ref) | `rm -rf build` 후 클린 빌드. C_DEFS(-D)는 의존성 추적 안 함. |
| **로컬 gcc 13.2 플래시 부풀음** | 호스트 빌드가 ~14KB 더 큼 → 초과 | docker gcc 15.2(`make release DOCKER=1`)가 정준. |
| **full-locale + RC_PROBE=1** | ~3KB 초과 (CJK 렌더링 코드가 intflash) | slim 설정(COVERFLOW=0 CHEAT_CODES=0 CJK-off) 사용 |

### 정준 빌드 명령 (authoritative)
- **릴리스** (RC_PROBE=0, 바이트 동일): `make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1` → intflash.bin=262100B.
- **RC_PROBE slim**: 위 §1 step 1 명령 → intflash.bin=253176B.

---

## 3. 두 개의 독립적 바인딩 제약 (rc 타당성의 본질)

| 제약 | 내용 | 상태 |
|------|------|------|
| **A. 맵 배치** | rc의 `rc_map` = 평탄 32MB 테이블, 매 옵코드마다 랜덤 읽기. RAM_EMU(724KB)에 안 들어감. 외부플래시 배치 = 매 옵코드 캐시미스 재앙. | **설계 결론: 맵은 무조건 DTCM(0x20000000, 128KB).** MPH 우선(~42KB for 8371 sites), banked bsearch 차선. 플래시 배치 전부 기각. Stage 1 실측으로 확인 예정. |
| **B. XIP 호출 빈도** | RAM 인터프리터 루프 → 외부플래시 사이트 함수 간접호출(blx reg), ~1-2M 회/초. 사이트 코드 1.26-1.49MB → XIP 필수. | **설계 결론: 간접호출(링커 베니어 아님)이 실제 메커니즘.** SM이 동일 XIP 패턴 실사용 중(무조건 치사 아님). 미검증 = 규모. Stage 2 실측으로 확인 예정. |

**DOOM 전례 + SM 반증:** DOOM은 RAM↔XIP `ldr pc,[pc]` 베니어가 XIP 실행 시 실패("HW 프로브 없이 못 풂"). 하지만 SM이 동일 명령형으로 세레스 인트로(필수 경유)를 실기에서 통과 → 베니어 자체가 무조건 치사는 아님, DOOM은 next-hack 특유(749KB 캐시를 XIP에 쓰면서 실행) 문제. rc의 진짜 위험 = **규모**(SM ~90 드문 베니어 vs rc 수천 사이트 × 수백만 회/초). Stage 2가 이 규모 위험을 직접 압박 테스트.

---

## 4. rc_probe 리그 아키텍처 (이미 빌드됨)

- **게이트**: `RC_PROBE` Make 변수(기본 0). `RC_PROBE=1`일 때만 rc_probe.c 컴파일 + rcprobe.xip 추출. 런타임은 GAME+TIME 버튼 콤보로 추가 게이트(릴리스 완전 불활성).
- **훅 위치**: `rg_main.c`의 sdcard_init/fs_mounted 직후(line 1195) — Stage 2가 SD 마운트 필요. (gba_probe는 main.c:519 유지, OSPI만 필요.)
- **Stage 2 메커니즘** (SM의 SM_CODE 패턴 복사):
  - 링커 센티넬 `RCPROBE_CODE (x) : ORIGIN = 0xD0D00000, LENGTH = 64K` (다른 센티넬과 충돌 피함).
  - `.xip_rcprobe` 섹션(64개 사이트, 각 고유 addend `+n*0x10000`로 -fipa-icf 병합 회피)이 센티넬에 링크.
  - Makefile이 `--only-section=.xip_rcprobe`로 `rcprobe.xip` 추출.
  - 런타임: `odroid_overlay_cache_file_in_flash_relocate("/roms/homebrew/rcprobe.xip", ...)`가 QSPI 플래시에 캐시 → 실제 주소(0x9000xxxx) 반환. 사이트 fn-ptr를 `sentinel_sym + (base - RCPROBE_CODE_BASE)`로 계산 → round-robin 간접호출(이게 실제 rc 디스패치 메커니즘, 링커 베니어 아님). DWT 측정 + 정합성 검증.

### 검증된 빌드 산출 (docker gcc 15.2, RC_PROBE=1 slim)
| 산출 | 값 |
|------|-----|
| `gw_retro_go_intflash.bin` | 253176B (256K 뱅크 적합) |
| `.xip_rcprobe` VMA | 0xd0d00000 (센티넬), 크기 0x800 (2048B = 64 사이트) |
| `rcprobe.xip` | 2048B @ sd_content/roms/homebrew/ |
| `rc_probe_run_if_requested` | 0x08119478 (내부 플래시) |
| `rc_xip_site_0` @ 0xd0d00000 | 실제 사이트 코드 역어셈 확인 (`ctrs[sid]++`) |
| rg_main.o | `U rc_probe_run_if_requested` (배선 정상) |

---

## 5. 변경된 파일 (rc 작업, 미커밋)

- `STM32H7B0VBTx_SDCARD.ld` — RCPROBE_CODE 센티넬 + `.xip_rcprobe` 섹션
- `Makefile.common` — RC_PROBE 게이트 + rcprobe.xip 추출
- `Core/Inc/rc_probe.h` — RC_PROBE 기본값 0
- `Core/Src/main.c` — 훅 제거(rg_main.c로 이동)
- `Core/Src/retro-go/rg_main.c` — 훅 추가(sdcard_init 후)
- `Core/Src/rc_probe.c` — Stage 2 전면 재작성 (커밋 6c5d7e75 위에 미커밋)
- `MEMORY.md` — rc 프로브 전문 + M7 판정 기록
- `tools/m7_qemu_rig/rig_snes_rc.c` — GNW_SNES_CORE 가드 수정 (b4 아크, M7 판정용)

### 건드리지 말 것 (다른 스레드)
- `Core/Src/porting/md32x/main_md32x.c` (32x 프로파일링, branch perf/32x-histogram)
- `Core/Src/porting/segacd/`, `tools/segacd_harness/`, `external/picodrive`
- `docs/32X_*`, `tools/m7_qemu_rig/{rig_32x,rig_mcd,rig_md,run_32x,run_mcd,run_md}*`

---

## 6. 핵심 지식 소재 위치

- **`MEMORY.md`** — rc 프로브 전 기록(제약 A/B, DOOM/SM, 3단계 설계, 빌드 config, 체인지로그). 가장 상세.
- **`CLAUDE.md`** — 프로젝트 전반, 빌드/플래시 워크플로우, 하네스 인덱스, 디버깅 가이드.
- **`tools/sfc_recomp/README.md`** — rc 호스트 PoC 구조 + 하네스 인덱스.
- **`tools/m7_qemu_rig/`** — QEMU M7 리그 (rig_snes_rc.c=rc, rig_snes.c=인터프리터 baseline).
- **`docs/HARNESSES.md`** — 모든 하네스 카탈로그 (어떤 질문에 어느 리그가 답하는가).
- **`Core/Src/porting/sm/main_sm.c`** — SM의 XIP 센티넬-캐시 메커니즘 (rc_probe가 복사한 템플릿).

---

## 7. 재현 명령 치트시트

```bash
# 호스트 게이트 (정합성, 1500f, ~37s)
bash tools/sfc_recomp/build.sh external/smw/smw.sfc 1500

# QEMU M7 A/B (insn-count 축, 이미 CLOSED)
RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes_rc.sh external/smw/smw.sfc 120   # rc
RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes.sh   external/smw/smw.sfc 120    # interp

# 온디바이스 프로브 빌드 (절대 fps 축, 실측 남음)
rm -rf build
make release DOCKER=1 RC_PROBE=1 COVERFLOW=0 CHEAT_CODES=0 \
    SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
    INTFLASH_BANK=2 ZH_CN=0 ZH_TW=0 KO_KR=0 JA_JP=0
# → build/gw_retro_go_intflash.bin + sd_content/roms/homebrew/rcprobe.xip
# 기기에 플래시, rcprobe.xip SD 복사, GAME+TIME 홀드 부팅 → LCD/printf 판독
```
