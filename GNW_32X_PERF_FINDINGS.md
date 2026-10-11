# GNW 32X(SH2) 성능 작업 — 실패 기록 및 재시도 금지 근거

> **2026-10-08 반복 방지 기준:** 새 후보를 고르기 전에
> [실패·보류 접근 목록](docs/32X_FAILED_APPROACHES.md)을 먼저 읽는다.
> 10-07 AOT 5종과 10-08 Thumb 실기 −15.21453% 기각까지 묶어 기록했다.
> 아래 “다음 방향”은 당시 이력이며 자동 재시도 목록이 아니다. 현재 기준은 PCM-r2 유지다.

> 적용 범위 보충 (2026-09-30): 아래 초기 불가/폐쇄 판정은 당시 Doom rig 작업량과
> 8% 게이트, 기존 interpreter/fold 구현에 대한 기록이다. 다른 장면·After Burner,
> 새 Thumb 블록 실행기나 메모리 접근 구조까지 불가능하다고 입증한 결과가 아니다.
> 현재 실기 결과와 범용 후속 방향은 §17–18을 따른다.

- 작성: 2026-09-11 (작업 기간 2026-09-10 ~ 09-11)
- 대상: DOOM 32X, QEMU M7 레그(`tools/m7_qemu_rig/run_32x.sh`), ubuntu-lab 워크트리 `/tmp/gnw_posthle_20260910`
- 결론 요약: **SH2 인터프리터 영역에서 8% 게이트(호스트 명령어 887k/프레임 절감)는 측정적으로 불가.**
  모든 시도(핸드블록, AOT 루프트레이스, fold 확장 검토)가 기각되었고, 그 이유가 각각 실증됨.
  다음 기회는 SH2 밖: MD VDP draw 10.2% > 32X 컴포지터 7.1% > YM2612 6.1%.

---

## 0. 절대 기준 (베이스라인 — 모든 비교의 분모)

| 항목 | 값 |
|---|---|
| avg host insn/frame | **11,086,997** (n=600, f≥1200, FRAME_HIST) |
| fb 해시 | f99=`9cd2510f` f299=`22ee77a6` f1799=`464dd41b` |
| snd 해시 | `8cdfb973` (calls=1800, samples=2,646,000) |
| dropped frames | 0, GATE3 **PASS** |
| .elf text | 3,984,112 B |
| SH2 디스패치/프레임 | ≈119,961 (fold 적용 후; 순수 게스트는 ≈457,452 → **fold가 게스트insn의 ~74% 소비**) |
| host insn / SH2 디스패치 | ≈65.7 |

측정 환경(반드시 동일하게):

```bash
RIG_OUT=<dir> FRAME_HIST=1 \
EXTRA_DEF='-DRIG_WARMUP=1200 -Wno-error=incompatible-pointer-types' \
bash tools/m7_qemu_rig/run_32x.sh /tmp/doom32x_unswapped.32x 1800
```

- ROM: `/tmp/doom32x_unswapped.32x` (run_32x.sh가 byteswap 수행)
- `-Wno-error=incompatible-pointer-types` 필수 (g68k_bus.c u32/unsigned-int 함수포인터 불일치, GCC 14.2.1에서 error 처리; 기존 캘리브레이션 환경도 warning으로 통과)
- 베이스라인 로그: `/tmp/rig_clean_base.log` (ubuntu-lab)

---

## 1. 시도 이력 — 전부 기각

### 1.1 핸드컴파일 블록티어 (RIG_BLOCKTIER, 2026-09-10, WARMUP=20 시절 기준 base=8,813,384)

| 버전 | avg host | 판정 |
|---|---|---|
| bt | 9,396,694 (+6.6%) | 느림 |
| bt2 | 9,261,348 (+5.1%) | 느림 |
| bt4 | 8,907,315 (+1.1%) | 느림 |

해시는 전부 일치(정확성 문제 없음). **순수 비용 문제로 기각.**

### 1.2 AOT 루프트레이스 (RIG_LOOPTRACE, 2026-09-10~11) — 사후 검증까지 완료한 본격 시도

- 대상: 마스터 SH2 텍스처 컬럼 루프 `0x02048f86..0x02049098` (128 insn, inner `9084..9094` / outer backedge `9096→f86`)
- 방식: 결정론적 생성기 `/tmp/gen_loop_trace.py` → `gnw_loop_trace.c`(2,966라인)를 sh2pico.c에 include, 인터프리터 루프 내 hooks, 실제 opXXXX 핸들러 재호출 + 인터프리터와 동일한 ppc/pc/icount/delay/IRQ 꼬리.
- **정확성: 최종적으로 게스트-완전 일치 달성** (fb 3종 + snd + GATE3 전부, NO_FASTLOOPS 환경에서 실증).
- **성능: 기각**

| 구성 | avg host | vs base |
|---|---|---|
| 기본(트레이스 비활성, 훅만 존재) | 11,572,963 | **+4.4% 느림** |
| 트레이스 활성(NO_FASTLOOPS, NF base=23,727,769 대비) | 28,657,247 | **+20.8% 느림** |

- 카운터(활성 시): entries=41,045 / inner backedge=1,599,094 / outer=25,784 / irq_exits=0 / icount_exits=14,631 / side_exits=26,414
- 코드 크기: +23,312 B (3,984,112 → 4,006,424)
- 패치 보존: `/tmp/gnw_looptrace_failed.patch` (sha256 `e10b1e2bb0d5d3703dc9b73816596650802026ae47b68eadd3bcc7be79644815`), 전체 스냅샷 `/tmp/gnw_looptrace_preserve/`
- 커버리지 본질 한계: 트레이스가 담는 게스트 insn ≈ 전체의 ~2.6% — 애초에 8% 게이트와 차원이 다름.

### 1.3 fold 확장 (2026-09-11, /tmp/glm_residual_fold.md 과업) — 구현 전에 상한 입증

측정 결과(아래 §3) 접을 수 있는 잔여 클러스터를 **전부 100% 효율로 합쳐도 ≈7.9%**, 단일 최고 후보(PCM 믹서)는 이론 3.0% / 실측 예상 ~2.5% → 게이트 8% 도달 불가. 과업 지침("8% 불가 증명 시 투기적 코드 금지")에 따라 **코드 추가 없이 종결**. 트리는 베이스라인과 바이트 동일.

---

## 2. 핵심 교훈 — 같은 것을 다시 들이대지 않기 위한 규칙

1. **거대 단일 AOT 함수는 아키텍처적으로 진다.** 3,000라인급 함수는 gcc -O2에서 레지스터 스플, 블록 간 레지스터 캐싱 불가. +20.8%는 튜닝으로 못 메움. (1.1의 핸드블록, 1.2의 루프트레이스가 동일하게 확인)
2. **"runtime off = 참 베이스라인"이 아니다.** 인터프리터 루프에 전역 플래그 load+test만 추가해도 인스패치당 비용이 쌓여 **+4.4%**. 참 비교를 원하면 컴파일 아웃(`#ifdef`)이 유일.
3. **fold 치환은 타이밍을 바꾼다(해시 위험의 실체).** 게스트 상태가 완전히 동일해도, fold가 raw 실행 대신 루프를 소비하면 **슬라이스 잔여 icount(±2)** 가 달라지고 → m68k 위상 드리프트 → 오디오 DMA 타이밍 → **snd 해시 불일치**(95bf9ed6 ≠ 8cdfb973) 발생. 새 fold 암은 "게스트 상태 동일"만으론 부족하고 **반복당 정확한 icount 청구 일치**(튜닝 경로 포함)까지 맞아야 함. 실증: f=746, 마스터 슬라이스 `in cyc=2341 pc=02039200`에서 base `ic=0 pc=0204908c` vs cand `ic=-2 pc=02049084`.
4. **sh2.c SHAR()은 인핸들러 다중소비자다.** 동일 SHAR 연속 실행을 fold로 먹고 `sh2->pc`를 전진시키며 icount를 청구함. 개별 insn을 재방출하는 어떤 메커니즘도 이를 다뤄야 함(루프트레이스가 fb f1799 불일치로 발견한 버그; post-guard `if (sh2->pc != <expected>) side_exit;` 패턴으로 해결).
5. **"zero dispatch is the whole point".** 범용 per-op-dispatch folder는 이미 시도되어 느렸음(12,797,823 vs 12,560,257). fold는 구조적 전체 매칭+매크로 리플레이(TexCol/Span 패턴)일 때만 이득.
6. **측정 윈도우를 먼저 확인하라.** whole-run 히스토그램 1위(컴핸드셰이크, 프레임당 988 호출)는 f150~600에만 존재, f750 이후 소멸 → f≥1200 게이트에 무관. whole-run 데이터로 기회를 판단하면 안 됨.

---

## 3. 상한 입증 (왜 SH2 fold로는 8%이 안 나오는가)

게이트 필요량: 11.09M × 8% ≈ **887k host/프레임** ≈ 마스터 디스패치 13,547감소/프레임.

후기 윈도우(f≥1200, `/tmp/rig_pch2.log`, 66.5M 카운트, 8,033 PC) 랜드스케이프:

- 디스패치 극도로 분산: **top-50 PC 누적 14.66%** (whole-run은 50%@113PC로 훨씬集中적 —早期 프레임 왜곡)
- fold 가능 클러스터(100% 효율 가정 시 상한):

| 클러스터 | 코어 | 위치 | 상한(전체 대비) |
|---|---|---|---|
| PCM 믹서 inner loop (249 iter/f) | slave | 0x0204e7d4–e814 | ~3.0% |
| comm 바이트폴 (293k×3 disp) | slave | 0x0204e844–e848 | ~2.2% |
| SHAR/RTS 체인 (805k+516k) | master | 0x0204e5c4–e5e4 | ~1.3% |
| 헬퍼 (220k×5) | slave | 0x0204e7ce–e7d2 | ~1.1% |
| BRA$ 셀프파킹 잔여 (247 wakeup/f) | slave | 0x0203612a | ~0.3% |
| **합계** | | | **≈7.9%** |

- 단일 최고 암(PCM 믹서)도 현실적 ~2.5% ≪ 8%. 4개 이상의 신규 해시-위험 암(교훈 3의 타이밍 위험 각각 부담)을 만들어야 7.9% — 게이트 미달 + 리스크만 추가.
- 위상별 잔여(PHASE_PROF, `/tmp/rig_phase.log`): msh2 59.0% / ssh2 11.9% / **draw(MD VDP) 10.2%** / **32X compositor 7.1%** / **fm(YM2612) 6.1%** / z80 2.0% / snd 1.0% / m68k 0.8% / other 1.5%

→ **SH2 인터프리터 최적화 노선 폐쇄. 다음 과제는 SH2 밖.**

---

## 4. 아티팩트 맵 (ubuntu-lab, /tmp 휘발성 주의 — 필요시 리포로 백업할 것)

| 경로 | 내용 | 비고 |
|---|---|---|
| `/tmp/gnw_looptrace_failed.patch` | 루프트레이스 전체 패치(기각) | sha `e10b1e2b…4815` |
| `/tmp/gnw_looptrace_preserve/` | 루프트레이스 시대 파일 전체 스냅샷 | sh2pico.c/rig_32x.c/run_32x.sh/.looptrace + 생성기 + 생성물 |
| `/tmp/gen_loop_trace.py` | 결정론적 트레이스 생성기 (481라인) | sha `620fd716…4deb7`, 2회 실행 cmp 동일 |
| `/tmp/gnw_loop_trace.c` | 생성물 (2,966라인) | sha `38442cd0…7835` |
| `/tmp/gnw_edge_guard_fix.patch` | RIG_EDGE_NOTE 가드 버그 수정(빌드 필수) | sha `f31b928e…1803`, §6 참조 |
| `/tmp/sh2dump` | 호스트 SH2 디스어셈블러 (sh2dasm.c 기반) | `/tmp/sh2dump <rom> <starthex> <endhex>` |
| `/tmp/rig_clean_base.log` | 확정 베이스라인 | 11,086,997, 전 해시 일치 |
| `/tmp/rig_phase.log` | PHASE_PROF 위상 귀속 | +0.23% 프로브세 |
| `/tmp/rig_pch.log` | whole-run PC 히스토그램 | early 왜곡 포함 |
| `/tmp/rig_pch2.log` | **후기 윈도우(f≥1200) 히스토그램** | 상한 입증의 근거 |
| `/tmp/rig_wp2.log` | 대기루프 타이밍 프로브 | wait 클러스터 f150-600 존재 입증 |
| `/tmp/rig_lt_*.log`, `/tmp/rig_ltnf_*.log`, `/tmp/rig_ws_*.log`, `/tmp/rig_lti_cand.log`, `/tmp/rig_s*_*.log`, `/tmp/rig_se_*.log` | 루프트레이스 A/B + 진단 로그 일체 | 상세 분석용 |

프로브 사용법: `SH2_PC_HIST=1` (+ `EXTRA_DEF`에 `-DRIG_PC_HIST_START=1200`), `PHASE_PROF=1`, `FRAME_HIST=1`.

## 5. 운영 교훈 (ubuntu-lab / 레그 사용 시)

- zsh: `===WORD===` 형태 마커 금지(=word expansion) — `--WORD--` 사용.
- rig 로그의 `f=%d`는 패딩 없음 — `f=150`로 grep(제로패딩 grep은 거짓 empty).
- 짧은 진단 런에 `-DRIG_WARMUP=1200`을 넣으면 n≤0 → "GATE3 FAIL: SH-2s never executed" **거짓 경보**.
- ubuntu-lab /tmp는 tmpfs — 빌드 디렉터리 수십 개가 볼륨을 채움(한 번 가득 참). 로그는 남기고 `rig_*` 빌드 디렉터리는 주기 정리.
- rig(QEMU semihosting)는 `getenv()`로 호스트 env을 못 읽음(리포 내 SRAM 코멘트로 실증) — A/B 스위치는 컴파일타임 `-D`로.
- QEMU 10.2.1: `-plugin` 인터페이스는 있으나 시스템용 플러그인/헤더 없음 → 심볼 귀속은 PC 히스토그램+호스트 디스어셈블러 조합으로.
- 병렬 레인: `nohup … ; touch /tmp/<name>.done` + done 마커 대기. 폴링 금지(AGENTS.md).

## 6. 남아 있는 실제 변경 (현재 트리 상태)

- 워크트리/서브모듈의 `M sh2.c / sh2pico.c / rig_32x.c`는 **작업 전부터 존재하던 캘리브레이션 수정** (이 문서의 작업이 남긴 것 아님).
- 유일하게 유지를 권장하는 수정: **RIG_EDGE_NOTE 가드 버그 패치** — `RIG_EDGE_PROBE` 블록이 `#ifdef RIG_SH2_PC_HIST` 내부에 중첩 정의된 반면 사용처(메인 루프)는 무가드라, `-DRIG_SH2_PC_HIST` 없는 빌드가 GCC 14에서 무조건 실패(implicit declaration). 수정은 매크로 블록 이동만으로 런타임 중립(고정 빌드 text=3,984,112로 바이너리 동일 입증). 미적용 시 빌드 파이프라인에 `-DRIG_SH2_PC_HIST` 강제 또는 본 패치 필요.

## 7. 차기 권장 (SH2 외, 측정 근거 순)

1. ~~**MD VDP draw — 10.2%**~~ → **§8로 종결(구조적 상한 입증)**.
2. ~~**32X 컴포지터 — 7.1%**~~ → **§9로 종결(Packed 모드 확인 + 잔여 여지 rig-불가시 클래스)**.
3. **YM2612 FM — 6.1%** (680,888 host/f): 사운드 코어.

각각 신규 과업 범위로 떼어내야 함(SH2 인터프리터 건은 이 문서로 종결).

## 8. MD VDP draw 과업 결과 (2026-09-13) — 구조적 상한 입증, 코드 미반영

전제: 베이스라인 11,086,997 host/f (n=600, f≥1200), draw 위상 10.2% = 1,141,912 host/f ≈ 5,046 host/렌더라인 × 224라인. 목표 ≥8% 절감 + 해시 4종 완전 일치.

### 8.1 탐색: "매 프레임 전체 재렌더"의 원인은 4겹이었다

drawprof 프로브(RIG_DRAWPROF): `PicoDrawSync` fast path(변화 없으면 포인터만 전진, 이전 픽셀 유지)가 **한 번도 발동 안 함(fast=0)** — 매 프레임 224라인 전체 재렌더. 원인을 순차적으로 추적:

| # | 차단자 | 위치 | 내용 |
|---|---|---|---|
| 1 | 데이터 포트 동일값 스킵이 SAT 영역 제외 | `videoport.c` 데이터 포트 게이트 | 68k가 매 프레임 SAT(스프라이트 테이블)를 **동일값으로 재기입** — 24,516/24,600 = 99.66%가 동일값(wrlog 프로브). 게이트가 SAT 영역 쓰기는 동일값이어도 `PicoVideoSync` 호출 → SYNC_NEEDED |
| 2 | `UpdateSAT()` 무조건 DIRTY | `pico_int.h:1008` | SAT 쓰기마다 값 무관 `PDRAW_DIRTY_SPRITES` 설정. `PicoDrawSync` 첫 분기가 DIRTY|PARSE → ParseSprites → 전체 렌더 폴스루(fast path는 else-if라 선점당함) |
| 3 | 프레임 끝 플러시의 SYNC_NEXT | `videoport.c` PicoVideoSync + `pico_cmn.c:231` | 224라인 모드에서 루프가 y==224에서 탈출하며 scanline=223 → 플러시 `PicoVideoSync(-1)`이 else 분기(active display로 오판)로 들어가 매 프레임 SYNC_NEXT 설정 → PicoFrameStart가 다음 프레임에 carry |
| 4 | **32X 모드 강제 (결정적)** | `32x/32x.c:941-943` | `PicoFrame32x()`: `if (Pico32xDrawMode != PDM32X_BOTH) rendstatus |= PDRAW_SYNC_NEEDED` — **무조건 매 프레임**. rig는 RGB555 출력 = PDM32X_32X_ONLY → 항상 발동 |

### 8.2 3개 수정 모두 해시-정확, 그러나 각각 ~0%

기제 GNW_VDP_SATSAMESKIP(컴파일 플래그 기본 OFF, 총 +216B)로 1~3을 순차 수정하며 A/B(1800f, RIG_VDPFIX=1):

| 수정 | avg host/f | 절감 | 해시 | 비고 |
|---|---|---|---|---|
| (베이스라인) | 11,086,997 | — | 전 일치 | |
| fix1: 데이터 포트 동일값 SAT 스킵 | 11,088,506→11,085,064 | −0.02% | 전 일치 | satskip_hits=24,516 정확히 발동. 빔 싱크 853→600회/프레임 |
| fix2: UpdateSAT DIRTY 값 게이트 | 11,067,300 | −0.18% 누적 | 전 일치 | parse 718→50회/프레임 (DIRTY 게이트 효과) |
| fix3: 플러시 SYNC_NEXT 억제 | 11,067,295 | −0.00% 누적 | 전 일치 | drawprof: 여전히 render=600/600 — #4 강제가 남음 |

### 8.3 상한 입증: 컴포지트-인-플레이스 아키텍처

- rig 출력 포맷 PDF_RGB555 → `Pico32xDrawMode = PDM32X_32X_ONLY`. 16비트 모드의 합성(do_line_dc)은 **MD 레이어를 목적 버퍼에 직접 그린 뒤 32X 레이어를 그 위에 오버레이** — 32X 투명 영역은 dest의 MD 픽셀이 그대로 보임.
- MD 레이어는 최종 출력에 **실제로 보인다**: MD 레이어 절단 실험(GNW_MD_ABLATE)에서 fb 해시 변화(f99/f299=0, f1799=4b9c8b64) — 단 snd 불변, avg 10,051,713(−9.3%)으로 "draw 비용 = MD 렌더" 상호 확인.
- 32X 레이어(3D 뷰)는 매 프레임 변화 + dest가 매 프레임 32X 픽셀로 오염 → **매 프레임 MD 224라인 재렌더는 이 아키텍처에서 구조적으로 필수** (32x.c:943의 강제는 정확한 동작).
- 대안은 전부 범위 밖: 8비트/DRAW2FB 경로 전환(출력 포맷 변경 → 해시 변화), 배경 캐시(320×224×2=143KB RAM — GNW 불가), 32X DRAM 변화 검출(512KB 비교).
- **결론: VDP draw 10.2%는 컴팩트 skip/caching 기제로 절감 불가. 원복 후 미반영.** (3개 수정은 전부 타당하나 성능 효과가 #4에 의해 0이 되고, #4는 올바른 동작이므로 건드릴 수 없음)

### 8.4 습득물

- 교훈: (i) 함수 **호출** 목록만 grep하면 직적 `rendstatus` 비트 쓰기는 안 보임 — 상태 플래그 세터는 토큰 단위 재검색 필요. (ii) "0% 측정 → 다음 차단자 프로파일" 반복이 3번 만에 아키텍처 바닥에 도달 — 각 단계 해시-정확성이 가설-반증 루프를 안전하게 만듦.
- 프로브 일체(drawprof/syncprof/wrlog)는 검증 후 전부 제거, 트리 원복(빌드 text=3,984,112 바이트 동일 입증, git status 과업 전 상태 일치).
- 아티팩트: `/tmp/gnw_vdp_satsameskip.patch`(videoport.c+pico_int.h git diff, 5,399라인, sha `8b3d730a…965d` — 해시-정확 3수정 통합), `/tmp/gnw_vdp_preserve/`(당시 파일 스냅샷), 로그 `/tmp/rig_vb.log`(베이스), `/tmp/rig_vc{,2,3}.log`(3차 A/B), `/tmp/rig_vcdp{,2}.log`(drawprof 증거), `/tmp/rig_ab.log`(ablate), `/tmp/rig_wl.log`·`/tmp/rig_sp2.log`·`/tmp/rig_dp.log`(프로브 레인, 전부 해시-정확).
- wrlog 덤 발견: 후기 윈도우 reg1=0x74(224라인), reg12=0x81(H40), SATa=dc00 — 렌더 파라미터는 정적, 68k의 VDP 쓰기는 SAT 재기입이 사실상 전부.

## 9. 빌드 플래그·컴포지터 후속 과업 (2026-09-14) — -O3 확보(−1.45%), 컴포지터 잔여 여지 봉쇄

전제: §7/§8 종결 후 잔여 경로(빌드 플래그, 컴포지터 마이크로) 조사. **트리 완전 무수정** — EXTRA_DEF 실험만으로 측정(해시-정확은 구조적으로 보장: 게스트 시맨틱 불변).

### 9.1 런타임 모드 확정 (RIG_MODEPROBE, 60f)

`PicoDraw32xLayer` 진입 시점 프로브(f=20-23 전부 동일, 즉시 원복): Doom 32X = **Packed Pixel 모드(Mx=1)**, PDM32X_32X_ONLY(dm=1), H40(h40=1), pri=0, fs=0.

### 9.2 컴포지터 잔여 여지 = rig-불가시 클래스 (기각 근거)

핫패스는 `gnw_line_pp`(32x/draw.c:168) — 이미 solid-run 검출 + octa/quad/pair SWAR 래더(u32 pmd 배경 테스트 + u32 스토어, GNW_PP_NO_RUNDET/QUAD/OCTA/WIDEBLAST 어블레이션 노브 전부 기본 ON, 디바이스 측정 이력 포함). 3D 뷰 영역(bg-run 경로, "Doom's 3D view leaves the MD layer at background" 코멘트)은 octa까지 최적화 완료. 유일하게 pair 수준인 non-bg(스테이터스바) 경로의 잔여 아이디어(palmd 조회 u32 스토어 병합)는 **스토어-병합 클래스 = rig가 0으로 측정**(트리 내 wideblast 코멘트 실증: rig 9,689,700 vs 9,690,235 — "no write buffer… prices every store the same") → 이 측정 체계에서는 판정 불가능한 최적화 → 기각. 8% 게이트 산술상 컴포지터 단독(7.1% 풀)도 애초에 미달.

### 9.3 빌드 플래그 A/B (1800f, WARMUP=1200 — 전 레인 해시 4종+GATE3 정확)

| 플래그 | avg host/f | vs -O2 | 판정 |
|---|---|---|---|
| -O2 (스톡) | 11,086,997 | — | 베이스라인 |
| **-O3** | **10,925,651** | **−1.45%** | **유일한 실측 개선 — 채택 후보** |
| -O3 -funroll-loops | 11,101,778 | +0.13% | 폐기(동적 명령수 증가) |
| -Os | 12,105,713 | +9.2% | 폐기 |

- LTO: 링크 라인(run_32x.sh:118-119)에 `$DEF` 미전달 → 스크립트 수정 필요 + 핫코드(SH2 인터프리터/컴포지터)는 단일 TU 내부 → 기대치 <0.5% → 미실시.
- **실기기 채택 전 제약**: -O3는 코드 사이즈 증가(flash/ITCM 압박, ICACHE 국소성) — rig는 스토어/캐시/쓰기버퍼 클래스를 대표하지 못함(§9.2 실증)이므로 **디바이스 재검증 후 채택** 권장. rig 체계 내에서는 무코드·무위험 −1.45%.
- 로그: `/tmp/rig_o3.log`, `/tmp/rig_o3u.log`, `/tmp/rig_os.log` (모드 프로브 출력은 stdout 즉시 판독 — 값은 §9.1에 기록).

### 9.4 시리즈 최종 누적 결론 (§10으로 갱신됨)

~~**8% 게이트 도달 경로 부재 입증 유지**(SH2 ≤7.9% 상한 + VDP 구조적 + 컴포지터 rig-불가시 + FM 해시-봉쇄) + 본 과업 실측 확보 **−1.45%(-O3)**.~~ → **§10 디바이스 실측으로 -O3 채택 후보 철회(부호 반전, 실기기 −2.2~−2.5% 느림).** 시리즈 최종 상태: 확보된 실측 개선 0%, 8% 불가 입증 유지, 단 rig↔디바이스 판정 불일치 클래스가 실측으로 확정됨(§10).

## 10. 디바이스(DWT/fps) 측정 체계 전환 과업 (2026-09-14) — -O3 부호 반전 입증, 채택 철회

전제: §9.3의 "-O3 채택 후보(디바이스 재검증 후)" 항목을 실기기로 판정. 측정 인프라 = `tools/gnw_probe/`(rpi-genie5 = 라즈베리파이 본체, ST-LINK/V2 SWD, gnwmanager, Docker 빌더 sylverb/retro-go-sd-builder:v1.5). **트리 무수정 원칙 유지**: 빌드 시에만 Makefile.common에 `MD32X_EXTRA_CFLAGS` 주입 훅(기본 빈)+RAM_EMU 고정 리스트를 잠시 넣고 양 암 빌드 후 `git checkout` 원복.

### 10.1 측정 설계

- 암: `o2d`(스톡 -O2) / `o3d`(RAM_EMU 상주 7+1 TU 제외 전역 -O3) / `o3i`(ITCM 상주 sh2pico만 -O3). 메모리 레이아웃: `.overlay_md32x`(RAM_EMU, 여유 ~1.5KB — 전역 -O3는 링크 오버플로→TU 고정 필수, make **명령줄 변수가 target-specific 변수를 이기므로 `override` 필요**), `.overlay_md32x_itc`(ITCM 12.3KB/64KB 여유), `.xip_md32x`(플래시 -O3 시 +29KB).
- 씬/프로토콜: Doom 32X 부팅→타이틀/메뉴 정착 후 `--no-reset` 300프레임 창 ×6 반복, 인접 샌드위치(o3d→o2d→o3d→o3i→o2d). 메뉴 씬이 100% 예산에 안착(31.9fps 캡)해 fps 판별력이 없던 초기 파일럿과 달리, 정착 후 프리런 45fps 국면은 **디바이스 한계 속도**로 판별력 확보(암내 산포 ±0.16fps vs 효과 ≥0.6fps). 프레임 예산 강제(`drawn_ab_press.sh`, mwh 반쓰기)는 보류 — 캡에 못 미치는 씬엔 불필요.
- 앵커 시도 부기: GNW_AUTOSAVE_FRAME=1200/4000 앵커(슬롯0)는 타이틀/메뉴에 안착 — 데모 게임플레이 앵커 프레임 인덱스는 미동결(현시점 측정은 프리런 메뉴 씬으로 충분).

### 10.2 결과 (샌드위치, fps 높을수록 좋음)

| 레그 | 평균 fps (6×300f) | 인접 o2d 대비 |
|---|---|---|
| o2d (중앙 컨트롤) | 45.59 | — |
| o3d 1차 | 44.46 | **−2.48%** |
| o3d 2차 | 44.58 | **−2.21%** |
| o3i (ITCM만) | 44.70 | **−1.95%** (양측 o2d 평균 45.37 대비 −1.48%) |
| o2d (최종 컨트롤) | 45.16 | 25분간 −0.9% 드리프트(문서화된 배회 범위 내) |

**판정: -O3는 실기기에서 모든 스코프에서 느림 — 채택 철회.** rig의 −1.45% 명령수 절감이 실기기에선 플래시 대기상태/I-캐시 압박(XIP +29KB)으로 상쇄·반전. **rig(QEMU icount)는 코드젠 플래그 변경의 성능을 판정할 수 없다**는 클래스가 샌드위치 실측으로 확정 — §9.2 "rig under-prices wide stores"와 동일 근원, 이제 빌드 플래그 축까지 실증. -O2 유지.

### 10.3 부발견 (차기 과업 감)

- **MD32X_DEVICE_PROFILE=1 암이 현재 트리에서 실기기 크래시**: 부팅 후 ~프레임 416/746(웜업 1200 완료 전, 프로브 무장 전) 재현성 정지, halt 시 PC=0x240e9110(BSS sndbuffer 인접 — 데이터 영역 실행), LR=0x7(EXC_RETURN). 8/19-20에는 동작하던 체계 → 이후 커밋(예: 2c98b55b DWT_CYCCNT 정리 등)과의 상호작용 의심. **DWT 사이클 귀속 프로파일링 재가동 전 수리 필요.**
- 문서 대로 "측정 창 사진" 확보(메뉴+스테이터스바 씬), 카드 슬롯0이 본 과업 앵커(f4000, 메뉴)로 교체됨(구팀 앵커 상태 미상 — 필요시 GNW_AUTOSAVE_FRAME으로 재작성).
- 습득 도구: `tools/gnw_probe/drawn_ab_press.sh`(drawn_ab 복제+프레임 예산 mwh 강제, 기본 무동작 — 신규 미트래킹 파일). 암 스냅샷: `/tmp/gnw_arms/{o2d,o3d,o3i,o2dp,o3dp,anchor,anchor2}` (빌드 로그 동봉).
- 서브모듈 26개 초기화(pi 체크아웃), 빌더 이미지 pull 완료 — 이후 디바이스 과업 진입 장벽 제거됨.

### 10.4 결론

디바이스 측정 체계는 가동됨(암 빌드→플래시→샌드위치 fps A/B, 재현성 ±0.35%). **남은 성능 경로는 전부 "rig 불가시 + 디바이스에서도 부"로 실측 종결**: -O3(본 절), 컴포지터 스토어 병합(§9.2). 8% 게이트는 측정 체계와 무관하게 부재 입증 유지. 유일 미결 실마리: 10.3 프로파일러 크래시 수리 후 DWT 사이클 귀속으로 남는 의문(예: OC 레벨×코드 레이아웃 상호작용) 추적 — 신규 과업 필요.

## 11. 재시도 금지 원장 (DO-NOT-RETRY) — 2026-09-14 시리즈 종결 시점

> 이 표의 근거는 전부 **실측**이다(해시-정확 A/B, 샌드위치 디바이스 측정, 또는 아키텍처 판명). 아래 경로를 다시 시도하는 것은 측정 장비를 다시 만드는 것보다 느리다. 재검토 유일 조건: **표의 전제를 깨는 변경**(예: 렌더 아키텍처 교체, 측정 정의 변경, 하드웨어 개정)이 별도로 들어온 경우뿐이다. GitHub 이슈 **#47**(2026-09-14자 종결 기록, not-planned)과 동일 내용.

| # | 금지 경로 | 시도 결과 | 봉쇄 근거 (절) |
|---|---|---|---|
| 1 | SH2 인터프리터 monolithic AOT 트레이스(루프 통째로 C 생성) | +4.4%(비활성) / **+20.8%(활성) 느림**, 해시는 정확 | §1 — 2966라인 함수 컴파일 품질, 커버리지 2.6% |
| 2 | SH2 fold-arm 확장(PCM 믹서/컴폴/SHAR 등 신규 arm) | 미구현 — 상한 입증 | §3 — 전 arm 합산 ≤7.9% < 8%, 단일 최고 ~2.5% |
| 3 | MD VDP draw 무변화 프레임 스킵/SAT 동일값 무효화 | 3개 수정 전부 해시-정확하나 **성능 0%** | §8 — 32x.c:943 compose-in-place 강제가 매 프레임 풀 재렌더 요구(구조적) |
| 4 | 컴포지터 스토어 병합/pair-수준 잔여 마이크로 | rig 판정 불가(0으로 측정됨) | §9.2 — rig 불가시 클래스, 풀 자체가 7.1%<8% |
| 5 | **빌드 플래그 -O3 (전역/ITCM-only 모든 스코프)** | rig −1.45%(명령수) → **디바이스 −2.2~−2.5% 느림(부호 반전)** | §10 — XIP +29KB 플래시/캐시 비용, 샌드위치 확정 |
| 6 | -Os, -O3 -funroll-loops | +9.2% / +0.13% 느림 | §9.3 |
| 7 | LTO | 미실시 — 링크 라인 구조상 기대치 <0.5% | §9.3 |
| 8 | 8% 게이트 자체(해시-정확 시맨틱 보존 전제) | **불가 입증** | §3+§8+§9+§10 — 모든 하위 시스템 상한의 합이 게이트에 못 미침 |

**예외적으로 열려 있는 길**(금지 아님, 신규 과업 필요): ① MD32X_DEVICE_PROFILE 실기기 크래시 수리(§10.3) 후 DWT 사이클 귀속 재판정 ② 측정 정의 밖 경로(빌드/메모리 레이아웃/하드웨어) ③ 렌더 아키텍처 교체(PDF_RGB555 compose-in-place 폐기) — 셋 다 본 시리즈 범위 밖.

## 12. 빈 타일 연속 탐색 축소 (2026-09-30) — rig 명령 수 −0.955%, 실기 미측정

사용자의 새 32X 성능 개선 요청으로, 8% 문턱을 전제로 하지 않는 작은 개선을 구현했다.
`external/picodrive/pico/draw.c`의 일반/인터레이스 `DrawStrip`에서 이미 빈 것으로
확인한 타일 코드가 8칸 연속되면 정렬된 32비트 비교 4개로 확인하고 함께 건너뛴다.
매 프레임 MD 레이어는 계속 렌더링한다. VRAM이나 화면을 캐시하지 않아 무효화 훅과
추가 RAM이 필요하지 않는다. 행 경계를 넘는 묶음, 그림자 모드의 고우선순위 타일,
강제 그리기, 열별 수직 스크롤은 원래 경로로 처리한다.

`GNW_32X_CORE`에서 적용하며 `GNW_MD_NO_BLANK_RUN`으로 컴파일에서 제외할 수 있다.
비32X 빌드는 수정 전후 전체 draw 객체가 바이트 단위로 일치했다.

### 비교 조건과 결과

현재 체크아웃 `8c533dca9ff18d3baf1e32709bbf1e5a9368bf73`, picodrive
`09298aca94159d5f91437a874c74c93bd76cce70`의 기존 dirty `videoport.c` 수정은
양쪽에 동일하게 보존했다. 컴파일러는 Arm GNU Toolchain 15.2.Rel1, `-O2`,
Cortex-M7 hard float. ROM SHA256은
`0bfa9a1a9341bca5b68f29e0e94e334d9d2f781f84d02d60f51a8be24f12f11c`.
외부 BIOS/세이브/패드 스크립트 없이 부팅부터 동일하게 실행했다.
`run_32x.sh`로 1,800프레임, `FRAME_HIST=1`, `-DRIG_WARMUP=1200`,
`-Wno-error=incompatible-pointer-types`; 양쪽 후기 600프레임을 비교했다.
위상 프로파일러는 사용하지 않았다. 과거 원장의 컴파일러/dirty 상태와 달라
이번 두 arm끼리만 비교한다.

| 항목 | A: 빈 타일 묶음 제외 | B: 빈 타일 묶음 적용 |
|---|---:|---:|
| 표본 수 | 600 | 600 |
| 평균 host 명령/프레임 | 10,817,002 | 10,713,689 |
| rig ELF text 바이트 | 3,994,088 | 3,994,280 |
| data / bss 증가 | — | 0 / 0 |

명령 비용 감소는 **0.9551%**(103,313 명령/프레임), 코드 증가는 **192바이트**.
양쪽 SH2 평균은 119,961, GATE3 PASS, 건너뛴 프레임은 0이다.
화면 f99=`9cd2510f`, f299=`22ee77a6`, f1799=`464dd41b`와 음원
`8cdfb973`(1,800 calls, 2,646,000 samples), energy/peak가 모두 일치했다.
`python3 tests/test_md32x_blank_run.py`는 실제 렌더러를 두 구성으로 컴파일해
스크롤/행 래핑/정렬/우선순위/그림자/인터레이스/강제 그리기 460,800조건의
픽셀·우선순위 목록·상태 플래그 **339,148,800바이트 전부 일치**를 확인했다.

실기 FPS, 다른 게임의 성능, A 재측정 드리프트는 미측정이다. 이는 전체 32X의
최대 성능이나 실기 속도 향상을 확정하는 수치가 아니다. 펌웨어 전체 링크/플래시는
수행하지 않았으며 Cortex-M7 rig의 양쪽 전체 컴파일/링크는 통과했다.

근거는 `build/32x-blank-run-20260930/`의 `inputs.json`, `reviewed-results.json`,
A/B 원본 로그, 빌드 로그, 차등검증 로그, dirty 패치에 보존했다(로컬 ignored 산출물).
`summarize.py`는 정상 히스토그램 줄 `skipped: n=0`의 SKIPPED를 실패로 오인해
거부했다. 원본/거부 결과는 그대로 보존했고, 위 수치는 완료된 로그의 결과 줄과
동일 서명을 직접 대조하여 산출했다.

## 13. 연속 빈 타일 가변 길이 처리 (2026-09-30) — 두 ROM rig 비용 약 −0.98%

추가 성능 개선 요청으로 §12의 고정 8칸 묶음을 가변 길이 처리로 교체했다.
한 번의 행 경계 계산 후 같은 빈 타일 코드를 8칸씩 확인하고, 남은 구간은
4칸·2칸씩 처리한다. 두 칸 이상 안전하게 확인한 구간만 함께 건너뛰며
클리핑/그림자 고우선순위/강제 그리기/열별 수직 스크롤은 기존 처리를 유지한다.
VRAM 쓰기 사이에 유지하는 캐시나 추가 RAM은 없다.

### 같은 조건의 비교

§12와 같은 체크아웃·컴파일러·기존 dirty 상태·`-O2`를 유지했다.
각 ROM은 외부 BIOS/세이브/패드 스크립트 없이 부팅부터 실행했으며 위상
프로파일러를 사용하지 않았다. 게임 이름만으로 실기 플레이 장면을 주장하지 않는다.

| ROM / 측정 프레임 / n | 원본 루프 | 고정 8칸 | 최종 가변 길이 | 원본 대비 비용 감소 |
|---|---:|---:|---:|---:|
| Doom / 1200–1799 / 600 | 10,817,002 | 10,713,689 | 10,711,125 | 0.9788% |
| After Burner / 200–599 / 400 | 22,004,989 | 21,838,330 | 21,788,677 | 0.9830% |

수치는 QEMU 평균 host 명령/프레임이며 FPS나 실기 사이클이 아니다.
고정 8칸 대비 추가 절감은 Doom **0.0239%**, After Burner **0.2274%**다.
Doom 고정 8칸 기준은 이번 실험에서 재빌드 후 평균 10,713,689가 재현되었다.
원본 A→B→A의 마지막 A는 측정하지 않아 드리프트/통계적 유의성은 확정하지 않는다.
After Burner ROM SHA256:
`5225e4a73a893bd40579b6fedd6f50fe294b06be23b8ab928024498ed6f633f3`.
Doom ROM 해시는 §12와 동일하다. 빌드/ELF 해시와 입력은 산출물의 `inputs.json`에 기록했다.

### 정확성과 크기

두 ROM의 모든 A/B arm은 GATE3 PASS, 건너뛴 프레임 0, 같은 SH2 평균을 보였다.
Doom의 화면·오디오·energy 서명은 §12와 전부 같았다. After Burner는
화면 f99=`c5aff44d`, f299=`650a9438`, f599=`393ea6e6`, 오디오 `60c1049b`,
600 calls / 882,000 samples, sum|s|=396656585, sum s²=1765316036377,
peak=28616이 모두 일치했다. 화면 서명은 주기적 검사이며 모든 프레임의 비교는 아니다.

실제 `draw.c` 차등검증을 팔레트/가로·세로 반전/우선순위 혼합/숨겨진 열 조건까지
확장했다. **552,960조건의 406,978,560 출력 바이트**(픽셀, 우선순위 목록,
상태 플래그)가 전부 일치했다. 비32X 구성의 수정 전후 draw 객체도
**43,840바이트 전부 일치**했다.
Doom rig ELF text는 원본 3,994,088 → 최종 3,994,664바이트(**+576바이트**),
data 2,252 / bss 9,977,724는 그대로다. ROM이 text에 포함된 같은 입력끼리 비교했다.

### 조사한 캐시 후보와 반영 범위

행 캐시도 별도로 측정했다. 기준은 고정 8칸의 Doom 10,713,689였다.

| 후보 | Doom 평균 host 명령/프레임 | 상태 |
|---|---:|---|
| 동일 타일 코드 행 캐시 / 16바이트 | 10,675,193 | 추가 무효화 경로가 필요; 최종 코드에서 제거 |
| 표시 구간만 검사 / 팔레트·가로 반전 무시 / 24바이트 | 10,684,701 | 첫 캐시보다 느림; 제거 |
| 혼합 우선순위·세로 반전 처리 / 24바이트 | 10,693,105 | 첫 캐시보다 느림; 제거 |
| 패턴 의존 페이지 비트맵까지 유지 / 48바이트 | 10,726,742 | 고정 8칸보다 느림; 제거 |

단순 캐시는 Doom rig에서 추가 이점이 있었으나 VRAM 쓰기, DMA, 초기화,
세이브 복원과 프레임 시작의 수명 관리가 필요했다. 그 이점의 실기 검증은 없다.
이번 반영은 추가 상태 없이 두 ROM 모두 개선한 연속 구간 처리다.
제거한 후보들의 로그·패치도 보존하여 동일 실험을 다시 추측으로 시작하지 않게 했다.

실기 FPS·화면·음원은 미측정이며 최대 성능을 입증한 결과는 아니다.
근거는 `build/32x-aggressive-20260930/`의 입력/결과 JSON, 원본 로그, 빌드 로그,
차등검증 로그, 기존·최종·제거 후보 패치에 보존했다. 요약기의 정상
`skipped: n=0` 오인 거부 결과도 보존하고 완료된 결과 줄과 서명을 직접 대조했다.

### 전체 펌웨어 링크 검증

별도 복사 작업트리에서 canonical release 옵션으로 `build/gw_retro_go.elf`를
전체 빌드했다. 종료 코드 0, 33개 코어의 교차 overlay 심볼 충돌 없음,
63개 blob 로고 인덱스 정렬, resident 초기화 배열과 70개 staged 로고 참조
검사가 모두 통과했다. 링크 메모리 배치와 ELF 해시는
`build/32x-aggressive-20260930/firmware-validation.json`에 기록했다.
원래 작업트리의 펌웨어 산출물은 덮어쓰지 않았으며 실기에 플래시하지 않았다.
검증 ELF는 `/home/pi/app/jupyterLab/notebooks/gnw-32x-validation-20260930/build/gw_retro_go.elf`에 있다.

## 14. 실기 완료 이벤트 측정기와 반복 스킬 준비 (2026-09-30)

사용자가 실기 전환과 컨텍스트를 아끼는 끈질긴 반복 스킬을 요청했다.
`skills/gnw-32x-device-loop/SKILL.md`를 만들고 AGENTS.md에 프로젝트 라우팅을 추가했다.
기존 gnw-ab-bench의 case ID/중복 거부/첫 실패 종료/입력 재해시 batch를 재사용한다.
새 실행기는 전체 device lock을 보유하고 intflash/core/XIP readback 후 한 번만 boot한다.
설치된 gnwmanager의 기존 OpenOCD 자동 종료는 로컬 shim에서 거절로 바꾸었다.

측정용 `GNW_32X_DEVICE_BENCH_FRAMES` 빌드는 동일한 완료 프레임 경계에서
warmup/window의 시간·emu/drawn/audio 카운터를 저장하고 resident flash marker로 들어간다.
모델/실행기가 frame counter를 반복 읽지 않고 GDB completion stop을 기다린다.
첫 live smoke는 시작 marker도 기다려 HAL_GetTick과 호스트 monotonic 시간의 구간을 대조한다.
cache clean/metadata/dump는 측정 종료 뒤에 한다. 추가 hook 비용은 양쪽 FPS에 포함된다.
측정 후 정지 arm은 배포판으로 채택하지 않는다.

호스트 7개 테스트(정확한 frame 종료/0 warmup/timer wrap/잘못된 구간·clock·mute·입력·
바이너리·dump·drift 거부/미검증 batch 거부/기존 debugger 보호)가 통과했다.
계측 header의 Cortex-M7 freestanding Clang 컴파일과 스킬 validator도 통과했다.
**완전한 계측 펌웨어 GCC 빌드와 실기 smoke/FPS는 미실행이다.**

초기 ST-Link 연결과 debugger 미사용을 확인한 뒤 bank2 262144바이트와 32x.bin,
32x.xip, off.sav를 백업했다. off.sav는 678925바이트로 비어 있지 않다.
이후 세션 권한이 제한되면서 Docker daemon socket과 gnwmanager debugger TCP socket의
접근이 거부되었다. 새 flash/reset은 하지 않았고 오류 뒤 한 번 확인한 OpenOCD 잔여는 없다.
예약은 해제했다. 준비/백업 hash, 오류, 검증, compact `state.json`, 실행을 차단한
`plan.pending.json`은 `build/32x-device-20260930/`에 있다. 상태 파일부터 재개하고,
실제 selector/ROM/BIOS/소비 save의 신원 및 live collector를 확인하기 전 batch를 켜지 않는다.

## 15. 실기 A/B/A 완료 (2026-09-30) — 고정 Doom 플레이 장면 +2.50%

실기 완료 이벤트 측정기를 검증하고 동일한 게임 진입 입력으로 원본(A)과 가변 빈 타일 처리(B)를 세 묶음 비교했다. 입력 없는 콜드 부팅 및 초기 입력 시퀀스는 메뉴에 남았으므로 플레이 FPS에서 제외했다. B 버튼을 loop 완료 프레임 600–607, 720–727, 840–847에 적용했을 때 메뉴 없는 레벨 1 화면을 확인했다. warmup=900 뒤 window=600, 340 MHz, fullscreen=0, tear_guard=0, 외부 BIOS 없음, 세이브 미소비, 측정 구간 입력 없음이다.

| 묶음 | A 앞 FPS | B FPS | A 뒤 FPS | 향상 | A 드리프트 |
|---|---:|---:|---:|---:|---:|
| 1 | 28.79770 | 29.58872 | 28.84615 | +2.66045% | +0.16827% |
| 2 | 28.79355 | 29.50433 | 28.80599 | +2.44640% | +0.04321% |
| 3 | 28.86003 | 29.51884 | 28.79770 | +2.39337% | −0.21598% |

A 6개 평균 28.81685 → B 3개 평균 29.53730 FPS: **+2.50007%**. 세 묶음 모두 B가 양쪽 A보다 빠르고 상승폭이 해당 드리프트보다 컸다. 사전 제한 0.5% 안에서 최대 절대 드리프트는 0.21598%였다. 9회 모두 600 emu/600 drawn, audio 600 calls/441000 samples, mute=0/input=0, 마지막 표시 loop=1500 및 화면·마지막 synthesized audio 해시가 일치했다. 전체 화면/음원 스트림 비교나 모든 게임에서의 최대 성능 입증은 아니다.

첫 timer smoke의 HAL 61260 ms와 호스트 61259.141 ms가 일치했다. 초기 GDB dump 명령의 따옴표 파일명 오류는 read_memory/Python 저장으로 고쳤으며 이미 완료된 기록을 재부팅 없이 회수했다. 반복 실행은 전체 기기 lock 아래 A→B→A→검증으로 이어지고 모델이 단계별 상태를 조회하지 않았다. case 입력은 마지막에 재해시했다.

근거: `build/32x-device-20260930/gameplay-summary.json`, `gameplay-integrity.json`, `jobs-gameplay/job-e5_633yt/result.json`, `plan.gameplay.json`, `arms-gameplay4/`. 고정 8칸의 실기 비교 arm은 `arms-fixed8/candidate/`에 준비했다(XIP가 가변 처리보다 352바이트 작음). 사용자의 시간·토큰 우려에 따라 추가 비교는 실행하지 않았다. 단순 16바이트 행 캐시 및 부하가 다른 장면도 다음 후보로 남겼다.

개선 소스와 정상 release 산출물은 보존했다. 기기에는 기존 bank2/core/XIP/선택 파일을 복원하고 설정·전원 종료 세이브 readback을 확인한다. 다른 설치 코어와 resident의 조합을 보존하기 위해 측정용 펌웨어를 정상 배포판으로 남기지 않는다. 복원 완료 증거는 `restoration/result.json`이다.

## 16. 고정 8칸 후보 기각과 가벼운 실기 구간 계측 (2026-09-30)

목표는 NTSC 60 emu/60 drawn FPS이며 아직 달성하지 못했다. 같은 Doom
레벨 1 정지 장면, warmup=900/window=600, 동일 3회 B 입력과 340MHz를 유지했다.

### 고정 8칸 대 가변 처리

3회 A/B/A 실행이 모두 완료됐으나 마지막 검증이 기준 드리프트로 실패했다.
유효 두 triplet에서 고정 8칸은 각각 −1.13991%, −0.79396% 느렸다.
세 번째는 A 전후 드리프트 0.66964%가 사전 한계 0.5%를 넘어 제외했다.
한계를 완화하거나 자동 재시도하지 않았다. 가변 처리를 현재 최선으로 유지한다.

근거: `build/32x-device-20260930/fixed8-summary.json`,
`jobs-fixed8/job-p0lpm4f2/result.json`.

### 명령 단위 프로브 없는 별도 진단 arm

GNW_32X_LIGHT_PROFILE 빌드는 기존 pprof의 CPU/렌더/음원 구간 합계만 DWT로
측정한다. MD32X_DEVICE_PROFILE은 꺼져 있고 SH2 명령별 카운터/PC-wall/fetch/data
프로브와 AHB percentile pool을 사용하지 않는다. 시작 시 DWT를 free-running으로
활성화하며, 완료 프레임 900에서 합계를 초기화한다. 다음 600프레임만 합산하고
동일 resident 완료 marker 뒤 GDB가 한 번에 raw 합계를 읽는다.
이 진단 arm의 FPS는 배포 후보 성능으로 사용하지 않는다.

완료 600 emu/600 drawn, 441000 audio samples, input=0, mute=0,
last_drawn_frame=1500, 기존 endpoint 화면/음원 해시 일치, 열린 bracket=0.

| 구간 | 계측된 전체 loop 대비 |
|---|---:|
| SH2 실행 두 구간 합계 | 57.89796% |
| 68000 | 16.51197% |
| MD 렌더 | 7.52426% |
| 32X 합성 | 5.65868% |
| FM | 4.15687% |
| Z80 | 3.12342% |

frame/PicoFrame은 자식 구간을 포함하므로 위 구간에 다시 더하지 않는다.
두 SH2 구간은 중첩된 다른 SH2 실행을 호출한 쪽에 귀속할 수 있으므로
개별 master/slave 비율보다 합계가 유효하다. 소스 행 단위 hotspot이나
무계측 시간 비중, 모든 게임 장면의 성능을 증명하는 숫자는 아니다.

근거: `light-diagnostic/result.json`, `light-diagnostic/interpretation.json`,
`light-diagnostic/run/capture/{light_pp,gnw_md32x_light_result}.bin`.
진단 source 변경은 private 빌드와 `arms-light/diagnostic/modified`에만 있다.
루트의 가변 처리 배포 소스에는 프로파일러를 넣지 않았다.
재현 빌더는 `build-light.py`, 수집기는 `tools/32x_light_diagnostic/`에 보존했다.
전체 33 core 링크, 63 logo, resident init-array/70 logo reference 게이트를 통과했다.

다음 큰 축은 SH2 인터프리터/메모리 버스다. 현재 정지 장면에서 확인한
29.53730fps를 60fps로 올리려면 약 50.77% 프레임 시간 단축이 필요하다.
빈 타일 미세 최적화만으로 목표 달성을 주장하지 않는다.
기존 -O3 전체/ITCM 시도의 실기 악화 근거를 재사용하며 무작정 반복하지 않는다.


## 17. PC 통계 진단과 SH2 정적 분기 표 (2026-09-30)

명령마다 프로브를 넣는 대신 기존 1kHz SysTick의 interrupted PC를
64바이트 블록으로 샘플링하는 별도 진단 arm을 사용했다. C prologue 전에
MSP/PSP를 선택하며 기본/FP 확장 exception frame의 PC를 모두 수집한다.
FP context를 제외한 첫 arm과 hash 충돌로 9.15%를 놓친 두 번째 arm은 무효로
보존했다. 세 번째 arm은 20221 samples, dropped=0으로 완료됐고 기존 정지
장면의 프레임/입력/설정 및 endpoint 화면·음원이 일치했다.
XIP 주소는 함수 상수의 virtual sentinel 대신 실제 g_xip_addr/offset으로 변환했다.
이미 완료된 arm에서 주소만 한 번 회수했으며 새로운 실행 샘플로 세지 않았다.

함수에 모호함 없이 귀속된 블록 중 SH2 interpreter는 전체 샘플의 40.73488%였다.
이 수치는 fetch/dispatch/inline opcode/tail을 포함하며 분기만의 비용이 아니다.
고정 1kHz sampling의 alias 가능성이 있고 함수 경계 블록은 mixed로 남겼다.
68000에서는 새롭게 안전하게 생략할 polling loop를 확정하지 못했다.
근거: pc-diagnostic3/symbolized.json, pc-diagnostic3/meter-notes.json.

실제 SH2 disassembly에서 top-nibble switch의 range check를 확인했다.
GNW_32X_CORE에서만 16-entry static const label table을 ITCM에 두는 후보를
만들었다. 과거 slice마다 stack으로 표를 복사했던 computed goto와 다르다.
16개 opcode body, delay slot, fastloop gate와 IRQ tail은 보존했다.
전체 33 core 링크 및 logo/resident reference 게이트를 통과했다.

정지 Doom 레벨 1, 900 warmup/600 측정 프레임, 340MHz에서 현재 가변 빈 타일
기준과 A→B→A 세 묶음을 비교했다.

| 묶음 | A 앞 FPS | B FPS | A 뒤 FPS | 향상 | A 드리프트 |
|---|---:|---:|---:|---:|---:|
| 1 | 29.60770 | 29.69709 | 29.50143 | +0.48225% | −0.35893% |
| 2 | 29.57413 | 29.76043 | 29.52030 | +0.72161% | −0.18204% |
| 3 | 29.56102 | 29.70591 | 29.60624 | +0.41335% | +0.15297% |

A 평균 29.56180 → B 평균 29.72114 FPS, +0.53901%.
각 묶음에서 양쪽 A보다 빨랐고 상승폭은 해당 드리프트보다 컸다.
모든 endpoint 화면/마지막 synthesized audio와 emu/drawn/audio/input/clock/settings가
일치했다. 전체 스트림 정확성이나 모든 게임의 성능을 증명하지 않는다.
근거: static-dispatch-summary.json, jobs-static-dispatch/job-ukp4m0ws/result.json.

16바이트 name-row cache 후보는 동일 이름 descriptor의 행만 증명해 재사용하고
pattern pixel은 매번 읽는다. CPU word/byte write, DMA, reset 및 state load에
무효화를 넣었다. 가변 처리 기준과 3317760 host cases/2441871360 output bytes가
일치했고 전체 core 빌드 게이트를 통과했다. 정지 장면 실기 A→B→A는 모두 정상
완료했으나 A 드리프트 0.70423%가 사전 한계 0.5%를 넘어 무효다.
한계를 완화하거나 재실행하지 않았다. 근거: test-rowcache.log,
jobs-rowcache/job-k9qnv7sx/result.json. 후보 채택은 별도 이동 검증 결과를 따른다.


## 18. 이동 검증 마무리와 범용 구조 변경 방향 (2026-09-30)

이동 측정기는 version2/300-frame u16 input trace와 FNV를 사용했다.
UP+A(65)를 900–1199 completed loop에 넣었고 매 프레임 입력이 같음을 확인했다.
smoke HAL 8836ms/host 8909.566641ms가 사전100ms 허용치 안에 들었다.
최종 화면은 pistol/HUD가 있는 닫힌 문 앞의 실제 플레이 장면이다. 전투/발사
검증으로 부르지 않는다. 양쪽 동일 hook과 설정이며 입력 trace 및 endpoint
화면·음원/프레임·audio counts가 비교에 포함된다.

SH2 정적 분기 표 이동 A/B/A: 33.87916 / 33.79520 / 33.89831 FPS,
−0.27600%, A 드리프트 +0.05650%. 정지 장면의 +0.54%와 부호가 달라
범용 배포 후보로 채택하지 않았다. 행 캐시 이동 A/B/A는 완료됐으나
A 드리프트 −2.12530%로 무효다. 한계 완화/자동 재실행 없이 이전 가변 빈 타일
개선을 유지했다. 아직 NTSC60 emu/60 drawn FPS를 달성하지 못했다.

원래 bank2/core/XIP/selector를 복원했다. 이 네 항목 및 CONFIG와 /data/off.sav
총6개 readback이 백업 hash와 일치했고 bank2 start가 성공했다.
측정/진단 arm이나 새 resident와 일부 코어만 섞은 firmware는 배포하지 않았다.
근거: final-candidates-summary.json, restoration-final/result.json.

사용자는 GBA m4a처럼 불필요한 guest 작업을 제거/네이티브화하되 After Burner도
얻는 범용 개선을 요청했다. Makefile의 MD32X_SSH2_SND_HLE는 기본0이다.
Doom sound-service HLE 코드는 존재하지만 기본 활성 상태가 아니며, Makefile에
보존된 과거 실기 결과는 23.97→24.02fps(약0.2%)다. rig9.64% 감소를 실기
기대값으로 쓰지 않는다. 현재 texture/span fold도 메모리 helper/guest op를
계속 호출하므로 모든 고수준 작업의 직접 네이티브 처리와 동일하지 않다.

범용 큰 후보는 SH2 hot-block의 Thumb 실행기다. 기존 compiler.c는 ARM에서
emit_arm.c의 A32 encoding을 선택하므로 M7에 DRC 플래그만 켜서 실행할 수 없다.
현재 private moving-rowcache map은 RAM_EMU 끝0x24100000, BSS 끝0x240fdbf8로
정적 여유9224bytes다. 이것은 runtime heap/stack까지 보증한 가용 code-cache가
아니다. 먼저 실제 normal build의 ITCM/AXI/DTCM/AHB 배정과 runtime 사용량을
확인하고 작은 cache의 hit/eviction 비용을 측정해야 한다.

첫 후속 측정은 After Burner와 Doom의 실제 이동 작업량에서 guest-PC와 basic
block에 wall samples를 귀속한다. 기존 host-PC sampler는 guest 함수의 범위를
증명하지 못한다. 표본은 각 SH2 실행 시점의 active core/guestPC를 기록하는
별도 진단 arm에서 얻으며 nested SH2 실행과 Handler mode를 구분한다.
그 다음 관측된 공통 opcode의 짧은 블록만 변환한다. 처음에는 MMIO/미지원명령/
분기-delay/IRQ 경계를 interpreter로 돌려보내고 동일 cycle budget과 state를
차등검증한다. SDRAM 실행 코드 쓰기/DMA/save-load 무효화와 M7 cache 동기화도
구현 gate다. 호출별 overhead와 기존 fold 손실을 포함해서 판단한다.
모든 타이틀의 속도 상승이나 60fps 달성을 지금 보장할 근거는 없다.


## 19. 범용 구조 변경의 재검토 — 기기 없이 조사 (2026-09-30)

기기는 사용자 지시에 따라 Sega CD가 사용한다. 이번 조사는 로컬 소스/완료 기록/
link map 및 외부 원저자 자료만 읽었다. SWD/flash/reset/SD read/기기 lock은 사용하지 않았다.

**중요한 정정:** §18의 작은 Thumb code-cache 제안은 이미 수행한 DRC 재검토를
충분히 반영하지 못했다. `docs/32X_CLOSED.md:344`의 2026-08-27 기록은
540-frame attract-class 임시 작업량에서 핫872 blocks/14998 guest insns,
번역117–234KiB 대 실행예산21KiB, 재번역167–592회/frame을 기록한다.
새 캐시 정책만으로 이를 해결했다고 주장하지 않는다. 현재 After Burner 플레이의
작업량과 같다고 가정할 수도 없다. 이를 뒤집으려면 저장 위치/실행 구조/작업량
전제가 실제로 바뀌어야 한다.

현재 private moving-rowcache map의 read-only 감사는 AXI 정적 여유9224bytes,
ITCM 코드31084bytes+데이터31304bytes, ITCM 정적 여유3144bytes를 확인했다.
DTCM heap90044bytes와 AHB heap87904bytes는 runtime allocation **전** 용량이다.
약83976bytes의 Draw2FB와 SH2 context12032bytes 등의 할당을 무시하고 code-cache
가용량으로 쓰지 않는다. 과거 DTCM 실행 probe와 후속21KiB 모델의 차이는 각
당시 빌드/예산에 속하므로 현재 map만으로 실행 권한이나 runtime free를 확정하지 않는다.
감사 도구 `tools/32x_research/memory_audit.py`는 map만 읽고 기기에 연결하지 않는다.

남는 후보는 다음 셋이다. 전부 미구현/미측정 가설이며 현재 FPS 개선이 아니다.

1. **고정 레지스터 배치의 Thumb 인터프리터:** 번역 캐시 없이 기존 실행기를
   교체한다. pc/cycles/context 등의 hot state와 opcode별 코드 생성을 직접 통제한다.
   기존52-host-insn scaffolding 추정은 후속 최적화 전 수치이므로 현재 ELF에서 다시
   계산해야 한다. 함수 호출/상태 저장/메모리 fallback 및 delay/IRQ를 포함해 비용을
   증명한 작은 opcode 계열부터 시작한다. 전체 재작성의 효과를 선결론내지 않는다.
2. **PC에서 생성한 짧은 Thumb native block을 XIP sidecar로 저장:** 동일 번역기가
   여러 ROM을 처리하고 game-specific engine port는 만들지 않는다. RAM cache와
   매-frame 재번역 전제를 피한다. 오래된 opXXXX 재호출 AOT 대신 직접 계산과
   live register 배치를 생성한다. ROM/core ABI identity, relocation, map 변경,
   RAM code fallback 및 XIP miss 비용은 새 gate다. After Burner의 ROM 실행 coverage는
   아직 모른다. Sega CD가 작업 중인 공용 flash allocator는 수정하지 않았다.
3. **PWM FIFO 상태 전이까지 정확한 poll collapse:** 원래 sound service는 실행하고
   바뀌지 않는 FIFO 대기만 건너뛴다. 기존 무조건 sleep과 달리 IRQ/FIFO/다른 CPU/DMA
   중 가장 이른 변화에서 멈춰 마지막 read/state/cycles를 재현한다. 원장의 아직
   미시도 shape이며 현재 타이틀별 지분은 미측정이다. 화면만 아니라 PWM write 시각과
   audio sample도 비교해야 한다. 큰 범용 이득이나60fps를 보장할 수 없다.

레이어 분리는 사용자가 이미 시도했다고 확인했으므로 새 게임 실험으로 반복하지
않았다. 이번에 작성한 작은 synthetic host harness는 frame의 row/palette setup이
불완전해 출력 불일치로 종료됐다. **후보의 정확성/성능 실패로 세지 않는다.**
원래 실험 근거를 찾기 전에는 이 경로를 새 후보로 내세우지 않는다.

외부 구현도 바로 사용할 M7 backend로 판정하지 않았다. [ST Cortex-M7 소개](https://www.st.com/content/st_com/en/arm-32-bit-microcontrollers/arm-cortex-m7.html)는
Thumb/Thumb-2와TCM/cache 기능의 근거다. [Yabause 원저자 설계](https://wiki.yabause.org/index.php5?title=SH2_dynamic_recompiler)는
register allocation/cycle check/invalidation 구조의 참고이며 desktop cache 예산은
본 기기와 다르다. [PSP SH2 원소스](https://raw.githubusercontent.com/PSP-Archive/Yabause-PSP/main/src/psp/sh2.c)는
MIPS 실행기여서 Thumb backend를 별도로 만들어야 한다. 이 자료들의 속도를 본
기기의 FPS로 환산하지 않았다. 상세 후보/gate/제외 항목: `build/32x-research-20260930/research.json`.


## 20. 첫 사전번역 프로토타입 — 호스트 검증 완료 (2026-09-30)

사용자의 구현 진행 지시에 따라 `tools/32x_research/native_blocks.py`에서 SH2의
1-cycle register-only opcode를 해독해 짧은 독립 C 함수를 생성하고 M7 Thumb로
컴파일했다. 고정 register/immediate 계산을 직접 생성하며 opXXXX 핸들러를
호출하지 않는다. 현재 root emulator/Makefile/linker/flash allocator는 변경하지 않았다.
기기와 공유 private firmware build workspace에도 접근/빌드/flash하지 않았다.
산출물은 repository build/에 저장했다.

opcode family/alias와 결정론 mixed block, After Burner/Doom 파일의 static ALU
후보를 합쳐291 blocks를 만들었다. ROM 후보는 코드/data 구별이나 runtime 실행을
증명한 주소가 아니다. synthetic ROM으로 옮겨 개별 명령 의미만 검증했다.
실제 현재 interpreter를 reference로 쓰되 ALU proof에서 버스/IRQ가 호출되면
즉시 실패하는 stub을 사용했다. 실제 memory/branch/IRQ correctness는 미검증이다.

첫 실행은 DT의 no_polling 변경 누락으로 실패했다. R/T만 비교했다면 놓칠
부분이며 전체6016-byte SH2 context 비교로 발견했다. 이 로그/실패JSON을 보존하고
코드를 수정했다. 수정본은13968 complete-state comparisons,2895 unchanged
entry declines를 통과했다. delay/test_irq/wrong PC/부족하거나 음수인 budget에서는
상태를 바꾸지 않고 거절한다. 실제 fallback caller 통합은 아직 하지 않았다.

ARM GNU15.2/O2 Cortex-M7 hard-float, QEMU mps2-an500/icount에서6개 isolated
microbench를 실행했다. synthetic4/8/16/32 guest-op 사례의 instruction cost 감소는
각각72.22/87.26/93.66/95.72%였다. ROM 유래32-op 예시는97.23/94.80%였지만
해당 코드의 실행/핫 여부를 확인하지 않았으므로 게임 이득으로 보고하지 않는다.
반복 실행의 시작pc 설정/호출/entry guard가 양쪽 비용에 포함돼 있다.
QEMU는 flash/cache latency를 반영하지 않고 memory/branch/workload coverage도
없으므로 실기FPS/전체frame 절감이나60fps 달성으로 환산할 수 없다.

독립 native291 functions의 text 합계23112bytes는227개 single-op proof 함수까지
포함한 시험용 크기다. 게임의 번역 working set 또는 제안 resident cache 크기가
아니다. 측정된 짧은 함수는84–156bytes였다. 실제 disassembly에서 함수 호출과
외부 함수로의 tail branch가0임을 확인해 이전 handler-replay AOT와의 차이를 검증했다.

근거: `build/32x-native-proof-20260930-v2/{result,code-audit,inputs,source-manifest}.json`,
generated.h/proof.elf/proof.map/disassembly.txt/qemu.log/source/.
처음 실패는 `build/32x-native-proof-20260930/failure.json`이다.
재현 방법과 한계는 `tools/32x_research/README.md`에 있다.
다음 gate는 memory/branch 경계와 actual After Burner/Doom 실행 coverage다.
XIP sidecar registry/loader/ROM-core identity/invalidation은 미구현이며,
기기는 Sega CD 사용 중이므로 실기 측정은 하지 않았다.


## 21. After Burner 실제 초기 비행 경로와 M7 native ALU 비용 (2026-09-30)

기기는 Sega CD에 예약된 상태로 접속하지 않았다. 기존 `tools/pico_host`로
입력을 찾아 기존 `tools/m7_qemu_rig`에 재생했다. 단일 START 시점을 추측한
앞선 census는 타이틀 화면으로 확인됐으며 gameplay/play라는 디렉터리 이름에도
플레이 근거로 쓰지 않는다. 초반 0–119프레임에서 START를 6프레임 눌렀다
6프레임 떼고 이후 무입력으로 두는 순서는 양쪽에서 Stage 1 비행에 진입했다.
앞선 콜드 부팅 반복은 낭비였으므로 이후 입력 검색은 먼저 빠른 host에서 한다.

근거: `build/32x-ab-verified-entry-trace-20260930/summary.json`.
540–719프레임에서 67,369,293 dispatched SH2 instructions, drop 0,
4,707 distinct dynamic dispatch runs. Timeslice/PC discontinuity/delay/32-op
경계로 나눈 run이며 canonical basic block 수가 아니다. 기존 fastloop가 접은
명령은 제외된다. 모든 관측 PC는 SDRAM이고 같은 PC의 opcode 변경은 없었다.
ROM 바이트와 일치하는 길이 4 이상 RAM run은 dispatched instruction의
88.53%다. 복사 출처나 향후 불변성 증명은 아니다.

현재 ALU subset은 dispatched instruction의 46.78%지만, delay slot을 제외한
2명령 이상 연속 구간은 17.27%, 상위 32구간은 16.06%만 덮는다. 따라서 작은
ALU 이득을 전체 게임 gain으로 환산하지 않는다. `0x060038ee`의 read/test/DT/
branch 5-op loop는 명령 수의 15.73%; 읽는 실주소와 wake deadline은 미확인이다.
`0x060069xx`와 `0x060068xx`의 반복 load/store + arithmetic run은 범용 memory
translation의 구체적인 다음 대상이다. 무조건 poll skip을 적용하지 않는다.

`build/32x-ab-flight-native-20260930/result.json`: 상위 32 ALU run을 PC relocation과
synthetic registers로 기존 M7 Thumb/-O2/icount meter에서 비교했다. RAM run은
ROM fast fetch 대신 실제 SDRAM fetch 분기를 사용한다. 격리된 비용 감소
57.90–86.25%, 전체 proof 291 blocks / 13,968 full-context comparisons /
1,963 unchanged declines / generated function calls 0. 함수 호출 및 guard는
포함하지만 실시간 game lookup, IRQ delivery, memory opcode, invalidation,
실기 cache/XIP stall은 미포함이다. 전체 게임 FPS나 실기 gain이 아니다.

Census는 경로 수집을 빠르게 하기 위해 icount 없이 실행하므로 해당 로그의
host-insn/time 필드는 모두 무효다. 성능 숫자는 별도의 icount ALU proof만 쓴다.
539/719 PNG는 항공모함 출발과 초기 비행 HUD(score 0)를 확인했으며 전투나
후반 stage가 아니다. rig가 RGB555를 선택하므로 최종 PNG는 기존 host RGB555
decoder로 만들었다(legacy RLE decoder는 RGB565로 해석한다).

다음은 뜨거운 memory run의 실제 주소/side effect와 poll dependency 수집,
범용 memory/branch native proof, 게임 연결 순서다. 새 산출물은 모두 build/에
저장했고 Sega CD 코드/flash allocator/기기 상태를 변경하지 않았다.


## 22. 범용 countdown wait fold: 전체 게임 M7 비용 -12.7402% (2026-09-30)

현재 실제 비행 구간의 주소를 계측한 결과, 뜨거운 0x060068xx/0x060069xx는
SDRAM 픽셀 읽기와 framebuffer overwrite 쓰기다. 이 모드는 source byte 0을
기존 destination byte로 보존한다. 대기 loop는 RAM 0x06003af1 플래그를
2,138,940번 읽으며 0은 2,138,850번, nonzero1/2/3은 각30번이었다. counter는
1,500,000으로 초기화된다. Host store 추적: producer PC0x06003776, clear
PC0x06003946. Producer 경로는 PC0x0600373a의 RTE로 복귀하지만 exact IRQ
source는 아직 미확정이다. 원본과 추가 계측의 SH2 dispatch count, screen719
전체 byte, sparse frame checksum, complete sound signature가 모두 같았다.

`tools/32x_research/countdown_fold.h`는 game/PC가 아닌 MOV.B/TST/BF/DT/BF
명령 패턴을 인식한다. 주소가 고정된 SDRAM zero-flag 반복만 현재 interpreter
budget 안에서 7-cycle iteration으로 계산한다. Delay/IRQ-test, counter 종료,
R0/address/counter alias는 fallback한다. PC/ppc/ea/T/no_polling 및 counter와
cycles를 보존하며 IRQ 처리 경계를 넘어가지 않는다. 루트 normal core나
firmware는 수정하지 않고 기존 M7 rig의 복사본에만 넣었다.

최종 safe 후보: `build/32x-ab-idle-fold-safe-20260930/result.json`.
기준: `build/32x-ab-idle-baseline-20260930/result.json`.
통합: `build/32x-countdown-proof-20260930/full-game-summary.json`.
초기 alias guard 전 candidate는 preliminary로 표시하고 최종 판단은 safe 결과만 쓴다.

측정 구간은 After Burner Stage1 flight frames540–719, input0; boot/entry
sequence 및 ROM과 동일한 source snapshot이며 변경은 sh2pico.c뿐이다.
Observer 없이 기존 M7 Thumb/-O2/icount meter를 사용했다.
전체 frame host instruction: 25,732,614 -> 22,454,234 (-12.7402%).
SH2 dispatch/frame: 374,273 -> 316,207 (-15.5143%).
전체720프레임에서 candidate는 125,359호출로 7,765,537 iteration을 접었다.
양쪽 screen539/719 전체 bytes, sparse checksum, complete sound hash/energy,
720calls/1,058,400samples가 모두 같고 GATE3 PASS다. 실기 FPS는 미측정이다.

별도 `countdown_proof.c`는 실제 interpreter + exact GNW SDRAM byte fastpath
fixture로 19,872 complete6016-byte context comparisons, folded1512 cases,
unchanged decline8을 통과했다. Isolated proof에서 다른 bus/IRQ는 trap이며
실제 IRQ delivery/bus를 쓰는 전체 game run으로 보완했다. 다른 title에서는
fold hit 없이 guard overhead만 생길 수 있으므로 generic adoption 전에 matched
Doom 비교가 남는다. 다음은 guarded SDRAM read + framebuffer overwrite native
blocks다. 기기에는 접속하지 않았다.

## 23. Offline native spans and wide multiplication (2026-09-30)

After Burner early Stage1 flight, matched frames540–719: generic SDRAM byte
read/framebuffer overwrite spans (up to32 guest operations) plus native wide
products lower total M7 icount cost from25,732,614 to21,175,016/frame,
**17.7114%**, including the previous countdown fold. Pixel adds4.7171% over
countdown; wide products add1.0285% over pixels. This is not hardware FPS.

Proofs: 2,048 complete game-context/touched-word shadow comparisons through
actual interpreter and bus;12 unchanged pixel declines;49,152 complete math
contexts with original handlers, SDRAM bus, signed edges, alias registers and
saturation states. Proof-free replay matches both complete endpoint screens,
sparse frame signatures and entire audio signatures. Only build snapshots
change; SegaCD retains the device. Evidence and reproducible inputs:
`build/32x-ab-native-combined-perf-20260930/summary.json`.

50% whole-frame cost reduction remains unmet. The residual needs39.2383%
further reduction. Frequent201f is MULS.W (not MAC.L); the next structural
candidate is whole arithmetic/control loops with their memory and branches.
Other titles/combat/hardware remain pending.

## Generic native control, PCM arithmetic and ROM selection (2026-10-01)

Completed evidence: `build/32x-native-final-20261001/summary.json`.
Same ROM/input/frame window, no frameskip, existing Cortex-M7 Thumb/O2
QEMU icount rig; all new artifacts reside on disk under build/.

| Measured scene | Original instructions/frame | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| After Burner early flight, frames540–719 | 25,732,614 | 16,597,935 | 35.4984% |
| After Burner later action, frames1620–1799 | 25,518,532 | 18,118,390 | 28.9991% |
| Doom rendered level/menu, frames540–719 | 8,115,783 | 8,115,783 | 0%, identical baseline |
| Clean prepared After Burner core, frames540–719 | 25,732,614 | 16,764,794 | 34.8500% |

Each comparison matches complete endpoint raw screens, sparse frame checksums
and complete audio hash/energy signatures. Later action is rendered flight
with explosions; input after frame119 is zero, so do not call it confirmed
interactive combat. Doom is a menu over the rendered level, not gameplay FPS.
Clean prepared code uses existing gnw_direct fetch/delay tracking and excludes
rig proof/reporting helpers. Its cost is reported separately from research code.

Implemented generic guarded code patterns: MULS/DMULS control and parameter
interpolation blocks; chained callback/loop execution; bounded inactive
zero-result iterations; pixel tails; word lookup spans; PCM scale/limit tail.
Patterns check current guest code, cycle budget, IRQ/delay conditions and bus
addresses; preserve clock, flags, PC/PPC, EA, registers and touched memory.
Callback arithmetic appears to be PCM mixing/interpolation from unique ROM
code matches and sample-ring stores, an inference from static code rather
than live peripheral tracing. Actual ring/MMIO stores remain interpreted.

Proof build: `build/32x-ab-auto-full-proof-20260930/result.json`.
Control, pixel, lookup, chain and PCM tail each receive 2,048 bounded live
full-context shadow checks per applicable kind through the real interpreter;
pixel/lookup also compare touched memory. Synthetic control tests: 4,480
full contexts and 7,808 unchanged declines; chain: 863/161; PCM tail: 896/3,200.
Previously retained countdown proof: 19,872 full contexts; wide arithmetic:
49,152. Context size is 6,016 bytes. These checks cover signed edges, branches,
budgets and meaningful unsafe-state guards; they are bounded evidence, not
exhaustive game-state verification.

Blanket native hooks slowed Doom by 3.9650%. Moving hooks inside opcode
handlers also failed: Doom +6.68% cost and slower After Burner. Both are
rejected. `native_selection.py` scans ROM opcode patterns without title names,
fixed PCs or ROM-hash whitelist; `--auto-native` prepares the matching native
variant or the exact baseline. The completed Doom fallback has identical cost
and output. Static pattern presence is eligibility, not execution coverage.
This is offline core selection; device loader selection is not connected.

Prepare a clean source snapshot in a fresh disk directory:

```sh
python3 tools/32x_research/prepare_native_bank.py --rom /path/to/game.32x \
  --output build/32x-prepared-new
python3 tools/32x_research/run_guest_trace.py --rom /path/to/game.32x \
  --output build/32x-replay-new --performance-only \
  --core-source build/32x-prepared-new/picodrive
```

Production compile gate: native CPU object text17,552B vs baseline12,808B
(+4,744B); rodata157 vs29B, bss140 vs68B. This exceeds the previously recorded
3,144B ITCM static margin if all CPU text is live. The compact-code forced
inlining experiment increases text to18,112B and is rejected. Full firmware
bank links and actual map/veneer accounting are mandatory before deployment;
object compilation alone does not establish fit. Root firmware/core/linker
remain unchanged and the Sega CD device has not been accessed.

The 50% whole-frame reduction and physical60FPS targets remain unmet. Early
flight research code still needs22.4822% reduction of its remaining cost to
halve the original. Next: exact production memory placement/bank links and
variant loader integration, more active game coverage, and hardware A/B once
the device is released. Remaining SH2 cost also warrants local validation reuse
within pure native chains; persistent RAM-code caches require proper writer
invalidation and must not assume copied guest code stays immutable.

## Native hardware first triplet and audio completion repair (2026-10-01)

Latest upstream bd60143b merged after preserving old local work in stash and
binary patches (`build/32x-device-20261001`). Restored32x instrumentation and
renderer changes; obsolete SegaCD changes remain preserved in stash. Private
firmware snapshots keep native out-of-line helpers in RAM_EMU and interpreter
in ITCM; bank link gates passed, candidate ITCM ends0xfdf8 (520B margin).

After Burner full-frame cold-boot input0..119 START6on/6off, warmup540,
window180,340MHz,fullscreen0,tear_guard0. Endpoint forced drawn in both arms.
Original A:9.99556FPS, candidate B:11.64370FPS, original A:9.99056FPS;
+16.5179% drawn/emuFPS, drift-0.04995%. All180 measured frames drawn.
Endpoint screen and last synthesized audio bytes match; identical guest clock,
input, samples and completion frame. One triplet is descriptive evidence only.
Result: `build/32x-device-20261001/jobs/job-y3sa31kw/round-000001/comparison.json`.

The completed test stopped the CPU while circular audio DMA remained running,
causing loud repeated audio. User reported this; emergency SAI disable stopped
output, then original current bank/core/XIP/selector were readback-verified,
restored and started. CONFIG/off.sav remained byte-identical; idle timeout600
restored in RAM, original settings remain saved on SD. Future benchmark source
now calls audio_stop_playing after end timestamp and before breakpoint; the
comparison md32x_snd is separate from the cleared DMA buffer. This new silent
completion path has not yet received a hardware validation; do not rerun old arms.

UI audit: RGB565 double buffering means LUT8 full-palette/CLUT upload issues
are inapplicable. Existing border/frame-skip and fullscreen row mapping tests
pass. Original installed32x core tag8c533dca differs from current resident tag
bd5a5da6, so a normal boot refuses it with an installation-warning dialog.
Matched normal UI arm is built at arms/ui but has not been installed or audited
live. Menu/pause/thumbnail/scaling/tearing live checks remain pending. Original
installation state was preserved rather than silently retagging incompatible code.


## 2026-10-01 실기 최종 정리

- 조용한 종료를 적용한 A/B/A: 10.1078 → 11.5778 → 10.1186 FPS, 후보 +14.4819%, 기준 드리프트 +0.1068%. 한 번의 triplet 결과이며 60 FPS 달성은 아니다.
- 동일 180 프레임, 화면 및 오디오 결과 일치. 세 arm 모두 종료 시 오디오 DMA 정지 확인.
- RGB565 이중 버퍼이므로 LUT8 팔레트/CLUT 업로드 수정은 해당하지 않는다. 메뉴 재도색 중 게임 배경이 검게 변하는 문제를 고쳤고 실기 화면에서 확인했다.
- 공통 스케일링 옵션을 32X 렌더링에 연결하는 수정은 양쪽 bank 빌드 성공까지 확인했다. 실기 스케일링, PAUSE, 저장 썸네일 및 나머지 화면 점검은 미완료다.
- 사용자 요청에 따라 추가 실행을 종료했다. 원래 bank2/core/XIP/선택 파일/CONFIG를 복원하고 해시 확인 후 bank2를 시작했다. off.sav도 원본과 일치한다. 원래 CONFIG의 자동 꺼짐 설정도 복원했다. 최적화 펌웨어는 상시 설치하지 않았다.
- 측정: `build/32x-device-20261001/jobs-quiet/job-jmdc2ory/round-000001/comparison.json`
- 복원: `build/32x-device-20261001/restoration-final-ui/result.json`
- 메뉴 확인: `build/32x-device-20261001/ui-live-fixed/held-menu.png`
