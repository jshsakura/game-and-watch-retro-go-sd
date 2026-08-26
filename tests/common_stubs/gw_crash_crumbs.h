/* Host-test stub for Core/Inc/gw_crash_crumbs.h. test_common.c includes the
 * whole of common.c, and its first statement is now the breadcrumb heartbeat
 * -- device-only bookkeeping in backup-domain registers. The stub keeps the
 * pacing-math test about pacing: no-op, no state, nothing to fake. */
#ifndef GW_CRASH_CRUMBS_H_STUB
#define GW_CRASH_CRUMBS_H_STUB
#define CRUMB_MODAL_NONE      0u
#define CRUMB_MODAL_ALARM     1u
#define CRUMB_MODAL_RESUME    2u
#define CRUMB_MODAL_MENU      3u
#define CRUMB_MODAL_SLEEPMENU 4u
#define CRUMB_MODAL_SLEEP     5u
static inline void gw_crumb_heartbeat(void) {}
static inline void gw_crumb_modal(uint32_t code, uint32_t input_bits) { (void)code; (void)input_bits; }
static inline void gw_crumb_modal_exit(void) {}
#endif
