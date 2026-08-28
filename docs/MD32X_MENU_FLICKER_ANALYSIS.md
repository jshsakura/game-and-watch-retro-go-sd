# MD32X 메뉴 플리커링(번쩍거림) 버그 분석 리포트

## 1. 버그 원인 (Root Cause)

이 버그는 32X 코어가 3번째 여유 버퍼(Frame Buffer)를 가질 RAM 공간이 없어서 발생하는 구조적 한계와, `md32x_repaint()`의 `memcpy` 로직이 결합되어 발생합니다.

`odroid_overlay_dialog` (일시정지 메뉴 루프)가 실행될 때 매 프레임마다 다음 순서로 동작합니다:
1. `lcd_clear_active_buffer()` 호출 -> **현재 active 버퍼를 검은색(0)으로 완전히 지움**
2. `repaint()` (`md32x_repaint()`) 호출
3. 배경 다크닝 (`odroid_overlay_darken_all`)
4. UI 렌더링 (`odroid_overlay_draw_dialog`)
5. `lcd_swap()` 호출 -> **active와 inactive 버퍼를 교체**

**[커밋 92425edd의 실패 이유 - 타임라인 분석]**
현재 `md32x_repaint`는 첫 프레임에 `frozen` 포인터를 `inactive` 버퍼(순수 게임프레임이 남은 곳, 예: `FB2`)로 고정합니다.
- **메뉴 1프레임째 (`active`=FB1, `frozen`=FB2):** `FB1`이 지워진 후, `memcpy(FB1, FB2)`로 순수 게임 프레임을 복사해옵니다. 그 위에 UI가 그려집니다. (성공적)
- **메뉴 2프레임째 (`active`=FB2, `frozen`=FB2):** `lcd_swap()`으로 인해 **`frozen` 버퍼였던 FB2가 `active` 버퍼가 됩니다.** 이때 메뉴 루프의 `lcd_clear_active_buffer()`가 **FB2를 검은색으로 밀어버립니다!** 즉, 유일하게 남아있던 "순수 게임 프레임"이 이 시점에 영구적으로 소멸합니다. `memcpy(FB2, FB2)`는 빈 짓을 하고, FB2 위에 UI가 그려집니다.
- **메뉴 3프레임째 (`active`=FB1, `frozen`=FB2):** `FB1`이 검은색으로 지워집니다. 이후 `memcpy(FB1, FB2)`가 실행되는데, 이제 `FB2`에는 "검은 배경 + 이전 프레임의 UI"가 들어있습니다. 따라서 **이전 프레임의 UI가 통째로 FB1으로 복사됩니다.** 그 복사된 UI 위에 다크닝이 들어가고, 또 새 UI가 덧그려집니다.

**결과적으로 3프레임 이후부터는 게임 프레임은 온데간데없고, 이전 프레임의 UI가 다음 프레임으로 계속 복사(Ghosting/Smearing)되면서 화면(특히 여백 부분)이 번쩍번쩍거리고 UI가 중첩되는 증상이 무한 반복되는 것입니다.** (메인루프가 덮어쓰는 것이 아니라, `memcpy`가 이전 UI를 복사하는 것이 원인입니다.)

---

## 2. 해결 방안 (의사코드 및 file:line)

32X 코어는 여유 RAM(SRAM 등)이 남아있지 않으므로 3번째 백업 버퍼를 만들 수 없습니다. (C64나 NES 코어처럼 별도의 RAM 공간에서 화면을 다시 렌더링해오는 것이 불가능합니다). 

따라서 `frozen` 버퍼가 `lcd_clear`에 의해 파괴되는 2프레임째부터는 **`memcpy`를 즉시 중단하여 이전 프레임의 UI가 복사되는 참사를 막아야 합니다.** 이렇게 하면 1프레임째에는 게임 프레임이 보이고, 2프레임째부터는 깔끔한 검은색 배경 위에 UI가 안정적으로 출력되어 번쩍거림과 잔상(Smearing)이 완벽히 사라집니다.

### 수정 위치 및 코드
**파일:** `Core/Src/porting/md32x/main_md32x.c`
**함수:** `md32x_repaint()` (대략 356라인 부근)

```c
static void md32x_repaint(void) {
  uint16_t *active = lcd_get_active_buffer();
  static uint16_t *frozen;
  
  if (md32x_repaint_first) {
    /* displayed (inactive) buffer still holds the pure game frame */
    frozen = (active == (uint16_t *)framebuffer1)
                 ? (uint16_t *)framebuffer2 : (uint16_t *)framebuffer1;
    md32x_repaint_first = 0;
  }
  
  if (frozen) {
    if (frozen == active) {
      /* [BUG FIX] 
       * 메뉴 루프의 lcd_swap()으로 인해 frozen 버퍼가 active 버퍼가 되었습니다.
       * 이미 메뉴 루프의 lcd_clear_active_buffer()에 의해 이 버퍼는 0으로 지워졌으므로
       * 순수 게임 프레임은 소멸했습니다. 더 이상 복사하면 UI 잔상(Ghosting)만 생깁니다.
       */
      frozen = NULL;
    } else {
      memcpy(active, frozen, 320 * 240 * sizeof(uint16_t));
    }
  }
  
  common_ingame_overlay();
}
```

이 수정안을 통해 32X 코어를 편집 중인 세션에서 코드를 반영하면 기기 실기에서의 플리커링 버그가 해결될 것입니다.

---

## 2026-08-27 update: two flicker defects, one file

**The menu ghosting described above is FIXED in-tree.** `md32x_repaint` now
guards on `frozen != active` before clearing (see the comment at
`main_md32x.c:546` — "overlay _repaint no longer pre-clears for
NO_BG_DARKEN callers — the original 92425edd bug"). The first fix attempt
(92425edd) was wrong; the current guard is the real one. This document's
analysis of the mechanism stands; its "still broken" framing is stale.

**A different defect shares the family name**: the in-game bottom-third banding
users saw at 33 fps is a scanout race, not ghosting. Mechanism (measured):
the panel lags software by one reload (ReloadEventCallback writes the shadow
CFBAR of the just-finished buffer), so during frame k+1's write window the
panel scans the same buffer the renderer writes. At 60 Hz the beam (63.5
us/line) always outruns the writer (~98 us/line) — coherent. At 33 fps the
render start walks the phase, and when writes begin before the latch the beam
reads a half-overwritten frame: 1.10 races/frame measured, beam positions
spread flat across the visible scan. The "black band" content is Doom's own
dark pixels (status bar region) shown a frame stale — nothing writes black.

Options measured end-to-end: wait-for-latch guard = races 0 (verified) but
-26.2% fps; triple buffer = impossible, the pool is exactly two frames
(307,200 B); **slowing the panel to 35 Hz flips the beam slower than the
writer (119.4 us/line) for zero fps cost** (12-run sandwich identical) —
pending the user's eye verdict. Race-counter instrumentation is knob-gated
(`MD32X_SWAP_RACE_COUNT`), not in release builds.
