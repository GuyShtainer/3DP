/* hw_os_test.c -- host tests for bet 1.3: the pure modules of source/hw (virtual time, timers 0-3, the event pump, the host-thread
 * hand-off, the once-per-site log, config.ini, MD5/SHA-1/HMAC). Built and run by tools/host-tests/build.sh (`make host-tests`) with the
 * build machine's cc and pthreads; no SDK headers, no libctru. Expected values come from the documented hardware numbers (GBATEK timer
 * rules, 268 111 856 Hz / 33 513 982 Hz) and the RFC / FIPS test vectors, never from the code under test.
 * Prints "N checks, 0 failures"; exits non-zero on any failure. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/hw/hw_cfg.h"
#include "../../source/hw/hw_ctr.h"
#include "../../source/hw/hw_dgt.h"
#include "../../source/hw/hw_log.h"
#include "../../source/hw/hw_pure.h"
#include "../../source/hw/hw_wmq.h"

static long g_checks;
static long g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        if (g_fails <= 40) {
            fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
        }
    }
}

/* ---- fake hw_ctr (what hw_log needs) ---------------------------------------------------------------------------------------- */
static char g_file_path[256];
static char g_file_buf[65536];
static int g_file_writes;
static int g_mkdirs;
static char g_console[4096];

int hwctr_file_write(const char *path, const void *buf, uint32_t len)
{
    assert(len < sizeof g_file_buf);
    if (strstr(path, "hwcount.txt") != NULL) {
        return (int)len;   /* the count table is a second file next to hwlog.txt; these tests watch the hwlog.txt dumps */
    }
    strncpy(g_file_path, path, sizeof g_file_path - 1);
    memcpy(g_file_buf, buf, len);
    g_file_buf[len] = 0;
    g_file_writes++;
    return (int)len;
}

void hwctr_mkdirs(const char *dir)
{
    (void)dir;
    g_mkdirs++;
}

void hwctr_console_write(const char *text)
{
    strncat(g_console, text, sizeof g_console - strlen(g_console) - 1);
}

/* ---- register helpers --------------------------------------------------------------------------------------------------------- */
static void w16(uint8_t *io, uint32_t off, uint16_t v)
{
    memcpy(io + off, &v, 2);
}

static uint16_t r16(const uint8_t *io, uint32_t off)
{
    uint16_t v;
    memcpy(&v, io + off, 2);
    return v;
}

static void w32(uint8_t *io, uint32_t off, uint32_t v)
{
    memcpy(io + off, &v, 4);
}

static uint32_t r32(const uint8_t *io, uint32_t off)
{
    uint32_t v;
    memcpy(&v, io + off, 4);
    return v;
}

/* ---- time ------------------------------------------------------------------------------------------------------------------- */
static void test_time(void)
{
    HwTime t;

    check(HWT_SYS_HZ == 268111856u, "sys clock 268 111 856 Hz", 0);
    check(HWT_SYS_HZ / HWT_SYS_PER_BUS == 33513982u && HWT_SYS_HZ % HWT_SYS_PER_BUS == 0u, "DS bus 33 513 982 Hz = sys/8 exactly", 0);
    check(HWT_SYS_PER_TICK == 8u * 64u && HWT_SYS_HZ / HWT_SYS_PER_TICK == 523655u, "OS tick = bus/64 = sys/512 (an integer ratio; 523 655.97 Hz)", 0);
    check(HWT_SYS_HZ / HWT_SYS_PER_CYCLE == 67027964u, "ARM9 cycle 67 027 964 Hz = sys/4", 0);
    check(HWT_FRAME_SYS > 4481000u && HWT_FRAME_SYS < 4482000u, "frame = sys / 59.8261 Hz", HWT_FRAME_SYS);
    hwtime_init(&t, 1000);
    check(hwtime_update(&t, 1000) == 0 && hwtime_os_tick(&t) == 0, "init: virtual time 0", 0);
    check(hwtime_update(&t, 1000 + HWT_SYS_HZ) == HWT_SYS_HZ, "one real second = 268 111 856 sys ticks", 0);
    check(hwtime_os_tick(&t) == 523655u, "one second = 523 655 whole OS ticks (floor of 523 655.97)", (long)hwtime_os_tick(&t));
    check(hwtime_update(&t, 500) == HWT_SYS_HZ, "time never runs backwards", 0);
    hwtime_spin_cycles(&t, 67027964u);
    check(t.virt == 2ull * HWT_SYS_HZ, "spin of one ARM9 second adds one second", 0);
    check(hwtime_update(&t, 1000 + HWT_SYS_HZ + 10) == 2ull * HWT_SYS_HZ, "virtual time stays ahead of real after a spin", 0);
    hwtime_set_os_tick(&t, 1000);
    check(hwtime_os_tick(&t) == 1000, "OS_SetTick", (long)hwtime_os_tick(&t));
    hwtime_spin_cycles(&t, 128u * 100u);   /* 100 OS ticks = 51 200 sys ticks = 12 800 ARM9 cycles */
    check(hwtime_os_tick(&t) == 1000u + 100u, "tick advances from the set value", (long)hwtime_os_tick(&t));
    hwtime_set_os_tick(&t, 0);
    check(hwtime_os_tick(&t) == 0, "OS_InitTick back to 0", 0);
    hwtime_init(&t, 1000);
    (void)hwtime_update(&t, 1000 + 5000);
    hwtime_steal(&t, 3000);   /* bet 2.1c2: 3000 real ticks do not exist for the game */
    check(hwtime_update(&t, 1000 + 5000 + 3000) == 5000, "steal: virtual time stands still for the stolen ticks", (long)t.virt);
    check(hwtime_update(&t, 1000 + 5000 + 3000 + 700) == 5700, "steal: and then runs on", (long)t.virt);
}

/* ---- timers ----------------------------------------------------------------------------------------------------------------- */
#define CTL_START 0x80u
#define CTL_IRQ   0x40u
#define CTL_CASC  0x04u

static void tm_setup(HwTimers *t, uint8_t *io, uint32_t n, uint16_t reload, uint16_t ctl)
{
    w16(io, HWR_TM0CNT_L + 4u * n, reload);
    w16(io, HWR_TM0CNT_L + 4u * n + 2u, ctl);
    (void)t;
}

static void test_timer_basic(void)
{
    static uint8_t io[0x1000];
    HwTimers t;

    memset(io, 0, sizeof io);
    hwtimer_init(&t, 0);
    tm_setup(&t, io, 0, 0xfff0, CTL_START | CTL_IRQ);
    check(hwtimer_advance(&t, io, 0) == 0 && r16(io, HWR_TM0CNT_L) == 0xfff0, "timer 0 latches its reload at the enable edge", 0);
    check(hwtimer_advance(&t, io, 15) == 0 && r16(io, HWR_TM0CNT_L) == 0xffff, "15 bus ticks: counter 0xffff, no overflow", r16(io, HWR_TM0CNT_L));
    check(hwtimer_advance(&t, io, 16) == (1u << 3), "16 bus ticks: overflow raises IF bit 3", 0);
    check(r16(io, HWR_TM0CNT_L) == 0xfff0, "counter reloads on overflow", r16(io, HWR_TM0CNT_L));
    check(hwtimer_advance(&t, io, 16 + 40) == (1u << 3), "two more overflows in one advance report once", 0);
    check(r16(io, HWR_TM0CNT_L) == (uint16_t)(0xfff0 + 40u % 16u), "multi-overflow leaves the right phase", r16(io, HWR_TM0CNT_L));
    w16(io, HWR_TM0CNT_L + 2, 0);
    check(hwtimer_advance(&t, io, 1000) == 0 && t.running[0] == 0, "stopped timer does not run", 0);
}

static uint32_t first_overflow(uint32_t n, uint16_t ctl, uint32_t limit)
{
    static uint8_t io[0x1000];
    HwTimers t;
    uint32_t bus;

    memset(io, 0, sizeof io);
    hwtimer_init(&t, 0);
    tm_setup(&t, io, n, 0xffff, (uint16_t)(CTL_START | CTL_IRQ | ctl));
    (void)hwtimer_advance(&t, io, 0);
    for (bus = 1; bus <= limit; bus++) {
        if (hwtimer_advance(&t, io, bus) == (1u << (HWIRQ_TIMER0 + n))) {
            return bus;
        }
    }
    return 0;
}

static void test_timer_modes(void)
{
    static uint8_t io[0x1000];
    HwTimers t;

    check(first_overflow(0, 0, 10) == 1, "prescaler 1: reload 0xffff overflows after 1 bus tick", 0);
    check(first_overflow(1, 1, 200) == 64, "prescaler 64: overflow at 64 bus ticks (timer 1 -> IF bit 4)", 0);
    check(first_overflow(2, 2, 600) == 256, "prescaler 256: overflow at 256 bus ticks (timer 2 -> IF bit 5)", 0);
    check(first_overflow(3, 3, 2000) == 1024, "prescaler 1024: overflow at 1024 bus ticks (timer 3 -> IF bit 6)", 0);
    memset(io, 0, sizeof io);
    hwtimer_init(&t, 0);
    tm_setup(&t, io, 0, 0xfff0, CTL_START);   /* no IRQ bit */
    (void)hwtimer_advance(&t, io, 0);
    check(hwtimer_advance(&t, io, 16) == 0 && r16(io, HWR_TM0CNT_L) == 0xfff0, "overflow without the IRQ bit raises nothing but wraps", 0);
}

static void test_timer_cascade_and_reload(void)
{
    static uint8_t io[0x1000];
    HwTimers t;

    memset(io, 0, sizeof io);
    hwtimer_init(&t, 0);
    tm_setup(&t, io, 0, 0xfffe, CTL_START);                          /* period 2 bus ticks, no IRQ */
    tm_setup(&t, io, 1, 0xffff, CTL_START | CTL_CASC | CTL_IRQ);   /* counts timer 0 overflows */
    (void)hwtimer_advance(&t, io, 0);
    check(hwtimer_advance(&t, io, 1) == 0, "cascade: no overflow before timer 0 wraps", 0);
    check(hwtimer_advance(&t, io, 2) == (1u << 4), "cascade: timer 0 wrap makes timer 1 overflow (IF bit 4 only)", 0);
    memset(io, 0, sizeof io);
    hwtimer_init(&t, 0);
    tm_setup(&t, io, 0, 0xfff0, CTL_START | CTL_IRQ);
    (void)hwtimer_advance(&t, io, 0);
    (void)hwtimer_advance(&t, io, 4);
    w16(io, HWR_TM0CNT_L, 0xf000);   /* software writes a new reload while running */
    check(hwtimer_advance(&t, io, 4) == 0 && r16(io, HWR_TM0CNT_L) == 0xfff4, "a reload write does not disturb the running counter (it reads back as 0xfff4)", r16(io, HWR_TM0CNT_L));
    check(hwtimer_advance(&t, io, 4 + 12) == (1u << 3), "overflow still due at the old phase", 0);
    check(r16(io, HWR_TM0CNT_L) == 0xf000, "new reload is used after the overflow", r16(io, HWR_TM0CNT_L));
}

static void test_timer_next_irq(void)
{
    static uint8_t io[0x1000];
    HwTimers t;

    memset(io, 0, sizeof io);
    hwtimer_init(&t, 100);
    check(hwtimer_next_irq(&t, io) == UINT64_MAX, "no timers: no wake-up", 0);
    tm_setup(&t, io, 0, 0xfff0, CTL_START | CTL_IRQ);
    (void)hwtimer_advance(&t, io, 100);
    check(hwtimer_next_irq(&t, io) == 116, "next overflow 16 bus ticks ahead", (long)hwtimer_next_irq(&t, io));
    tm_setup(&t, io, 1, 0xffff, CTL_START | CTL_IRQ | CTL_CASC);
    (void)hwtimer_advance(&t, io, 105);
    check(hwtimer_next_irq(&t, io) == 116, "cascade timers are not scheduled; timer 0 is", 0);
    w16(io, HWR_TM0CNT_L + 2, CTL_START);   /* timer 0 without IRQ */
    check(hwtimer_next_irq(&t, io) == UINT64_MAX, "no IRQ bit: no wake-up", 0);
}

/* ---- the pump ----------------------------------------------------------------------------------------------------------------- */
typedef struct PumpLog {
    uint64_t now;
    int waits;
    int sleeps;
    uint64_t last_sleep;
    int arm7;
    int masked;
    int disp[128];
    int ndisp;
    int order[128];
    int norder;
    int reraise;      /* dispatch handler re-raises IF bit (cap test) */
    uint8_t *io;
} PumpLog;

static uint64_t f_tick(void *u) { return ((PumpLog *)u)->now; }
static void f_wait(void *u)
{
    PumpLog *l = (PumpLog *)u;
    l->waits++;
    l->now = (l->now / HWT_FRAME_SYS + 1u) * HWT_FRAME_SYS;
}
static void f_sleep(void *u, uint64_t sys)
{
    PumpLog *l = (PumpLog *)u;
    l->sleeps++;
    l->last_sleep = sys;
    l->now += sys;
}
static void f_arm7(void *u)
{
    PumpLog *l = (PumpLog *)u;
    l->arm7++;
    l->order[l->norder++ & 127] = -1;
    w32(l->io, HWR_IF, r32(l->io, HWR_IF) | (1u << 16));   /* the ARM7 raises the IPC receive IRQ */
}
static void f_disp(void *u, int irq)
{
    PumpLog *l = (PumpLog *)u;
    l->disp[l->ndisp++ & 127] = irq;
    l->order[l->norder++ & 127] = irq;
    if (l->reraise) {
        w32(l->io, HWR_IF, r32(l->io, HWR_IF) | (1u << irq));
    }
}
static int f_masked(void *u) { return ((PumpLog *)u)->masked; }

static void pump_setup(HwPump *p, PumpLog *l, uint8_t *io, uint32_t *vb, int with_arm7)
{
    HwPumpOps ops = { f_tick, f_wait, f_sleep, with_arm7 ? f_arm7 : NULL, f_disp, f_masked, l, NULL, NULL };

    memset(io, 0, 0x1000);
    memset(l, 0, sizeof *l);
    l->io = io;
    *vb = 0;
    hwpump_init(p, &ops, io, vb);
}

static void test_pump_vblank(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;

    pump_setup(&p, &l, io, &vb, 0);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1);
    w16(io, HWR_DISPSTAT, 8);
    hwpump_run(&p, HWP_NOWAIT);
    check(l.waits == 0 && vb == 0 && l.ndisp == 0, "NOWAIT before the first vblank does nothing", 0);
    hwpump_run(&p, HWP_WAIT);
    check(l.waits == 1 && vb == 1 && p.vblanks == 1, "WAIT blocks to the vblank and counts it (HW_VBLANK_COUNT_BUF)", vb);
    check(l.ndisp == 1 && l.disp[0] == 0, "vblank IRQ (bit 0) dispatched once", l.ndisp);
    check((r32(io, HWR_IF) & 1u) == 0, "IF acknowledged before the handler runs", r32(io, HWR_IF));
    check(r16(io, HWR_DISPSTAT) & 1u, "DISPSTAT vblank flag set at line 192", r16(io, HWR_DISPSTAT));
    check(r16(io, HWR_VCOUNT) == 192, "VCOUNT 192 at the vblank", r16(io, HWR_VCOUNT));
    l.now += HWT_FRAME_SYS / 2;
    hwpump_run(&p, HWP_NOWAIT);
    check(r16(io, HWR_VCOUNT) == 60, "VCOUNT half a frame after the vblank = (192 + 131) mod 263 = 60", r16(io, HWR_VCOUNT));
    check(l.ndisp == 1, "no second vblank mid-frame", l.ndisp);
}

static void test_pump_gating(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;

    pump_setup(&p, &l, io, &vb, 0);
    w16(io, HWR_DISPSTAT, 8);
    w32(io, HWR_IE, 1);
    w32(io, HWR_IME, 0);
    hwpump_run(&p, HWP_WAIT);
    check(l.ndisp == 0 && (r32(io, HWR_IF) & 1u), "IME=0: IF raised, nothing dispatched", l.ndisp);
    w32(io, HWR_IME, 1);
    l.masked = 1;
    hwpump_run(&p, HWP_DISPATCH_ONLY);
    check(l.ndisp == 0, "CPSR I bit set: nothing dispatched", l.ndisp);
    l.masked = 0;
    hwpump_run(&p, HWP_DISPATCH_ONLY);
    check(l.ndisp == 1 && l.disp[0] == 0 && l.waits == 1, "interrupts re-enabled (Y5): the pending IRQ is taken, no time passes", l.ndisp);
    pump_setup(&p, &l, io, &vb, 0);
    w16(io, HWR_DISPSTAT, 8);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 0);
    hwpump_run(&p, HWP_WAIT);
    check(l.ndisp == 0, "IE=0: not dispatched", l.ndisp);
    pump_setup(&p, &l, io, &vb, 0);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1);   /* DISPSTAT vblank-IRQ enable clear */
    hwpump_run(&p, HWP_WAIT);
    check(vb == 1 && l.ndisp == 0 && (r32(io, HWR_IF) & 1u) == 0, "DISPSTAT bit 3 clear: counted, IF not raised", 0);
}

static void test_pump_order_and_cap(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;

    pump_setup(&p, &l, io, &vb, 1);
    w16(io, HWR_DISPSTAT, 8);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1u | (1u << 16));
    hwpump_run(&p, HWP_WAIT);
    check(l.arm7 == 1 && l.norder == 3, "ARM7 slice runs once per pump", l.norder);
    check(l.order[0] == -1 && l.order[1] == 0 && l.order[2] == 16, "order: ARM7 slice, then dispatch lowest bit first (0 then 16)", 0);
    pump_setup(&p, &l, io, &vb, 0);
    w16(io, HWR_DISPSTAT, 8);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1);
    l.reraise = 1;
    hwpump_run(&p, HWP_WAIT);
    check(l.ndisp == 64 && p.dispatched == 64, "a handler that re-raises its IRQ is cut at 64 dispatches per pump", l.ndisp);
}

static void test_pump_timer_and_overrun(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;

    pump_setup(&p, &l, io, &vb, 0);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1u << 3);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 100));
    w16(io, HWR_TM0CNT_L + 2, CTL_START | CTL_IRQ);
    hwpump_run(&p, HWP_NOWAIT);   /* latch */
    hwpump_run(&p, HWP_WAIT);
    check(l.sleeps == 1 && l.waits == 0, "WAIT sleeps to a timer that fires before the vblank", l.sleeps);
    check(l.last_sleep == 100u * HWT_SYS_PER_BUS, "sleep = 100 bus ticks = 800 sys ticks", (long)l.last_sleep);
    check(l.ndisp == 1 && l.disp[0] == 3, "timer 0 IRQ (bit 3) dispatched", l.ndisp);
    pump_setup(&p, &l, io, &vb, 0);
    w16(io, HWR_DISPSTAT, 8);
    l.now = 5u * HWT_FRAME_SYS + 123;
    hwpump_run(&p, HWP_NOWAIT);
    check(vb == 1 && p.overruns == 1, "5 frames late: vblank raised once, overrun counted", p.overruns);
    check(p.next_vblank == l.now + HWT_FRAME_SYS, "no catch-up: next vblank one frame after now", 0);
    hwpump_spin(&p, 100);
    check(p.time.virt >= l.now + 400u, "spin adds 4 sys ticks per cycle", 0);
}

/* ---- the host-thread hand-off ------------------------------------------------------------------------------------------------- */
typedef struct PtHandle {
    pthread_mutex_t m;
    pthread_cond_t c;
    int flag;
    pthread_t th;
    void (*entry)(void *);
    void *arg;
    int is_thread;
} PtHandle;

static PtHandle g_pt[8];
static int g_npt;

static void pt_init(PtHandle *h)
{
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    h->flag = 0;
}

static void pt_park(void *hh, void *u)
{
    PtHandle *h = (PtHandle *)hh;
    (void)u;
    pthread_mutex_lock(&h->m);
    while (!h->flag) {
        pthread_cond_wait(&h->c, &h->m);
    }
    h->flag = 0;
    pthread_mutex_unlock(&h->m);
}

static void pt_wake(void *hh, void *u)
{
    PtHandle *h = (PtHandle *)hh;
    (void)u;
    pthread_mutex_lock(&h->m);
    h->flag = 1;
    pthread_cond_signal(&h->c);
    pthread_mutex_unlock(&h->m);
}

static void *pt_body(void *p)
{
    PtHandle *h = (PtHandle *)p;
    pt_park(h, NULL);
    h->entry(h->arg);
    return NULL;
}

static void *pt_create(void (*entry)(void *), void *arg, uint32_t stack, void *u)
{
    PtHandle *h = &g_pt[g_npt++];
    (void)stack;
    (void)u;
    assert(g_npt < 8);
    pt_init(h);
    h->entry = entry;
    h->arg = arg;
    h->is_thread = 1;
    pthread_create(&h->th, NULL, pt_body, h);
    return h;
}

static void pt_exit(void *hh, void *u)
{
    (void)hh;
    (void)u;
    pthread_exit(NULL);
}

static void ct_noop(void *h, void *u) { (void)h; (void)u; }

static HwCtx g_ctx;
static char g_trace[64];
static int g_ntrace;
typedef struct Ctxs {
    uint32_t w11, w12;
} Ctxs;
static Ctxs g_main_ctx, g_b_ctx;

static void trace(char c)
{
    g_trace[g_ntrace++] = c;
    g_trace[g_ntrace] = 0;
}

static void b_after(void)
{
    trace('x');
    (void)hwctx_load(&g_ctx, &g_main_ctx, g_main_ctx.w11, g_main_ctx.w12, 0, 0, 0, 0, 1);   /* OS_ExitThread: retire, wake main */
}

static void b_entry(void *arg)
{
    trace('B');
    check(*(int *)arg == 42, "fresh thread starts with r0 = argument", *(int *)arg);
    (void)hwctx_save(&g_ctx, &g_b_ctx, &g_b_ctx.w11, &g_b_ctx.w12);
    (void)hwctx_load(&g_ctx, &g_main_ctx, g_main_ctx.w11, g_main_ctx.w12, 0, 0, 0, 0, 0);   /* park; main loads us back later */
    trace('b');
}

static void test_ctx_pingpong(void)
{
    HwCtxBackend be = { pt_create, pt_wake, pt_park, pt_exit, ct_noop, NULL };
    static int arg = 42;
    int rc, i;

    pt_init(&g_pt[g_npt]);
    g_npt++;
    hwctx_init(&g_ctx, &be, &g_pt[0]);
    check(g_ctx.cur == 0 && g_ctx.rec[0].used, "record 0 is the launcher thread", 0);
    rc = hwctx_save(&g_ctx, &g_main_ctx, &g_main_ctx.w11, &g_main_ctx.w12);
    check(rc == 0 && g_main_ctx.w12 == HWCTX_MAGIC && g_main_ctx.w11 == 0, "OS_SaveContext marks the image and returns 0", rc);
    check(hwctx_cur_owner(&g_ctx) == &g_main_ctx, "the running record owns its saved context", 0);
    rc = hwctx_load(&g_ctx, &g_b_ctx, 0, 0, (uintptr_t)b_entry, (uintptr_t)&arg, (uintptr_t)b_after, 0x10000, 0);
    trace('M');
    check(rc == 0 && g_ctx.cur == 0, "main resumes after B handed back", g_ctx.cur);
    check(strcmp(g_trace, "BM") == 0, "hand-off order: B ran first, then main", 0);
    check(g_ctx.rec[1].used && g_ctx.rec[1].owner == &g_b_ctx, "B has its own record", 0);
    rc = hwctx_load(&g_ctx, &g_b_ctx, g_b_ctx.w11, g_b_ctx.w12, 0, 0, 0, 0, 0);   /* resume B inside its load call */
    trace('m');
    check(strcmp(g_trace, "BMbxm") == 0, "B resumes, runs to its end (lr = ExitThread) and main resumes last", 0);
    check(!g_ctx.rec[1].used && g_ctx.cur == 0, "a terminated thread's record is retired", 0);
    check(hwctx_load(&g_ctx, &g_main_ctx, g_main_ctx.w11, g_main_ctx.w12, 0, 0, 0, 0, 0) == 0, "loading the running context is a no-op", 0);
    for (i = 0; i < g_npt; i++) {
        if (g_pt[i].is_thread) {
            pthread_join(g_pt[i].th, NULL);
        }
    }
}

static void *ct_create(void (*entry)(void *), void *arg, uint32_t stack, void *u)
{
    static int n;
    (void)entry;
    (void)arg;
    (void)stack;
    return *(int *)u ? NULL : (void *)(uintptr_t)(++n);
}

static void test_ctx_limits(void)
{
    int fail_create = 0;
    HwCtxBackend be = { ct_create, ct_noop, ct_noop, ct_noop, ct_noop, &fail_create };
    static HwCtx c;
    static uint32_t owners[HWCTX_MAX + 2];
    int i, rc = 0, filled = 0;

    hwctx_init(&c, &be, (void *)1);
    (void)hwctx_save(&c, &owners[0], &owners[1], &owners[2]);
    for (i = 1; i < HWCTX_MAX + 1 && rc == 0; i++) {
        c.cur = 0;
        rc = hwctx_load(&c, &owners[i], 0, 0, 0x1000, 0, 0, 0x10000, 0);
        filled += rc == 0;
    }
    check(filled == HWCTX_MAX - 1 && rc == -1, "63 fresh threads fit, the 64th fails (no allocation after init)", filled);
    hwctx_init(&c, &be, (void *)1);
    fail_create = 1;
    c.cur = 0;
    check(hwctx_load(&c, &owners[1], 0, 0, 0x1000, 0, 0, 0x10000, 0) == -1 && !c.rec[1].used, "a failed thread create frees the record", 0);
}

/* ---- the log ------------------------------------------------------------------------------------------------------------------ */
__attribute__((noinline)) static void stub_a(void)
{
    HWLOG_CALL("DWC_Fake");
}

static void test_log_sites(void)
{
    int i;
    static HwLogSite many[300];
    char buf[200];

    hwlog_reset();
    for (i = 0; i < 5; i++) {
        HWLOG("one site, five calls %d", i);
    }
    check(hwlog_sites() == 1 && hwlog_calls() == 5, "one site called 5 times: 1 line, 5 calls", hwlog_calls());
    HWLOG("a second site");
    check(hwlog_sites() == 2 && hwlog_calls() == 6, "a second site is a second line", hwlog_sites());
    hwlog_format(g_file_buf, sizeof g_file_buf);
    check(strstr(g_file_buf, "[x5]") && strstr(g_file_buf, "one site, five calls 0") && !strstr(g_file_buf, "calls 1"),
          "format: count 5, only the FIRST message kept", 0);
    hwlog_reset();
    for (i = 0; i < 2; i++) {
        stub_a();   /* one caller, two calls */
    }
    check(hwlog_sites() == 1 && hwlog_calls() == 2, "stub called twice from one caller (same return address): 1 line", hwlog_sites());
    stub_a();       /* a different caller */
    check(hwlog_sites() == 2 && hwlog_calls() == 3, "the same stub from a second caller: a second line", hwlog_sites());
    hwlog_call("f.c", 1, "DWC_Fake", (void *)0x1000);
    hwlog_call("f.c", 1, "DWC_Fake", (void *)0x1000);
    hwlog_call("f.c", 1, "DWC_Fake", (void *)0x2000);
    hwlog_call("f.c", 1, "DWC_Other", (void *)0x1000);
    hwlog_format(g_file_buf, sizeof g_file_buf);
    check(strstr(g_file_buf, "DWC_Other (stub) called from") && strstr(g_file_buf, "[x2]"), "once per (stub, caller) pair", 0);
    hwlog_reset();
    for (i = 0; i < 300; i++) {
        many[i].file = "t.c";
        many[i].line = i;
        many[i].count = 0;
        many[i].slot = -1;
        hwlog_emit(&many[i], "site %d", i);
    }
    check(hwlog_sites() == HWLOG_RING && many[0].slot == -1 && many[299].slot >= 0, "ring of 256: the oldest sites are evicted", hwlog_sites());
    hwlog_emit(&many[0], "again");
    check(many[0].count == 2 && hwlog_calls() == 301, "an evicted site keeps counting", many[0].count);
    check(hwlog_format(buf, 50) <= 49 && strlen(buf) <= 49, "format truncates at cap - 1", 0);
}

static void test_log_files(void)
{
    static HwLogSite s = { "t.c", 1, 0, -1 };

    hwlog_reset();
    g_file_writes = 0;
    g_console[0] = 0;
    hwlog_emit(&s, "hello %d", 7);
    hwlog_flush_if_due(10);
    check(g_file_writes == 0, "no dump before 60 vblanks", g_file_writes);
    hwlog_flush_if_due(60);
    check(g_file_writes == 1 && strcmp(g_file_path, "sdmc:/3ds/3DP/hwlog.txt") == 0, "dump to sdmc:/3ds/3DP/hwlog.txt after 60 vblanks", g_file_writes);
    check(strstr(g_file_buf, "hello 7") != NULL && g_mkdirs >= 1, "dump content and directory creation", 0);
    hwlog_flush_if_due(130);
    check(g_file_writes == 1, "nothing changed: no second dump", g_file_writes);
    hwlog_emit(&s, "again");
    hwlog_flush_if_due(119);
    check(g_file_writes == 1, "changed but under 60 vblanks since the last dump", g_file_writes);
    hwlog_flush_if_due(200);
    check(g_file_writes == 2, "changed and due: dump", g_file_writes);
    hwlog_show(5);
    check(strstr(g_console, "[x2] hello 7") != NULL, "bottom console shows the latest lines", 0);
}

/* ---- config.ini --------------------------------------------------------------------------------------------------------------- */
static void test_cfg(void)
{
    HwCfg a, b;
    char buf[1024];
    uint32_t skipped = 99;
    size_t n;
    const char *txt = "# comment\r\n  owner_name = ASH  \r\nowner_color=3\nbirth_month = 12\nbirth_day = 31 ; not a comment, rejected\n"
                      "language = 5\nmac = AA:bb:0C:0d:e0:F1\nrtc_offset = -86400\nbogus = 1\nnoequals\nowner_color = 16\nbirth_month = 0\n";

    hwcfg_defaults(&a);
    check(a.name_len == 6 && a.name[0] == 'P' && a.color == 11 && a.birth_month == 1 && a.language == 1 && a.rtc_offset == 0, "defaults", 0);
    n = hwcfg_format(&a, buf, sizeof buf);
    check(n > 0 && strstr(buf, "owner_name = PLAYER") && strstr(buf, "mac = 00:09:bf:3d:50:01"), "format of the defaults", (long)n);
    hwcfg_defaults(&b);
    b.color = 0;
    b.name_len = 1;
    check(hwcfg_parse(&b, buf, n, &skipped) == 8 && skipped == 0, "parse(format(defaults)) applies its 8 keys, skips none", skipped);
    check(memcmp(&a, &b, sizeof a) == 0, "config round trip is exact", 0);
    hwcfg_defaults(&a);
    check(hwcfg_parse(&a, txt, strlen(txt), &skipped) == 6, "messy file: 6 keys applied", 0);
    check(skipped == 5, "messy file: 5 lines ignored (bad day, unknown key, no '=', colour 16, month 0)", skipped);
    check(a.name_len == 3 && a.name[0] == 'A' && a.name[2] == 'H' && a.name[3] == 0, "trimmed name, CRLF handled", a.name_len);
    check(a.color == 3 && a.birth_month == 12 && a.birth_day == 1 && a.language == 5, "numeric keys, rejected values keep the old ones", 0);
    check(a.mac[0] == 0xaa && a.mac[1] == 0xbb && a.mac[2] == 0x0c && a.mac[3] == 0x0d && a.mac[4] == 0xe0 && a.mac[5] == 0xf1, "mac, mixed case", 0);
    check(a.rtc_offset == -86400, "negative rtc_offset", (long)a.rtc_offset);
    hwcfg_defaults(&a);
    check(a.allowed_channels == 0x1041u && strstr(buf, "allowed_channels = 0x1041") != NULL, "allowed-channel mask defaults to 0x1041 and is formatted", a.allowed_channels);
    check(hwcfg_parse(&a, "allowed_channels = 0x0001\n", 26, NULL) == 1 && a.allowed_channels == 1u, "allowed_channels hex", a.allowed_channels);
    check(hwcfg_parse(&a, "allowed_channels = 4161\n", 24, NULL) == 1 && a.allowed_channels == 0x1041u, "allowed_channels decimal", a.allowed_channels);
    check(hwcfg_parse(&a, "allowed_channels = 0\nallowed_channels = 0x4000\nallowed_channels = 0xzz\n", 71, &skipped) == 0 && skipped == 3 && a.allowed_channels == 0x1041u,
          "allowed_channels: 0, above channel 14 and non-hex are rejected", skipped);
    hwcfg_defaults(&a);
    check(hwcfg_parse(&a, "owner_name = ABCDEFGHIJK\n", 25, &skipped) == 0 && a.name_len == 6, "11-character name rejected", a.name_len);
    check(hwcfg_parse(&a, "owner_name = ABCDEFGHIJ", 23, NULL) == 1 && a.name_len == 10, "10-character name, no trailing newline", a.name_len);
    check(hwcfg_parse(&a, "mac = 00:11:22:33:44\n", 21, &skipped) == 0 && skipped == 1, "short mac rejected", 0);
    check(hwcfg_parse(&a, "", 0, &skipped) == 0 && skipped == 0, "empty file", 0);
    check(hwcfg_parse(&a, "owner_name = \xc3\xa9X\n", 16, NULL) == 1 && a.name[0] == '?' && a.name[1] == '?' && a.name[2] == 'X', "non-ASCII bytes become '?'", 0);
    memset(buf, 'a', 400);
    buf[400] = '\n';
    check(hwcfg_parse(&a, buf, 401, &skipped) == 0 && skipped == 1, "an over-long line is cut, not overrun", 0);
    check(hwcfg_format(&a, buf, 40) == 39 && strlen(buf) == 39, "format truncates at cap - 1", 0);
    check(hwcfg_format(&a, NULL, 10) == 0 && hwcfg_format(&a, buf, 1) == 0, "format with bad arguments returns 0", 0);
}

/* ---- the radio-less WM's reply queue (review 1.6, B2) ------------------------------------------------------------------------- */
static void test_wmq(void)
{
    HwWmQ q;
    uint16_t ctl = 0;
    uint8_t dst[HWWMQ_REPLY_BYTES], a[HWWMQ_REPLY_BYTES], b[HWWMQ_REPLY_BYTES];
    unsigned f, requests = 0, replies = 0, max_per_frame = 0, this_frame;
    int pending_scan;

    /* scan time: channels x maxChannelTime ms, 0 = the SDK's 30 ms default, whole 60 Hz frames, at least one */
    check(hwwmq_scan_frames(1, 30) == 2, "one channel x 30 ms = 2 frames (1.8 rounded up)", hwwmq_scan_frames(1, 30));
    check(hwwmq_scan_frames(1, 0) == 2, "maxChannelTime 0 = the default 30 ms", hwwmq_scan_frames(1, 0));
    check(hwwmq_scan_frames(3, 30) == 6, "three channels x 30 ms = 6 frames (5.4 rounded up)", hwwmq_scan_frames(3, 30));
    check(hwwmq_scan_frames(1, 1) == 1, "never less than one frame", hwwmq_scan_frames(1, 1));
    check(hwwmq_scan_frames(0, 30) == 1, "an empty list still takes one frame", hwwmq_scan_frames(0, 30));
    check(hwwmq_scan_frames(1, 65535) == 3933, "65535 ms = 3933 frames", hwwmq_scan_frames(1, 65535));

    /* an immediate reply goes straight into the buffer and takes the callback-control bit */
    hwwmq_init(&q);
    memset(a, 0xa5, sizeof a);
    memset(dst, 0, sizeof dst);
    check(hwwmq_post(&q, &ctl, dst, a, 0) == 1 && dst[0] == 0xa5 && dst[HWWMQ_REPLY_BYTES - 1] == 0xa5 && (ctl & 1u) == 1u, "immediate reply: written, busy bit set", ctl);
    /* a second one while the ARM9 has not released the buffer must NOT overwrite it: queued, delivered after the release */
    memset(b, 0x5a, sizeof b);
    check(hwwmq_post(&q, &ctl, dst, b, 0) == 0 && dst[0] == 0xa5 && hwwmq_pending(&q) == 1u, "reply while the ARM9 holds the buffer: queued, buffer untouched", dst[0]);
    check(hwwmq_tick(&q, &ctl, dst) == 0 && dst[0] == 0xa5, "tick while still busy: not delivered", dst[0]);
    ctl = 0;   /* WmClearFifoRecvFlag */
    check(hwwmq_tick(&q, &ctl, dst) == 1 && dst[0] == 0x5a && (ctl & 1u) == 1u && hwwmq_pending(&q) == 0u, "after the ARM9 released it: delivered, busy again", dst[0]);

    /* a delayed reply: nothing before its due frame, exactly one at it */
    hwwmq_init(&q);
    ctl = 0;
    memset(dst, 0, sizeof dst);
    check(hwwmq_post(&q, &ctl, dst, a, 2) == 0 && dst[0] == 0 && ctl == 0u, "delayed reply: not written by the post", dst[0]);
    check(hwwmq_tick(&q, &ctl, dst) == 0 && dst[0] == 0, "frame 1: no reply before the scan time", dst[0]);
    check(hwwmq_tick(&q, &ctl, dst) == 1 && dst[0] == 0xa5, "frame 2: the reply, exactly once", dst[0]);
    ctl = 0;
    check(hwwmq_tick(&q, &ctl, dst) == 0 && hwwmq_tick(&q, &ctl, dst) == 0, "no second delivery", 0);
    /* a delayed reply queued behind an earlier one keeps the order, and an immediate one never jumps the queue */
    hwwmq_init(&q);
    ctl = 0;
    check(hwwmq_post(&q, &ctl, dst, a, 3) == 0 && hwwmq_post(&q, &ctl, dst, b, 0) == 0 && hwwmq_pending(&q) == 2u, "an immediate reply queues behind a pending one", 0);
    for (f = 0; f < 3; f++) {
        (void)hwwmq_tick(&q, &ctl, dst);
    }
    check(dst[0] == 0xa5 && hwwmq_pending(&q) == 1u, "FIFO order: the delayed one first", dst[0]);
    ctl = 0;
    check(hwwmq_tick(&q, &ctl, dst) == 1 && dst[0] == 0x5a, "then the immediate one", dst[0]);
    /* overflow is counted, not silent */
    hwwmq_init(&q);
    ctl = 1;
    for (f = 0; f < HWWMQ_DEPTH; f++) {
        (void)hwwmq_post(&q, &ctl, dst, a, 5);
    }
    check(hwwmq_post(&q, &ctl, dst, a, 5) == -1 && q.dropped == 1u, "a full queue drops and counts", q.dropped);

    /* the livelock model: the game's scan-finished callback starts the next scan at once. With the reply after the scan time the request rate is
       bounded by the frame clock: at most one request per frame, and one scan per scan time (2 frames at 30 ms) */
    hwwmq_init(&q);
    ctl = 0;
    pending_scan = 1;                  /* the first WM_StartScan */
    requests = 1;
    check(hwwmq_post(&q, &ctl, dst, a, hwwmq_scan_frames(1, 30)) == 0, "scan queued", 0);
    for (f = 0; f < 120; f++) {
        this_frame = 0;
        if (hwwmq_tick(&q, &ctl, dst)) {   /* the reply reaches the game ... */
            replies++;
            ctl = 0;                       /* ... its callback runs, WmClearFifoRecvFlag ... */
            requests++;                    /* ... and starts the next scan inside the callback */
            this_frame++;
            (void)hwwmq_post(&q, &ctl, dst, a, hwwmq_scan_frames(1, 30));
        }
        if (this_frame > max_per_frame) {
            max_per_frame = this_frame;
        }
    }
    (void)pending_scan;
    check(replies == 60 && requests == 61, "120 frames of back-to-back scans: 60 replies (one per 2-frame scan), no spin", replies);
    check(max_per_frame == 1u, "never more than one scan restart per frame", max_per_frame);
}

/* ---- MD5 / SHA-1 / HMAC ---------------------------------------------------------------------------------------------------- */
static void hex(const uint8_t *d, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", d[i]);
    }
}

static void md5_is(const char *msg, const char *want)
{
    uint8_t d[16];
    char h[33];
    HwMd5 c;

    hwdgt_md5_init(&c);
    hwdgt_md5_update(&c, msg, strlen(msg));
    hwdgt_md5_final(&c, d);
    hex(d, 16, h);
    check(strcmp(h, want) == 0, "MD5 vector (RFC 1321)", (long)strlen(msg));
}

static void sha1_is(const char *msg, const char *want)
{
    uint8_t d[20];
    char h[41];
    HwSha1 c;

    hwdgt_sha1_init(&c);
    hwdgt_sha1_update(&c, msg, strlen(msg));
    hwdgt_sha1_final(&c, d);
    hex(d, 20, h);
    check(strcmp(h, want) == 0, "SHA-1 vector (FIPS 180)", (long)strlen(msg));
}

static void test_hashes(void)
{
    static uint8_t mil[1000000];
    uint8_t d[20];
    char h[41];
    HwSha1 s;
    HwMd5 m;
    size_t i, pos;

    md5_is("", "d41d8cd98f00b204e9800998ecf8427e");
    md5_is("a", "0cc175b9c0f1b6a831c399e269772661");
    md5_is("abc", "900150983cd24fb0d6963f7d28e17f72");
    md5_is("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    md5_is("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    md5_is("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f");
    md5_is("12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a");
    sha1_is("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1_is("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    memset(mil, 'a', sizeof mil);
    hwdgt_sha1_init(&s);
    for (i = 0; i < sizeof mil; i += 1000) {
        hwdgt_sha1_update(&s, mil + i, 1000);
    }
    hwdgt_sha1_final(&s, d);
    hex(d, 20, h);
    check(strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0, "SHA-1 of one million 'a' in 1000-byte chunks", 0);
    hwdgt_sha1_init(&s);
    for (pos = 0, i = 1; pos < sizeof mil; pos += i, i = i % 97 + 1) {
        hwdgt_sha1_update(&s, mil + pos, pos + i > sizeof mil ? sizeof mil - pos : i);
    }
    hwdgt_sha1_final(&s, d);
    hex(d, 20, h);
    check(strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0, "SHA-1 million 'a' in odd chunk sizes 1..97", 0);
    hwdgt_md5_init(&m);
    for (pos = 0, i = 1; pos < sizeof mil; pos += i, i = i % 130 + 1) {
        hwdgt_md5_update(&m, mil + pos, pos + i > sizeof mil ? sizeof mil - pos : i);
    }
    hwdgt_md5_final(&m, d);
    hex(d, 16, h);
    check(strcmp(h, "7707d6ae4e027c70eea2a935c2296f21") == 0, "MD5 of one million 'a' in odd chunk sizes", 0);
}

static void hmac_md5_is(const void *key, size_t kl, const void *data, size_t dl, const char *want)
{
    uint8_t d[16];
    char h[33];
    hwdgt_hmac_md5(d, data, dl, key, kl);
    hex(d, 16, h);
    check(strcmp(h, want) == 0, "HMAC-MD5 vector (RFC 2202)", (long)kl);
}

static void hmac_sha1_is(const void *key, size_t kl, const void *data, size_t dl, const char *want)
{
    uint8_t d[20];
    char h[41];
    hwdgt_hmac_sha1(d, data, dl, key, kl);
    hex(d, 20, h);
    check(strcmp(h, want) == 0, "HMAC-SHA1 vector (RFC 2202)", (long)kl);
}

static void test_hmac(void)
{
    uint8_t k0b[20], kaa[80], dd[50];
    const char *big = "Test Using Larger Than Block-Size Key - Hash Key First";

    memset(k0b, 0x0b, sizeof k0b);
    memset(kaa, 0xaa, sizeof kaa);
    memset(dd, 0xdd, sizeof dd);
    hmac_md5_is(k0b, 16, "Hi There", 8, "9294727a3638bb1c13f48ef8158bfc9d");
    hmac_md5_is("Jefe", 4, "what do ya want for nothing?", 28, "750c783e6ab0b503eaa86e310a5db738");
    hmac_md5_is(kaa, 16, dd, 50, "56be34521d144c88dbb8c733f0e8b3f6");
    hmac_md5_is(kaa, 80, big, strlen(big), "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd");
    hmac_sha1_is(k0b, 20, "Hi There", 8, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_sha1_is("Jefe", 4, "what do ya want for nothing?", 28, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    hmac_sha1_is(kaa, 20, dd, 50, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
    hmac_sha1_is(kaa, 80, big, strlen(big), "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    check(sizeof(HwMd5) == 88 && sizeof(HwSha1) == 92, "context sizes fit the SDK's DGTHash1/2Context", 0);
}

/* ---- review 1.3 fixes: boot IRQ state, timer re-arm ------------------------------------------------------------------------- */
static void test_irq_unmasked_at_boot(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;
    int i;

    check((HWOS_BOOT_CPSR & 0xc0u) == 0u && (HWOS_BOOT_CPSR & 0x1fu) == 0x1fu, "boot CPSR is SYS mode with the I and F bits clear (crt0.c:69-70)", HWOS_BOOT_CPSR);
    /* an OS_SpinWait loop ended by an IRQ handler: the loop waits for a flag only the timer-0 handler sets */
    pump_setup(&p, &l, io, &vb, 0);
    l.masked = (HWOS_BOOT_CPSR & 0x80u) != 0;
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1u << HWIRQ_TIMER0);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 1000));
    w16(io, HWR_TM0CNT_L + 2, CTL_START | CTL_IRQ);
    for (i = 0; i < 100000 && l.ndisp == 0; i++) {
        hwpump_spin(&p, 100);   /* OS_SpinWait(100) */
    }
    check(l.ndisp == 1 && l.disp[0] == 3 && i > 1 && i < 100000, "OS_SpinWait loop ends once the timer IRQ handler ran (IRQs unmasked at boot)", i);
    /* the old boot state (0x9f: I set) never delivers it: the same loop spins to its bound */
    pump_setup(&p, &l, io, &vb, 0);
    l.masked = 1;
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1u << HWIRQ_TIMER0);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 1000));
    w16(io, HWR_TM0CNT_L + 2, CTL_START | CTL_IRQ);
    for (i = 0; i < 5000 && l.ndisp == 0; i++) {
        hwpump_spin(&p, 100);
    }
    check(l.ndisp == 0 && i == 5000, "with the I bit set (the old boot state) the same loop would never end", i);
}

static void test_wait_irq_vblank(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;
    int ran_at_return;

    pump_setup(&p, &l, io, &vb, 0);
    l.masked = (HWOS_BOOT_CPSR & 0x80u) != 0;
    w16(io, HWR_DISPSTAT, 8);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1);
    hwpump_run(&p, HWP_WAIT);   /* OS_WaitIrq(VBLANK): the yield that Y7 is */
    ran_at_return = l.ndisp;
    check(ran_at_return == 1 && l.disp[0] == 0, "OS_WaitIrq(VBLANK) returns only after the registered vblank handler ran", ran_at_return);
    check(vb == 1 && l.waits == 1, "and after the frame boundary", vb);
    hwpump_run(&p, HWP_WAIT);
    check(l.ndisp == 2 && vb == 2, "the second wait is a second frame with a second handler call", l.ndisp);
}

static void test_timer_rearm_between_pumps(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    PumpLog l;
    uint32_t vb;

    pump_setup(&p, &l, io, &vb, 0);
    w32(io, HWR_IME, 1);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 1000));
    w16(io, HWR_TM0CNT_L + 2, CTL_START);
    hwpump_run(&p, HWP_NOWAIT);
    l.now = 400u * HWT_SYS_PER_BUS;
    hwpump_run(&p, HWP_NOWAIT);
    check(r16(io, HWR_TM0CNT_L) == (uint16_t)(0x10000 - 600), "timer running: 400 bus ticks after the latch", r16(io, HWR_TM0CNT_L));
    /* OS_SetTimerControl(re-arm): sync, new reload, enable bit written again while the register still has it set */
    l.now = 500u * HWT_SYS_PER_BUS;
    hwpump_sync(&p);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 100));
    w16(io, HWR_TM0CNT_L + 2, CTL_START);
    hwtimer_ctl_write(&p.tm, 0);
    l.now = 530u * HWT_SYS_PER_BUS;
    hwpump_run(&p, HWP_NOWAIT);
    check(r16(io, HWR_TM0CNT_L) == (uint16_t)(0x10000 - 100 + 30), "re-arm without a stop: the counter restarts at the new reload", r16(io, HWR_TM0CNT_L));
    /* without the hook (the old behaviour) the pump never sees the re-arm */
    pump_setup(&p, &l, io, &vb, 0);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 1000));
    w16(io, HWR_TM0CNT_L + 2, CTL_START);
    hwpump_run(&p, HWP_NOWAIT);
    l.now = 500u * HWT_SYS_PER_BUS;
    hwpump_sync(&p);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 100));
    w16(io, HWR_TM0CNT_L + 2, CTL_START);
    l.now = 530u * HWT_SYS_PER_BUS;
    hwpump_run(&p, HWP_NOWAIT);
    check(r16(io, HWR_TM0CNT_L) == (uint16_t)(0x10000 - 1000 + 530), "(control) without hwtimer_ctl_write the running counter is not restarted", r16(io, HWR_TM0CNT_L));
    check(p.tm.running[0] == 1, "timer 0 is running", 0);
}

/* ---- DMA ---------------------------------------------------------------------------------------------------------------------- */
#define DS_MAIN 0x02000000u
#define DS_PLTT 0x05000000u
#define DS_VRAM 0x06000000u
#define DS_OAM  0x07000000u
#define DS_GXF  0x04000400u
static uint8_t g_dmain[0x1000], g_dvram[0x1000], g_doam[0x400], g_dpltt[0x400];
static uint32_t g_gxlast;
static int g_gxn;

static void *dma_xlat_t(void *u, uint32_t addr, uint32_t bytes, HwDmaRegion *region)
{
    (void)u;
    if (addr >= DS_MAIN && addr + bytes <= DS_MAIN + sizeof g_dmain) {
        *region = HWDMA_MAIN;
        return g_dmain + (addr - DS_MAIN);
    }
    if (addr >= DS_VRAM && addr + bytes <= DS_VRAM + sizeof g_dvram) {
        *region = HWDMA_VRAM;
        return g_dvram + (addr - DS_VRAM);
    }
    if (addr >= DS_OAM && addr + bytes <= DS_OAM + sizeof g_doam) {
        *region = HWDMA_OAM;
        return g_doam + (addr - DS_OAM);
    }
    if (addr >= DS_PLTT && addr + bytes <= DS_PLTT + sizeof g_dpltt) {
        *region = HWDMA_PLTT;
        return g_dpltt + (addr - DS_PLTT);
    }
    if (addr >= DS_GXF && addr < DS_GXF + 0x40u) {
        *region = HWDMA_GXFIFO;
        return NULL;
    }
    *region = HWDMA_UNMAPPED;
    return NULL;
}

static void gx_sink_t(void *u, uint32_t w)
{
    (void)u;
    g_gxlast = w;
    g_gxn++;
}

static int g_blk_take;
static uint32_t g_blk_n, g_blk_first;

static int gx_block_t(void *u, const uint32_t *src, uint32_t n)
{
    (void)u;
    g_blk_n += n;
    g_blk_first = src[0];
    return g_blk_take;
}

static uint32_t cnt_of(uint32_t units, uint32_t dar, uint32_t sar, int w32bit, uint32_t extra)
{
    return units | (dar << 21) | (sar << 23) | (w32bit ? HWDMA_CNT_32BIT : 0u) | extra | HWDMA_CNT_ENABLE;
}

static void dma_go(HwDma *d, uint8_t *io, unsigned n, uint32_t sad, uint32_t dad, uint32_t cnt, uint32_t *units)
{
    w32(io, HWR_DMA0SAD + 12u * n, sad);
    w32(io, HWR_DMA0SAD + 12u * n + 4u, dad);
    w32(io, HWR_DMA0SAD + 12u * n + 8u, cnt);
    *units = hwdma_start(d, n);
}

static void test_dma(void)
{
    static uint8_t io[0x1000];
    static const HwDmaOps ops = { dma_xlat_t, gx_sink_t, NULL, NULL, NULL };
    HwDma d;
    uint32_t u, i, ok;

    memset(io, 0, sizeof io);
    memset(g_dmain, 0, sizeof g_dmain);
    memset(g_dvram, 0, sizeof g_dvram);
    memset(g_doam, 0, sizeof g_doam);
    memset(g_dpltt, 0, sizeof g_dpltt);
    hwdma_init(&d, &ops, io);
    for (i = 0; i < 64u; i++) {
        g_dmain[i] = (uint8_t)(i + 1u);
    }
    /* 32-bit, both increment, to VRAM, with the IRQ bit */
    dma_go(&d, io, 3, DS_MAIN, DS_VRAM + 0x10u, cnt_of(8, 0, 0, 1, HWDMA_CNT_IRQ), &u);
    check(u == 8 && memcmp(g_dvram + 0x10, g_dmain, 32) == 0, "DMA3 32-bit inc/inc copies 8 words main -> VRAM", u);
    check(!(r32(io, HWR_DMA0SAD + 36 + 8) & HWDMA_CNT_ENABLE), "the enable bit is cleared on completion (MI_WaitDma passes)", r32(io, HWR_DMA0SAD + 44));
    check(r32(io, HWR_IF) == (1u << (HWIRQ_DMA0 + 3)) && d.irqs == 1, "IRQ-on-complete sets IF bit 8 + n (DMA3 = bit 11)", r32(io, HWR_IF));
    check(d.bytes[HWDMA_VRAM] == 32 && d.started[3] == 1, "per-region byte count and per-channel start count", d.bytes[HWDMA_VRAM]);
    /* 16-bit, no IRQ bit: IF is untouched */
    w32(io, HWR_IF, 0);
    dma_go(&d, io, 0, DS_MAIN, DS_OAM, cnt_of(5, 0, 0, 0, 0), &u);
    check(u == 5 && memcmp(g_doam, g_dmain, 10) == 0 && g_doam[10] == 0, "DMA0 16-bit copies exactly 5 halfwords to OAM", u);
    check(r32(io, HWR_IF) == 0 && d.bytes[HWDMA_OAM] == 10, "no IRQ bit: IF unchanged", r32(io, HWR_IF));
    /* source fixed = a fill */
    g_dmain[0x100] = 0xaa;
    g_dmain[0x101] = 0xbb;
    g_dmain[0x102] = 0xcc;
    g_dmain[0x103] = 0xdd;
    dma_go(&d, io, 1, DS_MAIN + 0x100u, DS_PLTT, cnt_of(6, 0, 2, 1, 0), &u);
    ok = 1;
    for (i = 0; i < 6u; i++) {
        ok &= g_dpltt[4u * i] == 0xaa && g_dpltt[4u * i + 3u] == 0xdd;
    }
    check(ok && g_dpltt[24] == 0 && d.bytes[HWDMA_PLTT] == 24, "source fixed: a 32-bit fill into the palette", d.bytes[HWDMA_PLTT]);
    /* destination decrement: word i lands at dad - 4i */
    dma_go(&d, io, 2, DS_MAIN, DS_MAIN + 0x200u + 12u, cnt_of(4, 1, 0, 1, 0), &u);
    check(u == 4 && memcmp(g_dmain + 0x200, (uint8_t[]){ 13, 14, 15, 16, 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4 }, 16) == 0,
          "destination decrement writes the words in reverse order", u);
    /* source decrement */
    dma_go(&d, io, 2, DS_MAIN + 8u, DS_MAIN + 0x300u, cnt_of(3, 0, 1, 1, 0), &u);
    check(u == 3 && memcmp(g_dmain + 0x300, (uint8_t[]){ 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4 }, 12) == 0, "source decrement reads the words in reverse order", u);
    /* destination fixed (16 bit): the last halfword wins */
    dma_go(&d, io, 2, DS_MAIN, DS_MAIN + 0x400u, cnt_of(4, 2, 0, 0, 0), &u);
    check(u == 4 && g_dmain[0x400] == 7 && g_dmain[0x401] == 8 && g_dmain[0x402] == 0, "destination fixed: the last halfword lands on the single address", g_dmain[0x400]);
    /* unaligned addresses lose their low bits */
    dma_go(&d, io, 2, DS_MAIN + 1u, DS_MAIN + 0x503u, cnt_of(1, 0, 0, 1, 0), &u);
    check(g_dmain[0x500] == 1 && g_dmain[0x503] == 4 && g_dmain[0x504] == 0, "32-bit DMA aligns both addresses down (word 1,2,3,4 lands at 0x500)", g_dmain[0x500]);
    /* geometry FIFO: word by word into the placeholder sink */
    g_gxn = 0;
    dma_go(&d, io, 1, DS_MAIN, DS_GXF, cnt_of(5, 2, 0, 1, 0), &u);
    check(g_gxn == 5 && g_gxlast == 0x14131211u, "GXFIFO destination feeds the sink word by word (placeholder for bet 2.1)", g_gxlast);
    check(d.gx_words == 5 && d.bytes[HWDMA_GXFIFO] == 20, "and is counted", d.gx_words);
    {   /* bet 2.1c2: the block hook takes a word-wide, word-stepped run whole (the sink is not called) or declines it (the sink gets every word) */
        HwDmaOps bops = ops;
        HwDma bd;
        uint32_t u2;

        bops.gx_block = gx_block_t;
        hwdma_init(&bd, &bops, io);
        g_gxn = 0; g_blk_n = 0; g_blk_take = 1;
        dma_go(&bd, io, 1, DS_MAIN, DS_GXF, cnt_of(5, 2, 0, 1, 0), &u2);
        check(g_blk_n == 5 && g_blk_first == 0x04030201u && g_gxn == 0 && bd.gx_words == 5 && bd.bytes[HWDMA_GXFIFO] == 20, "gx_block taking the run: the sink is skipped, the words still counted", g_blk_n);
        g_blk_take = 0; g_blk_n = 0;
        dma_go(&bd, io, 1, DS_MAIN, DS_GXF, cnt_of(5, 2, 0, 1, 0), &u2);
        check(g_blk_n == 5 && g_gxn == 5 && bd.gx_words == 10, "gx_block declining the run: the sink gets every word", g_gxn);
        g_gxn = 0; g_blk_n = 0;
        dma_go(&bd, io, 1, DS_MAIN, DS_GXF, cnt_of(5, 2, 0, 0, 0), &u2);   /* 16-bit: never offered */
        check(g_blk_n == 0 && g_gxn == 5, "a halfword burst is never offered to gx_block", g_blk_n);
    }
    /* the SDK's dummy DMA0 rewrite and a disabled channel */
    dma_go(&d, io, 0, 0, 0, HWDMA_CNT_ENABLE | 1u, &u);
    check(u == 0 && d.dummies == 1 && !(r32(io, HWR_DMA0SAD + 8) & HWDMA_CNT_ENABLE), "dummy DMA0 (src = dst = 0) is a no-op that clears the enable bit", d.dummies);
    dma_go(&d, io, 0, DS_MAIN, DS_VRAM, cnt_of(2, 0, 0, 1, 0) & ~HWDMA_CNT_ENABLE, &u);
    check(u == 0 && g_dvram[0] == 0, "a CNT write without the enable bit does nothing", u);
    /* unmapped destination / repeat / timed */
    i = d.unmapped;
    dma_go(&d, io, 2, DS_MAIN, 0x30u, cnt_of(2, 0, 0, 1, 0), &u);
    check(d.unmapped == i + 1u && !(r32(io, HWR_DMA0SAD + 24 + 8) & HWDMA_CNT_ENABLE), "an unmapped destination is counted, nothing is written, the enable bit still clears", d.unmapped);
    dma_go(&d, io, 2, DS_MAIN, DS_MAIN + 0x600u, cnt_of(2, 0, 0, 1, HWDMA_CNT_REPEAT | (1u << 27)), &u);
    check(u == 2 && d.repeat_ignored == 1 && d.timed == 1 && g_dmain[0x600] == 1, "repeat and vblank timing run once, at once, and are counted", d.timed);
}

/* ---- divider and square root --------------------------------------------------------------------------------------------------- */
static void div_case(uint8_t *io, unsigned mode, int64_t n, int64_t dn, int64_t q, int64_t r, int div0, const char *what)
{
    uint64_t un = (uint64_t)n, ud = (uint64_t)dn, rq, rr;

    w16(io, HWR_DIVCNT, (uint16_t)(mode | HWCP_BUSY));
    memcpy(io + HWR_DIV_NUMER, &un, 8);
    memcpy(io + HWR_DIV_DENOM, &ud, 8);
    hwcp_div_run(io);
    memcpy(&rq, io + HWR_DIV_RESULT, 8);
    memcpy(&rr, io + HWR_DIVREM, 8);
    check((int64_t)rq == q, what, (long)(int64_t)rq);
    check((int64_t)rr == r, what, (long)(int64_t)rr);
    check(!(r16(io, HWR_DIVCNT) & HWCP_BUSY) && ((r16(io, HWR_DIVCNT) & HWCP_DIV0) != 0) == (div0 != 0), "BUSY clear and DIV0 flag as expected", div0);
}

static void test_cp(void)
{
    static uint8_t io[0x1000];
    uint32_t x, root;
    uint64_t v, s;
    static const uint64_t samples[] = { 0, 1, 2, 3, 4, 15, 16, 17, 255, 256, 65535, 65536, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu,
                                        0x100000000ull, 0xffffffffffffull, 0x3fffffffffffffffull, 0x4000000000000000ull, 0xfffffffffffffffeull,
                                        0xffffffffffffffffull };
    unsigned k;

    memset(io, 0, sizeof io);
    div_case(io, 0, 7, 2, 3, 1, 0, "32/32: 7 / 2");
    div_case(io, 0, -7, 2, -3, -1, 0, "32/32: -7 / 2 truncates toward zero, remainder takes the numerator's sign");
    div_case(io, 0, 7, -2, -3, 1, 0, "32/32: 7 / -2");
    div_case(io, 0, (int64_t)0x100000007ll, 2, 3, 1, 0, "32/32 mode ignores the upper numerator bits");
    div_case(io, 0, 5, 0, (int64_t)0x00000000ffffffffll, 5, 1, "32/32: 5 / 0 = -1 (upper word inverted), remainder 5, DIV0");
    div_case(io, 0, -5, 0, (int64_t)0xffffffff00000001ull, -5, 1, "32/32: -5 / 0 = +1 (upper word 0xffffffff), remainder -5, DIV0");
    div_case(io, 0, (int64_t)INT32_MIN, -1, (int64_t)1 << 31, 0, 0, "32/32: INT_MIN / -1 = +2^31, remainder 0, no DIV0");
    div_case(io, 1, (int64_t)1 << 40, 3, 366503875925ll, 1, 0, "64/32 (mode 1): 2^40 / 3");
    div_case(io, 3, (int64_t)1 << 40, 3, 366503875925ll, 1, 0, "mode 3 behaves as mode 1");
    div_case(io, 1, -((int64_t)1 << 40), 3, -366503875925ll, -1, 0, "64/32: negative numerator");
    div_case(io, 1, 100, 0, -1, 100, 1, "64/32: / 0 = -1, remainder = numerator, DIV0");
    div_case(io, 1, -100, 0, 1, -100, 1, "64/32: negative / 0 = +1");
    div_case(io, 2, ((int64_t)1 << 62) + 5, (int64_t)1 << 31, (int64_t)1 << 31, 5, 0, "64/64: (2^62 + 5) / 2^31");
    div_case(io, 2, 12345, 0, -1, 12345, 1, "64/64: / 0");
    div_case(io, 2, INT64_MIN, -1, INT64_MIN, 0, 0, "64/64: INT64_MIN / -1 = INT64_MIN, remainder 0");
    div_case(io, 2, INT64_MIN, 1, INT64_MIN, 0, 0, "64/64: INT64_MIN / 1");
    div_case(io, 2, -9, 4, -2, -1, 0, "64/64: -9 / 4");
    /* sqrt: floor(sqrt(x)) for 32-bit and 64-bit inputs, checked by squaring */
    for (k = 0; k < sizeof samples / sizeof samples[0]; k++) {
        v = samples[k];
        root = hwcp_isqrt(v);
        s = (uint64_t)root * root;
        check(s <= v && (root == 0xffffffffu || (uint64_t)(root + 1u) * (root + 1u) > v), "isqrt(x)^2 <= x < (isqrt(x)+1)^2", (long)root);
    }
    for (x = 0; x < 3000u; x++) {
        root = hwcp_isqrt(x);
        if (!((uint64_t)root * root <= x && (uint64_t)(root + 1u) * (root + 1u) > x)) {
            check(0, "isqrt dense 0..2999", (long)x);
            break;
        }
    }
    check(hwcp_isqrt(0xffffffffffffffffull) == 0xffffffffu && hwcp_isqrt(0xffffffffu) == 65535u && hwcp_isqrt(16) == 4, "isqrt anchors", 0);
    w16(io, HWR_SQRTCNT, (uint16_t)HWCP_BUSY);
    v = 0xffffffffffffffffull;   /* mode 0: only the low 32 bits count */
    memcpy(io + HWR_SQRT_PARAM, &v, 8);
    hwcp_sqrt_run(io);
    check(r32(io, HWR_SQRT_RESULT) == 65535u && !(r16(io, HWR_SQRTCNT) & HWCP_BUSY), "sqrt mode 0 uses 32 bits and clears BUSY", r32(io, HWR_SQRT_RESULT));
    w16(io, HWR_SQRTCNT, 1u);
    hwcp_sqrt_run(io);
    check(r32(io, HWR_SQRT_RESULT) == 0xffffffffu, "sqrt mode 1 uses all 64 bits", r32(io, HWR_SQRT_RESULT));
}

/* ---- PXI loopback, PM and CARD responders --------------------------------------------------------------------------------------- */
static int32_t g_card_block[4];

static void *arm7_xlat_t(void *u, uint32_t addr)
{
    (void)u;
    return addr == 0x1000u ? (void *)g_card_block : NULL;
}

static uint32_t pxi_word(unsigned tag, unsigned err, uint32_t data)
{
    return tag | (err << 5) | (data << 6);
}

#define SPI_S 0x02000000u
#define SPI_E 0x01000000u

static void test_pxi(void)
{
    static uint8_t io[0x1000];
    static const HwArm7Ops aops = { .xlat = arm7_xlat_t };
    HwPxi p;
    HwArm7 a;
    uint32_t w;

    memset(io, 0, sizeof io);
    hwpxi_init(&p, io);
    hwarm7_init(&a, &p, &aops);
    check(r16(io, HWR_PXI_CNT) & HWPXI_CNT_RECV_EMPTY && r16(io, HWR_PXI_CNT) & HWPXI_CNT_SEND_EMPTY, "fresh loopback: both FIFOs empty", r16(io, HWR_PXI_CNT));
    check((p.handle_bits & (1u << HWPXI_TAG_PM)) && (p.handle_bits & (1u << HWPXI_TAG_FS)) && (p.handle_bits & (1u << HWPXI_TAG_SOUND)) &&
          (p.handle_bits & (1u << HWPXI_TAG_OS)) && (p.handle_bits & (1u << HWPXI_TAG_RTC)) && (p.handle_bits & (1u << HWPXI_TAG_TOUCH)) &&
          (p.handle_bits & (1u << HWPXI_TAG_MIC)), "ARM7 handle bits seeded for PM, FS, SND, OS, RTC, TOUCH, MIC (the SDK's empty-loop waits pass)", p.handle_bits);
    /* PM_Init's sync: the command word 0x60, answered with the 0xe0 echo */
    w16(io, HWR_PXI_CNT, (uint16_t)(r16(io, HWR_PXI_CNT) | 0x0400u));   /* RECV_RI */
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | SPI_E | (0x60u << 8)));
    check(r32(io, HWR_IF) & (1u << HWIRQ_FIFO_RECV), "a reply raises the receive-FIFO IRQ (IF bit 18) when RECV_RI is set", r32(io, HWR_IF));
    check(!(r16(io, HWR_PXI_CNT) & HWPXI_CNT_RECV_EMPTY) && (r16(io, HWR_PXI_CNT) & HWPXI_CNT_SEND_EMPTY), "RECV not empty, SEND empty", r16(io, HWR_PXI_CNT));
    w = hwpxi_recv(&p);
    check((w & 31u) == HWPXI_TAG_PM && !((w >> 5) & 1u) && (w >> 6) == (SPI_S | SPI_E | 0xe000u), "PM sync reply: SYNC | 0x80 echo, result 0 (PMi_SyncFlag path)", w);
    check(r16(io, HWR_PXI_CNT) & HWPXI_CNT_RECV_EMPTY, "queue drained", r16(io, HWR_PXI_CNT));
    /* register write then read (two-word write) */
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | (0x64u << 8) | 2u));
    check(p.count == 0, "a command is answered only when its END word arrives", p.count);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, (1u << 16) | SPI_E | 0x5au));
    w = hwpxi_recv(&p);
    check((w >> 6) == (SPI_S | SPI_E | 0xe400u) && a.pmic[2] == 0x5a, "PM register write stores into the 5-register PMIC and acknowledges", w >> 6);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | SPI_E | (0x65u << 8) | 2u));
    w = hwpxi_recv(&p);
    check((w >> 6) == (SPI_S | SPI_E | 0xf200u | 0x5au), "PM register read returns the written value in REG0VALUE+2", w >> 6);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | SPI_E | (0x65u << 8) | 0u));
    w = hwpxi_recv(&p);
    check((w >> 6) == (SPI_S | SPI_E | 0xf000u | 0x0fu), "PM register 0 powers up as 0x0f", w >> 6);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | SPI_E | (0x65u << 8) | 7u));
    w = hwpxi_recv(&p);
    check((w >> 6) == (SPI_S | SPI_E | 0xe500u | 1u), "PM read of a register that does not exist: INVALID result", w >> 6);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_PM, 0, SPI_S | SPI_E | (0x61u << 8)));
    w = hwpxi_recv(&p);
    check((w >> 6) == (SPI_S | SPI_E | 0xe200u), "PM sleep start is answered with the sleep-end echo at once", w >> 6);
    /* CARD: INIT (2 words, err set), then a request */
    hwpxi_send(&p, pxi_word(HWPXI_TAG_FS, 1, 0));
    check(p.count == 0, "CARD INIT word 0 alone has no reply", p.count);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_FS, 1, 0x1000));
    w = hwpxi_recv(&p);
    check((w & 31u) == HWPXI_TAG_FS && ((w >> 5) & 1u) == 1u && (w >> 6) == 1u, "CARD INIT is acknowledged with CARD_REQ_ACK, err set", w);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_FS, 1, 5));
    w = hwpxi_recv(&p);
    check(w == pxi_word(HWPXI_TAG_FS, 1, 1) && g_card_block[0] == 3, "any other CARD request reports UNSUPPORTED in cmd->result and is acknowledged", g_card_block[0]);
    /* TOUCH: every SPI touch command is answered on the TOUCH tag (not PM) with its own echo, success; AUTO_ON's two words answer once, at the END word */
    hwpxi_send(&p, pxi_word(HWPXI_TAG_TOUCH, 0, SPI_S | SPI_E | (0x00u << 8)));
    w = hwpxi_recv(&p);
    check((w & 31u) == HWPXI_TAG_TOUCH && !((w >> 5) & 1u) && (w >> 6) == (SPI_S | SPI_E | 0x8000u), "touch SAMPLING is answered on the TOUCH tag: echo | 0x80, result 0", w);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_TOUCH, 0, SPI_S | (0x01u << 8) | 2u));
    check(p.count == 0, "touch AUTO_ON word 0 alone has no reply", p.count);
    hwpxi_send(&p, pxi_word(HWPXI_TAG_TOUCH, 0, (1u << 16) | SPI_E | 100u));
    w = hwpxi_recv(&p);
    check((w & 31u) == HWPXI_TAG_TOUCH && (w >> 6) == (SPI_S | SPI_E | 0x8100u) && a.touch_commands == 2u, "touch AUTO_ON is answered once, at its END word", w >> 6);
    /* SND is a placeholder: swallowed */
    hwpxi_send(&p, pxi_word(HWPXI_TAG_SOUND, 0, 0x1234));
    check(p.count == 0 && a.snd_swallowed == 1, "SND placeholder swallows (bet 1.5)", a.snd_swallowed);
    /* an unregistered tag echoes with ERR, like the SDK's own handler */
    hwpxi_send(&p, pxi_word(20, 0, 0x777));
    w = hwpxi_recv(&p);
    check(w == pxi_word(20, 1, 0x777) && p.unanswered[20] == 1, "unregistered tag: echoed with err = 1", w);
    /* reading an empty FIFO latches ERR once */
    (void)hwpxi_recv(&p);
    check(r16(io, HWR_PXI_CNT) & HWPXI_CNT_ERR, "reading an empty FIFO shows ERR", r16(io, HWR_PXI_CNT));
    hwpxi_cnt_sync(&p);
    check(!(r16(io, HWR_PXI_CNT) & HWPXI_CNT_ERR), "ERR is shown once", r16(io, HWR_PXI_CNT));
    /* overflow */
    for (w = 0; w < 20u; w++) {
        hwpxi_send(&p, pxi_word(20, 0, w));
    }
    check(p.count == HWPXI_FIFO_DEPTH && p.recv_overflow == 4u && (r16(io, HWR_PXI_CNT) & HWPXI_CNT_RECV_FULL), "a full receive FIFO drops and counts the overflow", p.recv_overflow);
}

/* ---- BLZ ------------------------------------------------------------------------------------------------------------------------ */
/* In-place note: the decoder writes downward from the end of the image over the very bytes it reads, so a stream is only valid when the
   output pointer never falls below the input pointer; the test data below is chosen so that holds. A reference encoder written from the format description (not from the decoder). Returns the compressed length (footer included) or 0 when
   it did not shrink. The compressed bytes are placed so that `bottom` = img + complen. */
static uint32_t blz_encode(const uint8_t *d, uint32_t n, uint8_t *stream, uint32_t cap)
{
    uint32_t pos = n, used = 0, k;
    uint8_t group[32];

    while (pos > 0) {
        uint32_t gl = 0, flag = 0;

        for (k = 0; k < 8u && pos > 0; k++) {
            uint32_t best = 0, bestd = 0, dist, len;

            for (dist = 3; dist <= 4098u && pos - 1u + dist < n; dist++) {
                for (len = 0; len < 18u && len < pos && pos - 1u - len + dist < n && d[pos - 1u - len] == d[pos - 1u - len + dist]; len++) {
                }
                if (len > best) {
                    best = len;
                    bestd = dist;
                }
            }
            if (best >= 3u) {
                uint32_t raw = bestd - 3u;

                flag |= 0x80u >> k;
                group[gl++] = (uint8_t)(((best - 3u) << 4) | (raw >> 8));   /* read first */
                group[gl++] = (uint8_t)(raw & 0xffu);
                pos -= best;
            } else {
                group[gl++] = d[pos - 1u];
                pos--;
            }
        }
        /* read order: flag, then the items; memory order is the reverse (read downward) */
        if (used + gl + 1u > cap) {
            return 0;
        }
        stream[used++] = (uint8_t)flag;
        for (k = 0; k < gl; k++) {
            stream[used++] = group[k];
        }
    }
    return used;
}

static void blz_roundtrip(const uint8_t *orig, uint32_t n, const char *what)
{
    static uint8_t stream[8192];
    static uint8_t img[8192 + 64];
    uint32_t used, complen, i, got;

    used = blz_encode(orig, n, stream, sizeof stream);
    complen = used + 8u;
    if (used == 0u || complen >= n) {
        check(0, "BLZ test data must compress", n);
        return;
    }
    memset(img, 0xee, sizeof img);
    for (i = 0; i < used; i++) {
        img[complen - 8u - 1u - i] = stream[i];   /* read order i sits i + 1 bytes below the footer */
    }
    {
        uint32_t info = (8u << 24) | complen, extra = n - complen;

        memcpy(img + complen - 8u, &info, 4);
        memcpy(img + complen - 4u, &extra, 4);
    }
    got = hwblz_uncompress(img + complen);
    check(got == n, what, got);
    check(memcmp(img, orig, n) == 0, what, 1);
    check(img[n] == 0xee, "BLZ writes nothing past the image", img[n]);
}

static void test_blz(void)
{
    static uint8_t data[2048], img[256];
    uint32_t i, x = 12345u, got;

    for (i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)("ABCDEFGH"[(i / 3u) % 8u]);
    }
    blz_roundtrip(data, 1500, "BLZ: periodic data (blocks only, max length 18)");
    for (i = 0; i < sizeof data; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (i % 41u == 0u) ? (uint8_t)(x >> 24) : (uint8_t)((i % 5u) * 17u);   /* sparse literals keep the in-place output ahead of the input */
    }
    blz_roundtrip(data, 2000, "BLZ: literals mixed with blocks");
    memset(data, 0x5a, sizeof data);
    blz_roundtrip(data, 2048, "BLZ: a constant run (distance 3 overlapped copies)");
    for (i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)((i * 31u) & 0xffu);
        if (i >= 600u) {
            data[i] = data[i - 600u];   /* a long-distance repeat: index bits above 8 */
        }
    }
    blz_roundtrip(data, 1800, "BLZ: distance beyond 256 (index high bits)");
    check(hwblz_uncompress(NULL) == 0, "NULL bottom (the SDK's cmp bottom, #0) does nothing", 0);
    /* corrupt streams stop instead of scribbling: a block with nothing to copy from */
    memset(img, 0, sizeof img);
    {
        uint32_t info = (8u << 24) | 11u, extra = 4u;   /* 3 stream bytes: flag 0x80, hi, lo */

        memcpy(img + 3, &info, 4);
        memcpy(img + 7, &extra, 4);
        img[2] = 0x80;
        img[1] = 0x00;
        img[0] = 0x00;
    }
    got = hwblz_uncompress(img + 11);
    check(got == 0, "a block as the first item (nothing written yet) is rejected", got);
    memset(img, 0, sizeof img);
    {
        uint32_t info = (4u << 24) | 11u;   /* footer length below 8 */

        memcpy(img + 3, &info, 4);
        got = hwblz_uncompress(img + 11);
        check(got == 0, "footer length < 8 is rejected", got);
    }
}

/* ---- bet 1.6b: the RTC chip, the input mapping and script, the ordered trace and the hit counters ------------------------------- */
static void test_rtc_chip(void)
{
    static const struct { int y; unsigned m, d; int64_t days; unsigned wk; } k[] = {   /* days since 2000-01-01 and weekday (Sunday 0), from Python's datetime */
        { 2000, 1, 1, 0, 6 }, { 2000, 2, 29, 59, 2 }, { 2000, 3, 1, 60, 3 }, { 2026, 10, 12, 9781, 1 }, { 2099, 12, 31, 36524, 4 }, { 2038, 1, 19, 13898, 2 },
    };
    unsigned i, bad = 0, bad_back = 0, bad_wk = 0;
    uint32_t dw, tw;
    int64_t secs, y, back;
    unsigned m, d;
    HwRtc r;
    uint8_t buf[HWRTC_BUF_BYTES];

    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (hwrtc_days_from_civil(k[i].y, k[i].m, k[i].d) != k[i].days) {
            bad++;
        }
        hwrtc_civil_from_days(k[i].days, &y, &m, &d);
        if (y != k[i].y || m != k[i].m || d != k[i].d) {
            bad_back++;
        }
        hwrtc_raw_from_secs(k[i].days * 86400 + 5, &dw, &tw);
        if ((dw >> 24) != k[i].wk) {
            bad_wk++;
        }
    }
    check(bad == 0, "days_from_civil matches Python's datetime (leap day, century end, 2038)", bad);
    check(bad_back == 0, "civil_from_days is its inverse", bad_back);
    check(bad_wk == 0, "weekday: 2000-01-01 Saturday, 2026-10-12 Monday (Sunday = 0)", bad_wk);
    /* 2026-10-12 13:45:30 -> BCD words */
    secs = 9781 * 86400 + 13 * 3600 + 45 * 60 + 30;
    hwrtc_raw_from_secs(secs, &dw, &tw);
    check(dw == (0x26u | (0x10u << 8) | (0x12u << 16) | (1u << 24)), "date word: year 26, month 10, day 12, week 1, BCD", dw);
    check(tw == (0x13u | 0x40u | (0x45u << 8) | (0x30u << 16)), "time word: 13 h (afternoon bit), 45 min, 30 s, BCD", tw);
    hwrtc_raw_from_secs(9781 * 86400 + 5 * 3600, &dw, &tw);
    check((tw & 0x7fu) == 0x05u, "05:00 has no afternoon bit", tw);
    check(hwrtc_secs_from_raw(0x26u | (0x10u << 8) | (0x12u << 16), tw | 0, &back) == 0 && back == 9781 * 86400 + 5 * 3600, "secs_from_raw inverts raw_from_secs", (long)back);
    check(hwrtc_secs_from_raw(0x2au, 0, &back) != 0, "an invalid BCD digit (0x2a) is refused", 0);
    check(hwrtc_secs_from_raw(0x26u | (0x13u << 8) | (1u << 16), 0, &back) != 0, "month 13 is refused", 0);
    /* the command set */
    hwrtc_init(&r, 60);
    memset(buf, 0, sizeof buf);
    check(hwrtc_command(&r, HWRTC_CMD_RESET, buf, 0) == HWRTC_RESULT_SUCCESS && r.status1 == 0x0002u, "RESET leaves the 24-hour format", r.status1);
    check(hwrtc_command(&r, HWRTC_CMD_READ_DT, buf, secs) == HWRTC_RESULT_SUCCESS, "READ_DATETIME succeeds", 0);
    memcpy(&dw, buf, 4);
    memcpy(&tw, buf + 4, 4);
    check((tw >> 16 & 0x7fu) == 0x30u, "READ_DATETIME fills the image (seconds 30)", tw);
    check((tw >> 8 & 0x7fu) == 0x46u, "the config offset (+60 s) moves the clock: now + 60 s = 13:46:30", tw);
    memset(buf, 0, sizeof buf);
    check(hwrtc_command(&r, HWRTC_CMD_READ_STAT1, buf, secs) == HWRTC_RESULT_SUCCESS && buf[0] == 0x02, "READ_STAT1 returns the status register", buf[0]);
    /* a time write moves the offset and keeps the date */
    memset(buf, 0, sizeof buf);
    buf[4] = 0x09; buf[5] = 0x30; buf[6] = 0x15;   /* 09:30:15 */
    check(hwrtc_command(&r, HWRTC_CMD_WRITE_TIME, buf, secs) == HWRTC_RESULT_SUCCESS, "WRITE_TIME succeeds", 0);
    memset(buf, 0, sizeof buf);
    (void)hwrtc_command(&r, HWRTC_CMD_READ_DT, buf, secs + 10);
    memcpy(&dw, buf, 4);
    memcpy(&tw, buf + 4, 4);
    check(dw == (0x26u | (0x10u << 8) | (0x12u << 16) | (1u << 24)) && (tw & 0x3fu) == 0x09u && ((tw >> 8) & 0x7fu) == 0x30u && ((tw >> 16) & 0x7fu) == 0x25u,
          "after WRITE_TIME the date is unchanged and the time runs from 09:30:15 (read 10 s later = 09:30:25)", tw);
    buf[4] = 0xff;   /* garbage BCD */
    check(hwrtc_command(&r, HWRTC_CMD_WRITE_TIME, buf, secs) == HWRTC_RESULT_INVALID && r.invalid == 1u, "a WRITE_TIME with invalid BCD is refused and counted", r.invalid);
    check(hwrtc_command(&r, 0x55u, buf, secs) == HWRTC_RESULT_INVALID, "an unknown command is refused", 0);
}

/* ---- bet 2.1e (Rule 8): rtc_fixed -- the epoch -> DS RTC conversion, and the clock that runs on DS vblanks ---------------------------------- */
static uint32_t bcd(unsigned v)
{
    return ((v / 10u) << 4) | (v % 10u);
}

static void test_rtc_fixed(void)
{
    static const struct { int64_t epoch; int64_t y; unsigned mo, d, h, mi, s, wk; } k[] = {   /* epochs and weekdays from Python's datetime (UTC) */
        { 1206187200, 2008, 3, 22, 12, 0, 0, 6 },    /* a spring Saturday noon */
        { 1237723200, 2009, 3, 22, 12, 0, 0, 0 },    /* the run.py / make default: Platinum's North American release day (2009-03-22), a Sunday */
        { 951868799, 2000, 2, 29, 23, 59, 59, 2 },   /* 2000 is a leap year (divisible by 400) */
        { 1204243200, 2008, 2, 29, 0, 0, 0, 5 },     /* 2008 is a leap year */
        { 1230767999, 2008, 12, 31, 23, 59, 59, 3 },
        { 946684800, 2000, 1, 1, 0, 0, 0, 6 },
        { 4102444799, 2099, 12, 31, 23, 59, 59, 4 },
        { 1614556800, 2021, 3, 1, 0, 0, 0, 1 },      /* the day after a non-leap February */
    };
    unsigned i, bad_f = 0, bad_w = 0;
    HwRtcFields f;
    HwCfg c;
    HwRtc r;
    uint8_t buf[HWRTC_BUF_BYTES];
    uint32_t dw, tw;
    int64_t s0;

    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        int64_t secs = hwrtc_fixed_secs(k[i].epoch, 0u);

        hwrtc_fields_from_secs(secs, &f);
        bad_f += !(f.year == k[i].y && f.mon == k[i].mo && f.day == k[i].d && f.hour == k[i].h && f.min == k[i].mi && f.sec == k[i].s && f.wday == k[i].wk);
        hwrtc_raw_from_secs(secs, &dw, &tw);
        /* the chip's BCD words: year (2 digits) | month << 8 | day << 16 | weekday << 24; time = hour (PM bit 0x40) | min << 8 | sec << 16 */
        bad_w += dw != (bcd((unsigned)(k[i].y % 100)) | (bcd(k[i].mo) << 8) | (bcd(k[i].d) << 16) | (k[i].wk << 24)) ||
                 tw != (bcd(k[i].h) | (k[i].h >= 12u ? 0x40u : 0u) | (bcd(k[i].mi) << 8) | (bcd(k[i].s) << 16));
    }
    check(bad_f == 0, "epoch -> y-m-d h:m:s + weekday for 8 dates (leap days 2000 and 2008, 2021-03-01, both ends of the range)", bad_f);
    check(bad_w == 0, "epoch -> the chip's BCD date / time words for the same dates", bad_w);
    /* 60 DS vblanks = 1 s, from the vblank count and nothing else */
    s0 = hwrtc_fixed_secs(1206187200, 0u);
    check(hwrtc_fixed_secs(1206187200, 59u) == s0 && hwrtc_fixed_secs(1206187200, 60u) == s0 + 1 && hwrtc_fixed_secs(1206187200, 3600u) == s0 + 60 &&
          hwrtc_fixed_secs(1206187200, 60u * 3600u * 12u) == s0 + 12 * 3600, "two reads N vblanks apart differ by N/60 s (59 -> 0 s, 60 -> 1 s, 3600 -> 1 min, 43200 s)", (long)(hwrtc_fixed_secs(1206187200, 59u) - s0));
    /* the roll-over the game would see: 2008-02-29 23:59:59 + 60 vblanks = 2008-03-01 00:00:00 */
    hwrtc_fields_from_secs(hwrtc_fixed_secs(1204329599, 60u), &f);
    check(f.year == 2008 && f.mon == 3 && f.day == 1 && f.hour == 0 && f.min == 0 && f.sec == 0, "23:59:59 on a leap day + 60 vblanks rolls to March 1", f.day);
    /* through the chip: a READ_DATETIME 1 s of vblanks later returns the next second, with no host clock anywhere */
    hwrtc_init(&r, 0);
    memset(buf, 0, sizeof buf);
    check(hwrtc_command(&r, HWRTC_CMD_READ_DT, buf, hwrtc_fixed_secs(1206187200, 0u)) == HWRTC_RESULT_SUCCESS, "READ_DT at vblank 0", 0);
    memcpy(&tw, buf + 4, 4);
    check((tw >> 16 & 0x7fu) == 0x00u && (tw & 0x3fu) == 0x12u, "the game reads 12:00:00 at vblank 0", tw);
    (void)hwrtc_command(&r, HWRTC_CMD_READ_DT, buf, hwrtc_fixed_secs(1206187200, 60u * 5u + 7u));
    memcpy(&tw, buf + 4, 4);
    check((tw >> 16 & 0x7fu) == 0x05u, "the game reads :05 after 307 vblanks", tw);
    /* the config key */
    hwcfg_defaults(&c);
    check(!c.rtc_fixed_set && c.rtc_fixed == 0, "rtc_fixed is unset by default", 0);
    check(hwcfg_parse(&c, "rtc_fixed = 1206187200\n", 23, NULL) == 1 && c.rtc_fixed_set == 1 && c.rtc_fixed == 1206187200, "rtc_fixed parses", (long)c.rtc_fixed);
    hwcfg_defaults(&c);
    check(hwcfg_parse(&c, "rtc_fixed = 5\nrtc_fixed = 4102444800\nrtc_fixed = -1\nrtc_fixed = abc\n", 61, NULL) == 0 && !c.rtc_fixed_set,
          "rtc_fixed outside 2000-01-01..2099-12-31 (or not a number) is skipped", c.rtc_fixed_set);
}

static void test_input_map(void)
{
    uint16_t x, y;

    check(hwin_keys_from_ctr(0x0001u) == HWIN_A && hwin_keys_from_ctr(0x0008u) == HWIN_START && hwin_keys_from_ctr(0x0800u) == HWIN_Y, "libctru A, START, Y map to the DS bits", 0);
    check(hwin_keys_from_ctr(1u << 29) == HWIN_LEFT && hwin_keys_from_ctr(1u << 31) == HWIN_DOWN && hwin_keys_from_ctr(1u << 28) == HWIN_RIGHT && hwin_keys_from_ctr(1u << 30) == HWIN_UP,
          "the circle pad becomes the d-pad", 0);
    check(hwin_keys_from_ctr(0xffffffffu) == HWIN_ALL, "unknown 3DS bits (ZL, ZR, touch...) are dropped", hwin_keys_from_ctr(0xffffffffu));
    check(hwin_keyinput(0) == 0x03ffu && hwin_keyinput(HWIN_A) == 0x03feu && hwin_keyinput(HWIN_ALL) == 0x0000u && hwin_keyinput(HWIN_X) == 0x03ffu,
          "KEYINPUT is active low over bits 0-9 (X and Y are not in it)", hwin_keyinput(HWIN_A));
    check(hwin_xybuf(0) == 0x2c00u && hwin_xybuf(HWIN_X) == 0x2800u && hwin_xybuf(HWIN_Y) == 0x2400u && hwin_xybuf(HWIN_X | HWIN_Y) == 0x2000u,
          "the XY word is 0x2c00 released, X clears bit 10, Y bit 11", hwin_xybuf(HWIN_X));
    hwin_touch_from_ctr(160, 120, &x, &y);
    check(x == 128 && y == 96, "the 3DS touch centre is the DS screen centre", x * 1000 + y);
    hwin_touch_from_ctr(32, 24, &x, &y);
    check(x == 0 && y == 0, "the 3DS point (32,24) is the DS origin", x);
    hwin_touch_from_ctr(0, 0, &x, &y);
    check(x == 0 && y == 0, "a touch left/above the DS area clamps to 0,0", x);
    hwin_touch_from_ctr(319, 239, &x, &y);
    check(x == 255 && y == 191, "a touch right/below the DS area clamps to 255,191", x * 1000 + y);
    check(hwin_raw_from_px(0) == 8 && hwin_raw_from_px(255) == 4088, "raw = pixel * 16 + 8", hwin_raw_from_px(255));
}

static void test_input_script(void)
{
    static const char good[] = "# a comment\n0 NONE\n\n120 A\n  200 START+T128,96  \n260 0x30 B\n300 NONE\n";
    static const char desc[] = "10 A\n5 B\n";
    static const char badname[] = "10 BOGUS\n";
    static const char badpen[] = "10 T300,10\n";
    static const char nofr[] = "A\n";
    HwInScript sc;
    uint32_t bad = 99;
    HwInState st;
    int n;

    n = hwin_script_parse(&sc, good, (uint32_t)strlen(good), &bad);
    check(n == 5, "a 5-line script parses (comments and blank lines skipped)", n);
    st = hwin_script_at(&sc, 0);
    check(st.keys == 0 && !st.touch, "frame 0: nothing held", st.keys);
    st = hwin_script_at(&sc, 119);
    check(st.keys == 0, "frame 119: still nothing (the line applies from its frame)", st.keys);
    st = hwin_script_at(&sc, 120);
    check(st.keys == HWIN_A && !st.touch, "frame 120: A", st.keys);
    st = hwin_script_at(&sc, 199);
    check(st.keys == HWIN_A, "frame 199: A is held until the next line", st.keys);
    st = hwin_script_at(&sc, 200);
    check(st.keys == HWIN_START && st.touch && st.x == 128 && st.y == 96, "frame 200: START and the pen at 128,96", st.keys);
    st = hwin_script_at(&sc, 260);
    check(st.keys == (0x30u | HWIN_B) && !st.touch, "frame 260: a hex mask OR a name (0x30 | B)", st.keys);
    st = hwin_script_at(&sc, 100000);
    check(st.keys == 0, "after the last line: released", st.keys);
    check(hwin_script_parse(&sc, desc, (uint32_t)strlen(desc), &bad) == -1 && bad == 2u, "frames out of order are refused at the offending line", bad);
    check(hwin_script_parse(&sc, badname, (uint32_t)strlen(badname), &bad) == -1 && bad == 1u, "an unknown key name is refused", bad);
    check(hwin_script_parse(&sc, badpen, (uint32_t)strlen(badpen), &bad) == -1, "a pen outside 256x192 is refused", 0);
    check(hwin_script_parse(&sc, nofr, (uint32_t)strlen(nofr), &bad) == -1, "a line without a frame number is refused", 0);
    check(hwin_script_parse(&sc, "", 0, &bad) == 0 && hwin_script_at(&sc, 5).keys == 0, "an empty script is valid and holds nothing", 0);
}

static char g_sink_lines[8][128];
static unsigned g_sink_n;
static void sink(const char *line)
{
    if (g_sink_n < 8u) {
        strncpy(g_sink_lines[g_sink_n], line, 127);
    }
    g_sink_n++;
}

static void count_site(unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        HWCOUNT("test site A");
    }
}

static void test_trace_and_counts(void)
{
    char out[1024];
    uint32_t c0 = hwtrace_count();
    size_t len;

    hwtrace("dropped %d", 1);   /* no sink yet */
    check(hwtrace_count() == c0 + 1u && g_sink_n == 0u, "a trace line with no sink is dropped but counted", g_sink_n);
    hwtrace_set_sink(sink);
    hwtrace("ovl: LOAD %u %s", 77u, "game_opening");
    hwtrace("second");
    check(g_sink_n == 2u && strcmp(g_sink_lines[0], "ovl: LOAD 77 game_opening") == 0 && strcmp(g_sink_lines[1], "second") == 0, "trace lines reach the sink in order, formatted", g_sink_n);
    hwtrace_set_sink(NULL);
    count_site(3);
    HWCOUNT("test site B");
    count_site(2);
    len = hwcount_format(out, sizeof out);
    check(len > 0 && strstr(out, "test site A\t5\t") != NULL, "a site's hits accumulate across calls (3 + 2 = 5)", 0);
    check(strstr(out, "test site B\t1\t") != NULL && strstr(out, "test site A") < strstr(out, "test site B"), "sites are listed sorted by name, each with its file:line", 0);
}

int main(void)
{
    test_time();
    test_timer_basic();
    test_timer_modes();
    test_timer_cascade_and_reload();
    test_timer_next_irq();
    test_pump_vblank();
    test_pump_gating();
    test_pump_order_and_cap();
    test_pump_timer_and_overrun();
    test_irq_unmasked_at_boot();
    test_wait_irq_vblank();
    test_timer_rearm_between_pumps();
    test_dma();
    test_cp();
    test_pxi();
    test_blz();
    test_ctx_pingpong();
    test_ctx_limits();
    test_log_sites();
    test_log_files();
    test_cfg();
    test_wmq();
    test_hashes();
    test_hmac();
    test_rtc_chip();
    test_rtc_fixed();
    test_input_map();
    test_input_script();
    test_trace_and_counts();
    printf("hw_os_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
