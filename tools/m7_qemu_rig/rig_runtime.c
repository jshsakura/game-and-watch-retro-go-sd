/* Minimal Cortex-M7 bare-metal runtime for QEMU's mps2-an500 machine.
 *
 * Purpose: run an emulator core on a REAL ARMv7-M instruction stream and
 * count executed instructions per frame (QEMU -icount shift=0 makes virtual
 * time advance exactly 1 ns per guest instruction; the board's CMSDK timer
 * runs on virtual time, so a timer delta IS an instruction count, scaled by
 * a factor the rig calibrates on itself at boot).
 *
 * What QEMU does NOT model: caches and memory wait states. So this rig
 * answers "how many instructions does a frame take" (A/B of algorithmic
 * changes, budget math against the CPU clock) — never "what fps will the
 * device show". The device's frame ledger remains the final judge.
 *
 * No rdimon, no crt0: own vector table, own .data/.bss init, semihosting
 * by hand (BKPT 0xAB), newlib syscall shims so printf works.
 */
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++) *dst = 0;
    main();
    /* semihosting SYS_EXIT */
    register uint32_t r0 __asm__("r0") = 0x18;
    register uint32_t r1 __asm__("r1") = 0x20026; /* ADP_Stopped_ApplicationExit */
    __asm__ volatile("bkpt 0xAB" :: "r"(r0), "r"(r1));
    for (;;) ;
}

static void Default_Handler(void) { for (;;) ; }

__attribute__((section(".isr_vector"), used))
static void (*const vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0,
    Default_Handler, Default_Handler,
};

/* ---- semihosting ---- */
static uint32_t sh_call(uint32_t op, void *arg)
{
    register uint32_t r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

/* ---- newlib syscall shims (semihosted stdout, arena sbrk, rest ENOSYS) ---- */
int _write(int fd, const char *buf, int len)
{
    (void)fd;
    /* SYS_WRITE0 wants a C string: bounce through a small buffer. */
    static char tmp[257];
    int done = 0;
    while (done < len) {
        int n = len - done > 256 ? 256 : len - done;
        for (int i = 0; i < n; i++) tmp[i] = buf[done + i];
        tmp[n] = 0;
        sh_call(0x04, tmp); /* SYS_WRITE0 */
        done += n;
    }
    return len;
}

extern uint32_t _sheap, _eheap;
void *_sbrk(ptrdiff_t incr)
{
    static char *brk;
    if (!brk) brk = (char *)&_sheap;
    if (brk + incr > (char *)&_eheap) return (void *)-1;
    char *prev = brk;
    brk += incr;
    return prev;
}

int _open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int _gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int wh) { (void)fd; (void)off; (void)wh; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
void _exit(int code) { (void)code; sh_call(0x18, (void *)0x20026); for (;;) ; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }

/* ---- instruction counter: CMSDK timer 0, virtual-clocked ---- */
#define CMSDK_TIMER0 ((volatile uint32_t *)0x40000000) /* CTRL, VALUE, RELOAD */

void rig_timer_init(void)
{
    CMSDK_TIMER0[2] = 0xFFFFFFFFu; /* RELOAD */
    CMSDK_TIMER0[1] = 0xFFFFFFFFu; /* VALUE (counts down) */
    CMSDK_TIMER0[0] = 1;           /* enable, no IRQ */
}

uint32_t rig_timer_now(void) { return ~CMSDK_TIMER0[1]; } /* up-counting ticks */

/* Calibration: a loop whose executed-instruction count we know statically.
 * 3 instructions per iteration (subs/nop/bne) x n + small epsilon. */
uint32_t rig_calibrate(uint32_t n)
{
    uint32_t t0 = rig_timer_now();
    __asm__ volatile(
        "1: subs %0, %0, #1\n"
        "   nop\n"
        "   bne 1b\n" : "+r"(n) :: "cc");
    return rig_timer_now() - t0;
}
