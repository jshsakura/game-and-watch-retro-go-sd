/* Host-test stub for Core/Inc/gw_crash_crumbs.h. test_common.c includes the
 * whole of common.c, and its first statement is now the breadcrumb heartbeat
 * -- device-only bookkeeping in backup-domain registers. The stub keeps the
 * pacing-math test about pacing: no-op, no state, nothing to fake. */
#ifndef GW_CRASH_CRUMBS_H_STUB
#define GW_CRASH_CRUMBS_H_STUB
static inline void gw_crumb_heartbeat(void) {}
#endif
