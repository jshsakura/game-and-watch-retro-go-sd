# Sega CD handoff — 2026-09-30

> **이 문서의 진단은 같은 날 오후에 뒤집혔습니다.** hang 9개와 "리프로 섹터0 비표준"은
> 전부 `segacd_fix_security.sh`가 만든 것이었습니다. 12개 디스크는 정상적인 **일본판**이고
> (지역 `J`, JP 보안 블록 문자열 0x30E), 일본 BIOS는 디스크가 있어도 메뉴에서 START를
> 기다립니다. 그걸 이미지 결함으로 오진해 US 보안 블록을 0x200~0x783에 덮었고, 그 자리는
> JP 디스크의 IP 코드 시작점(0x356, 루나는 여기서 0x49A1 핸드셰이크를 씀)이었습니다.
> 원본 섹터0 + `Core/Inc/porting/segacd/segacd_boot_start.h`(BIOS 메뉴에서 START 자동)로
> 15/15가 호스트에서 f640 안에 게임 코드에 들어갑니다. 소닉 CD 포함.
> 아래 "확립된 사실" 2·4·5·7번과 "루나 hang 해부", "다음 단계"는 기록으로만 남깁니다.
> 메인 68K 0x000000~0x01FFFF는 BIOS ROM이지 워드램이 아닙니다(`pico/cd/memory.c:1253`).

한 줄: **CHD→cue/bin 변환 + 보안 블록 패치로 15/15 게임 부트 성공. 4게임 완전 동작,
9게임이 인트로 후 동일 패턴으로 hang — 루나를 대표로 원인 추적 중이며 "메인 68K가
BIOS 예외 파킹 스텁(0x210)에 주차, 서브는 comm 0x49A1 대기"까지 규명됨.** 다음 세션은
"다음 단계"부터 시작.

## 경로 / 재현

| 것 | 경로 |
|---|---|
| 작업 워크트리 | `/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-segacd-pd2` (branch `exp/picodrive-segacd-20260914`) |
| 호스트 하네스 | `tools/pico_host_segacd/` — `./run.sh <rom.cue> <frames> <pat>` (pat: none/play/amash/...) |
| 변환 파이프라인 | `tools/segacd_chd_convert.sh` (CHD→cue/bin), `tools/segacd_fix_security.sh` (섹터0 보안블록 패치) |
| 변환된 라이브러리 | `/media/pi/EXTERNAL/gnw-segacd-cue/<게임명>/<게임명>.cue` (15게임, `.orig` 백업 있음) |
| 원본 CHD | `/media/pi/EXTERNAL/miyoo-library/public/roms/segacd/` (전부 CHD v5) |
| BIOS | `/media/pi/EXTERNAL/BIOS/bios_CD_{U,E,J}.bin` (3종 md5 정품 확인) |
| 디버그 산출물 | `/tmp/opencode/` — lunar_cdd_trace.log, wram.bin, prg0_sub.bin, bios_low_live.bin, *.gdb |
| 참조 에뮬 | `/tmp/opencode/picodrive-upstream` (notaz master + NO_32X 소패치), `/tmp/opencode/gpgx` (GPGX libretro) |
| libretro 러너 | `/tmp/libretro_runner2 <core.so> <cue> <frames>` (AUTO_START=<frame> env) — argv[1] 코어 정상 사용 |

하네스 빌드는 `tools/pico_host_segacd/build/`에 .o 캐시. `TOGGLE=1`(기본)이면
`ovr/pico/cd/`의 패치된 cd_parse/cd_image 사용, `TOGGLE=0`이면 순정. env: `FB_OUT`,
`PAL_OUT`, `STATE_IN`, `BIOS_DIR`. 프레임 로그: `grep -E '^f[0-9]{4} '`.

커밋: `90e5172a` (변환 스크립트+하네스+문서). ovr/ 실험 오버레이는 이번 커밋에 추가.

## 확립된 사실 (재검증 불요)

1. **CHD 온디바이스 디코딩 불가** (RAM 여유 ~30KB vs libchdr 150-200KB). cue/bin 변환이
   유일 경로. 런처는 segacd 확장자 "cue"만 등록.
2. **리프로 CHD의 섹터0 보안 블록은 비표준 위치**(정상 0x669에 `PRODUCED BY OR UNDER
   LICENSE...`가 있어야 하나 0x310 등 제각각) + 루나2는 시스템 ID도 비표준
   (`LUNAR2     `). → `segacd_fix_security.sh`가 둘 다 패치, 15/15 부트.
3. **부팅은 느리다**: MCD BIOS가 초기 체크 실패→CD 플레이어→수십 초 후 재시도로 부트.
   Hello World f620-1480 사이 부트. 600프레임 조사는 너무 짧음 — 판정은 1500f+ 권장.
4. **라이브러리 분류 (2000f 'play' 스윕)**:
   - 완전 동작(4): 파이널파이트CD, 마이티모핀, 리벤지오브더닌자, 안드로이드어설트
   - 불확실(2): 포플메일(메인 능동이나 lba=10 정지), 루나2(cdd=05 정지)
   - **hang(9)**: 소닉(lba=10249), 루나(19), 실피드(19), 유미미믹스(19),
     데토네이터오건(19), 경응유격대(31), 로드블라스터FX(2288), 타임갈(2289),
     3x3아이즈(-8) — 전부 인트로 후 `pc=000210 cdd=04` 동결
5. **hang은 코어 버그가 아님**: fork 하네스, notaz upstream 하네스, upstream libretro,
   GPGX(libretro) 4개 전부 동일 지점 정지. 반면 **미유미니(동일 CHD, libretro
   picodrive + USE_LIBCHDR 직접 CHD 로드)는 정상** (사용자 확인).
6. **TOC 시작점은 유일한 범인이 아님**: 오버레이로 TOC를 CHD 메타데이터 진실과
   정확히 일치시켜도(루나 t2@14323) hang 지속. 미유미니 chd_parse TOC(t2@~14174)는
   우리 무패치값(14173)과 사실상 동일. 즉 cue/bin 경로와 libchdr 경로의 **다른 차이**가
   범인.
7. **소닉 리프로는 별도 결함**: 트랙1=123MB(원본 추정 ~20MB) ISO 재조립 + 오디오
   재배치로 게임의 고정 MSF 테이블과 불일치 — 3개 에뮬 전부 동일 hang, 이미지 자체
   문제. 원본 덤프 없이 해결 불가.

## 루나 hang 해부 (대표 케이스, 전부 gdb 실증)

- 부팅 후 f840-f1120 인트로 애니메이션(메인 게임코드 실행, pc<0x20000 = 1M 모드
  워드램 뱅크 @0). f1140부터 완전 동결.
- **메인**: pc=0x210 = BIOS 하위 64K(쓰기 가능 사본)의 예외 파킹 스텁.
  스텅 내용(정체 규명됨):
  ```
  000200: lea.l   $fd00.w, a0        ; BIOS 기본 예외 핸들러
  000204: move.w  #$4ef9, d0         ; 0xFD00에 "jmp $884" 설치하고
  000208: move.w  d0, (a0)+
  00020a: move.l  #$884, (a0)+
  000210: bra.b   $210               ; <- 여기서 무한 파킹
  000212: rte
  ```
- 벡터 테이블(bios_low_live): vec2/12-25/27/29/31/48-63 → 0x200(파킹),
  vec3-11/26/28/30 등 → 0xfffffdxx(램 핸들러, 정상). 즉 **예약/유저 벡터로 들어온
  예외**(스퓨리어스 IRQ=vec24 포함)로 추정.
- **동결 시 wram.bin[0:0x400]은 텍스처 데이터**(유효 벡터 아님, 홀수 주소 포함).
  1M 모드로 뱅크@0 실행 시점에 게임이 자체 벡터를 0에 설치하지 않았다는 뜻.
  또한 애니메이션 중 pc가 있던 0x18f6/0x56e2 영역도 동결 시점엔 전부 0x00000000 —
  **어떤 시점에 워드램 내용이 교체/클리어됨** (뱅크 스왑? 다음 조사 대상).
- **서브**: pc2=0x631e = `cmpi.w #$49a1, $ff8018` 루프 — 메인이 comm 레지스터
  0xFF8018에 0x49A1 쓰기를 영원히 대기. 이후 WRAM 2M/서브 소유 전환 → 다음 스테이지
  로드 예정. 전 실행 기간 0x49A1 쓰기 0회. 수동 주입 시 서브는 진행하나 메인이 주차
  상태라 화면 정지 — 교착 확정.
- CDD 트레이스: f0780 게임이 cmd=06(PAUSE) at lba=19 → 이후 cmd=02(TOC 폴링)만
  무한. PLAY/SEEK 재발행 없음.

## 기각된 가설 (재시도 금지)

- fork GNW_MCD_SPLIT/BIOS_XIP 코드 버그 (vanilla도 동일)
- FAME 점프테이블/FAMEC_NO_GOTOS, 서브 페치맵 오염 (Fetch 실측 정상)
- TOC 오디오 트랙 시작점 (CHD 진실 일치시켜도 hang)
- 씨 cue INDEX00/pregap 구조 단순화, BIN 2352→2048 ISO, 리전/BIOS 페어링
- IP.BIN 헤더 내용 (소닉 완전 표준)
- POPT_EN_MCD_GFX 부재 (넣으면 CD 플레이어만 애니메이션, 부팅 무관)
- 코어 버그 전반 (upstream/GPGX 동일 hang — 단, GPGX도 우리 cue를 파싱하므로
  cue 해석 공통 버그 가능성은 잔존. 미유미니=CHD 직접이 유일한 차이)

## 다음 단계 (우선순위순)

1. **PARK_RTE 실험**: 하네스에서 PicoLoadMedia 후 `Pico_mcd->bios` 하위 64K의
   0x210 byteswap 저장 위치에 `rte`(0x4E73)를 박아 예외에서 복귀시켜 보라. 게임이
   계속 진행되면 스퓨리어스/일회성 예외(IRQ 타이밍) → 타이밍 문제로 좁혀짐.
   재발진하면 영구적 일리걸 = 데이터 손상 경로.
2. **libchdr libretro 참조 빌드**: `/tmp/opencode/picodrive-upstream`을
   `Makefile.libretro` + `USE_LIBCHDR=1`로 빌드(미유미니와 동일 구성)해 원본 CHD를
   직접 로드. 루나가 f1140을 넘어가면 → 동작 시점의 WRAM/PRG 덤프를 확보해 우리
   hang 덤프와 **바이너리 diff** → 어느 섹터 데이터가 다른지 → 그 섹터의 읽기 경로
   비교. 넘어가지 않으면 미유미니 차이는 코어 버전/BIOS 쪽.
3. **워드램 교체 시점 추적**: 애니메이션 중 실행 코드(0x18f6/0x56e2)가 동결 시 0으로
   클리어된 원인. gdb로 remap_word_ram/뱅크 전환 감시(watchpoint on s68k_regs[3] 쓰기
   또는 memory.c 오버레이에 로그) — 핸드오프 시점의 메모리 상태 이해가 예외 원인의
   직접 단서일 수 있음.
4. wram 벡터 영역(0x78=vec30 등)에 watchpoint: 게임이 언제/무엇으로 벡터 영역을
   덮는지, 아예 설치를 안 하는 건지 확인.
5. 소닉은 원본 덤프 확보 전 불가명시. 나머지 hang 8게임은 루나와 동일 메커니즘인지
   1-2개만 확인해 일반화.

## 도구 메모

- gdb 배치: cdd.c:852(커맨드 트레이스), `dump binary memory f Pico_mcd->prg_ram_b[0] Pico_mcd->prg_ram_b[0]+65536`, commands 블록 내 `next` 금지. 하네스 루프 변수는 -O1으로 심볼 소멸.
- 코어 메모리는 워드 단위 바이트스왑 저장 → capstone 전 페어 스왑. 값 0x00000210은 덤프에 `00 00 10 02`.
- zsh: `==` glob 에러, xxd 없음(python 사용), rg 없음.
- CHD 메타데이터 읽기: `head -c 8192 x.chd | strings | grep TRACK`.
- 캐노니컬 빌드 플래그/디바이스는 testbed 워크트리(GPT 32X 작업 중) 참고. 디바이스 배포는 아직 없음(하네스 단계).
