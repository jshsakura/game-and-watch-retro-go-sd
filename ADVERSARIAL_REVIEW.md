# Game & Watch Retro-Go SD - 적대적 코드 리뷰 (Adversarial Code Review)

본 문서에는 **game-and-watch-retro-go-sd** 프로젝트(실험적 연구용 포크)의 펌웨어 및 연동 레이어 코드를 분석하여 발견된 보안 취약점, 버퍼 오버플로우, 메모리 오염, 그리고 동작 안정성 위험 요소들에 대한 적대적 리뷰(Adversarial Review) 결과를 기술합니다.

---

## 1. 취약점 요약 (Vulnerability Summary)

| ID | 중요도 (Severity) | 취약점 종류 (Vulnerability Type) | 대상 파일 및 함수 | 내용 요약 |
| :--- | :--- | :--- | :--- | :--- |
| **AR-01** | **Critical** | Stack/Heap Buffer Overflow | `retro-go-stm32/retro-go/main/emulators.c`<br>`emulator_build_file_object()` | `strncpy` 복사 시 버퍼 크기 한계가 아닌 원본 문자열 길이를 인자로 전달하여 구조체 내 인접 멤버 오염 발생. |
| **AR-02** | **High** | Heap Buffer Overflow | `retro-go-stm32/retro-go/main/favorites.c`<br>`favorites_save()` | 즐겨찾기 저장 시 잘못된 버퍼 크기 계산(`favorites_count * 128`)으로 인해 힙 오버플로우 및 `NULL` 포인터 역참조 발생 가능. |
| **AR-03** | **High** | Global Buffer Overflow | `Core/Src/retro-go/rg_utils.c`<br>`rg_dirname()` | 100바이트 크기의 정적 버퍼(`static char buffer[100]`)에 대한 경계 검사가 누락되어, 긴 경로 처리 시 `.data`/`.bss` 영역 오염. |
| **AR-04** | **Medium** | Out-of-Bounds Memory Access | `Core/Src/gw_flash_alloc.c`<br>`circular_flash_write()` | 파일 크기가 홀수이거나 홀수 바이트를 읽었을 때 `byte_swap` 루프에서 uninitialized stack data를 참조하여 데이터가 손상되거나 스택 메모리가 플래시에 유출됨. |
| **AR-05** | **Medium** | Logical Bug / Path Resolution | `Core/Src/retro-go/rg_storage.c`<br>`rg_storage_get_adjacent_files()` | 루트 경로의 파일을 탐색할 때 디렉터리 경로가 빈 문자열(`""`)로 설정되어 FatFs에서 현재 작업 디렉터리(CWD)로 오조인됨. |
| **AR-06** | **Low** | Security Hardening (State Leak) | `Core/Src/retro-go/rg_rtc.c`<br>`GW_RTC_RestoreIfLost()` | 백업 도메인 레지스터 쓰기 방지를 풀고(`HAL_PWR_EnableBkUpAccess`) 다시 잠그지 않아 외부 요인에 의한 RTC 영역 오염 가능성 존재. |

---

## 2. 상세 취약점 분석 및 수정 방안 (Detailed Findings)

### AR-01: [Critical] `emulator_build_file_object` 구조체 멤버 메모리 오염
* **위치**: [`retro-go-stm32/retro-go/main/emulators.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/retro-go-stm32/retro-go/main/emulators.c#L162-L188)
* **코드 조각**:
  ```c
  memset(file, 0, sizeof(retro_emulator_file_t));
  strncpy(file->folder, path, strlen(path)-strlen(name)-1);
  strncpy(file->name, name, strlen(name)-strlen(ext)-1);
  strcpy(file->ext, ext);
  ```
* **문제 원인**:
  - `retro_emulator_file_t` 구조체는 [`retro-go-stm32/retro-go/main/rg_emulators.h`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Inc/retro-go/rg_emulators.h#L33-L50)에 정의되어 있으며, `folder`는 **32바이트**, `name`은 **128바이트** 크기입니다.
  - 하지만 복사할 때 `strncpy`의 세 번째 인자로 원본 폴더 경로의 길이(`strlen(path)-strlen(name)-1`)를 전달하고 있습니다.
  - 만약 SD 카드의 게임 경로가 `/roms/nes/very_long_nested_folder_name/game.nes`와 같이 폴더명이 31자를 초과할 경우, `file->folder`를 초과하여 스택/힙 공간에 오버플로우를 발생시킵니다.
  - 이로 인해 구조체의 인접 멤버인 `size`, `crc_offset`, `checksum`, `emulator` 등의 메모리가 무작위 문자열 데이터로 오염되어 하드폴트(HardFault)가 발생하거나 포인터 변조로 임의 코드가 실행될 수 있습니다. 또한, `strncpy`는 복사 크기가 소스 길이와 정확히 맞아떨어지거나 클 때 널 종료 문자(`\0`)를 보장하지 않습니다.
* **대응 방안**:
  - 목적지 버퍼의 실제 용량을 기준으로 경계를 체크하고 수동으로 널 종료를 보장해야 합니다.
  ```c
  size_t folder_len = strlen(path) - strlen(name) - 1;
  if (folder_len >= sizeof(file->folder)) {
      folder_len = sizeof(file->folder) - 1;
  }
  strncpy(file->folder, path, folder_len);
  file->folder[folder_len] = '\0';

  size_t name_len = strlen(name) - strlen(ext) - 1;
  if (name_len >= sizeof(file->name)) {
      name_len = sizeof(file->name) - 1;
  }
  strncpy(file->name, name, name_len);
  file->name[name_len] = '\0';
  ```

---

### AR-02: [High] `favorites_save` 함수에서의 힙 오버플로우 & 널 포인터 역참조
* **위치**: [`retro-go-stm32/retro-go/main/favorites.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/retro-go-stm32/retro-go/main/favorites.c#L116-L131)
* **코드 조각**:
  ```c
  static void favorites_save()
  {
      char *buffer = calloc(favorites_count, 128);

      for (int i = 0; i < favorites_count; i++)
      {
          if (!favorites[i].removed) {
              strcat(buffer, favorites[i].path);
              strcat(buffer, "\n");
          }
      }
      ...
  ```
* **문제 원인**:
  - `favorite_t` 구조체의 `path` 멤버는 최대 **168바이트** 크기입니다.
  - 그러나 버퍼 할당 시 `favorites_count * 128` 바이트만 할당하고 있습니다.
  - 만약 사용자가 추가한 즐겨찾기 경로가 평균 128자를 초과(최대 167자 가능)하는 파일들이 여러 개 존재한다면, 문자열을 덧붙이는 과정에서 `buffer` 범위를 넘어 힙 메모리를 손상시키는 힙 오버플로우가 발생합니다.
  - 또한, `favorites_count`가 `0`일 때 `calloc(0, 128)`은 플랫폼 컴파일러 구현에 따라 `NULL`을 리턴할 수 있으며, 이 경우 루프는 돌지 않더라도 후속 라인의 `odroid_settings_string_set(..., buffer)`로 `NULL`이 전달되어 오동작 혹은 크래시를 유발할 수 있습니다.
* **대응 방안**:
  - 버퍼 크기를 즐겨찾기 항목이 가질 수 있는 최악의 크기(`favorites_count * sizeof(((favorite_t *)0)->path) + 1`)로 안전하게 할당하거나, 저장 전 각 파일 경로들의 길이를 누적 합산하여 정밀하게 할당하도록 수정해야 합니다.
  ```c
  size_t required_size = 1; // Null terminator
  for (int i = 0; i < favorites_count; i++) {
      if (!favorites[i].removed) {
          required_size += strlen(favorites[i].path) + 1; // path + '\n'
      }
  }
  char *buffer = calloc(1, required_size);
  if (!buffer) return;
  ```

---

### AR-03: [High] `rg_dirname` 정적 버퍼 크기 부족으로 인한 글로벌 오버플로우
* **위치**: [`Core/Src/retro-go/rg_utils.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Src/retro-go/rg_utils.c#L32-L50)
* **코드 조각**:
  ```c
  const char *rg_dirname(const char *path)
  {
      static char buffer[100];
      const char *basename = strrchr(path, '/');
      ptrdiff_t length = basename - path;

      if (!path || !basename)
          return ".";

      if (path[0] == '/' && path[1] == 0)
          return "/";

  //    RG_ASSERT(length < 100, "to do: use heap");

      strncpy(buffer, path, length);
      buffer[length] = 0;

      return buffer;
  }
  ```
* **문제 원인**:
  - 디렉터리 경로명을 임시 보관하기 위해 정적 메모리 `static char buffer[100]`를 사용하고 있습니다.
  - `RG_ASSERT(length < 100)` 안전 장치가 주석 처리되어 있습니다.
  - SD 카드의 전체 파일 경로 중에서 상위 디렉터리 경로의 길이가 99바이트 이상이 될 경우, `strncpy`가 100바이트를 초과하여 복사하고 `buffer[length] = 0` 위치가 버퍼 밖이 되므로 글로벌 변수 영역(.data/.bss)을 훼손하여 시스템 전반에 의도치 않은 상태 변조를 유발합니다.
* **대응 방안**:
  - 디렉터리 길이의 최댓값을 펌웨어 시스템 상한(예: `RG_PATH_MAX`)으로 버퍼 크기를 확보하거나, 복사 길이를 안전하게 한정해야 합니다.
  ```c
  static char buffer[256]; // RG_PATH_MAX 수준으로 크기 확장
  if (length >= (ptrdiff_t)sizeof(buffer)) {
      length = sizeof(buffer) - 1;
  }
  strncpy(buffer, path, length);
  buffer[length] = '\0';
  ```

---

### AR-04: [Medium] `circular_flash_write` 홀수 바이트 파일 복사 시 버퍼 오버런 및 데이터 오염
* **위치**: [`Core/Src/gw_flash_alloc.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Src/gw_flash_alloc.c#L211-L325)
* **코드 조각**:
  ```c
  if (byte_swap) {
      for (size_t i = 0; i < bytes_read; i += 2) {
          uint8_t temp = buffer[i];
          buffer[i] = buffer[i + 1];
          buffer[i + 1] = temp;
      }
  }
  ```
* **문제 원인**:
  - ROM이나 기타 파일 데이터를 QSPI 플래시에 쓰기 전 바이트 스왑(`byte_swap`)을 진행할 때, 파일 크기가 홀수이거나 스트림 오동작 등으로 `bytes_read`가 홀수일 가능성이 있습니다.
  - 만약 `bytes_read`가 홀수일 경우 `i = bytes_read - 1` 일 때 `i < bytes_read` 조건이 통과되면서 `buffer[i + 1]`에 쓰기/읽기 접근을 시도하게 됩니다.
  - `buffer[i+1]`은 유효하지 않은 버퍼 범위를 가리키며(스택 쓰레기 값 포함), 이로 인해 파일의 마지막 바이트 데이터가 스택 프레임의 무작위 값과 교환되어 데이터 손상이 일어납니다. 더불어 민감한 스택 정보가 플래시에 함께 라이팅되는 정보 유출(Information Leak)이 일어납니다.
* **대응 방안**:
  - `bytes_read`가 홀수인 경우 마지막 1바이트는 스왑하지 않도록 루프 범위를 안전하게 한정하거나 파일 크기 홀수 여부를 미리 체크해야 합니다.
  ```c
  if (byte_swap) {
      size_t limit = bytes_read & ~1; // 짝수로 내림
      for (size_t i = 0; i < limit; i += 2) {
          uint8_t temp = buffer[i];
          buffer[i] = buffer[i + 1];
          buffer[i + 1] = temp;
      }
      // 홀수 번째 마지막 1바이트는 스왑하지 않고 그대로 유지
  }
  ```

---

### AR-05: [Medium] `rg_storage_get_adjacent_files` 루트 디렉터리 경로 오인
* **위치**: [`Core/Src/retro-go/rg_storage.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Src/retro-go/rg_storage.c#L425-L450)
* **코드 조각**:
  ```c
  char dir[RG_PATH_MAX];
  strncpy(dir, path, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  char *last_slash = strrchr(dir, '/');
  if (!last_slash) return false;
  *last_slash = '\0';
  ```
* **문제 원인**:
  - 만약 ROM 파일이 SD 카드의 최상위 폴더에 위치해 있어 `path`가 `/game.gbc`인 경우, `last_slash`는 인덱스 0이 되고 이를 `\0`으로 처리하면 `dir`은 빈 문자열 `""`이 됩니다.
  - 이후 `f_opendir(&dir_obj, dir)`을 수행할 때 빈 문자열 `""`을 전달하게 되는데, FatFs는 빈 문자열을 '현재 작업 디렉터리(Current Working Directory)'로 취급합니다.
  - 만약 시스템의 CWD가 루트 디렉터리가 아닌 다른 하위 디렉터리에 머무르고 있다면, 사용자가 선택한 파일과 다른 폴더를 조회하게 되어 이전/다음 파일 탐색 기능이 실패하거나 비동작하게 됩니다.
* **대응 방안**:
  - 최종 디렉터리가 빈 문자열인 경우에는 이를 루트 디렉터리 `"/"`로 안전하게 치환하는 코드가 들어가야 합니다.
  ```c
  if (dir[0] == '\0') {
      strcpy(dir, "/");
  }
  ```

---

### AR-06: [Low] 백업 도메인 레지스터 쓰기 방지 상태 방치
* **위치**: [`Core/Src/retro-go/rg_rtc.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Src/retro-go/rg_rtc.c#L291-L318)
* **코드 조각**:
  ```c
  void GW_RTC_RestoreIfLost(void) {
      if (HAL_RTCEx_BKUPRead(&hrtc, CLOCK_BKP_REG) == CLOCK_BKP_MAGIC) {
          return; /* domain intact: the clock is whatever the user set */
      }

      HAL_PWR_EnableBkUpAccess();
      ...
      HAL_RTCEx_BKUPWrite(&hrtc, CLOCK_BKP_REG, CLOCK_BKP_MAGIC);
  }
  ```
* **문제 원인**:
  - 배터리 완전 방전 등으로 RTC 백업 정보가 손실되었을 때, 8바이트 SD 스냅샷 파일로부터 시간을 복원하기 위해 백업 도메인에 접근 권한을 엽니다(`HAL_PWR_EnableBkUpAccess()`).
  - 작업을 마친 뒤 보호 상태를 재설정하는 `HAL_PWR_DisableBkUpAccess()`가 전혀 호출되지 않아 상시 백업 영역이 열려있는 상태로 남게 됩니다.
  - 이는 저전력/오동작 등 예기치 못한 상태에서 백업 레지스터 및 RTC 설정이 뜻하지 않게 변조/오염될 위험을 높입니다.
* **대응 방안**:
  - 백업 도메인 기록 및 설정을 마치는 지점에서 다시 잠금 처리를 수행해 주는 것이 권장됩니다.
  ```c
  HAL_RTCEx_BKUPWrite(&hrtc, CLOCK_BKP_REG, CLOCK_BKP_MAGIC);
  HAL_PWR_DisableBkUpAccess(); // 쓰기 제한 재잠금
  ```

---

### 결언 (Conclusion)
임베디드 타겟의 C 코드 특성상, 파일 경로나 사용자 데이터를 다루는 과정에서 고정 크기 버퍼와 경계 검사 없는 `strncpy` 사용이 크고 작은 안정성 문제 및 메모리 누수를 유발하고 있습니다. 특히 **AR-01**과 **AR-02**, **AR-03**은 실기에서 특정한 긴 파일 경로 접근 시 하드폴트를 유발할 수 있으므로, 제안된 패치 처리를 최우선적으로 반영할 것을 권장합니다.
