# 32X Performance Results — 측정 주도 최적화 결과

측정 방법과 가이드는 `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md`, rig 구조는
`docs/32X_RIG_ANALYSIS.md`를 본다.

## 베이스라인 환경

- 워크트리: testbed 메인, 브랜치 `perf/32x-histogram` (HEAD `f19ac3ae`).
- picodrive submodule: `4bdb3d7d0737e41c7f52990653693a87d579dd68`.
- qemu `mps2-an500`, `-icount shift=0`. arm-none-eabi-gcc 13.2.
- ROM SHA-256: doom=`0bfa9a1a…`, kolibri=`3e8bf99e…` (`/tmp/32x-prof/rom-hashes.txt`).

## 측정 1 — Doom BFS 카운트다운 루프 (keep ✓)

프로파일러(`RIG_SH2_PC_HIST`, fastloop ON, 50프레임)가 1순위 핫 루프를 특정:

```
0x02036f36: TST  R4,R4         ; T = (R4 == 0)
0x02036f38: BFS  $02036f36     ; if T==0 (R4 != 0) branch back, delay slot:
0x02036f3a: ADD  #$FF,R4       ; R4--  (0xFF = -1)
```
= `do { R4--; } while (R4 != 0)` 순수 카운트다운 지연 루프. 사이드 이펙트 = R4
감소뿐. 게스트 master SH-2 명령의 **62.68%** (top-3 합).

**기존 fastloop가 못 잡은 이유 2:** (1) `BFS`(0x8Fxx)가 pre-filter
`(opcode & 0xff80) == 0x8b80`(BF only)에 안 걸림. (2) body가 `TST+ADD#-1`(=CMP+DEC)
라 DT-only fastloop body 패턴이 아님.

### 패치

`external/picodrive/cpu/sh2/mame/sh2pico.c`:
1. pre-filter(라인 345)에 `(opcode & 0xff80) == 0x8f80`(BFS) 추가.
2. `gnw_sh2_fastloop()`에 BFS countdown case 추가: backward BFS + body 단일
   `TST Rn,Rn` + delay slot `ADD #-1,Rn` 매칭, `iter_cost=5`
   (TST 1 + BFS taken 3 + ADD 1), kmax 계산으로 icount 소모.
3. reject 캐시 동일 슬롯 사용.

### A/B (Doom 32X, 50프레임, PHASE_PROF=1)

| 항목          | base (no patch) | BFS patch   | 차이               |
|---------------|-----------------|-------------|--------------------|
| TOTAL host    | 20,530,253      | 8,563,188   | **−11,967,065 (−58.3%)** |
| msh2          | 16,363,937 (79.7%) | 4,383,798 (51.1%) | **−11,980,139 (−73.2%)** |
| ssh2          | 1,531,630 (7.4%)  | 1,544,598 (18.0%) | +12,968 (≈0, 패치 미접촉 확인) |
| 32x compositor| 1,159,464        | 1,159,461   | ≈0                 |
| draw (MD VDP) | 745,437          | 745,438     | ≈0                 |
| m68k          | 474,284          | 474,356     | ≈0                 |
| 게스트 sh2 avg| 246,898          | 70,115      | −176,783 (−71.6%)  |
| sh2 host/guest| 72.481x          | 84.552x     | (게스트 감소율 > 호스트) |
| **fb f49**    | de099d9f         | de099d9f    | **동일 ✓**         |
| GATE3         | PASS             | PASS        |                    |

**결론:** 이 루프 하나가 전체 32X 호스트 비용의 **58.3%**, msh2 phase 의 **73.2%**.
체크섬이 bit-identical → 사이클 정확, 디바이스 동작 불변. **keep 결정.**

`iter_cost=5` 는 체크섬 일치로 검증됨(정확한 사이클 비용).

## 측정 2 — Kolibri (no-match control, reject 조사)

가이드가 fastloop 0% 개선 ROM 으로 분류한 Kolibri(폴링 루프 의심)에 동일 패치 적용:

| 항목       | base          | BFS patch      | 차이          |
|------------|---------------|----------------|---------------|
| TOTAL host | 32,155,825    | 32,318,414     | +162,589 (≈0, 노이즈) |
| msh2       | 15,948,314 (49.5%) | 15,972,694 (49.4%) | ≈0      |
| ssh2       | 13,165,314 (40.9%) | 13,303,646 (41.1%) | ≈0      |
| fb f49     | 6c373493      | 6c373493       | 동일          |

**결론:** 패치 효과 0%. Kolibri는 이 카운트다운 패턴을 쓰지 않는다. SH-2 합
90.4%(msh2 49.5% + ssh2 40.9%)로 병목이긴 하나, 폴링/MMIO 루프일 가능성이 높아
reject 위험. **별도 프로파일링 필요.**

## 측정 3 — Winner ROM 일반화 (전부 no-match)

기존 fastloop winner 6종(가이드 QEMU 28–44% 개선 ROM)에 동일 BFS 패치 적용.
frame 수는 부팅 타이밍 차이: chaotix/vf/metalhead=50f, zaxxon/vr/tempo=150f(늦부팅).
**모든 fb 체크섬 base/patched 동일 → 사이클 정확.**

| ROM       | base TOTAL | patched TOTAL | Δ      | base msh2 % | base ssh2 % | 결정      |
|-----------|-----------|---------------|--------|-------------|-------------|-----------|
| chaotix   | 20,839,484 | 21,092,521   | +1.2%  | 45.8        | 42.0        | no-match  |
| vf        | 25,523,257 | 25,795,644   | +1.1%  | 43.0        | 45.7        | no-match  |
| metalhead | 25,576,717 | 26,008,478   | +1.7%  | 74.1        | 14.0        | no-match  |
| zaxxon    | 19,567,339 | 19,728,602   | +0.8%  | 85.0        | 0.3         | no-match  |
| vr        | 18,625,278 | 18,685,897   | +0.3%  | 28.7        | 51.9        | no-match  |
| tempo     | 23,287,066 | 23,470,473   | +0.8%  | 72.5        | 17.3        | no-match  |

**결론:** BFS + TST + ADD#-1 카운트다운 패턴은 **Doom 전용**. 다른 winner ROM 은 이
패턴을 쓰지 않는다. no-match 게임의 +0.3~1.7%는 pre-filter 에 BFS(0x8f80)를 추가한
probe/reject 오버헤드(모든 BFS 명령이 gnw_sh2_fastloop() 진입 후 reject 캐싱).

병목 분포는 ROM 마다 다르다 — zaxxon 은 msh2 85% 압도, vr 은 ssh2 52% 가 1위,
chaotix/vf 는 msh2·ssh2 균형(합 ~88%). 각 ROM 의 핫 루프는 별도 프로파일링(`SH2_PC_HIST`)
없이는 추측할 수 없다(측정 주도 원칙).

## 결산

| ROM       | 루프 정체                    | base TOTAL | 패치 후    | 개선          | 결정       |
|-----------|------------------------------|------------|------------|---------------|------------|
| Doom      | BFS + TST + ADD#-1 카운트다운 | 20.53M     | 8.56M      | **−58.3%**    | **keep ✓** |
| chaotix   | (다른 패턴)                  | 20.84M     | 21.09M     | +1.2% (노이즈)| 추가 조사  |
| vf        | (다른 패턴)                  | 25.52M     | 25.80M     | +1.1% (노이즈)| 추가 조사  |
| metalhead | (다른 패턴)                  | 25.58M     | 26.01M     | +1.7% (노이즈)| 추가 조사  |
| zaxxon    | (다른 패턴, msh2 85%)        | 19.57M     | 19.73M     | +0.8% (노이즈)| 추가 조사  |
| vr        | (다른 패턴, ssh2 52%)        | 18.63M     | 18.69M     | +0.3% (노이즈)| 추가 조사  |
| tempo     | (다른 패턴)                  | 23.29M     | 23.47M     | +0.8% (노이즈)| 추가 조사  |
| Kolibri   | (다른 패턴, 폴링 의심)       | 32.16M     | 32.32M     | +0.5% (노이즈)| 추가 조사  |

**종합:** BFS 카운트다운 패치는 Doom 1종에만 효과(−58.3%). 나머지 7종은 no-match.
Doom 은 단일 카운트다운 루프가 병목의 전부였던 특이 케이스; 일반적으로 32X 는
ROM 마다 서로 다른 핫 루프를 가지며, 각각 개별 프로파일링이 필요하다.

## 측정 4 — no-match ROM 개별 핫 루프 (`SH2_PC_HIST`)

BFS 패치가 no-match 였던 7종(가이드 winner 6종 + Kolibri)의 핫 루프를 `RIG_SH2_PC_HIST`
로 식별. 프레임 수는 부팅 타이밍(50f GATE3 FAIL ROM 은 150f). GATE3 전부 PASS.
로그: `/tmp/32x-prof/pchist/<rom>.log`.

### Kolibri — ssh2 86.6%, host/guest 516.9x (극도의 tight polling)

```
done 50 frames  avg host=193248317  min=32088520  max=403253200  avg sh2=367994
msh2 22727686 11.7% | ssh2 167478273 86.6%
sh2 host/guest: 190205959 / 367994 = 516.872
```
`host/guest = 516.9x` = Doom(74x)의 7배 — ssh2 가 극도로 짧은 tight loop 수백만 회전.

- #1-3 master (11.42% ea, 0x06001620-4): `MOV.W @R2,R1; TST R1,R1; BT $06001620` = **폴링 루프**.
- #4-6 slave (3.90% ea, 0x0207416a-c): `MOV.W @R2,R1; TST R1,R1; BF $0207416a` = **폴링 루프**.
- #7-9 master (3.84% ea, 0x06000b34-8): `MOV.W @R1,R0; TST R0,R0; BF $06000b34` = **폴링 루프**.
- #30-32 master (0.42% ea): `0x00000210 BT` 전역 카운트다운.

### Metal Head — msh2 74%, 1위 루프 59.41% (MMIO/GBR 폴링)

- #1-3 master (**59.41%!**, 0x0600b45c-60): `TST R0,R0; BFS back; MOV.W @(disp,GBR),R0`
  = **MMIO/GBR 폴링 루프** (GBR-indirect = 디바이스 레지스터 읽기).
- #4-5 slave (1.76%): `DT R12; BF` 카운트다운(소).
- #6-8 master (1.95%): `0x00000210 BT` 전역 카운트다운(Doom #8-10 동일).

### VR — ssh2 51.9%, master 1위 루프 87.23% (메모리 폴링)

- #1-3 master (**87.23%!**, 0x0600428e-92): `MOV.W @(8,R14),R0; TST R1,R0; BF back`
  = **메모리 폴링 루프**. (VR 의 msh2 28.7% 를 이 루프가 거의 독식.)
- #4-6 slave (4.5%): `NOP; DT R7; BF` 카운트다운.
- #14-16 master (1%): `0x00000210 BT` 전역 카운트다운.

### Tempo — msh2 72.5%, 1위 35.47% (폴링) + 13% (3D 연산)

- #1-3 (35.47%, 0x060024ae-b2): `MOV.W @R1,R0; TST R0,R0; BF back` = **메모리 폴링 루프**.
- #4-14 (~13%): `XTRCT/CMP/ADD/SHLR/DMULU.L/STS MAC` = **고정소수점 3D 연산**(reject).

### Zaxxon — msh2 85%, 1위 40% (직선 연산) + 15% (폴링)

- #1-5 (40%, 0x0600163a-46): `MOV.L/OR/MOV.L @R2` 직선 연산 코드, 사이드 이펙트 =
  reject(루프가 아닌 핫 함수).
- #6-10 (15%, 0x06001422-2e): `MOV.L @R11,R0; TST R0; BT; BRA; NOP` = **@R11 폴링 루프**.

### Chaotix — msh2 45.8% / ssh2 42% (균형), 연산·카피 루프

- #1-5 (20%, 0x06000872-8a): `SHLL; ROTCL; DT R11; BFS; ADD#$FF,R14` = 시프트/회전 연산 루프(reject).
- #6-10 (10%): `MOV.W @R4+; DT R1; MOV.W R2,@(R0,R8); BFS` = 메모리 카피 루프(reject).
- #11-15 (8%): 카피 + swap 루프.

### VF — msh2 43% / ssh2 45.7% (분산)

단일 핫 루프 부재. 각 PC ~3.2%로 분산, 함수 호출 체인(JSR/RTS). 특정 루프 타겟팅 불가.

## 결산 (8 ROM)

| ROM       | msh2/ssh2 %  | 1위 핫 루프                | 루프 분류        | 최적화 결정 |
|-----------|--------------|----------------------------|------------------|-------------|
| Doom      | 79.7 / 7.4   | TST+BFS+ADD#-1 카운트다운 62.68% | 순수 지연   | **keep ✓ (−58.3%)** |
| Metal Head| 74.1 / 14.0  | TST+BFS+MOV.W @(GBR) 59.41% | MMIO 폴링     | reject (단순 skip 위험) |
| VR        | 28.7 / 51.9  | MOV.W @(R14)+TST+BF 87.23%  | 메모리 폴링      | reject (단순 skip 위험) |
| Tempo     | 72.5 / 17.3  | MOV.W @R1+TST+BF 35.47% + DMULU 13% | 폴링 + 연산 | 폴링 reject / 연산 reject |
| Zaxxon    | 85.0 / 0.3   | 직선 연산 40% + @R11 폴링 15% | 연산 + 폴링   | reject (핫 함수) |
| Chaotix   | 45.8 / 42.0  | SHLL/ROTCL 루프 20% + 카피 18% | 연산/카피    | reject (실제 작업) |
| VF        | 43.0 / 45.7  | 분산 (각 ~3.2%)             | 함수 체인        | reject (타겟 부재) |
| Kolibri   | 11.7 / 86.6  | MOV.W @R2+TST+BF/BT 34% (3 루프) | 메모리 폴링 | reject (단순 skip 위험) |

### 종합

Doom 외 7개 ROM 의 병목은 3가지로 분류된다:

1. **메모리/MMIO 폴링 루프**(Metal Head 59%, VR 87%, Tempo 35%, Zaxxon 15%, Kolibri 34%):
   `TST; BF/BFS; MOV.W @mem/Rn/GBR` 패턴. 하드웨어/코어 동기화 대기(인터럽트나 다른 코어가
   플래그를 바꿀 때까지). GBA idle-skip 의 ALWAYS semantics(인터럽트로만 풀리는 루프)과 유사하나,
   32X 는 **하드웨어 종속성**(통신 레지스터/FIFO/공유 RAM)이라 폴링이 끝나는 조건을 인터프리터가
   모델링해야 한다. 단순 fastloop 확장(`r[rn]--` 식의 레지스터만 조작)으로는 안 됨 — 폴링 중인
   메모리 위치의 변경을 감지해야 한다.
2. **연산/카피 루프**(Chaotix 시프트 20%, Tempo DMULU 13%, Zaxxon 직선 40%): 실제 작업.
   정확성을 유지하면 스킵 불가(reject).
3. **분산**(VF): 단일 핫 루프 부재. 인터프리터 자체 비용.

순수 카운트다운 지연 루프(Doom 형태)는 8 ROM 중 1곳에서만. 32X 는 폴링이 압도적 주류.

## 측정 5 — VR PWM 폴링 cooperative idle yield (keep ✓)

VR pchist 가 master 0x0600428e-92 루프(게스트 87.23%)를 특정:
```
0x0600428e: MOV.W @(8,R14),R0   ; R0 = poll slot
0x06004290: TST R1,R0           ; T = ((R1&R0)==0)
0x06004292: BF  $0600428e       ; if T==0 loop
```
R14+8 = `0x20004038`. cache-through bit(0x20000000) strip → `0x00004038` = CS0 sysreg
region(`(a & 0x3ffc0) == 0x4000`), `a & 0x3e = 0x38` → **PWM register case**
(memory.c:808). VR은 PWM 레지스터 폴링. (초기 진단의 "cart ROM mirror" 추론은 오류;
task agent 가 CS0 sysreg/PWM 으로 정정.)

**근본 원인:** picodrive 인터프리터의 `p32x_sh2reg_read16`(memory.c:755)에서 comm(0x20-0x2e,
line 800)/H-count(0x04, line 766)는 `p32x_sh2_poll_detect()` 를 호출하지만, **PWM read
case(0x30-0x3e, line 804-812)는 `p32x_pwm_read16()` 만 부르고 poll_detect 를 안 부른다**.
RPOLL/CPOLL 이 세팅되지 않으니 `sync_sh2s_normal`(32x.c:552)의 IDLE skip/fast-forward 가
동작하지 않고, master 가 PWM 값을 full-speed 로 spin-read. (동일 gap 은 SDRAM read 경로에도
존재 — `p32x_sh2_poll_memory16/32`가 DRC 만 호출.)

VR 루프의 유일한 탈출 = PWM interrupt 또는 VINT. = GBA idle-skip ALWAYS semantics(인터럽트로만
풀리는 루프)와 동일. cooperative idle yield 의 정확한 타겟.

### 패치

`external/picodrive/pico/32x/memory.c` PWM read case 3 줄 추가(comm/H-count case 와 동일 패턴):
```c
    case 0x30/2: // PWM
    ...
    case 0x3e/2:
      p32x_sh2_poll_detect(a, sh2, SH2_STATE_CPOLL, 7);    // 추가
      sh2s_sync_on_read(sh2, sh2_cycles_done_m68k(sh2));   // 추가
      return p32x_pwm_read16(a, sh2, sh2_cycles_done_m68k(sh2));
```
`SH2_STATE_CPOLL` 재사용(PWM=32X 주변장치=comm 계열, addr 0x30-0x3e 로 comm 0x20-0x2e 와
충돌 없음). maxcnt=7. poll_addr 는 `a & 0x3e`(0x30-0x3e) 로 세팅, 매 폴링 반복에서 동일 →
`poll_cnt >= 7` 도달 시 `state |= CPOLL` + `sh2_end_run(0)` = timeslice yield. wakeup 은
`p32x_update_irls`(32x.c:68/75, PWM/VINT interrupt delivery 시 `SH2_IDLE_STATES & ~SLEEP`
clear)가 자동 수행 — 별도 write-side 훅 불필요(interrupt 로만 풀리는 루프이므로).

### A/B (VR 32X, 150프레임, PHASE_PROF=1, fb 체크섬 동일 = 사이클 정확)

| 항목 | base (no patch) | PWM poll patch | 차이 |
|------|-----------------|----------------|------|
| TOTAL insn/frame | 18,685,897 | 4,931,168 | **−13,754,729 (−73.6%)** |
| msh2  | 5,378,179 (28.7%) | 3,169 (0.0%) | −99.94% (폴링 완전 제거) |
| ssh2  | 9,715,720 (51.9%) | 1,286,121 (26.0%) | −86.8% |
| 32x (compositor) | 1,941,706 (10.3%) | 1,941,707 (39.3%) | 0 (이제 1위 phase) |
| draw  | 693,674 | 693,674 | 0 |
| m68k  | 576,947 | 576,959 | 0 |
| sh2 host/guest | 103.161x | 100.741x | — |
| fb f99 | 188bcea4 | 188bcea4 | 동일 ✓ |
| fb f149 | 14f27c5f | 14f27c5f | 동일 ✓ |

GATE3 PASS. master PWM spin 이 cooperative idle 로 전환되어 master phase 가 사실상 소멸(0.0%),
slave 도 master 대기로 막혀있던 분량이 풀려 −86.8%. 전체 −73.6% = 거의 4배.

### 범용성 검증 (PWM 패치는 VR 전용)

다른 4개 폴링 ROM 에 동일 패치 적용 → 전부 no-match(이들은 PWM 이 아닌 다른 region 폴링).

| ROM (frames) | base TOTAL | patched TOTAL | Δ | fb 동일 | 결정 |
|--------------|-----------|---------------|----|---------|------|
| Kolibri (50f) | 32,155,825 | 32,360,625 | +0.6% | 6c373493 ✓ | no-match |
| Metal Head (50f) | 25,576,717 | 26,008,478 | +1.7% | 4dddf644 ✓ | no-match |
| Tempo (150f) | 23,287,066 | 23,470,473 | +0.8% | 5d75780b ✓ | no-match |
| Zaxxon (150f) | 19,567,339 | 19,728,602 | +0.8% | c2f256fa ✓ | no-match |

patched 가 약간 느린 것 = poll_detect probe 오버헤드(PWM read 마다 호출). 사이클 정확.

## 측정 6 — SDRAM poll fastloop (BT/BF, 가드됨) (keep ✓)

Kolibri/Tempo 의 SDRAM 폴링 루프를 icount 폴링으로 안전하게 잡는다. b15 의 RPOLL 접근
(memory.c 수정, b13 옵션 a/b)이 Kolibri 의 양방향 tight 폴링(두 코어가 같은 slot 0x06000800/0x802
를 교대 producer/consumer 로 폴)에서 데드락/blank 실패한 것과 대조적으로, 이 패치는
sh2pico.c 단독 fastloop 로 매 iteration 진짜 `RW()` 읽기(사이드 이펙트 보존) + icount 차감
= 데드락 구조적 불가.

### 폴링 정체

- **Kolibri** master #13 BT 0x06001624, R2=0x26000802 → SDRAM 0x802 (slot 폴).
  slave #20 BF 0x0207416e, R2=0x26000800 → SDRAM 0x800. 양쪽 교대 producer/consumer.
- **Tempo** master #7 BF 0x060024b2, R1=0x06002518 → SDRAM slot 폴.
- **Metal Head** master BFS 0x0600b45e, GBR 기반 MOV.W @(4,GBR) → BFS(0x8f) 폴링이라
  이 BT/BF 케이스 타겟 아님(no-match 예상대로).

### 패치 (external/picodrive/cpu/sh2/mame/sh2pico.c, Doom BFS 위에 추가)

1. **pre-filter(라인 446)** BT(0x8980)/BTS(0x8d80) backward 추가.
2. **gnw_sh2_fastloop SDRAM poll case** (0xaffe 후, BFS countdown 전):
   - BT(0x89)/BF(0x8b) backward, body 2 insn(`MOV.W @Rm,Rn` 0x6nm1 또는 `MOV.W @(disp4,Rm),R0`
     0x85dm + `TST Rm,Rn` 0x2nm8 dest 일치).
   - **가드(핵심):** `target & 0xc6000000) == 0x06000000`(body in SDRAM) 체크.
     없으면 Metal Head 회귀 — body opcode fetch 의 `RW(target)` 가 CS0 sysreg/BIOS(0x0000xxxx)
     등 핸들러 영역을 타면 `poll_detect` 가 부작용으로 발화해 게스트 CPOLL/poll_addr 오염 → fb 변경.
     SDRAM fast path(MAP_MEMORY 직접 포인터)만 호출하면 안전.
   - 매 iteration `RW(pa)` 읽어 `R[dest]` 갱신 + T 계산(self_test 면 mask=0xffff). exit
     (t≠want_t_loop: BT 는 T==1 루프, BF 는 T==0 루프)시 T 설정·return / slice 고갈시
     T=want_t_loop·return(BT/BF taken). iter_cost=5. 매칭 실패 fall through(캐싱 X) →
     BFS/BF countdown 이 잡음.

### A/B (진짜, 진단 off, PHASE_PROF=1, fb 동일 = 사이클 정확)

**Kolibri (50f):**

| 항목 | base (Doom BFS) | patched (+SDRAM poll, 가드) | 차이 |
|------|-----------------|------------------------------|------|
| TOTAL insn/frame | 32,318,414 | 21,561,764 | **−10,756,650 (−33.3%)** |
| msh2 | 15,972,694 (49.4%) | 5,018,245 (23.2%) | **−68.6%** |
| ssh2 | 13,303,646 (41.1%) | 13,501,224 (62.6%) | +1.5% (slave 는 다른 패턴, 안 잡힘) |
| 게스트 sh2 avg | 367,994 | 213,059 | −42.1% |
| sh2 host/guest | 79.556x | 86.921x | — |
| fb f49 | 6c373493 | 6c373493 | 동일 ✓ |

**Tempo (150f):**

| 항목 | base (Doom BFS) | patched (+SDRAM poll, 가드) | 차이 |
|------|-----------------|------------------------------|------|
| TOTAL insn/frame | 23,470,473 | 17,566,616 | **−5,903,857 (−25.2%)** |
| msh2 | 17,049,928 (72.6%) | 11,095,339 (63.1%) | **−34.9%** |
| ssh2 | 4,078,106 (17.3%) | 4,128,993 (23.5%) | +1.2% |
| 게스트 sh2 avg | 270,465 | 185,704 | −31.3% |
| fb f149 | 5d75780b | 5d75780b | 동일 ✓ |

### 범용성 검증 (Metal Head 회귀 해결)

| ROM (frames) | base TOTAL | patched TOTAL | Δ | fb 동일 | 결정 |
|--------------|-----------|---------------|----|---------|------|
| Metal Head (50f) | 26,008,478 | 26,511,673 | +2.0% | 4dddf644 ✓ | no-match(가드로 회귀 해결) |

가드 전엔 fb 가 84b7d301 로 바뀌어 회귀였으나, body-in-SDRAM 가드 추가로 fb=4dddf644
(base 동일) 확보. Metal Head 는 BFS/GBR 폴링이라 이 BT/BF 케이스 타겟 아님 = no-match(+2% =
probe 오버헤드). 사이클 정확.

### 교훈

- **`RW(target)` 로 body opcode 를 fetch 할 때 region 을 확인해야 한다.** SDRAM/ROM/DRAM/
  data-array(MAP_MEMORY 직접 포인터)는 부작용 없지만, CS0 sysreg/comm/periph(MAP_HANDLER)는
  `poll_detect` 가 부작용으로 발화해 게스트 상태를 오염시킨다. Doom BFS 패치는 body 가 항상
  같은 region 에 있어 우연히 안전했지만, SDRAM poll case 는 폴링 루프가 어떤 region 의
  opcode 를 가리키는지 모르므로 명시적 가드가 필수.
- **양방향 tight 폴링(교대 producer/consumer)은 RPOLL cooperative idle yield(b13 설계)로 못
  잡는다.** 양쪽 동시 sleep → 데드락 또는 blank. sh2pico.c icount 폴링(매 iteration 진짜 읽기)
  만이 안전하다.

## 측정 7 — Metal Head BFS GBR 폴링 (keep ✓)

측정 6 의 Metal Head 는 BFS/GBR 폴링이라 BT/BF 케이스 타겟이 아니었다(no-match). Doom BFS
countdown case 와 동일한 구조(BFS + body 1 insn + delay slot)지만 delay slot 이 `ADD #-1`
(카운트다운)이 아니라 `MOV.W @(disp,GBR),R0`(폴링 읽기)인 변형을 추가로 잡는다.

### 폴링 정체

- master pc=0x0600b45e op=**0x8ffd(BFS)** disp8=-3 target=0x0600b45c, GBR=**0x0600d4f8**(SDRAM 내부).
- 루프: 0x0600b45c `TST R0,R0`(body 1 insn, 0x2008) → 0x0600b45e `BFS back`(0x8ffd) →
  0x0600b460 delay slot = `MOV.W @(disp*2,GBR),R0`(폴링 읽기, **0xc502**).
- poll_addr = GBR + 0x02*2 = 0x0600d4f8 + 4 = **0x0600d4fc (SDRAM)**. BFS taken(T==0) 루프,
  R0==0(T==1) 탈출. 게스트 59.41%(#1-3 합).

Doom BFS countdown(b9)과 구조 동일(BFS body 1 + delay slot)이나 delay slot 이 ADD#-1(카운트다운)
이 아니라 MOV.W 폴링 읽기. 기존 Doom countdown 매칭(line 437 ADD#-1 체크) 실패 → reject 캐싱.

### opcode 정체 (진단으로 정정)

초기 가정 `0x91xx`(MOV.W @disp,R0)는 틀림. 진단 샘플이 `bop2=0xc502` 를 잡음.
picodrive `sh2dasm.c` op1100 switch on `(opcode>>8)&15`: case 5(**0xC5xx**) = `MOV.W
@($disp*2,GBR),R0` LOAD(case 1 0xC1xx 는 STORE). 정확한 마스크 = **0xC500**.

### 패치 (sh2pico.c, Doom BFS countdown case 확장)

Doom BFS countdown case 의 delay slot 체크를 2분기로 재작성:
- `ADD #-1,Rn`(0x70ff, Rn 일치) → Doom 카운트다운(기존 로직, iter_cost=5).
- **`MOV.W @(disp*2,GBR),R0`(0xc500)** → SDRAM poll 루프(신규):
  ```c
  if (T) return;                       /* R0 != 0 → BFS not-taken → 루프 탈출 */
  while (icount >= 5) {
      val = RW(poll_addr);             /* 매 iteration 진짜 SDRAM 읽기 */
      R[0] = val; icount -= 5;
      if (val == 0) { sr |= T; return; }   /* 다음 BFS not-taken */
  }
  sr &= ~T; return;                    /* slice 고갈 → BFS taken, 다음 slice producer run */
  ```
  iter_cost=5(TST 1 + BFS taken 3 + MOV.W 1). 사이드 이펙트 보존(매 iteration RW 읽기),
  데드락 구조적 불가(icount 고갈→taken→다음 slice 상대 코어 run).
- 둘 다 아니면 reject.

### A/B (진짜, 진단 off, 50f, PHASE_PROF=1, fb 동일 = 사이클 정확)

| 항목 | base (Doom BFS + SDRAM poll BT/BF) | patched (+BFS GBR poll) | 차이 |
|------|-------------------------------------|--------------------------|------|
| TOTAL insn/frame | 26,008,478 | 12,649,889 | **−13,358,589 (−51.3%)** |
| msh2 | 19,370,645 (74.4%) | 5,936,421 (46.9%) | **−69.3%** |
| ssh2 | 3,684,039 (14.2%) | 3,712,749 (29.3%) | +0.8% |
| sh2 host/guest | 70.604x | 93.712x | — |
| fb f49 | 4dddf644 | 4dddf644 | 동일 ✓ |

GATE3 PASS. BFS case 의 MOV.W GBR 분기가 Metal Head 59% 폴링을 잡음. **Metal Head = 5번째 keep.**

### 회귀 A/B (BFS countdown case 2분기 확장이 기존 keep 에 영향 안 주는지)

Doom BFS countdown case 를 2분기(ADD#-1 / MOV.W GBR)로 재작성했으므로 Doom/Kolibri/Tempo
회귀 확인. 50f/150f, 진단 off. 50f 는 CK_A=99 > 50 이라 fb 라인 출력 없음 → 회귀 판정은
host/sh2/msh2 수치가 이전 A/B 와 정확히 동일한지로(동일 게스트 실행 = 동일 fb).

| ROM (frames) | 이전 A/B (host/sh2/msh2%) | 현재 (동일 패치) | 회귀 |
|--------------|---------------------------|------------------|------|
| Doom (50f) | 8,625,401 / 70,115 | 8,625,401 / 70,115 | 없음 ✓ |
| Kolibri (50f) | 21,561,764 / 213,059 / 23.2% | 21,561,792 / 213,059 / 23.2% | 없음 ✓ |
| Tempo (150f) | 17,566,616 / 185,704 / 63.1% | 17,566,645 / 185,704 / 63.1% | 없음 ✓ |

3종 전부 수치 동일 = 회귀 없음.

## 측정 8 — Zaxxon/Chaotix/VF (reject) + 전체 개선 (역효과 폐기)

### 남은 3종 핫 루프 (`SH2_PC_HIST`)

- **Zaxxon** (msh2 85%, host/guest 1500x): #1-5 (40%) 0x0600163a 직선 5명령 시퀀스(MOV.L/OR/
  MOV.L@R2/MOV.L@R5) = 핫 함수 패스, reject(사이드 이펙트). #6-10 (15%) 0x06001422
  `MOV.L @R11,R0; TST; BT; BRA; NOP` = comm 폴링(R11=0x2000402e → 0x0000402e CS0 comm).
  MOV.L(32-bit) + BT forward + BRA backward 복합 구조(body 5명령)라 fastloop skip 타겟 아님.
- **Chaotix** (msh2 46% + ssh2 45%, host/guest 121x): #1-5 (20%) 시프트/회전 연산 루프
  (SHLL/ROTCL/DT/BFS/ADD). #6-10 (10%) 메모리 카피. #11-15 (8%) 카피+swap. 전부 실제 작업(reject).
- **VF** (msh2 38% + ssh2 55%, host/guest 134x): #1-27 (86%) 27명령 직선 시퀀스 동일 빈도 =
  인라인 확장 함수 본체(reject, 루프 아님). 분산.

**결론:** 3종 전부 fastloop skip 타겟 아님. 6번째 keep 불가.

### 전체 개선 (ROM 무관) — RW 인라인 fetch 역효과

task agent(ses_09257f1a) 분석: 매 게스트 명령마다 opcode fetch 의 `RW()` = `p32x_sh2_read16`
외부 함수 호출(LTO 없어 인라인 불가)이 host/guest 비율 74-1500x 의 주원인. 최적화 지점 A
(fetch SDRAM fast path 직접 디스패치) 구현:

`sh2pico.c` non-DRC 매크로 블록에 `GNW_FETCH_SD` 매크로 추가(SDRAM 0x06/0x26 직접 포인터,
cache-through bit 0x20000000 무시), 4 fetch 사이트(두 디스패치 루프의 delay/direct 분기)를
매크로로 교체.

**A/B (Doom 50f):** base(fetch off) TOTAL 8,625,401 / sh2 70,115 / host-guest 85.4x.
patched(fetch on) TOTAL 8,896,277 / sh2 89.3x. **+3.9% 느림(역효과).** fb 동일.

QEMU icount 환경에서 매크로 분기+직접 포인터 접근이 함수 호출(BL p32x_sh2_read16)보다 비쌈 —
함수 쪽이 QEMU 번역 캐시에 유리. host 장비(GCC -O2)에서는 다를 수 있으나 **QEMU rig 측정
기준으로는 이득 없음**. 폐기. 매크로는 캐스팅 RW 로 유지(원본 동작).

> 구현 중 캐스팅 버그(`(UINT32)(UINT16)` 누락)로 base 가 22.5M/ssh2 69% 로 비정상 측정된 적이
> 있음 — `*(s16*)` 부호확장이 opcode 상위 비트를 채워 `0xaffe`(BRA-self) pre-filter 매칭 실패 →
> slave idle fastloop 진입 안 함. 캐스팅 복구 후 정상. fetch 매크론느 캐스팅 RW 로 확정.

**DRC 활성화** 도 검토했으나 RAM 요구사항(drcblk_ram+drclit_ram 256KB + drcblk_da 8KB +
tcache 4MB 기본)이 RAM_EMU 724KB 초과 → **불가**.

## 종합 (최종)

**확정 keep 5종 (전부 진짜 A/B, fb bit-identical = 사이클 정확, 회귀 없음):**

| ROM | 패치 | Δ TOTAL | 병목 정체 |
|-----|------|---------|-----------|
| Doom | BFS countdown (sh2pico.c) | **−58.3%** | 순수 카운트다운 지연 루프 62.68% |
| VR | PWM cooperative idle yield (memory.c) | **−73.6%** | PWM register 폴링 87.23% |
| Kolibri | SDRAM poll BT/BF (sh2pico.c) | **−33.3%** | SDRAM slot 양방향 교대 폴링 34% |
| Tempo | SDRAM poll BT/BF (sh2pico.c) | **−25.2%** | SDRAM slot 폴링 35.5% |
| Metal Head | BFS poll MOV.W GBR (sh2pico.c) | **−51.3%** | BFS GBR SDRAM 폴링 59.41% |

**reject 3종 (fastloop skip 타겟 아님):**

| ROM | 병목 | 분류 | 결정 |
|-----|------|------|------|
| Zaxxon | 직선 연산 40% + comm 폴링 15% | 연산(핫 함수) + 폴링(복합 구조) | reject |
| Chaotix | 시프트 20% + 카피 18% | 실제 작업 | reject |
| VF | 분산 86% (루프 아님) | 함수 체인 | reject |

**전체 개선 (ROM 무관):** RW 인라인 fetch 역효과(폐기), DRC RAM 제약(불가).

### 32X 금광 지도 (세가CD 스타일)

```
┌──────────────┬──────────────────────────┬──────┬──────────────────────────────┬───────────┐
│ ROM          │ 병목                     │ %    │ 최적화 전략                   │ 상태      │
├──────────────┼──────────────────────────┼──────┼──────────────────────────────┼───────────┤
│ Doom         │ BFS 카운트다운           │ 62.7 │ fastloop BFS countdown        │ ✅ −58.3% │
│ VR           │ PWM 레지스터 폴링        │ 87.2 │ PWM cooperative idle yield    │ ✅ −73.6% │
│ Kolibri      │ SDRAM slot 교대 폴링     │ 34+  │ SDRAM poll BT/BF (icount)     │ ✅ −33.3% │
│ Tempo        │ SDRAM 폴링               │ 35.5 │ SDRAM poll BT/BF              │ ✅ −25.2% │
│ Metal Head   │ BFS GBR 폴링             │ 59.4 │ BFS countdown MOV.W GBR 분기  │ ✅ −51.3% │
├──────────────┼──────────────────────────┼──────┼──────────────────────────────┼───────────┤
│ Zaxxon       │ 직선 연산 40% + comm 15% │ —    │ reject(연산) / comm 미탐구    │ ❌        │
│ Chaotix      │ 시프트 20% + 카피 10%    │ —    │ reject(실제 작업)             │ ❌        │
│ VF           │ 분산 86% (루프 아님)     │ —    │ reject                        │ ❌        │
├──────────────┼──────────────────────────┼──────┼──────────────────────────────┼───────────┤
│ 전체 개선    │ host/guest 74-1500x      │ —    │ RW 인라인 fetch               │ ❌ 역효과 │
│ (ROM 무관)   │ (인터프리터 오버헤드)    │      │ DRC 활성화                    │ ❌ RAM제약│
└──────────────┴──────────────────────────┴──────┴──────────────────────────────┴───────────┘
```

검증된 5종 keep: 8 ROM 중 5개, ROM당 −25~−74%. 나머지 3종은 fastloop skip 타겟 아님.
전체 개선(RW 인라인)은 QEMU rig 에서 역효과, DRC 는 RAM 제약 불가.

### 핵심 교훈 (GBA idle-skip 과 같은 궤적)

1. **32X 는 폴링 루프가 주 병목.** Doom 형 순수 카운트다운은 8 ROM 중 1곳뿐. ROM 마다 서로
   다른 핫 루프 — 일반화 안 됨, 개별 프로파일링 필수(측정 주도 원칙).
2. **`RW(target)` body fetch 는 region 가드 필수.** SDRAM(0x06) 직접 포인터만 safe.
   sysreg/comm(0x00/0x20) 핸들러 타면 poll_detect 부작용으로 fb 깨짐(Metal Head 회귀 원인).
3. **양방향 tight 폴링(Kolibri slot 교대)은 cooperative RPOLL(memory.c)이 데드락/blank 실패.**
   icount 폴링(sh2pico.c 단독, 매 iteration 진짜 RW 읽기)만 안전 — 사이드 이펙트 보존,
   데드락 구조적 불가.
4. **A/B 는 fb 체크섬 bit-identical 로 사이클 정확을 증명.** 진단 카운터는 pprof phase 영역에
   잡혀 수치를 부풀리므로, 진짜 성능 측정은 진단 off 필수.
5. **전체 개선(RW 인라인)은 QEMU rig 에서 역효과.** host 장비에서는 다를 수 있으나, 측정
   도구(rig)의 판정을 따른다. DRC 는 RAM 예산이 절대적 장벽.

### 커밋 상태

- picodrive 서브모듈: `0b5efd9f` "gnw: SH-2 fast-loop BFS countdown + RIG_SH2_PC_HIST profiler
  — Doom 32X −58.3%"(Doom BFS 만, detached HEAD). SDRAM poll BT/BF + BFS GBR poll + 진단은
  working tree(미커밋).
- memory.c: VR PWM 패치(−73.6%)는 `/tmp/32x-prof/pwm_poll.diff` 백업(미적용).
- testbed 메인: `b57cae6f` (Doom, docs/32X_RIG_ANALYSIS.md). docs/32X_PERFORMANCE_RESULTS.md
  업데이트 본 커밋은 아직.

### 다음 후보

- (a) Zaxxon comm 폴링(15%, MOV.L 32-bit 복합 구조) 도전 — 6번째 keep 가능성.
- (b) keep 5종 커밋(picodrive: SDRAM poll + BFS GBR poll; memory.c: PWM poll) + push.
- (c) 디바이스 DWT 검증(QEMU rig 수치 → 실제 fps 환산).
- (d) 세가CD 작업으로 전환.
