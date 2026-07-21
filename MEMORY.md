# 32X 성능 최적화 작업 — 세션 재개 메모

> 이 파일은 한 세션을 끊고 이어서 작업할 수 있도록 **현재 상태 스냅샷**을 남긴다.
> 상세 분석/측정 데이터는 `docs/32X_PERFORMANCE_RESULTS.md`(결과)와
> `docs/32X_RIG_ANALYSIS.md`(rig 구조/설계)를 본다. 실행 가이드는
> `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md`.
> 마지막 갱신: 2026-07-18

## 한 줄 요약

QEMU M7 histogram rig(측정 주도)로 32X 에뮬레이션 병목을 찾아, **keep 5종 최적화를 완료·커밋**했고,
지금은 **디바이스 DWT로 실기 검증** 단계. 프로젝트 규칙: rig은 상대비교/정확성, 절대 fps 판정은 항상 기기.

## 현재 커밋 상태

- testbed 메인: **`39b74a7a`** (perf/32x-histogram 브랜치)
- picodrive 서브모듈: **`e9e7ecb6`** (github.com/jshsakura/picodrive, detached HEAD — push 시 브랜치 필요)
- 작업 워크트리: `/home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd` (testbed 메인)
- 별도 워크트리: `/home/ubuntu/app/jupyterLab/notebooks/gnw-32x` (브랜치 explore/32x-feasibility) — 배선 원본, 건드리지 않음

## KEEP 5종 (전부 진짜 A/B, fb 체크섬 동일 = 사이클 정확, 회귀 없음)

| # | ROM | 최적화 | 개선 | 파일 |
|---|-----|--------|------|------|
| 1 | Doom | BFS 카운트다운(TST+BFS+ADD#-1) | **−58.3%** | sh2pico.c gnw_sh2_fastloop |
| 2 | VR | PWM cooperative idle yield(인터럽트로만 풀리는 루프) | **−73.6%** | memory.c PWM read case (백업 /tmp/32x-prof/pwm_poll.diff, **미적용**) |
| 3 | Kolibri | SDRAM poll BT/BF(icount 폴링, 양방향 tight slot) | **−33.3%** | sh2pico.c SDRAM poll case |
| 4 | Tempo | SDRAM poll BT/BF | **−25.2%** | sh2pico.c SDRAM poll case |
| 5 | Metal Head | BFS poll(MOV.W @(disp,GBR),R0, mask 0xc500) | **−51.3%** | sh2pico.c Doom BFS countdown case 2분기 확장 |

REJECT 3종: Zaxxon(직선 연산 40%+comm 15%), Chaotix(시프트/카피), VF(분산 86%) — fastloop skip 타겟 아님.
전체 개선 시도(RW 매크로 인라인)는 QEMU 역효과(+3.9%)로 폐기, DRC는 RAM 제약(264KB+4MB) 불가.

## 현재 working tree (커밋 후 변경분, 미커밋)

- `Core/Src/porting/md32x/main_md32x.c`: **`MD32X_DEVICE_PROFILE` DWT 계측 추가(+258줄, 가드됨, 기본 빌드 byte-identical 확인 완료)**. 가이드 §8 준수 — 6 disjoint bucket(pace/proc/pico/blit/audio/loop), drawn/skip 분리, p50/p90/p95/p99, 600프레임 후 /32x_dwt.txt 1회 controlled dump. 풀은 ahb_calloc(md32x BSS 예산 99.8%라 static 금지).
- `external/picodrive/cpu/sh2/mame/sh2pico.c`: e9e7ecb6 상태(keep 5종 전부). 진단 매크로(RIG_*)는 가드되어 byte-identical.
- `external/picodrive/pico/32x/memory.c`: base(e9e7ecb6). **VR PWM 패치(−73.6%)는 /tmp/32x-prof/pwm_poll.diff 백업만, 미적용.**

## 다음 단계 (이어서 할 작업)

1. **VR PWM 패치 복구**: `git -C external/picodrive apply /tmp/32x-prof/pwm_poll.diff` (memory.c). 복구 후 keep 5종 전부 활성.
2. **DWT 계측 빌드**:
   ```
   make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
        DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1 \
        ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 \
        CFLAGS_EXTRA="-DMD32X_DEVICE_PROFILE=1"
   ```
   (기본 빌드는 CFLAGS_EXTRA 제외 — byte-identical)
3. **기기 측정(사용자 실행, 하드웨어 필요)**:
   - base 빌드 플래시 → Metal Head(또는 keep 5종 각) 실행 10초 → /32x_dwt.txt 수집.
   - patched 빌드(keep 5종 + MD32X_DEVICE_PROFILE) 플래시 → 동일 측정.
   - 같은 save-state/게임플레이 위치, 같은 clock/volume/frameskip. 여러 윈도우.
   - 유효 결과 = p95/p99 또는 over-budget count 감소. QEMU 수치(−51.3% 등)가 실기에서도 나오는지 확인.
4. 결과를 `docs/32X_PERFORMANCE_RESULTS.md` 측정 9(디바이스 DWT)로 추가.
5. keep 5종 + DWT 계측 커밋.

## 핵심 규칙 / 환경

- **측정 주도, 추측 금지.** rig(QEMU)로 측정 → A/B(fb 체크섬 동일=사이클 정확) → keep/reject. 절대 fps는 항상 기기.
- **A/B 순서**: `git -C external/picodrive diff cpu/sh2/mame/sh2pico.c > /tmp/.../patch.diff` 백업 → `git -C external/picodrive checkout <commit> -- <file>` base → 측정 → apply 복구. (서브모듈 stash는 no-op 위험 — 쓰지 말 것.)
- **ROM 코퍼스**: `/tmp/32x-prof/roms/*.32x` (영문 symlink 15개, 원본은 한글 이름). SHA-256 = `/tmp/32x-prof/rom-hashes.txt`. git 넣지 말 것. 빌드 산물 rom.2x는 byteswapped — run_32x.sh 재입력 = double-swap 금지.
- **run_32x.sh**: `<rom.32x> [frames]`. env: `RIG_OUT`(빌드디렉토리), `PHASE_PROF=1`, `SH2_PC_HIST=1`, `FRAME_HIST=1`, `POLL_PEEK=1`, `SDRAM_POLL_DIAG=1`, `EXTRA_DEF`, `RIG_TIMEOUT=1800`.
- **SegaCD untracked 파일**(Core/Src/porting/segacd/, tools/m7_qemu_rig/{md_shim/,rig_mcd.c,run_mcd.sh,rig_md.c,run_md.sh}, tools/segacd_harness/)은 건드리지 말 것 — 별도 작업.
- toolchain: arm-none-eabi-gcc 13.2(CLAUDE.md의 15.2는 문서값). qemu-system-arm 8.2.2 mps2-an500 -icount shift=0.
- picodrive fork: github.com/jshsakura/picodrive. 커밋 스타일: `gnw:` 접두사, 상세 본문 + A/B 표. genpatch.py 안 씀(직접 커밋).

## 핵심 교훈 (재사용)

1. **32X는 폴링 루프가 주 병목**(Doom 순수 카운트다운은 예외). ROM마다 서로 다른 핫루프 — 일반화 안 됨, 개별 프로파일링 필수.
2. **RW body fetch는 region 가드 필수**: SDRAM(0x06) 직접 포인터만 safe. sysreg/comm(0x00/0x20) 핸들러 타면 poll_detect 부작용으로 fb 깨짐(Metal Head 회귀 원인).
3. **양방향 tight 폴링**(Kolibri slot 교대)은 cooperative RPOLL(memory.c)이 데드락/blank 실패. **icount 폴링**(sh2pico.c 단독, 매 iteration 진짜 RW 읽기)만 안전 — 사이드 이펙트 보존, 데드락 구조적 불가.
4. **fb 체크섬 동일 = 사이클 정확 = 디바이스 동작 불변.** 모든 최적화의 수락 조건.
5. **RW 인라인은 QEMU rig에서 역효과**(함수 호출이 QEMU 번역 캐시에 유리). host 장비에서는 다를 수 있으나 rig 기준 폐기.

## 세션 히스토리 포인터

이전 세션의 상세는 압축 블록(b1~b28)에 보존. 핵심:
- b1-b8: 셋업/프로파일러 구현/Doom BFS/VR PWM/winner 일반화 no-match/8 ROM 프로파일링.
- b13(task ses_094144): picodrive RPOLL 인프라 완전 분석. VR 폴링 = PWM(0x4038) 정정.
- b14: RIG_POLL_PEEK 진단으로 4종 region 파악(Kolibri/Metal Head/Tempo=SDRAM, Zaxxon=comm).
- b15: SDRAM cooperative idle yield 실패(데드락/blank).
- b17-b22: SDRAM poll fastloop(BT/BF) Kolibri/Tempo 성공 + Metal Head 회귀→가드 해결.
- b20-b22: Metal Head BFS poll(0xc500) −51.3%, keep 5종 확정.
- b26(task ses_09257f1a): SH-2 인터프리터 디스패치 비용 분석. RW 인라인 최적화 지점 식별.
- b27: fetch 인라인 A/B 역효과(+3.9%) 폐기.

## 문서 인덱스

- `docs/32X_PERFORMANCE_RESULTS.md` — 측정 1~8 + 32X 금광 지도 + 교훈 5조 + 커밋 상태.
- `docs/32X_RIG_ANALYSIS.md` — 환경/rig 구조/init 순서/phase profiler/§5-§6 설계.
- `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md` — 실행 가이드(§1 셋업~§11 완료기준). §8 = 디바이스 DWT.
- `MEMORY.md` — 이 파일(세션 재개용).
