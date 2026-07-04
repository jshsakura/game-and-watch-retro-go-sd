# Preserved instrumentation patches

Diagnostic hooks that were used during closed investigations, kept re-appliable
instead of leaving the working tree permanently dirty.

- `pce-go-host-diag.patch` — host-experiment hooks in the `retro-go-stm32`
  submodule (pce-go h6280/VDC trace prints + timer-kill switch) from the
  Dynastic Hero boot investigation (closed 2026-07-04). The `g_pcecd_trace` /
  `g_pce_kill_timer` globals they reference live in `linux/pce/main.c`.
  Re-apply with: `git -C retro-go-stm32 apply ../linux/patches/pce-go-host-diag.patch`
