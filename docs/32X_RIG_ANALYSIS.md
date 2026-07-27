# 32X QEMU M7 Rig — 구조 분석 및 측정 설계

> ⛔ **0727: 여기 설명된 리그는 삭제됐습니다**(32X 코어와 함께 — `docs/32X_CLOSED.md`).
> 구조 설명은 다음 코어의 리그를 세울 때 참고 자료로 남깁니다.

측정 주도 32X 성능 개선 작업의 분석 문서. 실행 가이드는
`docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md`를, 결과는 `docs/32X_PERFORMANCE_RESULTS.md`를 본다.

## 환경

- 워크트리: testbed 메인 (`/home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd`),
  브랜치 `perf/32x-histogram` (testbed HEAD `f19ac3ae`, baseline 조상 `93fb0a70`).
- picodrive submodule: `4bdb3d7d0737e41c7f52990653693a87d579dd68` (확정).
- toolchain: `arm-none-eabi-gcc 13.2.1` (CLAUDE.md의 15.2는 문서상 값; 실제 13.2).
- qemu: `qemu-system-arm 8.2.2` (Debian), 머신 `mps2-an500`, `-icount shift=0,align=off,sleep=off`.
- ROM 코퍼스: `/tmp/32x-prof/roms/*.32x` (원본 SEGA big-endian, 영문 symlink). SHA-256 은
  `/tmp/32x-prof/rom-hashes.txt`. git 에 넣지 않는다.
- SegaCD untracked(`tools/m7_qemu_rig/{rig_mcd.c,run_mcd.sh,rig_md.c,run_md.sh,md_shim/}`,
  `tools/segacd_harness/`, `Core/Src/porting/segacd/`)은 건드리지 않는다.

## rig_32x.c 구조 (`tools/m7_qemu_rig/rig_32x.c`, 506줄)

같은 trimmed picodrive 소스셋(`-DGNW_32X_CORE -DEMU_G68K -DTABLES_FULL -D_USE_CZ80`)을
device overlay 와 동일하게 크로스컴파일해 QEMU Cortex-M7 의 Thumb 스트림 위에서 돌린다.
host build 가 닿지 못하는 Thumb 전용 fault 클래스(map 함수포인터 bit0 등)를 여기서 잡는다.

**init 순서 = libretro (LAZY).** `PicoInit` → 옵션/사운드세팅 → `PicoLoadMedia`(game.32x,
zero-copy, PRE-byteswapped ROM) → `PicoLoopPrepare` → draw/sound 바인딩. `Pico32xStartup`
을 미리 부르지 않는다: 게임의 68K 부트코드가 `0xA15101` 에 ADEN 을 써야 `PicoWrite8_32x`
가 LAZY startup 을 호출하고, 그 안에서 `emu_32x_startup()` 이 draw 포맷/버퍼를 재적용한다.
미리 부르면 VF 의 68K 가 `0x88088e` 의 nop/bra idle 루프에 영원히 걸린다(`rig_32x.c:407`).
`PicoReset` 도 부르지 않는다 (`PicoLoadMedia → PicoCartInsert → PicoPower` 가 이미 리셋).

**프레임 루프 (`main`, 441–481):** 매 프레임 `skipf=skip_this_frame(f)` →
`PicoIn.skipFrame=skipf` → `PicoIn.pad[0]=pad_script(f)` → `t0=rig_timer_now()` →
`PicoFrame()` → `t1` → `insn=(t1-t0)*ipt/1000`. `g_sh2_insns` 델타로 게스트 SH-2 명령수.
warmup(`RIG_WARMUP=20`) 이후 `tot/mn/mx/sh2_tot` 누적, drawn/skip 분리, first/last-500 드리프트
윈도우. `CK_A=99 / CK_B=299 / CK_LAST` 에서 fb 체크섬.

**GATE3 (500–505):** `(nb100||nb300||nbend) && (cks 들이 다름) && sh2_tot>0`. PASS = 프레임이
진행되고 fb 가 살아있고 SH-2 가 실행 중. 이것이 유일한 정상부팅 게이트다.

## phase profiler (114–191, `-DRIG_PHASE_PROF`)

picodrive 의 pprof probe(컴파일 단위 곳곳의 `pprof_start/pprof_end`)가 `pp_counters` 배열에
icount 타이머 값을 쓴다. `pprof.h` 가 `pprof_get_one()` 을 `rig_timer_now()` 로 라우팅하므로,
각 bucket delta = executed-instruction count(교정 `ipt_x1000` 로 insn/tick 환산, 보통 40).
중첩 phase 는 `pprof_end_sub` 로 enclosing pause → disjoint bucket. leak(refcount≠0) 은
pprof scope 가 early-return 으로 빠져나간 것 = 데이터 의심.

phase 테이블(9): `m68k / msh2 / ssh2 / z80 / fm(ym2612) / pwm / sound / draw(MD VDP) /
draw32x(compositor)`. `other(sched/ev/mem)` = `pp_frame` 합에서 위 합을 뺀 나머지.
`phase_snapshot()` 은 warmup 끝(f=20)에서 base 를 저장, 이후 `phase_insn()` 가 post-warmup
평균 insn/frame 을 낸다. `phase_report()` 가 sh2 host/guest 비와 leak 도 같이 찍는다.

## §4 세 baseline 프로파일 (PHASE_PROF=1, RIG_PAD_SCRIPT, 1200프레임)

모두 `PHASE_PROF=1` + 1200프레임 + 각 ROM 마다 고유 `RIG_OUT`:

| 프로파일 | EXTRA_DEF | 의미 |
|----------|-----------|------|
| **base**    | `-DRIG_PAD_SCRIPT`                                | 기본(fastloop ON), device 의 정상 부하. 패드 스크립트로 gameplay 측정. |
| **skip3**   | `-DRIG_PAD_SCRIPT -DRIG_SKIP3`                    | device frameskip(draw 1/3). drawn vs skipped 평균 + headroom. |
| **fastoff** | `-DRIG_PAD_SCRIPT -DGNW_SH2_FASTLOOPS_DEFAULT=0`  | fast-loop OFF. 제거된 idle 루프가 드러남. 잔존 핫셋 비교 기준. |

> `RIG_PAD_SCRIPT` 없이 돌리면 no-keys 정지화면 베이스(파이프라인/게이트 검증용).
> `RIG_SKIP3` = `PicoIn.skipFrame=1` on 2-of-3, 체크섬 체크포인트는 drawn 프레임에 맞춤.
> `RIG_STATE_TEST` = §7 round-trip(save 120 warm-up 후, 30프레임, restore, 재생, 체크섬 일치).

실행: `RIG_OUT=tools/m7_qemu_rig/build/<name> PHASE_PROF=1 RIG_TIMEOUT=2400 \
bash tools/m7_qemu_rig/run_32x.sh /tmp/32x-prof/roms/<rom>.32x 1200`

## 벤치마크: Doom base (fastloop ON, no-pad, 1200프레임)

```
[32x-qemu] done 1200 frames  avg host=22658258  min=18587920  max=27611200  avg sh2=260732
[32x-qemu] drift: first500 avg=20813710  last500 avg=25067463
[32x-phase] host insn/frame by phase (1180 frames post-warmup):
  m68k (interp+bus)       523261    2.3%
  msh2 (interp+bus)     17810052   78.6%   ← 압도적 1위
  ssh2 (interp+bus)      1555525    6.8%
  z80  (interp+bus)       166878    0.7%
  fm   (ym2612)           264925    1.1%
  pwm  (chip)               8271    0.0%
  snd  (psg+dac+mix)      117458    0.5%
  draw (MD VDP line)      895106    3.9%
  32x  (compositor)      1179268    5.2%
  other(sched/ev/mem)     137484    0.6%
  PicoFrame TOTAL       22658228  100.0%
[32x-phase] sh2 host/guest: 19365577 host / 260732 guest insn = 74.273
GATE3 PASS  fb f99=9cd2510f f299=22ee77a6 f1199=93ec1cd8 (모두 변경, nb=1)
```

**결론:**
1. **병목은 msh2 78.6%** 에 집중. ssh2 포함 SH-2 합 85.4%. 나머지 phase 는 각각 ≤6.8%.
2. **sh2 host/guest = 74.3x.** 게스트 SH-2 명령 1개당 호스트 ARM 명령 74개 소모 = 인터프리터
   오버헤드가 거의 전부. 이 74x 안에서 fast-loop 가 이미 제거한 idle 루프와, **잔존 핫 루프**
   를 구분하는 것이 §6 의 목적이다.
3. **drift first/last-500 = 20.8M → 25.1M.** 데모가 진행되며 부하 증가. 단일 평균만 보지 말 것.
4. fastloop OFF 대비: 이전 세션 `doom_off.log`(600f, no-pad) 평균 33.8M vs 지금 22.7M ≈ **33% 감소**.
   가이드의 "Doom 28–44% 개선" 범위 내. fast-loop 가 실제로 msh2 의 큰 덩어리를 빼고 있다.

**다음 병목 타겟:** msh2 78.6% 의 내역을 SH-2 guest-PC 히스토그램(§6)으로 쪼개, idle/폴링 루프인지
연산 핫패스인지 가린다. fastoff 빌드로 "fast-loop 가 지운 루프" 를 먼저 보고, on 빌드로 잔존 핫셋.

## §5 RIG_FRAME_HIST 구현 설계 (rig_32x.c, 미구현)

`#ifdef RIG_FRAME_HIST` 가드. off 시 byte-identical. icount 정확도로 per-frame cost 의
분포(p50/p90/p95/p99)를 낸다 — 평균만 보면 bimodal(drawn/skip, 데모구간) 이 감춰진다.

- **저장:** `static uint32_t s_fh_drawn[RIG_FRAMES], s_fh_skip[RIG_FRAMES];` + 카운트. rig RAM 4MiB
  안에서 1200×4×2 ≈ 9.6KiB. warmup(f<20)은 인덱스에서 제외(`f-RIG_WARMUP`).
- **삽입점:** 루프 `if (f >= RIG_WARMUP)` 블록(457–465) 안의 drawn/skip 분기(461–462) 옆.
  ```
  if (skipf) { tot_skip += insn; n_skip++; s_fh_skip[n_skip-1] = insn; }
  else       { tot_drawn += insn; n_drawn++; s_fh_drawn[n_drawn-1] = insn; }
  ```
- **보고:** `phase_report()` 호출(495) 앞. 각 배열을 복사→`qsort`(uint32)→p50/p90/p95/p99 인덱스,
  min/max, 20-bin 히스토그램(bin 경계는 (max-min)/20, 카운트는 64-bit). drawn/skip 각각 출력.
- **검증:** 기본 빌드(RIG_FRAME_HIST off)의 체크섬/size/GATE3 가 on 빌드와 동일한지 확인.

## §6 RIG_SH2_PC_HIST 구현 설계 (sh2pico.c, 미구현)

`#ifdef RIG_SH2_PC_HIST` 가드. 게스트 SH-2 의 핫 PC 와 루프 엣지를 잡는다. rig RAM 전용
(device BSS 아님). per-core(master/slave) 8192-slot sparse open-addressed 테이블:
key = 32-bit PC + occupied 플래그, value = 64-bit count + direct/delay 분리 카운트.
총 ~192KiB(rig 4MiB RAM OK).

- **훅 위치:** 메인 디스패치 루프의 `RIG_SH2_TICK()` 호출점 = **sh2pico.c:287**(루프1) 과
  **:399**(루프2). 이 지점에서 `sh2->ppc`(명령 주소), `opcode`, 그리고 직전 경로(delay slot
  분기 257/387 vs 일반 278/393)가 모두 확정된다.
  - direct vs delay 분리: 루프 진입 시 `sh2->delay` 분기(257/387)를 거쳤으면 delay bucket,
    일반(278/393)이면 direct bucket. delay slot 카운트는 분기 명령의 진짜 타깃을 가린다.
  - master/slave: `sh2->is_slave` 로 테이블 선택(각 코어 독립 8192-slot).
- **fastloop OFF 먼저, ON 다음.** OFF 빌드에서 fast-loop 가 지운 루프(BRA$/NOP, DT Rn 카운트다운)
  가 핫셋으로 드러나고, ON 빌드에서 잔존 핫셋을 본다. 두 차이가 fast-loop 의 효과 영역이다.
- **출력:** main 끝에서 top-50/core + 루프엣지 뷰(`prev_pc→cur_pc` 전이 또는 backward branch).
  `sh2dasm.c DasmSH2()` 옵션으로 명령 디코딩.
- **수락 기준:** 핫패스가 게스트 명령 3–5% 이상, 다중 윈도우/ROM 에서 재현, 명령 시퀀스와
  제어흐름 엣지 파악됨. MMIO/통신/타이머/VDP/PWM/FIFO 폴링 루프는 **reject**(사이드 이펙트).

## 스크립트/인터페이스 요약

`run_32x.sh <rom.32x> [frames=600]`. 환경변수: `RIG_OUT`(빌드디렉토리, 병렬 lane 필수),
`PHASE_PROF=1`(`-DRIG_PHASE_PROF`), `EXTRA_DEF`(추가 define), `RIG_TIMEOUT=1800`. 스크립트가
입력 ROM 을 python 16-bit byteswap → `OUT/rom.32x` → objcopy `.rom` section.
**주의:** 빌드 산물 `rom.32x` 는 이미 byteswapped 이므로 재입력하면 double-swap(금지).

## 현재 상태

- §1 셋업 완료(브랜치/ROM 풀/SHA). §4 첫 벤치마크(Doom base, no-pad) 완료 → 파이프라인 정상,
  GATE3 PASS, phase report 정상. 위 분석은 이 데이터에 근거.
- 남은 §4: base/skip3/fastoff 세 프로파일 × 코퍼스(Doom winner + Kolibri/Stellar control).
- §5(RIG_FRAME_HIST)/§6(RIG_SH2_PC_HIST) 미구현 — 위 설계대로 구현 후 재측정.
