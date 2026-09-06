> **Recovered 2026-09-07.** This file lived at the repository root as `MEMORY.md` and was
> overwritten wholesale by an unrelated 32X session note one commit later (`6c5d7e75` ->
> `9e9516fb`), so the rc investigation lost its primary record while
> [RESUME_GNW.md](RESUME_GNW.md) went on citing it as the single source of truth. Restored
> here from `6c5d7e75` under a name no other workstream will claim.
>
> The content below is as it stood on that commit and has not been re-verified against the
> tree since. Treat dates, commit hashes and "current status" as of then.

# MEMORY — SNES rc (65816→C static recompiler) device-feasibility probe

> 세션 간 지식 누적용 러닝 로그. 새 사실이 생기면 **맨 아래 "Changelog"에 한 줄 추가**하고
> 본문 해당 섹션을 갱신. 최신 상태는 항상 이 파일이 단일 진실 공급원(single source of truth).
> `piloci_recall`은 프로젝트 토큰이 없어 현재 접근 불가 → 디스크 파일이 유일한 영속 매체.

---

## 1. 한 줄 요약

rc(65816 정적 재컴파일러)의 **insn-count 축은 QEMU M7 리그에서 결정적으로 판정났다 (PASS)**:
SMW 120프레임에서 인터프리터 8.20M → rc 4.63M ARMv7-M insn/frame = **1.77× 감소(44% 절감)**,
STATEHASH/프레임버퍼/오디오 전부 bit-identical (gba식 bit-exact 정합성). 480MHz 예산(60fps=8.0M
cyc/frame) 대비 rc ≈ 58% → 60fps 달성 가능. **남은 미검증 한 축 = 온디바이스 캐시/대기상태 비용**
(아래 §6–§8 온디바이스 프로브, 트리에 남김, 현재 paused).

---

## 2. M7 QEMU 판정 (LOCKED — 금광, 이 리포의 주결과)

**SMW (`external/smw/smw.sfc`), 120프레임, window=40, 풀렌더(프레임스킵 없음), hard-float fpv5-d16.**

| 리그 | emu insn/frame | apu insn/frame | total | STATEHASH | window fb fnv1a |
|------|----------------|----------------|-------|-----------|-----------------|
| **인터프리터** (`run_snes.sh` / `rig_snes.c`) | 7,921,423 | 279,503 | **8.20M** | 17012c30 | 63112383,63112383,ee057301 |
| **rc native** (`run_snes_rc.sh` / `rig_snes_rc.c`) | 4,379,536 | 253,823 | **4.63M** | 17012c30 | 63112383,63112383,ee057301 |

- **정합성 (gba식 bit-exact):** STATEHASH 동일(17012c30) + 매 윈도우 프레임버퍼 fnv1a 동일 +
  lit-pixel 수 동일(0,0,301) → 정적 재컴파일이 M7-ISA 수준에서 bit-exact SNES 상태 생성.
  rc 커버리지 99.99%(120f: native=1722113 interp=130) / 100%(40f).
- **가속:** 8.20M → 4.63M insn/frame = **1.77× 감소**.
- **예산 수학 (480MHz Cortex-M7, 60fps = 8.0M cyc/frame):** 인터프리터 ~8.2M insn/frame ≈ 예산
  한계(오버헤드 합치면 60fps 불가능 경계). rc ~4.63M insn/frame ≈ 예산 58% → ~2× 여유 → PPU 렌더 +
  디스패치-맵 비용(아래 제약 A) 합쳐도 60fps 충분한 headroom.
- **호스트 게이트 (build.sh, 1500f):** GATE PASS. state=233327bf9446f28b audio=94d050800eeb0814
  bit-identical, native=21914546 interp=6500 coverage=99.97%, baseline 1.308 vs hybrid 0.952 ms/frame.
- **재현 명령:**
  ```
  bash tools/sfc_recomp/build.sh external/smw/smw.sfc 1500          # gen + 호스트 게이트
  RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes_rc.sh external/smw/smw.sfc 120   # rc
  RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes.sh   external/smw/smw.sfc 120    # interp
  ```
  두 리그 모두 `GNW_SNES_CORE` + hard-float fpv5-d16 + 동일 SNES 코어 소스 → STATEHASH 비교 가능.
- **rig_snes_rc.c 수정 (이 아크):** GNW_SNES_CORE 정의 시 `free(cart->rom)+repoint` 블록이
  언가드였던 것을 `#if !defined(GNW_SNES_CORE)`로 가드 (rig_snes.c와 동일). 언가드 시 cart->rom이
  링크된 .rom_blob(0x60000000)을 직접 가리켜 free(비힙)→newlib 아레나 무한루프(행). 가드 후 40f
  exit 0, 첫 윈도우 emu=3,729,872 = 120f 실행(3,729,869)과 일치.
- **QEMU 한계 인지:** mps3-an500 `-icount`는 insn 수는 정확히 주지만 **캐시/대기상태를 못 모델링**
  (HARNESSES.md:98-99). 위 수치는 알고리즘적 A/B + 예산 수학이지 **절대 실기 fps가 아님**. 절대 fps는
  온디바이스 ledger(아래 §6 프로브)에 속함.

---

## 3. 질문과 구조

**질문:** rc의 per-opcode 디스패치 패스가 실제 칩에서 사이클 예산 안에 드는가?

**rc 구조** (`tools/sfc_recomp/`, HOST PoC):
- `rc_core.c` 가상 핫경로(169줄): `uint16_t id = rc_map[((uint32_t)cpu->k<<16)|cpu->pc];` → `rc_fns[id-1](cpu);`
- **`rc_map` = 평탄 32MB 테이블** (`calloc(1u<<24, sizeof(uint16_t))`) — 매 옵코드마다 랜덤 읽기.
- 사이트 코드: 젤다 ~4,355 / SMW ~8,371개 함수, 1.26~1.49MB → 외부플래시 XIP 필수.
- 디스패치 빈도 ~1-2M 회/초. QEMU는 캐시/대기상태를 못 모델링 → **반드시 온디바이스 DWT 측정**.

---

## 4. 두 개의 독립적 바인딩 제약

| 제약 | 내용 | 결론 |
|------|------|------|
| **A. 맵 배치** | 매 옵코드마다 룩업하는 구조. 32MB 평탄 맵은 RAM_EMU(724KB)에 안 들어감. 외부플래시 배치 = 매 옵코드 캐시미스 재앙(DOOM 위험의 맵 판). | **맵은 무조건 DTCM(0x20000000, 128KB).** MPH 우선, banked bsearch 차선. 플래시 배치 전부 기각. |
| **B. XIP veneer 호출 빈도** | RAM 루프 → 외부플래시 사이트 함수 호출(veneer `ldr.w pc,[pc];.word`), ~1-2M 회/초. | veneer 명령 자체는 SM이 동일 패턴으로 실사용 중(무조건 치사 아님). 미검증 = **규모**. |

---

## 5. DOOM 전례 + SM 반증 (중요)

- **DOOM**: RAM 오버레이(0x24xxxxxx)↔XIP 플래시(0x90xxxxxx) `ldr pc,[pc]` veneer가 XIP에서 실행될 때
  실패. 결론 "HW 프로브 없이는 못 풂". 하지만 next-hack 특유 문제(749KB 캐시를 XIP 영역에 쓰면서
  동시 실행)였을 가능성 높음 → **"XIP에서 ldr pc,[pc] = 무조건 죽는다"는 하드웨어 법칙이 아님.**
- **SM 반증**: `Core/Src/porting/sm/main_sm.c` + 링커 `.xip_sm`(SM_CODE sentinel 0xDEAD0000/512K)가
  sm_8b.o 콜드뱅크(세레스 인트로 컷신)를 QSPI 플래시에서 직접 실행. objdump로 확인한 veneer 명령형이
  DOOM을 죽인 것과 **동일** (`ldr.w pc,[pc]; .word`). SM은 기기에서 부팅→인트로(이 경로 필수
  통과)→플레이→세이브 검증 완료. 즉 XIP→RAM veneer 크로싱은 실기에서 이미 수천 회 살아남음.
- 차이: SM은 ~90개 드문 veneer, rc는 수천 개 사이트를 매초 수백만 회 호출 → **진짜 위험 = 규모**.

---

## 6. 프로브 리그 설계 (3단계)

복사 템플릿 = `feat/gba-probe` 브랜치의 `gba_probe.c`(부팅 시 버튼 콤보로 게이트, DWT 사이클 측정,
LCD+printf 리포트, halt 루프). 파일: `Core/Inc/rc_probe.h`, `Core/Src/rc_probe.c`,
`main.c`(OSPI_Init 직후 훅, 510줄), `Makefile`, `STM32H7B0VBTx_SDCARD.ld`(`.xip_rcprobe` 섹션).

- **Stage 1 — 맵 룩업 비용**(XIP-exec 의존 없음, 결정적): placement(DTCM/AHB/flash) ×
  scheme(hash/bsearch) × N(4355/8371) × 스트림(uniform/지역성). PRIMARY 축 = 배치.
- **Stage 2 — XIP veneer 호출 비용 + 정합성**(DOOM 위험 테스트): K=64 사이트 함수를 외부플래시에서
  실행, RAM 루프가 volatile fn-ptr 테이블로 호출 → veneer 강제. M≥1M 회 측정 + per-site 카운터로
  정합성 검증(veneer 부패 = DOOM 위험 신호).
- **Stage 3 — 결합 per-opcode 디스패치**: DTCM-맵-룩업 + XIP-사이트-호출 + 리턴, 사이클/옵코드.

---

## 7. 현재 상태 (커밋 기준)

### 빌드
- **릴리스(`RC_PROBE=0`)**: 바이트 동일. `make docker` 통과. `gw_retro_go_intflash.bin`=262100B.
  map에 `rc_probe` 0건, `.xip_rcprobe` 0바이트. **rc_probe.o 빌드 안 됨.**
- **풀 프로브(`RC_PROBE=1`)**: 풀 로케일+CJK 유지 시 3156B 초과. CJK off→2708B 초과.
  **Coverflow+cheats off + CJK off로 통과**:
  ```
  make release DOCKER=1 RC_PROBE=1 COVERFLOW=0 CHEAT_CODES=0 \
              SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
              INTFLASH_BANK=2 ZH_CN=0 ZH_TW=0 KO_KR=0 JA_JP=0
  ```
  `gw_retro_go_intflash.bin`=252516B, rc_probe.o 빌드됨(map에 311건).

### Stage별 상태
| Stage | 상태 | 비고 |
|-------|------|------|
| **1** | ✅ FULL | DTCM/AHB/flash × hash/bsearch × N × 스트림. hash N=8371은 96KB>81KB(DTCM힙)로 skip(bsearch로 N축 커버). |
| **2** | ⚠️ STUB (`S2 SKIP extflash-not-flashed`) | 이유는 §8 참조. |
| **3** | ✅ 동작(RAM-stub 하한) | XIP veneer 미포함으로 "하한" 명시. |

### 링커 veneer 확인 (RC_PROBE=1 빌드, objdump)
```
rc_force_veneer (0x0810b4a2): b.w 8130d90 <__rc_xip_site_0_veneer>
__rc_xip_site_0_veneer (0x08130d90):
  f85f f000  ldr.w pc, [pc]      @ 8130d94
  90010001   .word 0x90010001    @ rc_xip_site_0 (Thumb bit)
```
내부플래시(0x081xxxxx)→외부플래시(0x9001xxxx) ~2GB 갭을 veneer가 브릿지. **DOOM을 죽인 것과
동일 명령형.** `.xip_rcprobe`@0x90010000 (0x70바이트, 사이트 4개).

### 메모리 배치 확인 (map)
- DTCM 힙: `_heap_start=0x20005410`, `_heap_end=0x20019810` (0x2000xxxx ✓)
- AHB 힙: `__ahbram_heap_start__=0x300088a0` (0x3000xxxx ✓)
- 사이트: `rc_xip_site_0..3` @ 0x90010000..0x90010054 (EXTFLASH VMA ✓)

---

## 8. Stage 2가 STUB된 진짜 이유 (핵심 — 다음 세션에서 풀 것)

**SD카드 변형(`SD_CARD=1`)에서는 `extflash.bin`이 외부플래시 칩에 플래시되지 않는다.**

- `Makefile.common:1619`의 `flash` 타겟은 `intflash.bin`만 내부플래시에 기록.
- `extflash.bin`(오버레이 섹션들)은 **런타임에 SD에서 코어를 스트리밍**하는 방식으로만 사용 →
  부팅 시 외부플래시 칩에 rc 사이트 바이트가 존재하지 않음.
- 따라서 `.xip_rcprobe`@0x90010000을 실행하면 OFW/체인로더가 남긴 잔해가 돌아감.
- 메커니즘 자체는 동작(SM/GBA가 외부플래시 실행을 증명)하지만, **런타임 SD-load+cache+relocate
  시퀀스가 필요**하고 부팅-타임 프로브는 그것을 쓸 수 없음.

**Stage 2를 풀려면(옵션):**
1. `.xip_rcprobe` 섹션 바이트를 SD 파일로 만들고, 프로브 시작 시 SD→외부플래시 캐시로 로드(SM의
   `store_file_in_flash_relocate()` 패턴 재사용) — 그래야 부팅 프로브가 실행 가능.
2. 또는 비-SD 빌드(`SD_CARD=0`)에서 extflash.bin이 플래시되는 경로로 검증(하지만 이 리포는 SD 변형).
3. 또는 프로브를 부팅 타임이 아니라 런타임(코어 로드 후)으로 옮김.

`.xip_rcprobe` 링커 섹션과 veneer는 objdump 검증용 + 미래 비-SD 빌드용으로 그대로 둠.

---

## 9. 오픈 / 다음 단계

> **insn-count 축은 §2에서 CLOSED (rc PASS, 1.77× 절감, bit-exact).** 아래 항목은 남은
> **온디바이스 캐시/대기상태 축**(절대 실기 fps)에 속함.

1. **Stage 2 실측**(위 §8 옵션 중 하나로 XIP 실행 확보 후 veneer 호출 비용+정합성 측정) —
   절대-fps 타당성의 결정적 미검증 남은 한 축.
2. **실기 PC 트레이스 리플레이**: `harness_main.c:86` 히스토그램 덤프로 얻은 실제 PC 분포를
   프로브에 넣어 지역성 효과 확인(현재는 합성 스트림).
3. **MPH 생성**: 현재 hash/bsearch만 측정. translate.py가 사이트 집합을 알 시점에 recsplit/CHD
   변위 테이블 생성 → 결정론적 ~6-8cyc 룩업 검증.
4. **DTCM 예산**: SMW 8371사이트 × 6바이트 ≈ 50KB. 현재 DTCM 힙 ~81KB. 코어 구조체(Cpu/Snes)와
   공존 가능한지 런타임 확인 필요. 16K사이트(96KB)는 DTCM 한계 근접 → banked 차선 검증.
5. **다른 ROM으로 M7 판정 확장**: Zelda(4355사이트, ROM 미커밋) 등으로 §2 A/B 재측정 — SMW
   최악값 케이스가 이미 통과했으므로 우선순위 낮음.

---

## 10. 건드리면 안 되는 것 (트리에 있는 미커밋 작업)

- `docs/32X_PERFORMANCE_RESULTS.md`
- `tools/m7_qemu_rig/{rig_32x.c,run_32x.sh,rig_mcd.c,rig_md.c,run_mcd.sh,run_md.sh,md_shim/}`
- `Core/Src/porting/segacd/`, `tools/segacd_harness/`, `external/picodrive` 서브모듈

---

## Changelog

- **(M7 판정 LOCKED)** SMW 120f QEMU M7 A/B: 인터프리터 8.20M → rc 4.63M insn/frame = **1.77× 절감**,
  STATEHASH/프레임버퍼/오디오 bit-identical (gba식 bit-exact 정합성). 480MHz 예산 대비 rc ≈ 58%
  → 60fps headroom 충분. insn-count 축 CLOSED. `rig_snes_rc.c` free(cart->rom) 블록 GNW_SNES_CORE
  가드 누락 행 버그 수정(rig_snes.c와 동일 가드). 호스트 게이트(1500f) PASS.
- (최초) rc 프로브 리그 구축: RC_PROBE 게이트 + Stage 1 FULL + Stage 2 STUB(extflash-not-flashed) +
  Stage 3 RAM-stub. veneer(objdump) + DTCM/AHB/flash 배치(map) 확인. 풀 프로브 빌드 config 확립.
