# Sega CD 작업 이력

PicoDrive Sega CD 코어를 출하하기까지의 기록이다. 커밋은 `testbed`(release 브랜치 `release/segacd-picodrive`)에 있다.

## 커밋 순서

| 커밋 | 날짜 | 내용 |
|---|---|---|
| af3319d5 | 2026-09-14 | PicoDrive Sega CD 코어를 기기에서 처음 실행 |
| 90e5172a | 2026-09-30 | CHD를 cue로 바꾸는 파이프라인, 이미지 복구, 호스트 부팅 rig |
| 1d3b1a01 | 2026-09-30 | 일본판 디스크를 손대지 않고 부팅. 이를 깨뜨리던 섹터 0 "복구" 제거 |
| 96969688 | 2026-09-30 | 호스트 rig에 백업 RAM 입출력(BRAM_IN/BRAM_OUT) |
| 78caf2ad | 2026-09-30 | 코어 출하: 일본판 부팅, BRAM 세이브, 키 재배치 |
| e48fc8d9 | 2026-09-30 | 뜨거운 68K/Z80/타일 코드를 ITCM에서 실행. Final Fight CD 14 → 60fps |
| 5432c2a6 | 2026-09-30 | 256색 전체 팔레트에서 메뉴, 어둡게 하기, 세이브 썸네일 |
| bd5a5da6 | 2026-09-30 | 스케일링, 찢김 없는 화면, 단일 프레임버퍼에서 깨끗한 메뉴·테두리, 340 MHz 복귀 |
| bd60143b | 2026-10-01 | 색이 바뀔 때만, 빔이 아래 블랭킹에 있을 때 팔레트 업로드 |

## 성능

- Final Fight CD: 14 → 60fps(그린 프레임). 핵심은 hot 코드의 ITCM 배치였다. `tools/segacd_itc_gen.py`가 280개
  핸들러를 `segacd_itc_ops.ld`로 생성하고, 링크마다 `scripts/check_xip_sentinels.py`가 sentinel 창에 명령어가
  걸리지 않는지 검사한다.
- 클럭: 오버클럭 없이(레벨 0) 시험했으나 CD 오디오(SD를 SPI로 읽는 스트리밍)가 CPU의 약 33%를 먹어 프레임이
  떨어졌다. 레벨 2(340 MHz)로 복귀했다.

## 화면

- LUT8 단일 프레임버퍼(LCD 풀이 PRG RAM을 담는다). LTDC CLUT로 256색 전체를 쓰며 0xF8 이후는 메뉴 색, 0xFD는 검정.
- 행 쓰기는 빔 뒤를 따라간다(`LTDC->CPSR`/`BPCR`). 찢김 줄과 위쪽 흔들리는 줄을 이것과 팔레트 업로드 시점으로 없앴다.
- 스케일링: 끄기(가운데), 맞춤(H32를 320으로 늘림), 전체(224→240까지, 기본값), 사용자 지정(전체와 같음).
  필터링은 Sega CD에서 효과가 없다.

## 기기에서 확인한 것

세이브/불러오기, 메뉴 색, 썸네일(검지 않음), 전원 끄기 후 재개, 스케일링 "전체", 찢김 줄 없음, 메뉴 안정.
사용자가 화면을 보고 확인했다.

## 확인하지 못한 것

스케일링 "끄기"와 "맞춤"은 같은 코드 경로지만 기기 화면으로 확인하지 않았다(그때 다른 세션이 디버그 프로브를 쓰고 있었다).

## 관련 문서

`docs/SEGACD_HANDOFF_0930.md`, `docs/SEGACD_INVESTIGATION.md`, `docs/SEGACD_REASSESSMENT_2026-09-14.md`,
`Core/Src/porting/segacd/CLAUDE.md`, `docs/OPTIMIZATION_LEDGER.md`, `docs/HARNESSES.md`.
