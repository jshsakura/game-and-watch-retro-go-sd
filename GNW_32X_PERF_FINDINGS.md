# GNW 32X(SH2) 성능 작업 — 실패 기록 및 재시도 금지 근거

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

## 12. 재개봉: MD VDP 무변화 프레임 스킵 — §11 예외③(렌더 아키텍처) 충족으로 원장 #3 해소 (2026-09-23, 세션 1.3)

**전제 변화(§8.3 종결 논리의 오류):** §8.3는 "PDF_RGB555 compose-in-place에서 32X가 dest를 오염시키므로 매 프레임 MD 재렌더가 구조적으로 필수"라 종결했다. **이는 이 fork의 실제 구조와 맞지 않았다.** 코드 판명(011f8c5c, 2026-08-20 — §8 측정 당시에도 존재):

- `PicoDrawSetOutFormat32x`(32x/draw.c:706-725, **업스트림 원본**): PDF_RGB555에서 `PicoDrawSetInternalBuf(Pico.est.Draw2FB, 328)` — MD fast 렌더러의 HighCol=**Draw2FB(인덱스드)가 곧 MD 레이어 백버퍼**.
- 32X 컴포지터(do_loop_pp/dc/rl)는 매 라인 **전 픽셀 재기입**(불투명=32X색, 투명=palmd[*pmd]) — MD FinalizeLine555가 RGB dest에 쓴 내용은 컴포지터가 **항상 덮어쓰는 순수 중복**. 즉 "dest 오염"은 없고, Draw2FB+HighPal만 무변화면 MD 재렌더 스킵해도 출력이 프레임 단위로 정확.
- `PicoDrawSync`(draw.c:1871-1905)에 스킵 fast path(`!(rendstatus&NEEDED)` → 포인터만 전진)가 **이미 존재** — 잠금은 32x.c:941-943의 `if (Pico32xDrawMode != PDM32X_BOTH) rendstatus |= PDRAW_SYNC_NEEDED;`(업스트림 보수 강제, ④) 하나뿐이었다. §8의 3수정(①②③)이 0%였던 이유: ④가 매 프레임 NEEDED를 다시 세웠기 때문.

**4수정(워크트리 /tmp/opencode/w32x, 패치=/tmp/opencode/w32x_4fixes.patch):**
1. `pico/pico_int.h` UpdateSAT: 값 변화 시에만 `PDRAW_DIRTY_SPRITES`(캐시 기록은 항상)
2. `pico/videoport.c:903-910` 데이터포트 동일값 스킵을 SAT에도 확장(VdpSATCache 비교; VRAM 기록 자체 유지, sync만 스킵) — 업스트림 스킵이 SAT를 명시 제외하고 있던 것이 §8.2 관측(99.66% 동일값 재기입)의 원인
3. `pico/videoport.c` PicoVideoSync: SYNC_NEXT 세팅 `else` → `else if (skip >= 0)`(프레임 끝 플러시 skip=-1은 제외 — 224라인 모드에서 매 프레임 NEXT 강제였음)
4. `pico/32x/32x.c` PicoFrame32x: 위 강제 NEEDED 2줄 삭제(fork 컴포지터는 Draw2FB 읽기 전용; PDM32X_BOTH가 같은 구조로 dirty-tracking만으로 동작하는 것이 선례)

**측정(QEMU mps2-an500 icount rig, Doom 32X, ROM=/tmp/doom32x_unswapped.32x, 로그 /tmp/opencode/32x_prof_{base,cand}{,600→빈,1800}.log):**

| 창 | arm | avg insn/f | draw(MD VDP) | 32x 컴포지터 | 비고 |
|---|---|---|---|---|---|
| 600f 부트 | base | 6,254,799 | 740,490 (11.8%) | 696,944 | |
| 600f 부트 | **cand** | **5,574,783 (−10.87%)** | **60,590 (−91.8%)** | 696,941 (불변) | 해시·GATE3 전부 일치 |
| 1800f | base | 8,645,564 | 980,185 (11.3%) | 769,844 | drift last500=10,666,116 |
| 1800f | **cand** | **7,739,208 (−10.48%)** | **75,414 (−92.3%)** | 769,843 (불변) | last500=9,633,457 (−9.7%) — 최동적 구간에서도 유지 |

- **출력 동일성**: snd hash(97becd29/8cdfb973)·에너지·fb f99/f299/f599/f1799·sh2 guest 카운트(69,878/93,247)·GATE3 전 창에서 baseline과 완전 일치.
- **D단계**: draw 버킷만 감소, 나머지 버킷(m68k/msh2/ssh2/z80/fm/pwm/snd/other) 전부 노이즈 내 불변 — 버킷 이동 없는 순수 이득. 부트창 편향 아님(장창·최동적 구간 일관).
- **한계 명시**: rig icount는 실기기 속도가 아님(§9/§10 실증 바이어스 클래스). 실기기 채택 판정은 인접 arm A/B로 별도(세가CD 과업 종료 후). mid-frame 실제 값 변경은 PicoVideoSync(InHblank)→부분 싱크 경로가 그대로 담당(의미 보존).

**원장 갱신**: 표 #3은 "예외③ 충족·해소"로 폐쇄. #8(8% 게이트)은 이 축(draw −92%)이 draw 풀(11.3%)에 한정되므로 여전히 8% 미충족이나, 게이트 정의가 "합산 순이익"이면 Doom 32V 한정 −10.5%로 충족 가능 — 다만 실기기 부호 확정 전 유보.

### §12.1 정정 — 실기 측정 후 (2026-09-23, 사용자 코드 리뷰 반영)

**§12의 "컴포지터가 매 라인 전 픽셀 재기입" 서술은 실기 호출 모드에 부정확했다 (사용자 지적 정확):**
- 실기 모드 = PDM32X_32X_ONLY = **GNW_PP_MD_NONE** (draw.c make_do_loop :580, 모드 선택 :654-670): 컴포지터는 32X 우선순위 픽셀만 기입하고, MD 비백그라운드 픽셀은 **기존 RGB dest 값 유지** (GNW_PP_MD_WRITE 매크로 :159-166 — PAL 변형에서만 palmd[*pmd] 기입).
- 따라서 무변화 프레임 MD 스킵의 안전성은 "컴포지터 전픽셀 재기입"이 아니라 **"dest에 잔존하는 직전 MD finalize 값 = 무변화면 정확"** 자기정합 논리로 성립. rig(s_fb 단일 고정)에서의 fb 해시 일치는 이 구조와 모순 없음.

**실기에서 draw 스킵은 중립화됨 (rig/실기 괴리의 원인, 코드 판명):**
- main_md32x.c 프레임 루프: 매 드로운 프레임 `set_out_buffer()` = `PicoDrawSetOutBuf(lcd_get_active_buffer(), 320*2)` 후 `lcd_swap()` — **dest가 매 드로운 프레임 A↔B 교대**.
- draw.c PicoDrawSetOutBufMD: `if (dest != DrawLineDestBase) rendstatus |= PDRAW_SYNC_NEEDED;` — **dest 교체마다 NEEDED 강제** → 수정④와 무관하게 실기는 매 드로운 프레임 MD 풀 재렌더 (안전 — 잔상/깜빡임 없음, 전 MD 픽셀 재기입).
- rig는 s_fb 1회 고정 바인딩 → 이 경로 미발생 → rig −10.5%가 실기에 전이되지 않음.

**실기 샌드위치 (Doom 32X, 900f×3 drawn_ab, 어트랙트 데모 창, base1↔fix1 교차):**
- base1: 39.94 / 39.89 / 39.56 / 39.84 / 39.85 (n=5, 평균 39.816)
- fix1: 40.01 / 40.27 / 40.20 (n=3, 평균 40.160)
- **Δ = +0.86%** (재현성 ±0.35%의 ~2배 — 작지만 실재 추정). 출처 = draw 스킵(중립화됨)이 아닌 ①②③의 ParseSprites/rendstatus 부기 절감으로 추정. 8% 게이트 미충족.
- 모든 런 ratio=1.0000, guard charged≈1900-1934, 위드홀 없음.

**결론:** 4수정은 실기에서 무해(잔상 없음)·소폭 플러스(+0.86% 경계선). rig −10.5%의 잔여 분(draw 스킵분)을 실기로 실현하려면 별도 설계 과업 필요: (a) 변경 프레임에 양 LCD 버퍼 렌더 (b) 버퍼별 dirty 플래그로 SetOutBufMD NEEDED 전파 조건화 (c) 내부 단일 렌더버퍼 + 런처 복사. 현재 패치는 보존(/tmp/opencode/w32x_4fixes.patch, 워크트리 dirty 유지).

## 12.2 After Burner Complete 센서스 — SH2축 병목 정체와 레버 상한 (2026-09-23, 세션 1.3)

**배경:** 사용자 지정 주 검증 타이틀=애프터버너 컴플리트(GM MK-84507-00, 실화면 식별 완료 — f299 AFTER BURNER 적색 워드마크). 실기 base1 어트랙트 bench=**12.85 emulated fps**(ratio 1.0, guard 1814). rig 프로파일(600f): avg 19,468,732 host insn/f, **SH2 84.4%**(msh2 54.1%+ssh2 30.3%), draw(MD VDP) 2.7%, 컴포지터 3.8%, m68k 5.6%. → MD-VDP 스킵축(§12/§12.1)은 AB에 무력(지시⑤ 준수, 효과 있는 척 않음).

**옵코드 센서스(RIG_OPCOST 1/64 샘플 + RIG_OPHIST, 600f, 로그 /tmp/opencode/ab_opcost_600.log·ab_skel_600.log):**
- **msh2**(~173k guest insns/f, host/guest≈61, 제어 중심): 0x8 BT/BF/CMP **34.23%**(avg 2.3틱) / 0x2 MOV.x Rm,@Rn 스토어 18.98%(2.2) / 0x3 ALU 14.58%(2.1) / 0x4 shift/DT/JSR 14.34%(2.1) / 0x6 로드 6.39%(2.0) / 0x0 misc/NOP/RTS 4.91%(2.1) / 0xC 1.48%(3.1). GBR 0xC 이상치 **없음**(Doom의 106.5사이클 comm I/O급 무).
- **ssh2**(~105k/f, 데이터 이동 중심): 0x0 12.08%(1.8) / 0x4 13.98%(2.1) / 0x5 MOV.L @(d,Rm) 12.35%(2.0) / 0x8 12.33%(2.6) / 0x3 10.72%(2.0) / 0x2 10.52%(2.0) / 0x6 6.66%(2.6) / 0x7 ADD# 4.71%(1.6) / 0xD MOV.L @(d,PC) 4.59%(1.9). 로드·분기 avg 2.6=메모리 접근 밀집(프레임버퍼/comm 라이트).
- **GBR 씬서스(ssh2)**: c1/c2/c5/c9/ca 각 ~155-156k 등빈도 = 프레임당 ~1,300회 플러시 핸드셰이크 = ssh2 전체의 **1.2% — 폴링 스톰 아님, idle-poll fast-forward 레버 없음**(Doom c5 0.76%와 동일 급). NOP도 무해(ssh2 2.8%, msh2 0.17%).
- **스켈레톤 암(RIG_SH2_SKELETON)**: 그룹 avg 틱 1.6-2.0 vs 실핸들러 2.0-2.3 → **핸들러 실작업 추가분 ≈0.2-0.3틱, 스켈레톤 고정비(디스패치+패치+PC+icount+폴)가 dispatch 비용의 ≈84-87%** — Doom 종결(49-51/60.9=81-84%)과 동일 구조. 타이머 오버헤드 0.147틱(캘리브 보정).

**레버 산술(실기 12.85fps 기준):** 30fps=2.3×, 60fps=4.7×. ① C레벨 상한(핸들러 비용 전부 0 가정) ≈1.15-1.2× ≈15fps ② 수퍼인스트럭션 퓨전(msh2 top4 그룹 82% 결합·스켈레톤 절반) 최선 ≈1.6-1.7× ≈21fps — 30 미달 ③ **Thumb-2 DRC만 2-3× 도달 가능** — fork에 drc 부재(cpu/sh2/mame 인터프리터뿐), 월 단위 신규 프로젝트, 원장 #1(monolithic AOT 금지)과 별개의 '측정 정의 밖' 클래스=§11 예외② 신규 과업.

**판정:** AB 병목=순수 실작업 밀집 SH2 부하(병리 핫스팟·폴링 스톰·GBR 이상치 전부 부재). 인터프리터 C레벨 잔여 없음(기존 Doom 종결 기록을 AB에서 재확인). **AB 정상구동(30fps+)은 DRC급 신규 축 없이 불가 — 상한 근거와 함께 기록.** 4fix(§12/12.1)는 Doom급 draw 비중 타이틀용으로 유효 유지.

## 12.4 정정 — 2026-09-23 팀장(GPT/codex 1.1) 독립 검토 반영 (§12.2 수치의 유효 범위 한정)

1. **§12.2 '스켈레톤 고정비 84-87%' 철회.** RIG_SH2_SKELETON(sh2pico.c:1726-1748)은 분기 포함 전 옵코드 핸들러를 volatile 카운터로 교체 — 게스트 흐름 자체가 변형(avg SH2 279,858→510,299/f, master 0x0 비중 4.91%→90.10%, fb 전 0, sound energy 0, GATE3 FAIL). 정상/skeleton 두 흐름의 그룹별 avg 틱 비는 'AB 고정비'로 해석 불가. synthetic dispatch benchmark 기록으로만 남김.
2. **계측세 명시.** OPHIST/OPCOST 동시 계측 암 avg=26,312,611 vs 클린 19,468,732(+35.15%) — §12.2의 모든 per-op 수치는 계측 포함 분모. 타이머 오버헤드(0.147틱) 보정이 per-op census 세금을 제거하지 않음.
3. **분모 구분.** 'SH2 84.4%/draw 2.7%'는 ab_fix600.log의 QEMU phase 분모 — 실기 12.85fps와 결합한 기기 병목 비율 계산에 사용 금지(실기 점유 별도 계측 전제).
4. **분포 해석 주의.** NOP은 전무하지 않음(slave 0x0009≈2.78%). opcode 분포만으로 poll 부재/존재 판정 불가 — 실제 PC/루프/주소 확인 필요(master BF 0x8b 19.11%·0x84 9.31% 등).
5. **목표 산술 정정.** 12.85→30fps=전체 비용 57.17% 절감; 실기 SH2 점유 84.4%가 확인되고 나머지 불변이면 SH2 67.73% 절감=**SH2 ~3.10배**. 'C 상한 1.15-1.2×/수퍼인스트럭션 1.6-1.7×'는 정상-게이트 계측 전 확정 근거 아님(스켈레톤 근거 무효화로 특히 약화). 30fps 도달 가능성은 DRC 실현성 산정(AB 자체 block working set+손익표) 이후 판정.
6. **계측창 게이팅 원칙.** 기존 수치는 부트 포함 600f(f100-160 저부하↔f180+ 고부하 혼합) — 전투 앵커+동일 입력창 고정 후 재계측 필요. 계측 시간은 성능 판정에서 제외.

## 12.5. 2막 C단계 — fastloop shape③: SDRAM 메일박스 바이트 폴+타임아웃 폴드 (2026-09-23, 세션 1.3)

**배경(§12.2/§12.4 정정 후 재분석):** 게이티드 센서스(RIG_SH2_PC_HIST+POLL_PEEK, 부트 포함 전창 — 전투창 재검 대기중)로 AB의 실행 정체 확정:
- **msh2 실행의 81.1%(backedge 11.76M 중 9.54M, 전체 insns의 28.4%)가 단일 5인스 루프 0x060038ee-0x060038f6**: `MOV.B @(1,R1),R0 / TST R0,R0 / BF $06003938(정방향 탈출) / DT R2 / BF -12(백엣지)` — R1=0x06003af0(peek #29 op=8bfa)=**SDRAM 소프트웨어 메일박스 바이트 폴**(MMIO 아님), R2=타임아웃 카운트다운, 대기 조건=바이트 0→비0.
- slave는 0x060003e2-0x06000432 MULS/STS MACL 누산 루프(backedge 50.6%)=실연산.
- 기존 fastloop shape①(MOV.W+TST 2인스)/②(Doom BT/S 프레임대기)는 이 body(5인스+MOV.B+DT) 미커버 — 네거티브 캐시 주석이 'comm/VDP poll loops' 거부 명시와 정합.

**구현(sh2pico.c gnw_sh2_fastloop 내, SDRAM poll shape 뒤 1450-1473 +89줄):** 진입=(opcode&0xff00)==0x8b00(BF 역방향) && disp8<0 && !t_flag && target+8==ppc && body 4인스 패턴 [0x84xx MOV.B(disp4,Rm),R0 / 0x2008 TST R0,R0 / 0x8bxx 정방향 BF 탈출 / 0x4n10 DT Rn] && pa=r[base]+disp(0x20000000 스트립)==SDRAM && v=r[dt]>=2. 폴드: kmax=(icount-1)/7(7=MOV.B1+TST1+BFnt1+DT1+BFt3), kmax=min(kmax,v-1), k회 동안 매 이터 RB(pa) 재판독(프로듀서 가시성) — val!=0 검출 시 그 이터 미소모 정지 → 실 f6 BF taken → 실 패스가 탈출 분기 자연 수행(countdown shape의 '마지막 1회 실해석' 계약 동일). 패턴 미스 시 GNW_DL_REJ_SLOT 거부 캐시.

**검증(rig, QEMU icount):**
- **AB 600f: avg host 19,468,732 → 15,942,464 = −18.1%**, fb f99/f299/f599=c5aff44d/650a9438/393ea6e6, snd hash=60c1049b, 에너지(396656585/1765316036377/28616) **전부 baseline과 bit-exact**, GATE3 PASS. 게스트 dispatch 279,858→216,947/f(폴드된 이터 감소=사이클은 icount 차감 보존).
- **Doom 600f 회귀: fb 9cd2510f/22ee77a6/ced1080b, snd 97becd29, avg 5,569,611(4fix cand 5,574,783 대비 0.09%), sh2=69,878 동일 — 완전 무간섭.**
- PHASE_PROF: msh2 10.55M(54.1%)→7.07M(44.1%)=−33%(폴드 직격), ssh2 5.91M→5.93M 불변, draw/컴포지터 불변 — 이득 전부가 폴 루프 버킷.
- 한계: rig icount≠실기기 속도(§9/§10), 부트 포함 창(전투창 게이팅 재측 대기), 실기 실측은 device arm(w32x_s3) 샌드위치로 대기.

**의미:** §11 표 #1(monolithic AOT)과 무관한 '데이터 의존 SDRAM 폴' 범용 fastloop 확장 — AB 마스터의 대기 시간이 사이클 정확하게 접힘. 잔존 병목: msh2 실연산 44%/slave MUL 37% → DRC(D단계) 대상.

### 12.6 실기 샌드위치 확정 (2026-09-23, 세션 1.3 / arm w32x_s3 = 4fix + fullfill + shape③)

기기(base1→s3→base1 교차, CORE=32x bench 900f×3, autoboot AB 어트랙트/Doom 각 재픽):

| ROM | base1 | s3 | Δ | 비고 |
|---|---|---|---|---|
| 애프터버너 컴플리트 | 12.85 / 12.87(재샌드위치) | **16.80 fps** | **+30.6%** | 드리프트 ±0.02, ratio 1.0, guard 1814 |
| 둠 (Doom) | 39.82 (fix1 40.16) | **42.71 fps** | **+7.3%** | emu=drawn=1025, guard 2050 |

- s3 arm: intflash 260,732B / 32x.bin 64,505B / 32x.xip 744,920B (PATH=/usr/bin:/bin:$PATH 명시 필수 — 백그라운드 셸에서 docker 미발견 사고 있었음).
- **AB +30.6%는 rig −18.1% insn 대비 초선형** — 폴드가 인터프리터 디스패치 반복(XIP 페치/I-캐시 압박)을 제거하는 실기 특유 이득으로 해석. **Doom +7.3%는 fullfill(SetOutBuf32X 스왑-NEEDED 분리 + 32X_ONLY 전픽셀 재기입 컴포지터)이 §12.1의 실기 draw-skip 중립화를 해제** + shape③ 합산.
- 모든 런 ratio=1.0000 'PRESSURE BUT NOTHING WITHHELD' — 위드홀 없음. 씬=어트랙트(전투 조작 검증은 팀장 지시② 후속 과제).
- 원장 정정(§12.2/§12.4): "AB 폴링 병리 없음(GBR 씬서스 1.2%)" 판정은 **오판이었음** — 진폴은 GBR 아닌 **SDRAM 소프트웨어 메일박스 바이트 폴**(0x060038ee MOV.B @(1,R1),R0/TST/BF/DT/BF, 마스터 백엣지 81%/실행 28%)이며 옵코드 그룹 씬서스(0xC 1.5%)로는 검출 불가였음. shape③ 검증이 이를 입증.

**shape③ 범용성 근거(팀장 지시③):** 구현은 순수 패턴 매칭 — [MOV.B @(d4,Rm),R0(0x84xx)] + [TST R0,R0(0x2008)] + [정방향 BF 탈출(0x8bxx, disp>0)] + [DT Rn(0x4n10)] + 역방향 BF 백엣지(target+8==ppc) + 폴 주소 SDRAM(0x06xxxxxx) 영역 게이트. **AB 주소/ROM 식별자는 코드에 없음**(주석 출처로만 언급). 사이클 회계: 7cyc/iter 폴드, 매 이터 RB 재판독(타 코어/DMA 가시성 유지), 마지막 1회 실해석 계약, !test_irq 게이트=IRQ 경계 보존, GNW_DL_REJ 거부 캐시. 검증: AB/둠 2 ROM fb+snd 해시·에너지 bit-exact(rig), 실기 ratio 1.0.

## 12.7 D-stage: DRC feasibility inputs (2026-09-23, post-s3)

**Memory audit (real map, w32x_s3 build):**
- ITCM: .overlay_md32x_itc 0x4-0x7b48 (30.8K code) + .overlay_md32x_itc_bss
  0x7b48-0xf580 (31.3K hot state) → **free 0xA80 = 2.6K only**
- DTCM 128K: 100% placed (_persistent/.data/.bss/heap 87.9K/stack 24.2K)
- RAM_EMU tail: .overlay_md32x_bss ends 0x240FD708; region end 0x24100000 →
  **free ~0x28F8 = 10.4K, writable AND executable** (overlay code itself
  executes from 0x2404b000 — same AXI SRAM). This is the only viable pool.
- No arbitrary 64K gate assumed; single-FB untouched.

**Block structure (from pchist/census, full 600f window, boot incl.):**
- Slave hot region 0x060003e2..0x06000432 is NOT one loop: it is a
  **dispatcher** (e2: MOV.L @(0x18,R14),R0 / e4: MOV.L @(0x0C,R14),R1 /
  e6: JMP R0 (indirect!) / e8 delay: MOV.L @(8,R14),R8) + one dispatched
  **MAC kernel** (41c: MOV #0,R1 / 420: MULS.W R1,R0 / 424: STS MACL,R2 /
  428: ADD R2,R12 / 42c: ADD R2,R11 / 430: BFS e2 / 432 delay: ADD #0x20,R14).
  2.47M dispatches/600f. JMP R0 = DRC needs an indirect-target lookup at
  block exit (standard), not a new blocker; avg block 8-14 insns (mean-run
  15.2) — translation amortization is fine.

**Sizing probe (Thumb-2 hand estimate on the kernel block):**
- 13 SH2 insns → ~30-40B Thumb-2 + JMP-R0 exit stub ~16-24B ≈ 48-64B/block.
- 90% CDF = 442 PCs ≈ 40-60 blocks ≈ 3-6K code + ~40B/block meta + PC→block
  direct-mapped cache 1024×4B = 4K → **total ≈ 8-10K ≈ fits the 10.4K tail
  at ~90% coverage (99% = 1058 PCs does NOT fit)**.

**Profit estimate (rig, shape③ build):** ssh2 = 5.93M/16.0M insn-cycles
(37.0%, PHASE_PROF full window). Interpreter ~60 host cyc/guest-insn →
translated ~15-20 → potential ssh2 −60-70% ≈ total −20-25% IF coverage and
exit overhead hold. Entry/exit overhead per block: PC-hash probe + reg
context spill ~20-40 cyc; IRQ check at block exit (test_irq flag) — engine
already only delivers at slice boundaries; SDRAM code writes are boot-time
(load-and-go), invalidation via existing write path flag.

**Decision (team-lead E):** minimal generic subset translator (load/store,
ADD/MOV/MULS+STS/DT/BT/BF/BRA+delay), interpreter fallback, direct-mapped
PC cache, DRC pool = RAM_EMU tail via linker section — separate candidate
arm (w32x_drc), never mixed into s3.

## §12.10 s3b read-count separation (2026-09-25, team-lead A/C/D protocol)

Question: did s3b's single-read arithmetic fold (vs s3's per-iteration
re-read) cause the -3.5% device regression (§12.9)?

Arms (runtime switches gnw_sh2_s3_off/_once, -DGNW_S3_ARM=0/1/2, DRC
compiled out; rig static pool NOT armed):

- A oracle (fold off, matcher probes only): fb c5aff44d/650a9438/393ea6e6,
  snd 60c1049b, avg host 19,468,126, avg sh2 279,858, folds=0 — clean
  reference reproduced.
- C fold + per-iteration re-read: identical hashes/energy, avg host
  15,833,022 (-18.7% vs oracle), folds 7,787,136, avg sh2 217,128.
- D fold + single read (s3b behaviour) + unsigned-safe clamp:
  identical hashes, avg host 15,832,811, folds 7,787,136 — C == D to
  the folded-iteration count.
- Doom 600f on the C-default worktree: 9cd2510f/22ee77a6/ced1080b,
  snd 97becd29, avg 5,568,071 — regression clean (matcher never fires).

CPU-state differential: it-threshold snapshots (cumulative loop
iterations 2/1000/60000/65530) print identical r[dt]/r0/T/pc in C and
D; the oracle's matcher lifetime is shortened by the DL_REJ negative
cache after a byte!=0 dispatch (structural, all arms share it), so its
snapshot series is not directly comparable — output-hash equality plus
identical guest dispatch accounting (avg sh2) is the oracle-vs-fold
equivalence evidence here.

Also hardened while here: unsigned clamp `k > (int)(v-1)` -> unsigned
compare (v-1 could exceed INT_MAX); alias guard base!=0 && dt!=0 &&
base!=dt was already the corrected form.

Verdict: read-count reduction is NOT the s3b device-regression driver
(measured, per the team-lead rule — no scheduler/event/guest-cycle
difference exists between C and D). Remaining suspects: the generic
decode/alias/forward-BF changes altering which loops fold (fold-set
delta) and device-only XIP/I-cache effects. s3b stays non-adopted
(§12.9). Production default is now C (re-read) — correct by
construction (no no-writer proof required) at zero measured cost on
rig.

Artifacts: /tmp/opencode/{ab_s3_arm0,ab_s3_arm1,ab_s3_arm2,
doom_s3c_600}.log, rig_s3_{0,1,2}.elf, s3_build_{0,1,2}.log.

### §12.10.1 Correction (2026-09-25, team lead m4301 review — generated-code audit)

The §12.10 verdict above is superseded in four points; the raw numbers
stand, their interpretation does not.

1. C == D is a compiler artifact, not an axis comparison. Disassembly of
   the C and D arms (artifacts/2026-09-32x/case-s3cd-compilecheck/,
   s3dis_{1,2}.txt, fold block at lines 2960-29aa in
   sh2_execute_interpreter) shows the source-level per-iteration re-read
   loop was fully eliminated: RB is a non-volatile direct load, the
   compiler hoisted it past the fb==0 gate and proved all re-reads
   identical, leaving one sign-clamp `bic` toggled by `cbnz once`. The
   211 insn/frame delta equals the ~211 fold events/frame. The C axis
   never existed in machine code under the production (non-volatile)
   build.
2. "Read-count reduction is not the device-regression driver" is
   WITHDRAWN: a null rig delta cannot reject a device-side cause.
   Remaining suspects unchanged (fold-set delta from the generic decode,
   device XIP/I-cache effects — neither confirmed).
3. CPU differential verification is NOT complete. The it-snapshots print
   the threshold value rather than the actual reached iteration, carry no
   per-core/per-PC identity, and the oracle arm exits the instrumentation
   path via the DL_REJ negative cache — the three arms did not share one
   observation point. fb/snd hashes do not prove internal CPU equality.
4. avg sh2 denominators corrected: oracle 279,858/frame vs C/D 217,128
   (fold effect) — never report all three as 217,128. C is a conservative
   candidate only, not "correct by construction", not production
   confirmed; unconditional folds counter / matcher fflush / snapshot
   probes are to be moved under a compile-time diagnostic guard
   (GNW_S3_DIAG) and must not be quoted as clean performance.

Required follow-ups (open): diagnostic-only volatile re-read arm with
codegen re-verified; full differential harness (identical initial state
and slice budget, same termination boundary, full register/PC/SR/T/ea/
delay/icount/IRQ/memory compare, per-core/per-PC actual iterations,
unsigned-large/alias/byte-exit/slice-boundary cases); s3-vs-correct-decode
fold-set diagnostic (PC as diagnostic key only, no execution whitelist).

### §12.10.2 shape³ differential-verification harness (case-s3cd-harness)

Proper harness per lead m4301(4): synthetic SH2 + SDRAM 5-insn mailbox poll loop at
0x06000200, identical initial state / slice budget, arms O(off) vs D(production single-read
arithmetic fold) vs C(DIAG volatile re-read), full-state compare (r[16]/pc/ppc/pr/sr/t/gbr/
vbr/mach/macl/delay/test_irq; ea excluded, non-architectural). Per-insn trace measured the
real iteration cost: 4 insns×1cyc + BF-taken 3cyc = 7cyc/iter — exactly the fold's k×7
charge (fold skips host dispatches only, not guest cycles).

Results (artifacts/2026-09-32x/case-s3cd-harness/, rig_s3h_diag.elf + s3h_run6.log):
9/9 PASS with genuine fold activity — dt_v100 D folded=98 (O≡D≡C), dt_v65536 folded=65534,
slice_B61 folded=8 with final state r2=1 ic=-2 pc=L identical to the same-budget oracle
(fold-evidence line), unsigned_v80000001 folded=199 r2=0x7fffff39 identical to oracle
(case the old (int)(v-1) clamp bug would have silently skipped). byte_exit / alias_dt_base /
dt_v2 / slice_B6 verify the no-fold gates.

Debugging note (recorded to prevent re-runs): early "all arms identical, folded=0" was
DL_REJ negative-cache cross-contamination — the oracle arm's matcher rejects poisoned the
shared reject slots and later arms skipped the fastloop gate entirely. Fixed with
gnw_sh2_dlrej_clear() between arms (diagnostic only).

Limitations: single-core no-writer mid-fold (D cannot observe a flip inside its slice by
architecture; C-arm volatile re-read is the covering logic, no direct test); C arm is
GNW_S3_DIAG-only, production keeps the single-load arithmetic fold. "Conservative
candidate", not "correct by construction" nor production-confirmed on device.

### §12.10.3 fold-set diagnostic: s3-era decode vs corrected decode (case-s3cd-foldset)

Lead m4301 item 5. One diagnostic rig run (AB 600f, fold active, GNW_S3_SETDIAG tallies)
evaluating both predicate sets per matcher dispatch. s3-era flaws modeled from the
preserved s3 patch: address operand swap (r[disp]+m vs correct r[m]+d), unsigned exit-BF
test, missing alias guards. Result: 125,045 dispatches both-accept, old-only=0, new-only=0,
neither=1; hashes clean. On AB the two decodes fold the IDENTICAL loop set — the s3b
device regression cannot be explained by fold-set membership. Remaining candidates
(predicate codegen layout, device variance) stay unconfirmed; no cache attribution.
Evidence: artifacts/2026-09-32x/case-s3cd-foldset/.

## 12.8 SH2 DRC (Thumb-2 dynamic translation) — correctness complete (2026-09-26)

Architecture (all in external/picodrive/cpu/sh2/mame/sh2pico.c, `#ifdef GNW_SH2_DRC`):
- Translation pool: `.overlay_md32x_drc` 10K NOLOAD at RAM_EMU tail (ld; device) / static 16K (rig).
  Layout: u16 idx[1024] direct-map (slot=(gpc>>4)^(gpc>>12), base>>1) + per-block hdr
  (u32 gpc tag / u16 total / u16 n) + Thumb-2 code + literal pool + guest-word shadow (gops).
- Dispatch hook (interpreter main loop, `gnw_direct && !delay && !test_irq && !(state&idle)`):
  lookup → miss: drc_translate(gpc, op0-first) → hit: shadow re-verify (op0 word pre-check +
  drc_fastw loop; -1/virtual-addr => skip) → fn() |1. Blocks are straight-line (≤12 insns);
  branches/delay-slots/traps/system ops end the block and fall back to the interpreter.
- Generated code contract: AAPCS, r4=SH2 ctx, r5=icount mirror (post-charge `subs #1; bgt`
  after each insn body — pause exits with pc = *next* unexecuted insn), memory via C thunks
  (READ: r0=a,r1=sh2 / WRITE: r0=a,r1=d,r2=sh2; blx r6 with thunk addr literal loaded LAST
  — bug: str_ctx clobber), live icount store before thunk + test_irq/idle-state (0x1e)/icount
  checks after (thunk may end slice / post IRQ). Epilogue: str icount, str pc, str ppc=r0-2
  (== last executed insn, interpreter-exact — poll_detect attribution).
- Cycle model: 1/insn charge (interpreter parity; IRQ take = -5 in interpreter tail, hook
  routes via gnw_drc_irq label). Invalidation: 6 write hooks (sh2_write*_sdram, p32x_sh2_write*,
  24-bit mask window) + dmac hook; wseq removed (shadow verify supersedes).
- Emitter subset: MOV immediates/ADD#(imm8)/MOV.L·W @(disp,PC) (disp = **8-bit**, (gpc+4)&~mask),
  MOVLS4/MOVLL4/MOV.W-L·B loads+stores incl. **@-Rn pre-dec store / @Rm+ post-inc load (n≠m)**,
  ALU (ADD/SUB/AND/OR/XOR/TST/CMP), MULS/MULU→MACL, DT/BT-adjacent via ITE (mask 0x__0C!) +
  MOV.W (non-flag-setting), shifts/rotates/EXTU/EXTS/NOT/NEG/NEGC/SWAP.B(low-16 only)/SWAP.W,
  STS MACL/MACH/PR (0x0-group subop table per sh2.c opcode&0x3f: 0x0A MACH/0x1A MACL/0x2A PR),
  NOP/CLRT/SETT. JIT-unknown => block end (safe fallback). Negative cache: rej[1024] tag +
  hotness(3 misses) + single-occupancy slots.

Bugs found & fixed (1-24 chronology, all in sh2pico.c unless noted): bpl/bhi pause-skip
offset, ROTL carry polarity, cbz position-dependence→cmp+beq, L_common fixup target,
run() int contract (translate-fail falls through), emitter rollback, invalidation hook
sites (SDRAM alias 0x0604xxxx &0x3ffff mask!), virtual/physical address mask, live-icount
around thunks, idle-state veil + hook gate, ppc store, MOVWI/MOVLI disp8 (opcode&0xff),
delay-slot dispatch leak into hook (gnw_direct gate — JMP@R8 target clobber), **double-fetch
leak (translate/shadow re-reading destructive comm/FIFO handlers — drc_fastw/op0-first
fixed stub arm to bit-exact)**, r6 thunk-address literal clobber (8 sites), self-modifying
boot (shadow verify), **IT vs ITE mask typo (0xBF08→0x0C family; T≡0) + MOV.W instead of
MOVS**, **0x0-group table mis-entry (RTE executed as STS MACH — min repro [372e,3760))**,
ppc=r0-2 exactness, **decode table 0x2/0x6 sub 4/5/6 = pre-dec store/post-inc load, NOT
@(R0,Rn) — SH2 ISA misread; DIV0S≠MUL.L; SWAP/NEG/NOT/EXT operand corrections**.

Performance-bomb incident (final): true-clean & stub arms "100x slow" = (a) rig progress
printf referencing unguarded gnw_sh2_drc_* externs broke NO-FLAG builds → stale DRC elf ran
unnoticed; (b) sh2.c JMP icount-- temp-comment left in (interpreter JMP 1-cycle — guest
timing explosion 12x). Fixed: JMP restore + printf #ifdef guard. Measurement lesson:
`qemu | grep -m` pipe = SIGPIPE false slowdowns — always file-redirect then grep.

Verification (rig 600f, file-redirect protocol, goldens from §12/s3cd): true clean
(text 2,943,520) avg 15,802,827 · sh2 217,128 · fb c5aff44d/650a9438/393ea6e6 · snd 60c1049b
· GATE3 PASS (golden match). DRC arm (rig_drc_final.elf, -DGNW_SH2_DRC -DGNW_DRC_RIG): AB 600f avg host =
37,656,627 ARM instructions/frame vs true-clean 15,802,827 (2.38x more host instructions —
cause decomposition still open: hook-check tax, lookup/negative-cache walks, shadow
validation, emitted-block execution under TCG; do NOT attribute this number to "TCG
translation cost" without evidence — icount counts executed ARM instructions, and QEMU
wall time is a separate quantity again). sh2 guest dispatches 215,482 (blocks absorb some).
CORRECTION (team lead 2026-09-26, accepted): the shared `runs=0` in that log is NOT
"did not execute" — the shared counter only increments in the STUB diagnostic branch;
native execution increments runs_m/runs_s (per-core), and rig_tbudget=30000 (GNW_DRC_RIG
diagnostic) limited translations to 290 in that run. Coverage of THAT arm was therefore
narrow. A later production-flag arm (-DGNW_SH2_DRC only, no diagnostic throttle, on=1 —
ab_drc_prod2.log) translated 9,387 blocks and DIVERGED (snd 81b23b93, sh2 168k): remaining
emission bugs exist beyond the 290-block coverage. DRC correctness is proven only for the
budgeted coverage; NOT a universal-correctness claim. Doom regression
(DRC): fb 9cd2510f/22ee77a6/ced1080b · snd 97becd29 · GATE3 PASS · 0 Illegal.

Status: correctness complete on both ROMs. Device arm (w32x_drc, Makefile GNW_SH2_DRC +
pool registration already in main_md32x.c) pending team-lead approval; on-device A/B
sandwich (base1/s3/drc) is the next measurement step. Rig icount ≠ device speed.

## 12.9 DRC production arm + device-safety audit (2026-09-26, team-lead cycle)

Code hygiene (all runtime diagnostics now compile-time isolated): gnw_drc_probe (r1snap/
fine/exitw/first-dump/dd0), drcvm (DT-R8-specialised check), translate audit prints, hook
one-shot print — ALL under GNW_DRC_RIG. Production flag = -DGNW_SH2_DRC only. rig_32x.c
end report now prints per-core runs_m/runs_s (shared `runs` labelled stub-only).
rig_tbudget=30000 stays GNW_DRC_RIG-only (diagnostic) — confirmed NOT in production build.

Build-gate incident class (recurring, now gated): a no-flag "true clean" build silently
failing and a stale ELF being run has happened twice (unguarded externs; then again with
probe refs) — builds now verified by [rig] build-only gate + text-size fingerprint before
any run; true-clean 2,943,520 reproduced.

Device-safety audit (static, QEMU does not substitute for it):
- Pool: .overlay_md32x_drc 6K NOLOAD at RAM_EMU tail (was 10K; DRC globals rej[1024]u16 +
  hot[1024]u8 + fx/lit/bfx ~3.3K moved the BSS tail, pool shrunk to fit; ASSERT to
  __RAM_EMU_END__ kept). Pool is executable (default PRIVDEF map, no XN).
- Caches: I+D caches ON (main.c), RAM_EMU cacheable → NEW publish hook contract:
  gnw_sh2_drc_publish() called after every block install; device impl in main_md32x.c =
  SCB_CleanDCache_by_Addr(range) + __DSB + SCB_InvalidateICache() + __ISB (Cortex-M7 has
  no range I-cache invalidate; installs are rare so full-drop is acceptable). Rigs leave
  the hook NULL (coherent sim). 6 write-hook invalidation + shadow re-verify unchanged.
- Runtime enable: candidate arms turn gnw_sh2_drc_on=1 at 32X start inside #ifdef
  GNW_SH2_DRC; release builds do not define the flag → no DRC code at all.
- Placement: DRC code (translator+lookup ~7K) overflowed ITCM (2.6K free) and internal
  FLASH (4.6K over). Final: .drcx.* sections XIP'd inside .overlay_md32x (appended per the
  no-reorder rule) — same QSPI sentinel-patch path as the rest of non-ITC md32x code.
  w32x_drc arm built clean: intflash 260,732 / 32x.bin 71,145 / 32x.xip 745,132 (kept in
  /tmp/gnw_arms/w32x_drc).

Rig production arm result (ab_drc_prod2.log, -DGNW_SH2_DRC only, on=1, no budget):
GATE3 PASS but snd hash 81b23b93 != golden 60c1049b, sh2 168,283, fb f299/f599 differ —
DIVERGES once coverage widens (trans 9,387 vs the budgeted 290-clean run). Remaining
emission bug(s) live outside the previously-verified block set. DEVICE FLASH OF w32x_drc
IS NOT APPROVED (hash gate fails); arm kept as build artifact only.

Next minimal unknown axis (single diagnostic case, no hash re-runs): bisect the
production-arm divergence via GNW_DRC_MIN/MAX_GPC range gates to the offending block(s).
On-device A/B protocol per team lead: DRC-off vs DRC-on sharing ALL other changes
(universality fixes + fullfill), not against base1/s3 historical anchors; polling-based
device bench remains a blocker (no firmware instrumentation approved), do not route around.

### §12.8 addendum: bug-26/bug-27 — production full-coverage correctness complete (2026-09-26 02:30)

The §12.9 production-arm divergence (snd 292ac67d vs golden) is fully resolved by two final bugs:

- **bug-26 (pool fit off-by-shadow):** drc_translate fit check used `8+total+n*2` while install
  writes shadow as **(n+1)*2** (op0 word, bug-23) — up to 2B overrun past pool end at the
  last-fit block (RAM_EMU tail corruption). Fixed both fit checks.
- **bug-27 (DT no_polling unmarked):** the interpreter's DT handler marks
  `sh2->no_polling = SH2_NO_POLLING(0x400)` (poll_detect discounts real-work loops); DRC-emitted
  DT blocks didn't → poll_cnt accumulated +1 per work-loop pass → VPOLL threshold hit one
  poll-read earlier → f=7 master comm poll diverged Δ8 cycles → f=8 slave slice phase Δ182 →
  CORE-DIV (b232) → snd-only drift (fb identical — visual path unaffected). Traced via
  [pd] per-call poll_detect trace (first divergence: [pd]#13 f=7 ppc=0600d0e2 a=410b,
  stub cnt=0-reset vs DRC cnt=1-increment). Fix: DT emission stores SH2_NO_POLLING to
  ctx->no_polling (drc_off_np baked offset). Note: shape³ fold's DT skip is common to both
  arms/golden — not involved.

**Verification (production config, -DGNW_SH2_DRC only, NO budget, full 600f coverage):**
- AB (ab_drc_bug27.log): fb c5aff44d/650a9438/393ea6e6 · snd 60c1049b · energy
  396656585/1765316036377/28616 — ALL golden bit-exact · GATE3 PASS · 0 Illegal.
  avg host 36.4M (QEMU interprets generated ARM code under TCG — not a perf number),
  avg sh2 dispatches 215,636 (block absorption; golden 217,128).
- Doom (doom_drc_bug27.log): fb 9cd2510f/22ee77a6/ced1080b · snd 97becd29 · energy
  164291158/515533918562/32767 · GATE3 PASS · 0 Illegal · avg sh2 68,034 (golden 69,878).

DRC correctness is now complete at FULL translation coverage on both ROMs (previous
rig_drc_final clean was budget-limited ~290-trans coverage). Device arm rebuilt
(w32x_drc, bug-26/27 sources) — flash still requires team-lead approval; on-device
DRC-off vs DRC-on comparison is the remaining measurement step. Known perf items
(unsolved, orthogonal): 1024-slot single-occupancy misses storm (104M misses/600f).

## 12.11 DRC on-device conclusion (2026-09-27): not adopted for release; correctness proven

Device saga after bug-28 init fix: DRC-on arms (drcnf/drc2) boot AB, run DRC natively
(trans 37, runs_m settles at 127,032 = master leaves blocks early and lives in the
interpreter+fastloop fold path at 0x060038xx; slave runs the MUL-loop block, runs_s grows),
but hit WWDG resets at emu 31f (drcnf) → 159f (drc2, after bug-29). bug-29 = backward
BT/BF excluded from the hook (loop heads must reach the fastloop gate inside case 0x8;
rig stays hash-clean on both ROMs, avg host improves 12.7M→10.5M on Doom). Live 4s sample:
host pc inside drc_emit, lookups 2.0M, "misses" counter reads 12e8 (mapping suspect),
hot(3-miss)+rej negative cache confirmed active → translate attempts are rare; the master
is NOT storming. WWDG timing varies 4–12s => margin problem, not deterministic blowup:
on-device block round-trips (slave MUL head block + per-iter interpreter branch) plus
per-dispatch hook tax leave too little watchdog margin.

Verdict: for this title's boot phase the DRC's on-device upside is marginal (master is
already folded by shape-3; slave loop barely benefits) while WWDG risk persists. Release
keeps DRC compiled out (no flag). Evidence: artifacts/2026-09-32x/case-drc-deviceaudit/
(+ bug29 logs ab_drc_bug29/doom_drc_bug29 in case-drc-bug28-init area, drc2_cap.log),
device restored to w32x_noff (4fix+shape-3, AB 12.4fps visual baseline). DRC correctness
remains proven (rig, both ROMs, unlimited coverage); on-device performance is a separate
future axis (needs: miss-path tax removal, per-block profit gating, watchdog margin).

## 12.12 WWDG root cause + DRC on-device speed verdict (2026-09-28)

WWDG resets decoded: wdog_refresh() lives ONLY in the LCD output paths; WWDG1 is
prescaler 128 / counter 127 ~= 277ms — any single emulation frame longer than that
resets the MCU. DRC arms' translate/flush transients blew the window at emu 31..159f.
Fix (host-only, guest-invisible): wdog_refresh() safety net once per frame in the md32x
frame loop (main_md32x.c) — bounds exposure to the frame time itself; also a product
robustness win (kept for release).

With resets gone the on-device DRC speed finally became measurable: w32x_drc3
(bug-29 + safety net) survives indefinitely, 438 frames / 75s = **5.8fps vs noff 11.8**
— the DRC is a consistent ~2x SLOWER on-device (rig's QEMU numbers never showed this:
translation execution in AHB SRAM + per-block overheads are the suspects; AHB-vs-AXI
pool placement untested — future axis). Verdict: DRC stays compiled out for release;
correctness remains proven (rig, both ROMs); the frame-loop watchdog safety net ships.

## 12.13 Residual-bottleneck audit + AXI-pool attempt (2026-09-28)

Fresh census on the shipping config (noff profile, PC_HIST): the master poll loop
(0x060038ee, formerly 28%) is GONE from the top — shape-3 folds it. What remains is
pure compute: slave MAC kernel 0x060003e2..0x06000432 (~17%, BFS back-edge, MULS/STS/
ADD chain) and a master pixel-pump at 0x060068xx (MOV.W @(R0,R0)/ADDC/MOV.W @-R6).
CDF 50% at ~#50 — long tail of real-work loops. No foldable polls left => interpreter
axis only.

AXI-pool attempt for the DRC (AHB ~2x-slow hypothesis): rej/hot tables moved to a
dedicated AHB overlay (0x30000000, 2K; RAM_EMU bss shrank 0xEB8), idx 1024->512,
pool 3K. `> RAM_EMU` region placement put the pool at the overlay HEAD 0x2404b000 —
overlapping executing .drcx code (unsafe, arm discarded, never flashed). Pool reverted
to AHB @0x30000800; the 512-slot + rej/hot-AHB layout is validated rig-clean on AB
(fb c5aff44d/650a9438/393ea6e6, snd 60c1049b, GATE3 PASS) and preserved in arm
w32x_drc4 for a future proper tail-VMA overlay rework. Conclusion unchanged: C-level
residuals exhausted; further on-device speed needs the DRC AXI rework (or another
translation strategy) as its own project.

## 12.14 shape4 (record-walk kernel fold) — outcome: no gain, archived (2026-09-28)

Census target: slave MAC kernel 0x060003e2-0x432 dispatcher loop (cens2: 31% of slave
window, 2.48M iters @18cyc). Full decode: dispatcher (MOV.L @(0x18,R14),R0 / MOV.L
@(0x0C,R14),R1 / JMP @R0 / delay MOV.L @(0x08,R14),R8) + two per-record target kernels
at 0x41c (zero-generating: MOV#0/MULS/SHLR16/STS/ADD/DT R7/BF/S back edge) and 0x3ee
(accumulating MULU kernel), exit epilogue 0x434-0x452 (literals/ADDC/STS MACH/CMP-PL).
BF/S at 0x430 (0x8fd7) is the per-iteration back edge (18 cyc/iter confirmed).

Fold (shape4): signature-matched 15-word opcode check (address-relative, ROM/SDRAM
agnostic), per-iteration [R14+0x18]==0x0600041c re-read, kmax=min((ic-1)/18, R7-1),
zero-kernel semantics (R0/R1/R2/MACL=0, R7-=k, R14+=0x20k, R8=last delay load,
no_polling parity). Bugs found: game opcode 0x8Fxx is BF/S (was outside the fastloop
gate — entry never reached), reject-cache permanent blocking on kk==0 (bug-30), word
order of target assembly (bug-31 non-bug, reverted). All hash-clean throughout.

Final measurement (same run): pchist 8fd7 dispatches = 2,477,839 but fold successes
tot=1 (k=12). Conclusion: the 041c zero-kernel records are a tiny minority of the
2.48M iterations; the dominant records target the 3ee accumulating MULU kernel, whose
semantics (MULU.W, ADDC carry chains, MACH/MACL state) were not folded. shape4 as
implemented gains ~0; kept in tree (harmless, hash-clean, folds only when target
matches). Next-axis options: (a) extend to 3ee kernel semantics (host-side MULU+carry
simulation — new cycle of work), (b) command-poll loop 0x060003c0 (6.6%, 2-stage
constant-address word poll — shape3-style extension), (c) conclude perf axis.

## 12.15 shape4 record-walk zero-kernel fold — device +32% (2026-09-28)

Loop (slave MAC kernel, game-phase dominant): dispatcher 3e2-3e8 (4 insns
+ delay) + kernel 41c-432 (zero-generating: MOV#0/MULS/SHLR16/STS MACL/ADD/DT,
ends 8fd7 BF/S back edge with 7e20 delay) + exit tail 434-452 (accumulate
epilogue, runs once). 18 guest cycles/iter (kdump boundary measurement).
Census: 041c target 100% of dispatcher arrivals at 3e4 ([tgtc], boot-phase
2.47M iterations); s4src: 93.7% of 3e2 arrivals came from 0x432 (the BF/S
delay slot) = 2.21M back edges.

Fold (fastloop case-0x8 shape, sh2pico.c): entry at 8fd7 dispatch
(opcode&0xff00==0x8f00 gate added — it was missing, bug-30 family),
15-word signature match (opcode constants, address-independent), per-fold
record walk with per-iteration [R14+0x18]==target re-verification +
rollback on mismatch (bug-33), kmax=min((ic-1)/18, R7-1), state: R0/R1/R2/
MACL=0, R7-=k, R14+=0x20k, R8=[R14+8] last delay load, no_polling parity
(bug-27), T=0 (pending BF/S re-taken by real dispatch). Fold-count check
first (signature match => unconditional return, no reject-cache pollution —
bug-32: last-iter r7<2 fallback used to poison reject slot 0x430 forever).

Bugs 30-33 (all fixed, each with observed-case comments): 30 reject-cache
poisoning via fallthrough, 31 word-order assembly red herring (revert),
32 r7>=2 gate => reject poison (the real blocker: 2.21M dispatches, 4 folds),
33 R8 off-by-one + last-record tgt unverified (snd hash cc415f4a mismatch
with 2.0M folds — fixed to full-verification with rollback).

Verification (file-redirect protocol): AB 600f production build
(text 2,947,432): folds=2,031,645, fb c5aff44d/650a9438/393ea6e6, snd
60c1049b, energies golden-exact, GATE3 PASS — **avg host 14,045,782 vs noff
15,802,826 = -11.1% pure rig reduction**; avg sh2 162,538 (25% of guest
dispatches absorbed). Doom 600f regression: fb 9cd2510f/22ee77a6/ced1080b,
snd 97becd29, sh2 69,878 golden-identical, folds=0 (kernel absent in Doom —
zero interference).

Device: arm w32x_s4 (32x.bin md5 68b85c12, no extra flags; build required
moving YM2612 chip state .bss.ym2612 3,216B from ITCM bss to RAM_EMU bss —
shape4 code growth tipped "MD32X ITC BSS overflow", ld change documented).
Flash + 75s capture: emu=1311 frames = **17.5 fps vs w32x_final 13.2 =
+32% device gain** (superlinear vs rig -11.1%, same pattern as shape3:
rig -18% -> device +30.6%). Crumb cause=0 (no crash) through capture.

Artifacts: case-s4-fold/ (ab_s4v/w/x, doom_s4 logs + rigs), arm w32x_s4.

## 12.16 shape-5 command-poll fold — clean, zero rig gain (2026-09-28)

Structure (DasmSH2-verified, kdump4/5): main poll 0x3c0 (d834 R8=[0x494] / 6181 R1=cmd /
8581 R0=[pA+2] ref / 3100 CMP/EQ / 8b02 BF fw-exit / a229 BRA 0x820, delay NOP) +
aux poll 0x820 (d808 R8=[0x844] / 6081 R0=status / 2008 TST / 89e5 BT->0x7f4 — NO delay
slot) + backedge 0x7f4 (ade4 BRA 0x3c0). Persist = cmd==ref AND status==0. Real iteration
measured 17 cyc (BT no-delay => 0x828 MOV#0 not in loop; 5+2+1+3+3+2+1; bug-34: first
impl charged 18 — the +1/cycle drift broke snd hash).

Result: with 17cyc + full-record verify (shape-4 rollback pattern) — AB 600f
fb/snd/energy bit-exact golden, GATE3 PASS, s4 folds intact. Production pure build avg
14,212,153 vs s4 14,045,782 = +0.12% (noise): the true poll instance (cmd==ref, c1=0x3c)
is rare; the dominant 0x3c0 arrival (r1=0x5c2) is a 348cyc command-processing loop the
fold correctly never touches. Conclusion: kept in tree behind gnw_sh2_s5_off (default
active but harmless), no device flash (no rig basis). Axes exhausted at C level: master
SDRAM mailbox (shape-3) + slave MAC record-walk (shape-4) captured; remaining hot code
is real compute (pixel pump / MULU accumulate) => interpreter ceiling reached; DRC
(rejected 2x slower on device) is the only bigger lever.

## 12.17 shape6 (3ee record-walk MULU kernel fold) — attempted, rejected (2026-09-28)

Full kernel decode (DasmSH2-verified, /tmp/opencode/k3ee): dispatcher(3e2-3e8: R0=[R14+0x18]
target / R1=[R14+0x0C] / JMP@R0 / delay R8=[R14+0x08]) + 3ee kernel (SUB R8,R13 / DMULS.L
R1,R13 -> MACH:MACL / ADD R8,R13 / R1=[R14+0x10] / MACL>=R1 ? BT->418 patch path (writes
[R14+0x18]=0x0600041c, switches record to zero-kernel) : nt path (R2=[R14+0x04] / R8=MACL&ffff
/ MACL>>=16 / +=R2 / byte-table MOV.B @R0+ twice / -=R3 / MULS.W / >>8 / +=R3 delay) + common
tail 420-432 (MULS x2 / R12+= / R11+= / DT R7 / BFS -> 3e2 / R14+=0x20 delay).

Fold implemented (shape6): per-iter full simulation (record RL x5, byte-table RB x2 with
fast-addr guard, DMULS s64, CMP/HS break on patch path, accumulators), 041c zero-semantics
shared. Result: fb clean but snd diverged on every cycle candidate (37/38/36 each different
snd hash) = semantic/invocation divergence, not cycle error. Pure 3ee per-iter cycle not
directly measurable (s4cyc2: BT->418 patch-path phase-locked oscillation dominates samples).
Separated test (3ee break, 041c-only): snd 60c1049b clean -> 3ee sim was sole contaminant.

Verdict: REJECTED for 3ee (semantics risk + tiny volume: +78,899 iters vs 041c 2,029,495).
Final production state = 041c-only fold (= shape4 semantics, bug-30/32/33 hardened):
avg 14,422,818 (vs s4 14,045,782; +0.27% = s5/s6 signature-check overhead, documented),
all hashes golden-clean, GATE3 PASS. Device best remains w32x_s4 17.5fps (s6 not flashed —
rig-equivalent to s4 within noise; no device delta expected).

Remaining hot regions after shape3/4: pixel pump 0x060068xx (real arithmetic, interpreter-
bound), GBR bit-poll 0.86%, C-level interpreter exhausted per prior analysis.

## 12.18 CORRECTION — w32x_s4 device regression: black screen + no audio (2026-09-28)

User visual/audible check (the only valid output verification) revealed w32x_s4 (shape4 arm,
"17.5fps") produced NO video and NO audio on device despite emu frame counter advancing
(1311f/75s) and no crash crumbs. rig hashes were all golden-clean — the regression is
device-only and invisible to QEMU. Prime suspect: the s4 build's ld experiment moving
pico__sound__ym2612.o (.bss.ym2612, 3216B) into .overlay_md32x_bss (ITC BSS overflow
workaround) — buffer works in QEMU but breaks real-hardware access (region/cache attribute
or DMA/audio-path dependency). Device restored to w32x_final (intflash verified dc4e7b56,
audio+video confirmed by user).

Verdict corrections: (a) shape4 "device +32%" claim RETRACTED — the 17.5fps measurement was
taken on an output-dead build; shape4's real device gain is UNMEASURED. (b) Best verified
device config = w32x_final (4fix+shape3+WWDG safety net), AB ~13.2fps, screen+audio OK.
(c) To salvage shape4 on device: rebuild without the ym2612 bss move (need alternate space,
e.g. shrink another ITC resident or move a cold object instead) — future axis.

## 12.19 Reboot black-screen defect — root cause & fix (2026-09-29)

Defect (facts): after ANY re-boot of the same image (SWD reset, power cycle,
battery disconnect), 32X games show black screen + no audio while emulation
advances (emu frames progress at ~88-95% of first-boot rate), no crash
(crumb cause=0), both SH2 cores alive in game init code, vdp_regs[0] stays
0x8000 (mode0 blanking; normal boot reaches 0x8001), fb1/fb2/Draw2FB all
zero, skipFrame=0, drawn==emu. First boot after flash = normal (4x observed).
Reproduced on final/y/x arms alike => experiment-arm independent; core defect
present in final.

Root cause (established by code + controlled diff): main_md32x reboot path
calls PicoInit→PicoLoadMedia→CartInsert→PicoPower each time, but the 32X guest
RAM lives in a static overlay buffer (gnw_32xmem, RAM_EMU, survives reboots).
Pico32xStartup (32x.c:337-365) gated its init on `Pico32xMem == NULL` —
Pico32xShutdown (the only NULL-setter) never runs on forced reboot, so on
re-entry the guest SDRAM/DRAM/palette contents from the previous session
remained, and the game's init took a different (stalled) path before VDP mode
set. Controlled SDRAM diff (normal vs black snapshots): black is a strict
subset of normal content (no black-only residue) — consistent with stale-state
divergence rather than corruption.

Fix (minimal, missing-init only): in Pico32xStartup's GNW_32X_CORE path,
always memset the static guest RAM (gate removed); sh2_init still gated on
NULL (context re-alloc via emu_32x_startup hook). Rig: AB 600f fb/snd hashes
golden-identical (first-boot unaffected), avg 13,609,540.

Verification: arm w32x_y2 (intflash 0a8a2261 / 32x.bin 66,281 / xip 744,896):
first boot 1247f/75s (~16.6fps) with screen+audio user-confirmed; SWD reset →
re-boot 1219f/75s (~16.3fps) progressing — [screen confirmation pending at
write time; to be amended]. Team-lead session ended mid-verification; work
continued per user instruction.

Also from this saga: XIP-flash-cache pollution hypothesis (m6259-m6295) was
RETIRED as primary cause (black re-occurred with same image + battery
disconnect); the 2-stage reflash "recipe" coincided with first-boot-after-
flash = the always-clean condition. Autosave corruption (repeated forced
resets) was a real secondary incident (files deleted; evidence not preserved).
A/B/A performance numbers on hold during this work: A1 final 17.3fps / B y
15.6fps (post-save-delete fresh boot scene) — pending controlled re-run.

Addendum (same day): Doom regression with the fix — fb 9cd2510f/22ee77a6/
ced1080b, snd 97becd29, avg 5,560,319, sh2=69,878, GATE3 PASS — golden-
identical; fix is interference-free on both ROMs. Artifacts:
artifacts/2026-09-32x/case-reboot-blackscreen/ (snapshots, logs, builds).

## 12.20 shape-7 pixel-pump fold — REJECTED (2026-09-29, axis close-out)

Target: master DDA pixel-scaler pump 0x0600680a-0x684a (8x unrolled
[ADDC R13,R8 / MOV.W @(R0,R0),R1 / ADDC R14,R0 / MOV.W R1,@-R6] + DT R7 +
backward BF/S), ~5% of master guest dispatches (cens2). Pure guest-memory
R/W per iteration -> C re-execution fold (shape-3 style) with exact ADDC
carry-chain reproduction; ea=r0+r0 self-doubling source, r6 -=2 dest via
real write handler.

Result: rig-clean on FIRST 34cyc charge — AB 600f diagnostic arm
fb c5aff44d/650a9438/393ea6e6, snd 60c1049b, energy exact, GATE3 PASS,
s4 folds 2,031,645 preserved. Production arm (no diagnostics): avg host
13,772,843 vs adopted S4-only 13,609,539 = **+1.2% regression**. Root
cause: 33-word signature check executes on EVERY backward BF/BFS dispatch
across the whole game, while pump loop instances are short (kdump6
observed r7=0 at dispatch) — check cost > fold gain.

Verdict: REJECTED — code retained behind #if defined(GNW_S7_ON) (default
off, ledger-documented like shape-5). Device anchor unchanged: w32x_y2
(S4-only production; predates this experiment, no reflash needed).

## 12.21 32X performance axis — CLOSED (cumulative)

Adopted chain (device w32x_y2, intflash 0a8a2261 / 32x.bin 657ae932):
4fix (MD VDP no-change skip) + shape-3 SDRAM mailbox poll fold (C
re-read, unsigned clamp, bug-27 DT no_polling parity) + shape-4 041c
record-walk fold (.drcx.fold noinline XIP append, bugs 30-33) + WWDG
safety net + 32x.c Pico32xStartup always-memset (reboot-blackscreen
mitigation attempt). Rig: AB 13,609,539 insn/f vs noff 15,802,826
(-13.9%), both ROMs golden-exact. Device: first-boot 16.6-17.3 fps vs
base1 12.85 (attract, post-autosave-clean state; A/B/A drift analysis
pending due to reboot-blackscreen core defect). Rejected axes: shape-5
cmd-poll (clean, +0.12%), shape-6 3ee MULU kernel (snd pollution,
volume tiny), shape-7 pixel pump (+1.2%), fullfill compositor (device
black), DRC on device (2x slower, WWDG). Remaining known bottleneck:
interpreter C-level exhausted (pixel pump / MAC kernel are real work);
next material axis would be a working DRC, out of scope. Reboot
black-screen core defect (68K guest never starts 32X boot protocol on
reboot; first boot only) documented in case-reboot-blackscreen —
separate bug, pre-existing in final baseline.
