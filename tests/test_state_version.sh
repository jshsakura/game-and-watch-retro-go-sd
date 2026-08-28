#!/bin/bash
# Version-match gate: the firmware's savestate format and the rig's loader must
# agree. Learned 2026-08-28: the rig read v2 while the firmware wrote v3, and the
# mismatch surfaced as a silent f0 death -- not a loud version complaint. Both
# sides define MD32X_STATE_VERSION; if they drift, every anchored rig measurement
# is measuring a different workload than the device ([[rule-two-copies-of-a-file-
# need-two-rigs]]). This gate refuses the drift loudly.
FW=$(sed -n 's/^#define MD32X_STATE_VERSION[[:space:]]*\([0-9]*\).*/\1/p' Core/Src/porting/md32x/main_md32x.c | head -1)
RIG=$(sed -n 's/^#define MD32X_STATE_VERSION[[:space:]]*\([0-9]*\).*/\1/p' tools/m7_qemu_rig/rig_32x.c | head -1)
HDR_FW=$(sed -n 's/^#define MD32X_STATE_HDR[[:space:]]*\([0-9]*\)u*\.*/\1/p' Core/Src/porting/md32x/main_md32x.c | head -1)
HDR_RIG=$(sed -n 's/^#define MD32X_STATE_HDR[[:space:]]*\([0-9]*\)u*\.*/\1/p' tools/m7_qemu_rig/rig_32x.c | head -1)
fail=0
[ -n "$FW" ] && [ -n "$RIG" ] || { echo "GATE FAIL: version constant not found (FW='$FW' RIG='$RIG')"; fail=1; }
[ "$FW" = "$RIG" ] || { echo "GATE FAIL: firmware writes v$FW, rig reads v$RIG -- anchored measurements measure the wrong workload"; fail=1; }
if [ -n "$HDR_FW" ] && [ -n "$HDR_RIG" ] && [ "$HDR_FW" != "$HDR_RIG" ]; then
  echo "GATE FAIL: header size FW=$HDR_FW RIG=$HDR_RIG"; fail=1
fi
[ $fail -eq 0 ] && echo "state version gate: firmware v$FW == rig v$RIG (hdr ${HDR_FW:-n/a}) OK"
exit $fail
