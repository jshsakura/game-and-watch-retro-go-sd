# RC_DISPATCH 구조 및 OOM 크래시 분석 리포트

## 1. 구조 및 문제 상황 파악
- **동작 방식**: `rc_dispatch`는 SMW 정적 재컴파일러(rc)의 핫패스로, 매 옵코드마다 `cpu_runOpcode`에서 호출(`cpu.c:147`)되어 현재 PC가 번역된 사이트인지 찾습니다.
- **기존 OOM 원인**: 기존 구현은 활성화 시점(`rc_dispatch_init`)에 각 뱅크별 해시테이블을 DTCM 힙에서 `malloc`으로 동적 할당했습니다. 런처가 이미 DTCM 81KB 중 약 72KB를 선점해 가용 힙이 ~8KB밖에 남지 않았고, 이에 85KB 크기의 해시테이블 할당을 시도하다 OOM이 발생했습니다.
- **해시 알고리즘 로직**: Knuth Multiplicative Hash (`(pc * 2654435761u) & mask`)와 Linear Probing을 사용하는 Open-addressing 구조입니다. 테이블 사이즈(`sz`)는 `next_pow2(count * 2)`로 산출해 Load Factor 0.5 이하를 유지합니다.

## 2. rc_addrs 실제 분포 및 할당 바이트 분석
`generated/rc_smw/rc_sites.inc`의 `rc_addrs` 배열(8371개)을 파싱한 뱅크별 실제 분포와 필요 해시테이블(`rc_entry_t`, 4바이트) 크기입니다.

| Bank | Count | 테이블 크기(sz) | 할당 요청 바이트(sz * 4) |
|---|---|---|---|
| 0x00 | 3767 | 8192 | 32768 Bytes |
| 0x01 | 572 | 2048 | 8192 Bytes |
| 0x02 | 279 | 1024 | 4096 Bytes |
| 0x04 | 1501 | 4096 | 16384 Bytes |
| 0x05 | 1550 | 4096 | 16384 Bytes |
| 0x07 | 70 | 256 | 1024 Bytes |
| 0x0D | 632 | 2048 | 8192 Bytes |
| **Total** | **8371** | | **87040 Bytes (85.00 KB)** |

- **오류 분석**: 총 필요 크기는 주석의 ~60KB가 아닌 **85.00 KB**입니다. 
- **OOM `need=16392`의 정체**: Bank 0x04 또는 0x05 할당 시 요청된 16384바이트에 `malloc` 헤더 오버헤드 8바이트가 더해진 크기입니다. 순차적 할당 도중 힙 여유 공간(8KB)을 초과하여 터진 것입니다.

## 3. RAM_EMU 정적해시(Static Array) 타당성 검증
`build/gw_retro_go.map` 및 링커스크립트 `STM32H7B0VBTx_SDCARD.ld` 분석 결과:
- `RAM_EMU` 총 크기: **741,376 Bytes** (0xB5000)
- 현재 `.overlay_snes` 섹션 크기: **74,788 Bytes**
- 현재 `.overlay_snes_bss` (WRAM 등 포함) 섹션 크기: **362,436 Bytes**
- SNES 오버레이 총 사용량: **437,224 Bytes**
- 정적 해시테이블 크기 추가: **87,040 Bytes**
- 합계: 437,224 + 87,040 = **524,264 Bytes**
- **마진(Margin)**: 741,376 - 524,264 = **217,112 Bytes (약 212 KB)**

**결론**: `RAM_EMU`(.overlay_snes_bss) 공간에는 212KB의 넉넉한 마진이 존재합니다. 다른 오버레이(PCE, MSX 등)는 실행 시 상호 배타적이므로 영향을 주지 않습니다. 따라서 `malloc`을 제거하고 정적 배열 버퍼로 옮기는 현재 GLM의 수정 방향은 완벽하게 올바르고 안전합니다.

## 4. 대안 구조 판정 및 최적화 제안
요구되는 3가지 제약 조건: (a) `O(1)` 수준의 극단적 핫패스 속도, (b) 8KB 미만의 DTCM 힙 소모, (c) XIP 플래시 접근 최소화 (RAM/ITCM 상주).

1. **빌드타임 정렬배열 이진탐색 (Binary Search)**
   - 메모리는 가장 작음(약 33KB). 
   - 하지만 뱅크0(3767개)의 경우 평균 11~12회의 루프 반복과 메모리 분기 비용이 발생합니다. 이는 인터프리터 절약분(-42%)을 심각하게 갉아먹으므로 기각됨이 타당합니다.
2. **2단계 페이지 테이블 (2-Level Page Table)**
   - 계산 결과: 전체 8371개 사이트가 포함된 256-Byte 페이지는 총 198개입니다. 
   - 메모리 요구량: L1 테이블(7뱅크 * 256 * 2B) + L2 테이블(198페이지 * 256 * 2B) = **104,960 Bytes (약 102.5 KB)**
   - 2번의 메모리 접근이 필요하고 크기마저 기존 오픈어드레싱 해시(85KB)보다 큽니다.
3. **현재의 Open-addressing 해시 (Load Factor 0.5)**
   - `O(1)`에 가장 근접하며, 대부분 1번의 캐시라인 읽기로 룩업이 끝납니다. 
   - 총 85KB 크기이며 RAM_EMU 정적 공간에 완벽히 들어갑니다.

**권고**: 추가적인 복잡한 자료구조 도입 없이, 현재의 Knuth Multiplicative Open-addressing 해시 알고리즘을 유지하되 **BSS 정적 배열 할당**으로만 전환하는 것이 최선(Best Solution)입니다.

## 5. 재발방지 검증 및 링크타임 보호
1. **링커 ASSERT 방어벽**
   - 링커 스크립트(`STM32H7B0VBTx_SDCARD.ld`)에 이미 존재하는 아래의 ASSERT문이 이 문제를 컴파일(링크) 타임에 정확히 잡아냅니다.
   ```ld
   ASSERT(ABSOLUTE(_OVERLAY_SNES_BSS_END) < __RAM_EMU_END__, "Error: SNES BSS overflow");
   ```
   - 해시테이블을 정적 버퍼로 전환하면 힙 부족(Runtime OOM)이 아닌 BSS 증가로 반영되므로, RAM_EMU 용량을 초과할 경우 빌드가 실패하여 실기 부팅 전 문제를 100% 차단합니다.
2. **호스트 테스트 베드 검증 (tests/test_rc_dispatch_heap.c)**
   - `malloc` / `free` 호출을 래핑(Wrapping)하여 **할당 횟수가 0건**인지 `check()` 하는 테스트 로직을 구성해야 합니다 (DTCM 힙 보호 증명).
   - 원본 해시 결과나 선형 탐색(Linear Scan)과 비교해 모든 8371개 PC에 대해 정확한 `id`가 반환되는지 무결성 테스트가 병행되어야 합니다.
