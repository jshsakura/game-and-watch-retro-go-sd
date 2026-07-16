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

**최적화 다음 단계 후보 — "cooperative idle yield":** 폴링 루프를 감지하면 남은 timeslice 를
다른 코어에 양보(yield), 폴링 조건이 바뀌면 재개. 메모리 쓰기 훅을 통해 폴링 주소의 변경을
통보받아 깨우는 구조. picodrive 의 32x comm register(`0x4000408x`)는 현재 소스에서
no-match(실제 폴링 주소는 ROM 코드 영역 `0x060xxxxx`), 공유 RAM/메모리 맵 기반 감시 필요.

다음: (a) Doom 패치의 디바이스 검증(DWT) + keep 확정/커밋, (b) cooperative idle yield
프로토타입(VR/Metal Head 폴링 루프 대상, 사이드 이펙트 보존 검증 포함).
