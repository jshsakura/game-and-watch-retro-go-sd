/* Exactly what Core/Src/porting/sm/main_sm.c provides to the sm core, minus the
 * firmware (LCD, SD, flash). If main_sm.c stops defining one of these, this file
 * will not stop defining it — so device_parity.sh keeps linking and says nothing.
 *
 * Which is fine: the point of the parity link is the OTHER direction. It fails
 * when the CORE needs a symbol that NOBODY on the device defines — and that is
 * the failure mode that shipped, because the firmware linker resolves such a name
 * against another overlay core instead of rejecting it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "src/types.h"
#include "src/sm_rtl.h"
#include "src/snes/snes.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/spc_player.h"

/* sm's SDL main.c owns these upstream */
bool g_debug_flag;
bool g_new_ppu = true;
bool g_other_image;
int  g_got_mismatch_count;
SpcPlayer *g_spc_player;

/* sm_cpu_infra.c owned these — the file the device does not compile */
Snes *g_snes;
bool g_use_my_apu_code = true;
bool g_fail;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void Die(const char *e) { fprintf(stderr, "DIE: %s\n", e); exit(1); }
void Warning(const char *e) { fprintf(stderr, "WARN: %s\n", e); }
void RtlDrawPpuFrame(uint8 *pb, size_t pitch, uint32 f) { (void)pb; (void)pitch; (void)f; }

void Call(uint32 addr) { (void)addr; }
void DebugGameOverMenu(void) {}
void RtlUpdateSnesPatchForBugfix(void) {}
uint16 currently_installed_bug_fix_counter;

void apu_reset(Apu *a) { (void)a; }
void apu_cycle(Apu *a) { (void)a; }
/* snes.c batches through apu_run since the snes-perf bump; main_sm.c stubs it
 * on device, and main_sm.c is excluded here, so the stub must exist on both
 * sides or this parity link reports a false "would alias another core". */
void apu_run(Apu *a, int cyclesToRun) { (void)a; (void)cyclesToRun; }
void apu_free(Apu *a) { (void)a; }
void apu_saveload(Apu *a, SaveLoadFunc *f, void *c) { (void)a; (void)f; (void)c; }
void ppu_copy(Ppu *a, Ppu *b) { (void)a; (void)b; }
int  CpuOpcodeHook(uint32 addr) { (void)addr; return 0; }
bool HookedFunctionRts(int l) { (void)l; return false; }

/* firmware allocators the core calls under TARGET_GNW — not used in this build,
 * but declared so the link is a fair test if TARGET_GNW is ever turned on here */
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *ahb_malloc(size_t s) { return malloc(s); }

int main(void) { printf("sm device source set links\n"); return 0; }
