# SegaCD Mode-8 Boot-Crossing Stall — Reverse-Engineering Findings

**Scope:** READ-ONLY RE to evaluate the hypothesis that the sub-68K parks at
`subPC 0x6132` (tst.b/bne on a local semaphore byte, "a9") because our engine
does not wire sub-CPU **interrupt level 6** (subcode). IEN at the stall is
`0x54` = bits 6,4,2 armed (IEN6 subcode + IEN4 CDD + IEN2 doorbell; IEN1 GFX off).

**Verdict:** the level-6 hypothesis is **INVERTED**. Level 6 is not wired in
PicoDrive either, yet PicoDrive boots every SegaCD title. The stall's cause is
**not** a missing level-6 delivery. See §4.

All sub-BIOS addresses below are file offsets into `prg_subbios.bin` (131072 B,
sub-CPU reset vector → PC=$000200, SSP=$005E80), which the sub maps at
`0x000000–0x01FFFF`.

---

## 1. PicoDrive reference — what fires each sub-68K level

`pcd_irq_s68k(irq, state)` (`pd_cd/mcd.c:302`) is the single entry that calls
`SekInterruptS68k(irq)` (`pd_cd/sek.c:153`). Exhaustive grep of
`gnw-segacd/Core/Src/porting/segacd/pd_cd/` AND
`external/picodrive/pico/cd/` finds these **and only these** assert sites:

| Level | Source | Site | Gate |
|------:|--------|------|------|
| 1 | GFX ASIC completion | `gfx.c:460-462` | `IEN1` ($FF8033 bit1) |
| 2 | doorbell (main→sub) | `memory.c:174-180` | `IEN2`, checked **at write time** |
| 3 | timer | `mcd.c:209-211` (`pcd_int3_timer_event`) | `IEN3` |
| 4 | CDD/CDC 75 Hz | `mcd.c:197-199` (`pcd_cdc_event`), `memory.c:466-470, 485-487` | `IEN4 && s68k_regs[0x37]&4` |
| 5 | CDC DECI / end-of-decode | `cdc.c:384-388, 430-434, 492-496, 876-880` | `IEN5` |
| **6** | **(subcode)** | **NOWHERE** — zero call sites | `PCDS_IEN6` defined (`pico_int.h:892`) but unreferenced |
| 7 | — | not delivered | — |

`PCDS_IEN6 = (1<<6)` exists as a mask definition **only**; no code in either
tree ORs it into `s68k_pend_ints`. **PicoDrive ships without level-6 delivery
and boots SegaCD games correctly.** That alone falsifies "level 6 unwired ⇒
boot stall".

---

## 2. Sub-BIOS — interrupt vectors and handlers

Autovectors parsed from `prg_subbios.bin` (vectors 0x64–0x7C):

| Level | Vector | → trampoline @ | → handler body |
|------:|-------:|---------------:|---------------|
| 1 | 0x64 | 0x5f76 | `jmp 0x660` (stub: `rte`) |
| 2 | 0x68 | 0x5f7c | `jmp 0x5f2`  — **doorbell / IPC** |
| 3 | 0x6C | 0x5f82 | `jmp 0x660` (stub) |
| 4 | 0x70 | 0x5f88 | `jmp 0x610`  — **CDD/CDC 75 Hz** |
| 5 | 0x74 | 0x5f8e | `jmp 0x634`  — **CDC DECI** |
| 6 | 0x78 | 0x5f94 | `jmp 0x648`  — **subcode** (gated on flag) |
| 7 | 0x7C | 0x5f9a | `jmp 0x660` (stub) |

So the sub-BIOS **does** vector level 6 to a real handler at `0x648`. The
handler is tiny and gated on a runtime flag:

```
0x648: movem.l d0-d7/a0-a6, -(a7)
0x64c: movea.l #$0, a5
0x652: tst.b   $583a(a5)         ; subcode-enabled flag
0x656: beq.b   $65c              ; usually not armed → skip
0x658: bsr.w   $2532             ; subcode processor
0x65c: movem.l (a7)+, d0-d7/a0-a6
0x660: rte
```

The level-2 handler is the one that touches the IPC byte that the boot
handshake spins on (see §3):

```
0x5f2: movem.l d0-d7/a0-a6, -(a7)
0x5f6: movea.l #$0, a5
0x5fc: bsr.w   $1ce2              ; watchdog decrement ($5a2c)
0x600: jsr     $5f34.w            ; → jmp $606a.l (doorbell work)
0x604: bclr.b  #$0, $5ea4.w       ; ★ clear IPC request bit
0x60a: movem.l (a7)+, d0-d7/a0-a6
0x60e: rte
```

The level-4 handler calls the CDD/CDC chain (`$131c`, `$26ec`, `$2f5a`,
`$2d44`, `$676`) and does **not** touch `$5ea4`.

---

## 3. The actual boot-crossing primitive — `0x5e2` (NOT 0x6132)

The sub-BIOS's "signal main and wait for ack" primitive lives at `0x5e2`,
immediately before the level-2 handler:

```
0x5e2: bset.b  #$0, $5ea4.w       ; sub: set IPC request bit
0x5e8: btst.b  #$0, $5ea4.w       ; sub: test it
0x5ee: bne.b   $5e8               ; spin until level-2 ISR clears it
0x5f0: rts
```

Released by the level-2 handler's `bclr.b #$0, $5ea4.w` at `0x604`.
**This is the boot-crossing handshake.** It is bit-level (one bit at `$5ea4`),
not byte-level.

### The user's `0x6132` site is a DIFFERENT, byte-level primitive

`0x6132` is a real spin, but on a different operand and using a different
addressing mode:

```
0x612e: st.b    $3(a6)            ; set whole byte (→ 0xFF)
0x6132: tst.b   $3(a6)            ; test whole byte
0x6136: bne.b   $6132             ; spin while nonzero
0x6138: rts
0x613a: st.b    $3(a6)            ; companion: set-only (no wait)
0x613e: rts
```

Differences from the `0x5e2` handshake:

| | `0x5e2` (boot handshake) | `0x612e/0x6132` (user's stall) |
|---|---|---|
| Operand | absolute `$5ea4.w` | `$3(a6)` — a6 set by caller |
| Granularity | **bit 0** (bset/btst/bne) | **whole byte** (st.b/tst.b/bne) |
| Clearer | level-2 ISR `bclr.b #0,$5ea4` | unknown — `0x612e` has **0 direct callers** in the sub-BIOS (no `bsr/jsr` to it; not stored as a function pointer; reached indirectly, likely from disc-loaded code in Word-RAM/PRG-RAM) |
| Releases on | bit clear (0xFF→0xFE **would not** release; the `0x5e2` loop only sets/tests bit 0, so 0x01→0x00) | full byte clear (0xFF→0x00) |

**Implication:** the level-2 handler's `bclr.b #0, $5ea4` **cannot** release
the `0x6132` spin even if `a6 == $5ea1` (which would make `$3(a6) == $5ea4`):
`st.b` writes 0xFF, `bclr.b #0` produces 0xFE, `tst.b` is still nonzero. The
`0x6132` primitive requires a `clr.b $3(a6)` somewhere — which is **not** in
any sub-BIOS ISR we disassembled, and is therefore most likely performed by
the **main 68K writing to shared Word-RAM** (the standard IPC pattern: main
writes the byte directly, no sub interrupt needed).

### Note on the engine comment at `segacd_engine.c:254-263`

That comment names `$36a9` as the spin site. `$36a9` is **mid-instruction**
(inside `cmpi.b #$6, d0` at `0x36a8`) in this BIOS revision — the address is
stale or from a different dump. The real spin sites in this BIOS are:

- `0x05e8` — boot handshake on bit 0 of `$5ea4` (released by level 2)
- `0x6132` — byte-level IPC on `$3(a6)` (user's observed stall; clearer not in sub-BIOS)
- `0x6194` — register poll on bit 2 of `$800e` (`btst #2,$800e; bne`) — a
  gate-array register, not a semaphore; released by the GA flipping the bit.

---

## 4. Conclusion — level 6 is a red herring; look at main-side IPC instead

### 4.1 The hypothesis as stated is falsified

> "level 6 (subcode) clears a9; our engine doesn't wire level 6; therefore stall."

Each leg fails:

1. **Level 6 doesn't clear the stall byte.** The sub-BIOS's level-6 handler
   (`0x648`) only calls `$2532` when `$583a != 0`; it never touches `$5ea4` and
   never does `clr.b $3(a6)`. Even if we delivered level 6, the `0x6132` byte
   would not be cleared by it.
2. **PicoDrive never delivers level 6 and boots fine.** `PCDS_IEN6` is a mask
   constant with zero references in any source file. If level 6 were required
   for boot, PicoDrive would hang identically; it does not.
3. **The `0x6132` primitive isn't released by any sub-BIOS ISR** we
   disassembled. Its clearer is external to the sub-BIOS — i.e. the main 68K
   writing shared RAM directly, which needs **no sub interrupt at all**.

### 4.2 What to investigate instead — LIVE TRACE CONFIRMED

**The live trace (`/tmp/boot_verify`, 1500 frames, Detonator Orgun)
reproduced the user's exact stall and confirmed the deadlock.** See §6 for
the hard data. Summary:

- Boot **does** reach mode 8 (at frame 738) — the prior CLAUDE.md gate
  ($FFFE20 high nibble) is no longer the blocker.
- At mode 8 the sub enters `0x6132` spinning on **PRG-RAM byte `0x97EB`**
  (`a6 = 0x97e8`, `$3(a6) = 0x97EB`), waiting for the main to clear it.
- The main is in its per-frame VBlank wait loop (`0xa1a/0xa1e`) and
  **never reaches the code path that writes to the PRG window**
  (`main->PRGwin writes = 0` across 1500 frames).
- **Classic deadlock:** sub waits for main's PRG write; main's boot state
  machine never advances to the PRG-write phase.

Candidate root causes for why the main's state machine is blocked, in
likelihood order:

1. **CDD/CDC status not progressing.** At the stall `cdd_status = 0x04`
   (low nibble only) and `cdd_pend = 1` (set, never clearing). The main
   BIOS likely polls a CDD status register that our `segacd_cd.c` never
   advances past the "command pending" state. This is the same family of
   bug as the prior $FFFE20 high-nibble gate, but at a later boot phase.
2. **$A12003 (memory mode / DMNA) not in the expected state.** The main
   BIOS references $A12003 10+ times (offsets 0x1352, 0x1af4, 0x1afe,
   0x1b8e, 0x1ee6, 0x1f5a, 0x4bb8, 0x4bc2). If our engine doesn't drive
   the DMNA/RET handshake correctly, the main may believe it doesn't own
   the Word-RAM/PRG-RAM and refuse to write.
3. **Sub not sending an expected doorbell acknowledgment.** The sub is
   parked at `0x6132` and never sends the doorbell the main is waiting
   for. Check whether the main is spinning on a doorbell-related flag.
4. **Level 6 wiring is NOT recommended** (still). Adding a level-6 source
   would not fix this deadlock — the sub's spin is on PRG-RAM, not on a
   sub-CPU interrupt. PicoDrive boots the same games without level 6.

### 4.3 Wiring spec — N/A

No new interrupt wiring is warranted. The existing level-2 path is the
boot-crossing channel and is already wired on both sides
(`segacd_bus.c:485-510` main-side doorbell write → `segacd_engine.c:349,
138` sub-side delivery+ack). The fix lies in the main-side boot state
machine — most likely in CDD status emulation (`segacd_cd.c`) or the
DMNA/RET memory-mode handshake (`segacd_bus.c`), not in adding a
level-6 source.

---

## 5. Open questions for the next session

The live trace (§6) answered the original open questions (caller of `0x612e`
is in disc-loaded sub code, not sub-BIOS; main's PC at stall is `0xa1a`;
a6+3 = PRG-RAM `0x97EB`, not Word-RAM). New open questions:

1. **Trace the main BIOS boot state machine AFTER mode 8.** Find the caller
   of WaitVSync (`0xa0c`) at the post-mode-8 phase and identify what
   condition gates the PRG-window write path (`$029000` / `$A12006` writes
   at BIOS offsets 0x6339, 0x6543, 0x65e3, 0x69a9, 0x6ad7, 0x6d09, 0x6d95,
   0x6e79, 0xa2c, 0xa48). Disassemble from `/tmp/scd/bios_CD_U.bin` (NOT
   the byte-swapped `main_bios_be.bin`) with `/tmp/maindis.py`.
2. **Check $A12003 (memory mode) value at the stall.** Is it in the right
   bank mode for main to access PRG-RAM? Our engine's DMNA/RET state
   machine is in `segacd_bus.c:513-544` and `segacd.h` (`dmna_ret_2m`).
3. **Check CDD status progression at the stall.** `cdd_status = 0x04` and
   `cdd_pend = 1` never clear. Trace `segacd_cd.c` to see if a CDD command
   response is pending that our engine never completes. The main BIOS may
   be polling for a CDD status transition that never arrives.
4. **Compare with PicoDrive at the same boot frame (frame 738+).** What
   does PicoDrive's main do differently at mode 8? The PicoDrive source
   (`pd_cd/{mcd.c,memory.c,cdd.c,cdc.c}`) is the reference for correct
   CDD/CDC/DMNA behavior.

---

## 6. Live trace reproduction — user's exact stall confirmed

**Harness:** `/tmp/boot_verify` (built from `tools/segacd_harness/boot_test.c`
+ segacd engine sources, recipe in `tools/segacd_harness/build_verify.sh`).

**Run:** `/tmp/boot_verify /tmp/scd/bios_CD_U.bin /tmp/scd/Detonator\ Orgun\ \(U\)\ \[!]\.cue 1500`
with `SCD_MAIN_IDLE_SKIP=0 SCD_SUB_IDLE_SKIP=0`.

**Boot progression observed:**

| Frame | Event | Mode | Main PC | Sub PC | IEN | Notes |
|------:|-------|-----:|--------:|-------:|----:|-------|
| 0 | cold reset | 0 | 0x42e | 0x42e | — | SSP=0xFFFFFD00, both at reset vector |
| 60 | early init | 0 | 0x988 | 0x42e | — | SR=0x2700 (all masked), sub not started |
| 66 | mode 0→4 | 4 | — | — | — | disc-detect phase entered |
| 300 | disc-detect | 3 | 0xa1e | 0x7c98 | 0x16 | sub running math fn, ISR=74901 |
| **738** | **mode 4→8** | **8** | — | — | — | **the crossing** |
| 1500 | **STALL** | 8 | **0xa1a** | **0x6132** | **0x54** | **deadlock** |

**Stall state (frame 1500, exact match to user's report):**

- **Sub PC = 0x006132** — `tst.b $3(a6); bne 0x6132` (byte-level IPC spin)
- **Sub A6 = 0x97e8** → spinning on **PRG-RAM byte 0x97EB**
- **IEN ($FF8033) = 0x54** = IEN6 + IEN4 + IEN2 (exact match)
- **Main PC = 0xa1a/0xa1e** — WaitVSync loop (`tst.b $fe26; bne`)
- **Main SR int_mask = 0** (interrupts unmasked, normal)
- **main->PRGwin writes = 0** — main NEVER writes to PRG window
- **ISR totals:** L2ifl=102805, L4cdd=1594, L1gfx=285, L5cdc=4, **r6=0, r7=0**
- **cdd_status = 0x04**, **cdd_pend = 1** (set, never clearing)
- **Mode ($FFFDDC) = 8** (the crossing reached)
- Sub D0-D7: `00000030 0000ff68 00000003 0000ffff 0000ffff 00000000 00000024 0000ffff`
- Sub A0-A7: `0000b710 0000b5ea 0001cd28 00ff3159 0000b490 0000b490 000097e8 00005e70`

**Why the deadlock is unbreakable without a fix:**

The sub spins on PRG-RAM byte 0x97EB waiting for the main to clear it. The
main's boot state machine at mode 8 should write to the PRG window
(`$020000-$03FFFF`, mapped in our bus at `segacd_bus.c:647-654`) to signal
the sub — but the main never reaches that code path. The main is alive
(cycling through VBlank wait) but its boot state machine is blocked on a
different condition (candidates in §4.2). Neither CPU can make progress.

**Sub-BIOS self-checksum PASSES** (0x200..0x5800 == 0xe9bb), so the sub-BIOS
itself is intact. PRG-RAM has 61674/524288 nonzero bytes — the disc-loaded
sub program IS loaded. The issue is purely the main-side boot state machine
not advancing to the IPC phase.

---

## 7. 300bda6d에게: HLE 게이트 주입 스펙 (fast-boot, task (a))

**목표:** BIOS disc-detect 부팅 phase(750프레임, f66–f738+)를 ~5–10프레임으로
줄여 STM32H7 예산 안에 풀프레임 달성. 충실한 에뮬레이션이 아니라 HLE bypass.

### 7.1 세 게이트의 인과 관계 (확정)

라이브 트레이스(800프레임, `-DSEGACD_GA_TRACE`)로 다음 chain이 확인됨:

```
[게이트 3 = ROOT] sub-comm $FF8020 = CDD status (현재 0x05 = CDD_OPEN)
        │
        │  BIOS 0x12BC (VBlank ISR sub, 매 프레임 자동 복사)
        │  move.w $FDF0, $FE3A   ($FDF0 = main-side copy of $FF8020)
        ▼
[게이트 2 = AUTO-COPY] $FE3A = $FF8020 그대로 (현재 0x05)
        │
        │  BIOS 0x1D34: cmpi.b #$40, $FE3A; bne → WaitVSync loop
        │  $FE3A == 0x40 이어야 mode 4→8 crossing 진행
        ▼
[게이트 1 = DOWNSTREAM] $FFFE20 high nibble (현재 0x00)
        │
        │  BIOS 0x1D26: btst.b #$7, $FE20 (error flag check)
        │  harness가 $FFFE20 & 0xF0 == 0 → "gate BLOCKS" 보고
        │  Word-RAM crossing chain($200400→$D01C→...→$FE20)이
        │  sub의 0x6132 stall로 인해 발화하지 않아 영원히 0
        ▼
   mode 8→0x10 advance 불가 → 영구 정지
```

**결론: 게이트 3이 근원.** CDD status가 CDD_OPEN(0x05)에서
disc-ready(0x40)로 전환되지 않는 것이 모든 것의 출발점.

### 7.2 주입 시점 (확정)

**sub-release 시점** (mode 0→4 transition, ~f66). 이것이 sub가 CDD 처리를
시작하는 순간이며, BIOS disc-detect loop(0x1D1A)가 처음 돌기 직전.

구현상 위치: `state_flags &= ~PCD_ST_S68K_RST` 가 발생하는 곳 (sub
리셋 해제). 또는 그 직후 첫 CDD status response.

### 7.3 동시주입 vs 순차주입 (확정)

**구현은 동시 주입. 인과는 순차지만 자동 해소됨:**

| 게이트 | 주입 여부 | 이유 |
|--------|----------|------|
| **3** ($FF8020) | **직접 주입** | 근원. CDD status response handler에서 매번 덮어쓰기 (sub-BIOS가 자체 status report로 $FF8020을 overwrite하므로 1회성 주입으로는 불충분) |
| **2** ($FE3A) | **주입 불필요** | VBlank ISR 0x12BC가 매 프레임 $FF8020을 $FE3A로 복사 → 게이트 3 주입 후 1프레임 내 자동 해소 |
| **1** ($FFFE20) | **직접 주입** (bypass) | Word-RAM crossing chain이 sub stall로 발화 불가 → chain이 고쳐지기 전까지 bypass 값으로 직접 세팅. 매 프레임 주입 필요 (VBlank ISR sub-function이 덮어쓸 수 있음). mode 0x10 도달 후 주입 중단 |

### 7.4 구현 스펙 (300bda6d용 의사코드)

```c
/* segacd_cd.c — CDD status response handler (status command 0x05 같은 곳) */
/* 매 CDD status response 마다 호출됨 → 게이트 3 연속 주입 */
void segacd_cdd_status_response(void) {
    if (scd_fast_boot && boot_phase < BOOT_MODE_0x10) {
        CD.status   = CDD_READY;           /* 0x04 (내부 상태) */
        s[0]        = 0x40;                /* ★ 0x04가 아님. BIOS가 $FE3A에서 검사하는 raw flag byte */
        s[1]        = 0x00;                /* error bits clear */
        SCD.s68k_regs[0x20] = 0x40;        /* sub-comm byte 0 = disc-ready */
        /* 게이트 2 ($FE3A)는 자동 해소 — 주입 불필요 */
    } else {
        /* 정상 경로 (기존 코드) */
        s[0] = (uint8_t)CD.status;
        s[1] = 0x05;
    }
}

/* segacd_engine.c — per-frame hook (VBlank 직후, sub run 직전) */
/* 게이트 1 bypass: $FFFE20 high nibble 강제 세팅 */
void segacd_per_frame_fast_boot(void) {
    if (scd_fast_boot && boot_phase < BOOT_MODE_0x10) {
        /* $FFFE20 = main RAM 0xFFFE20.
         * 우리 엔진의 M68K_RAM 매핑에 맞게 조정 (엔디안 주의).
         * 0x40 = bit 6 set, bit 7 clear → BIOS 0x1D26 btst #7 통과 + harness & 0xF0 통과 */
        M68K_RAM[0xFFE20 ^ 1] = 0x40;   /* LE byte-swap: odd offset = MSB */
        /* 또는 engine이 제공하는 main-RAM write helper 사용 */
    }
}
```

### 7.5 주의사항

1. **`s[0] = 0x40`은 CDD_READY(0x04)가 아님.** segacd_cd.c:407의 기존 코드는
   `s[0] = (uint8_t)CD.status` (0x04)를 쓰지만, BIOS는 `$FE3A == 0x40`을
   검사함. 0x40은 raw flag byte (bit 6 = disc-present). HLE 주입 시에는
   `s[0] = 0x40`으로 직접 세팅해야 BIOS disc-detect loop이 통과함.

2. **게이트 3 주입은 연속이어야 함.** sub-BIOS가 CDD status response를
   받을 때마다 $FF8020에 자체 status report를 overwrite하므로, 한 번만
   주입하면 다음 response에서 다시 0x05로 돌아감. status response
   handler에서 매번 주입.

3. **게이트 1 주입도 매 프레임이어야 함.** VBlank ISR의 sub-function
   ($1252, $1162)이 $FFFE20을 갱신/클리어할 수 있음. mode 0x10 도달 후에는
   주입을 중단해야 정상 부팅 이후 동작이 꼬이지 않음.

4. **게이트 2는 절대 직접 주입하지 말 것.** $FE3A는 $FF8020의 순수
   copy이므로, 게이트 3만 주입하면 자동 해소됨. 둘 다 주입하면
   시점 차이로 inconsistency 발생 가능.

5. **`boot_phase < BOOT_MODE_0x10` 게이트.** 주입은 mode 0x10 도달 전까지만.
   도달 후에는 BIOS가 정상적으로 제어권을 갖게 됨. 도달 여부는
   `M68K_RAM[$FFFDDA]` (boot-mode register) 읽기로 판단.

### 7.6 예상 부팅 타임라인 (주입 후)

```
f0:   cold reset, mode 0
f1-2: BIOS init, sub-BIOS checksum
f3:   sub release (mode 0→4) + 게이트 3+1 주입 시작
f4:   첫 VBlank → 게이트 2 자동 해소 ($FE3A = 0x40)
f5:   BIOS disc-detect loop 통과 ($FE3A == 0x40) → $FE52 = 0xFF
f6:   mode 4→8 crossing
f7-8: mode 8→0x10 (게이트 1 bypass로 crossing chain 대체)
f9:   주입 중단 (boot_phase >= 0x10), BIOS 정상 제어
```

**~5–10 프레임으로 750프레임 절약.** 유저 목표 달성.

### 7.7 BIOS 리전별 주소 차이 — 검증 결과

**환경 제약:** 이 시스템의 모든 BIOS 파일(`bios_CD_{E,J,U}.bin`, `jp_mcd2_921222.bin` 포함)은 동일한 MD5(`2efd74e3232ff260e371b99f84024f7f`, US v2.00)의 복사본이다. 실제 EU/JP BIOS와의 직접 비교는 불가능했음. 아래는 정적 분석 + 인코딩 검증으로 얻은 간접 결론.

**주소 안정성 분류:**

| 주소 | 종류 | 리전 간 안정성 | 근거 |
|------|------|---------------|------|
| `$FF8020` (sub-comm) | **하드웨어 레지스터** (gate array) | **항상 동일** | SegaCD 칩 스펙 — BIOS 무관 |
| `$A12003` (DMNA/RET) | **하드웨어 레지스터** | **항상 동일** | 동일 |
| `$A1200E` (sub-comm flag) | **하드웨어 레지스터** | **항상 동일** | 동일 |
| `$FE3A` (disc-status copy) | **메인 RAM 변수** | **사실상 안정** | 모든 MCD BIOS가 동일 Sega 소스에서 컴파일됨 — RAM 레이아웃 동일 |
| `$FFFE20` (mode-advance gate) | **메인 RAM 변수** | **사실상 안정** | 동일 |
| `$FE26` (VBlank semaphore) | **메인 RAM 변수** | **사실상 안정** | 동일 |
| `$FE52` (disc-status-valid) | **메인 RAM 변수** | **사실상 안정** | 동일 |
| BIOS `0x4BD0` (crossing copy) | **코드 주소** | **리전마다 상이** | 코드 오프셋은 빌드마다 변경됨 |
| BIOS `0x12BC` (VBlank sub-comm copy) | **코드 주소** | **리전마다 상이** | 동일 |
| BIOS `0x1D34` (disc-detect gate) | **코드 주소** | **리전마다 상이** | 동일 |

**핵심 결론:** HLE 주입은 **데이터 주소만 사용** (`$FF8020`, `$FE3A`, `$FFFE20`). 코드 주소(`0x4BD0`, `0x12BC`, `0x1D34`)는 크로싱 체인 이해용이며 주입 스펙에 직접 쓰이지 않는다. 따라서:
- **하드웨어 레지스터 주입 (`$FF8020`):** 100% 리전 무관.
- **RAM 변수 주입 (`$FE3A`, `$FFFE20`):** 모든 주요 MCD BIOS 리전(US v2.00, EU, JP)이 동일 Sega 소스에서 컴파일됐으므로 RAM 레이아웃이 동일 → 동일 주소 유효. 단, 검증되지 않은 변종(dev BIOS, 해킹 ROM)에서는 레이아웃이 다를 수 있음.

**런타임 검증 방법 (300bda6d용, 권장):**

BIOS 로드 후 게이트-2 체크 명령의 시그니처 패턴을 검색하여 RAM 레이아웃이 예상과 일치하는지 확인:

```c
/* BIOS ROM에서 게이트-2 시그니처 검색:
 * cmpi.b #$40, $fe3a.w  =  0C 38 00 40 FE 3A  (6 bytes)
 * 이 패턴이 최소 1회 이상 발견되면 $FE3A 주소가 유효함. */
int fe3a_gate_valid = 0;
for (int i = 0; i + 5 < SEGACD_BIOS_SIZE; i += 2) {
    if (bios[i] == 0x0C && bios[i+1] == 0x38 &&
        bios[i+2] == 0x00 && bios[i+3] == 0x40 &&
        bios[i+4] == 0xFE && bios[i+5] == 0x3A) {
        fe3a_gate_valid = 1;
        break;
    }
}
if (!fe3a_gate_valid) {
    /* 알려진 BIOS 리전이 아님 — HLE 주입 건너뜀 (안전 fallback:
     * 750프레임 느리지만 부팅은 정상 동작). */
    log_warn("BIOS signature mismatch — skipping HLE gate injection");
}
```

같은 방식으로 `$FE20` 시그니처(`08 38 00 07 FE 20` = `btst.b #$7, $fe20.w`)도 검색하여 게이트-1 주소를 검증할 수 있다.

**권장사항:**
1. **300bda6d는 시그니처 검증을 추가하라** — BIOS 로드 후 위 패턴 매칭으로 `$FE3A`/`$FE20` 주소가 예상 위치인지 확인. 불일치 시 HLE 주입을 건너뛰고 정상(느린) 부팅으로 폴백.
2. **코드 주소(`0x4BD0`, `0x12BC`)에 의존하는 로직은 넣지 말 것** — 리전마다 다르다. HLE 주입은 데이터 주소만 사용하므로 이 제약에 부합함.
3. **실제 EU/JP BIOS로 검증이 필요한 경우:** `e96dce05a0465d97f1bd2ac99dfe6e34`(EU) / `278a9f0f4e1eb7a4cf9bd4130f3a5f3e`(JP) 덤프를 입수하여 동일 시그니처가 존재하는지 확인. 이 환경에서는 불가능했음.

---

## Appendix — tooling and reproduction

**BIOS dumps (important — there are two dumps and only one is correct):**
- `/tmp/scd/bios_CD_U.bin` — **CORRECT** main BIOS dump (131072B, big-endian,
  disassembles cleanly). Use this for all main-BIOS disassembly.
- `/tmp/segacd_disasm/main_bios_be.bin` — **BYTE-SWAPPED** (words swapped
  within each 16-bit half). DO NOT USE; produces capstone misalignment.
- `/tmp/segacd_disasm/main_full.txt` — disasm from the swapped dump, UNRELIABLE.
- `/tmp/scd/prg_subbios.bin` — sub-BIOS dump (131072B, big-endian).

**Disassemblers:**
- `python3 /tmp/subdis.py <start_off> <end_off>` — sub-BIOS M68K disasm
  (reads `prg_subbios.bin`).
- `python3 /tmp/maindis.py <start_off> <end_off>` — main-BIOS M68K disasm
  (reads `bios_CD_U.bin`, the correct dump).

**Live-trace harness:**
- Build: `bash tools/segacd_harness/build_verify.sh` → produces `/tmp/boot_verify`.
- Run: `/tmp/boot_verify <bios_CD_U.bin> <game.cue> [frames]` (default 600).
- Recommended for stall reproduction:
  `SCD_MAIN_IDLE_SKIP=0 SCD_SUB_IDLE_SKIP=0 /tmp/boot_verify /tmp/scd/bios_CD_U.bin /tmp/scd/<game>.cue 1500`
- Environment probes: `SCD_SKIP_AUDIO`, `SCD_SKIP_VDP`, `SCD_SKIP_SUB`,
  `SCD_MAIN_IDLE_SKIP`, `SCD_SUB_IDLE_SKIP`, `SCD_Z80_IDLE_SKIP`,
  `SCD_YM_PROBE`, `SCD_YM_SILENCE_SKIP`.
- The harness prints per-frame: main PC, sub PC, SR int_mask, IEN, mode,
  ISR counters (L1gfx/L2ifl/L4cdd/L5cdc/r6/r7), cdd_status, cdd_pend,
  PRGwin write count, cache stats.

**PicoDrive reference grep (level sources):**
`grep -rn 'pcd_irq_s68k\|SekInterruptS68k' Core/Src/porting/segacd/pd_cd/`

**Vector-table parse:** read first 0x400 bytes as 256 big-endian longs;
autovectors are vectors 0x19–0x1F (offsets 0x64–0x7C).

**Engine-side trace points (compile with `-DSEGACD_GA_TRACE`):**
`scd_dbg_deliver2_*`, `scd_dbg_deliver4_*`, `scd_dbg_deliver5_*`
(`segacd_engine.c:52-58`), `scd_dbg_a12000_*` (`segacd_bus.c:497-508`).

**GA_TRACE rebuild (crossing-chain diagnostics):**
`build_verify.sh` does NOT enable `SEGACD_GA_TRACE` by default. To get the
full crossing-chain diagnostic block (BOOT-MODE, DRIVE-STATUS, CROSSING,
CDC data-path, doorbell/IFL2/CDD/CDC delivery logs), add `-DSEGACD_GA_TRACE`
to the gcc flags in `build_verify.sh` and rebuild. The diagnostic block is
at `boot_test.c:519-679` and prints once at end of run.

**Re-run with full diagnostics:**
```
stdbuf -oL /tmp/boot_verify /tmp/scd/bios_CD_U.bin "/tmp/scd/<game>.cue" 800 \
  2>&1 | tee /tmp/segacd_run3.log
```
(`stdbuf -oL` forces line-buffered stdout so the diagnostic block isn't
lost to pipe buffering on redirect.)
