# SegaCD Investigation Report: CDC DMA Chain & $6132 Stall

## 1. CDC DMA 체인 분석 및 $6132 데드락의 진실

### 하드웨어 정상 동작 체인 (vs 현재 오해)
`300bda6d` 세션이 "DTEI 인터럽트 → DTRG 프로그램"으로 오해하고 있으나, 하드웨어 스펙 및 서브 BIOS 분석 결과는 다음과 같습니다:
1. **CDD Play (cmd3)**: CDD가 디스크를 읽기 시작.
2. **CDC Decoder (75Hz)**: 섹터 헤더를 읽고 **`DECI` (Decode Interrupt, Level 5)** 발생. (데이터가 디코드되었다는 레디 신호는 `DTEI`가 아니라 `DECI`입니다).
3. **Sub-CPU ISR (`0x634`)**: `DECI` 인터럽트(Level 5)를 받아 서브 BIOS의 ISR이 실행됨. 섹터 헤더를 확인하고 `STAT3` 레지스터를 읽어 `DECI` 상태를 클리어함.
4. **Sub-CPU DTRG 프로그래밍**: 원하는 섹터 데이터가 버퍼에 실리면, ISR이 `DTRG` (CDC Register 6) 레지스터를 기록하여 DMA 전송 개시.
5. **DTEI (Data Transfer End Interrupt)**: DMA 전송이 **완료**된 후 CDC가 발송하는 인터럽트.

### 우리 구현이 멈춘 진짜 이유 (Trace 로그 분석)
CDC DMA가 0회인 이유는 서브 CPU가 CDD 응답을 기다리며 무한 재시도 중인 것이 아닙니다. 
1. **Main 68K 크래시 (가장 핵심적인 원인)**: 하네스 테스트 로그(`task-132.log`)를 분석한 결과, HLE Bypass가 `main PC 004bb4 -> $FF0000 (direct IP entry)`로 점프했습니다. `$FF0000`은 소닉CD Initial Program의 코드 시작점이 아니라 "SEGA" (아스키코드 `53 45 47 41`)가 적힌 **헤더 데이터 영역**입니다. 이를 코드로 실행하다가 `Illegal Instruction` 예외가 발생했고, Main 68K는 BIOS 예외 핸들러인 `0x210` (무한 루프)에 빠져 크래시(사망)했습니다.
2. **Sub 68K $6132 데드락**: 이전 문서(`SEGACD_BOOT_CROSSING_RE.md`)의 분석과 동일하게, `$6132`는 PRG-RAM `0x97EB` 바이트를 Main 68K가 클리어해주길 기다리는 IPC 스핀 루프입니다. Main 68K가 `0x210`에서 죽었으니 영원히 클리어되지 않습니다.
3. **IEN5 마스킹**: 크래시 당시 Sub CPU의 인터럽트 마스크는 `IEN = 0x54` 였습니다. 즉, Level 5(`0x20`) 인터럽트가 비활성화된 상태입니다. `DECI`가 발생해도 ISR(`0x634`)이 호출될 수 없으므로 `DTRG`를 프로그램할 코드가 돌지 못합니다. (Main CPU가 정상 동작하여 상태가 넘어가야 IEN5가 열릴 것으로 보입니다).

## 2. PicoDrive 레퍼런스 재검토

- **PicoDrive의 DECI/DTEI 구현**: PicoDrive의 `cdc.c`는 `check_decoder_irq_pending()` 함수에서 `DECI` 비트를 약 67,250 사이클 후에 자동으로 클리어(auto-clear)합니다. 반면 우리는 `STAT3` 읽기 시에만 클리어합니다 (`segacd_cd.c`의 FIXME 주석 참고). 하지만 이는 현재 교착의 원인이 아닙니다.
- **PicoDrive Standalone이 소닉CD에서 멈추는 이유**: 코어 에뮬레이터 버그가 아닙니다. 독립형 PicoDrive의 단순한 `.cue` 파서는 소닉CD 같은 Multi-track (Data + Audio) 디스크 처리 시 Track 1을 제대로 Data로 파싱하지 못하는 경우가 있습니다. `t->is_audio`가 `true`로 오인되면 `cdc_decoder_update`를 패스해버려 데이터를 밀어넣지 않고, 똑같이 부팅 불가에 빠집니다.

## 3. Mednafen 스냅샷 경로 현실성 판별

**결론: 완전 비현실적 (Unrealistic) - 즉시 중단 권고**
Mednafen은 gwenesis(PicoDrive/Genesis Plus GX 기반)와 아키텍처, 68K 코어 구현, VDP 상태, CDC/CDD 내부 메모리 모델이 100% 다른 독립 에뮬레이터입니다. Mednafen의 savestate 바이너리를 파싱해서 gwenesis의 C 구조체(메모리, 레지스터, 타이머 사이클 등 수만 가지 상태)에 1:1로 매핑하는 것은 사실상 새로운 에뮬레이터를 작성하는 수준의 엄청난 역공학 작업입니다. 스냅샷 시도는 시간 낭비입니다.

## 4. 정직한 방향 권고 (GO)

**현재 구현은 게임플레이 부팅에 매우 근접해 있습니다. HLE 점프 주소 실수만 고치면 됩니다 (방향: GO).**

1. **HLE Bypass 복구/수정 (가장 시급)**: `boot_test.c`의 IP 강제 점프 주소를 `$FF0000`에서 코드가 실제로 시작되는 `$FF0100`으로 수정하거나, 과거 성공했던 방식인 BIOS IP 체크 루틴(`$064C`)으로 롤백하십시오. `$FF0000` 점프는 아스키 문자열을 M68K 명령어로 해석하게 만들어 Main CPU를 즉사(`0x210` 예외 루프)시킵니다. Main CPU 크래시만 풀리면 Sub CPU의 `$6132` IPC 교착도 연쇄적으로 해소됩니다.
2. **DTEI vs DECI 오해 정정**: 300bda6d 세션에 "서브 코드가 `DTRG`를 치게 만들려면 `DTEI`가 아니라 `DECI`(Level 5) 인터럽트가 트리거되어야 함"을 인지시키십시오. Main CPU 크래시가 수정되면, Sub CPU가 정상 진행되어 `IEN5` 마스크를 해제할 것이고 자연스럽게 CDC DMA 체인이 작동할 것입니다.
