/*
 * Build-test for Phase 12's LTDC dual-layer split (docs/CPS1_MAME_
 * ALIGNMENT.md section 9): cps1_core_render_ltdc_bottom() must combine
 * SCROLL1+SCROLL2 into the buffer that becomes LTDC hardware layer 0,
 * and must NOT be affected by SCROLL3 or sprites (those stay in
 * cps1_core_run_frame_device_cost()'s own buffer, LTDC layer 1). Also
 * covers cps1_core_crop_to_panel(), the 384->320 panel-width fix this
 * phase found was never addressed by any prior phase.
 *
 *   ./build/cps1-ltdc-bottom-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int failures = 0;

    int ok1 = cps1_core_selftest_ltdc_bottom();
    const uint16_t *bottom = cps1_core_get_ltdc_bottom_buffer();
    printf("[cps1-ltdc-bottom-selftest] ltdc_bottom: %s (px(0,0)=0x%04x px(8,0)=0x%04x)\n",
           ok1 ? "OK" : "FAIL", bottom[0], bottom[8]);
    if (!ok1) failures++;

    int ok2 = cps1_core_selftest_crop_to_panel();
    printf("[cps1-ltdc-bottom-selftest] crop_to_panel: %s\n", ok2 ? "OK" : "FAIL");
    if (!ok2) failures++;

    if (failures) {
        fprintf(stderr, "[cps1-ltdc-bottom-selftest] FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-ltdc-bottom-selftest] OK\n");
    return 0;
}
