/* Why every video frame came back "hal=2 err=0" — and why the fix is the fix.
 *
 * The JPEG peripheral cannot be driven on a host, so this does not decode
 * anything. It does not have to: the bug was never in the image. It was in the
 * HAL handle's lock, and that part of HAL is pure state, transcribed here from
 * the driver we ship, with line numbers so it can be checked:
 *
 *   Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_jpeg.c
 *     :520, :541   the ONLY two places the driver ever writes Lock = HAL_UNLOCKED,
 *                  and both sit inside `if (hjpeg->State == HAL_JPEG_STATE_RESET)`
 *     :1641        HAL_JPEG_Decode starts with __HAL_LOCK(hjpeg)
 *     :~1712       ...which returns HAL_BUSY at once if the handle is locked
 *     Init tail    State = READY, ErrorCode = HAL_JPEG_ERROR_NONE, Context = 0
 *
 * Put those together and a handle that is ever left LOCKED is locked for the rest
 * of the session: Init will not clear it (State is READY, not RESET), so every
 * later decode returns HAL_BUSY without looking at the image, and ErrorCode reads
 * 0 because Init just cleared it. hal=2, err=0, rej=0 — the device's exact words.
 *
 *   cc -o lock_test lock_test.c && ./lock_test
 */
#include <stdio.h>
#include <string.h>

typedef enum { HAL_UNLOCKED = 0, HAL_LOCKED = 1 } HAL_LockTypeDef;
typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;
typedef enum {
  HAL_JPEG_STATE_RESET = 0, HAL_JPEG_STATE_READY, HAL_JPEG_STATE_BUSY,
  HAL_JPEG_STATE_BUSY_DECODING, HAL_JPEG_STATE_ERROR,
} HAL_JPEG_STATETypeDef;

typedef struct { HAL_LockTypeDef Lock; HAL_JPEG_STATETypeDef State; unsigned ErrorCode; } JPEG_HandleTypeDef;

#define __HAL_LOCK(h)   do { if ((h)->Lock == HAL_LOCKED) return HAL_BUSY; (h)->Lock = HAL_LOCKED; } while (0)
#define __HAL_UNLOCK(h) do { (h)->Lock = HAL_UNLOCKED; } while (0)

/* stm32h7xx_hal_jpeg.c:538-541 and the tail of HAL_JPEG_Init */
static HAL_StatusTypeDef HAL_JPEG_Init(JPEG_HandleTypeDef *h) {
  if (h->State == HAL_JPEG_STATE_RESET)
    h->Lock = HAL_UNLOCKED;            /* :541 — the only unlock, and it is gated */
  h->State = HAL_JPEG_STATE_READY;
  h->ErrorCode = 0;
  return HAL_OK;
}

/* HAL_JPEG_Decode, reduced to the two things that decide hal= */
static HAL_StatusTypeDef HAL_JPEG_Decode(JPEG_HandleTypeDef *h, int image_is_good) {
  __HAL_LOCK(h);                        /* :1641 — locked handle -> HAL_BUSY, at once */
  if (h->State != HAL_JPEG_STATE_READY) { __HAL_UNLOCK(h); return HAL_BUSY; }
  h->State = HAL_JPEG_STATE_BUSY_DECODING;
  if (!image_is_good) { h->ErrorCode = 0x20; __HAL_UNLOCK(h); h->State = HAL_JPEG_STATE_READY; return HAL_TIMEOUT; }
  __HAL_UNLOCK(h);
  h->State = HAL_JPEG_STATE_READY;
  return HAL_OK;
}

/* ------------------------------------------------------------------ ours --- */
static JPEG_HandleTypeDef jpeg;

static void JPEG_HandleReset(void) {         /* the fix, in hw_jpeg_decoder.c */
  jpeg.State = HAL_JPEG_STATE_RESET;
  jpeg.Lock = HAL_UNLOCKED;
  jpeg.ErrorCode = 0;
}

static unsigned JPEG_Run(int good, int with_fix) {
  HAL_StatusTypeDef st = HAL_JPEG_Decode(&jpeg, good);
  unsigned hal = (unsigned)st, err = jpeg.ErrorCode;
  if (st != HAL_OK) {
    if (with_fix) {                          /* the recovery, in JPEG_Run */
      jpeg.Lock = HAL_UNLOCKED;
      jpeg.ErrorCode = 0;
      jpeg.State = HAL_JPEG_STATE_READY;
    }
    printf("      frame rejected: hal=%u err=%u\n", hal, err);
    return 1;
  }
  return 0;
}

static int play(const char *what, int with_fix, int wedged_before_init) {
  printf("  %s\n", what);
  memset(&jpeg, 0, sizeof jpeg);
  jpeg.State = HAL_JPEG_STATE_READY;
  if (wedged_before_init) jpeg.Lock = HAL_LOCKED;   /* somebody left it locked */

  if (with_fix) JPEG_HandleReset();
  HAL_JPEG_Init(&jpeg);                             /* video_decode_init() */

  int shown = 0;
  for (int f = 0; f < 5; f++)
    if (JPEG_Run(1, with_fix) == 0) shown++;        /* five perfectly good frames */
  printf("      -> %d/5 frames decoded\n\n", shown);
  return shown;
}

int main(void) {
  printf("\nA handle left LOCKED, then Init, then five good frames:\n\n");
  int broken = play("without the fix (what shipped)", 0, 1);
  int fixed  = play("with the fix", 1, 1);

  printf("A frame that really is bad, then four good ones:\n\n");
  memset(&jpeg, 0, sizeof jpeg); jpeg.State = HAL_JPEG_STATE_READY;
  JPEG_HandleReset(); HAL_JPEG_Init(&jpeg);
  JPEG_Run(0, 1);                                   /* the bad one */
  int after = 0;
  for (int f = 0; f < 4; f++) if (JPEG_Run(1, 1) == 0) after++;
  printf("      -> %d/4 frames after the bad one\n\n", after);

  int ok = (broken == 0) && (fixed == 5) && (after == 4);
  printf(ok ? "PASS: reproduces hal=2 err=0 on every frame, and the fix clears it\n\n"
            : "FAIL\n\n");
  return !ok;
}
