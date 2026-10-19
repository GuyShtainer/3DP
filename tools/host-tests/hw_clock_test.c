/* hw_clock_test.c -- bet 2.2a: the frame-driven virtual clock (config.ini `clock = frame`) over the pure pump with a fake host. Checks: N waits = N vblanks and
 * virt == N * FRAME whatever the host clock says; a non-waiting yield never raises a vblank; a spin advances 4 sys ticks per cycle and fires a timer due inside the
 * frame; a spin that would cross the vblank is clamped, and only an exhausted budget forces one (counted); the sleep branch jumps without sleeping; OS_GetTick polls
 * make progress; steal is a no-op; two runs with different host clocks are bit-identical; WALL behaviour is unchanged. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw_pure.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long v)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL: %s (%ld)\n", what, v);
    }
}

static uint32_t r32(const uint8_t *io, uint32_t off) { uint32_t v; memcpy(&v, io + off, 4); return v; }
static void w32(uint8_t *io, uint32_t off, uint32_t v) { memcpy(io + off, &v, 4); }
static void w16(uint8_t *io, uint32_t off, uint16_t v) { memcpy(io + off, &v, 2); }

typedef struct Host {
    uint64_t now;     /* the fake host clock: set by the test, advanced by nothing unless noted */
    uint64_t skew;    /* added to every host read: a host that runs far ahead / behind */
    int waits, sleeps, disp;
    int last_irq;
} Host;

static uint64_t h_tick(void *u) { Host *h = (Host *)u; return h->now + h->skew; }
static void h_wait(void *u) { ((Host *)u)->waits++; }   /* the host clock is NOT advanced: FRAME must not care */
static void h_sleep(void *u, uint64_t sys) { Host *h = (Host *)u; (void)sys; h->sleeps++; }
static void h_disp(void *u, int irq) { Host *h = (Host *)u; h->disp++; h->last_irq = irq; }
static int h_masked(void *u) { (void)u; return 0; }

static void setup(HwPump *p, Host *h, uint8_t *io, uint32_t *vb, HwClock clk)
{
    HwPumpOps ops = { h_tick, h_wait, h_sleep, NULL, h_disp, h_masked, h, NULL, NULL };

    memset(io, 0, 0x1000);
    *vb = 0;
    hwpump_init(p, &ops, io, vb);
    hwpump_set_clock(p, clk);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, 1);
    w16(io, HWR_DISPSTAT, 8);
}

static void test_waits(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;
    int i;

    memset(&h, 0, sizeof h);
    h.skew = 1000u * HWT_FRAME_SYS;   /* the host is 1000 frames ahead: irrelevant in FRAME */
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    for (i = 0; i < 25; i++) {
        hwpump_run(&p, HWP_WAIT);
    }
    check(vb == 25u && p.vblanks == 25u && h.waits == 25, "25 waits = 25 vblanks = 25 host waits", (long)vb);
    check(p.time.virt == 25u * (uint64_t)HWT_FRAME_SYS, "virt == 25 * HWT_FRAME_SYS exactly", (long)p.time.virt);
    check(p.overruns == 0u && p.forced_vblanks == 0u && h.sleeps == 0, "no overrun, no forced vblank, no host sleep", (long)p.overruns);
    check(p.next_vblank == 26u * (uint64_t)HWT_FRAME_SYS, "next_vblank one frame ahead", 0);
    check(hwtime_os_tick(&p.time) == 25u * (uint64_t)HWT_FRAME_SYS / HWT_SYS_PER_TICK, "OS tick follows virt", (long)hwtime_os_tick(&p.time));
    check(h.disp == 25, "vblank IRQ dispatched once per wait", h.disp);
}

static void test_nowait(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;
    int i;

    memset(&h, 0, sizeof h);
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    h.now = 100u * (uint64_t)HWT_FRAME_SYS;   /* the host clock is a hundred frames ahead */
    for (i = 0; i < 10; i++) {
        hwpump_run(&p, HWP_NOWAIT);
        hwpump_sync(&p);
        hwpump_run(&p, HWP_DISPATCH_ONLY);
    }
    check(vb == 0u && p.vblanks == 0u && h.disp == 0, "NOWAIT / sync / DISPATCH_ONLY with a host far ahead raise no vblank", (long)vb);
    check(p.time.virt == 0u, "virt did not move", (long)p.time.virt);
    {
        uint64_t base = p.time.real_base;
        hwtime_steal(&p.time, 777u);
        check(p.time.real_base == base, "steal leaves real_base alone", (long)p.time.real_base);
    }
}

static void test_spin_timer(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;

    memset(&h, 0, sizeof h);
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    w32(io, HWR_IE, 1u << 3);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 100));   /* 100 bus ticks = 800 sys ticks = 200 ARM9 cycles */
    w16(io, HWR_TM0CNT_L + 2, 0x80u | 0x40u);
    hwpump_run(&p, HWP_NOWAIT);   /* latch the enable edge at t = 0 */
    hwpump_spin(&p, 100);
    check(p.time.virt == 400u && h.disp == 0, "a spin of 100 cycles advances 400 sys ticks, timer not yet due", (long)p.time.virt);
    hwpump_spin(&p, 100);
    check(p.time.virt == 800u && h.disp == 1 && h.last_irq == 3, "the second spin reaches the timer's due time: IRQ 3 dispatched", h.disp);
    check(vb == 0u, "no vblank inside the frame", (long)vb);
}

static void test_spin_budget(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;
    const uint32_t big = 1500000u;   /* ARM9 cycles = 6 000 000 sys ticks: more than a frame, less than the budget */
    long polls = 0;

    memset(&h, 0, sizeof h);
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    hwpump_spin(&p, big);
    check(vb == 0u && p.time.virt == HWT_FRAME_SYS - 1u && p.forced_vblanks == 0u, "a spin crossing the vblank is clamped below it, nothing raised", (long)p.time.virt);
    hwpump_spin(&p, 1000u);
    check(vb == 0u && p.time.virt == HWT_FRAME_SYS - 1u, "further spins stay clamped while the budget lasts", (long)p.time.virt);
    hwpump_spin(&p, big);
    check(vb == 1u && p.vblanks == 1u && p.forced_vblanks == 1u, "an exhausted budget (>= 2 frames of spun cycles) forces exactly one vblank", (long)p.forced_vblanks);
    check(p.time.virt == HWT_FRAME_SYS && p.next_vblank == 2u * (uint64_t)HWT_FRAME_SYS && p.frame_spin == 0u, "forced vblank: virt at the boundary, budget reset", (long)p.frame_spin);
    check(h.disp == 1 && p.overruns == 0u, "the forced vblank's IRQ is dispatched, no wall-style overrun", h.disp);
    /* OS_GetTick polls: a tick-delta busy wait must end */
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    {
        uint64_t t0 = hwpump_poll(&p), t;

        do {
            t = hwpump_poll(&p);
            polls++;
        } while (t - t0 < 3u * (uint64_t)HWT_FRAME_SYS && polls < 200000);
        check(polls < 200000, "a poll loop waiting 3 frames of ticks terminates", polls);
        check(p.forced_vblanks >= 1u && p.vblanks >= 1u, "it terminated through forced vblanks (counted)", (long)p.forced_vblanks);
        check(polls > 3L * (long)(HWT_FRAME_SYS / (HWT_POLL_CYCLES * HWT_SYS_PER_CYCLE)) / 2, "polls only advance time by their own cost (clamped inside a frame): more than 1.5 frames of polls", polls);
    }
}

static void test_sleep_branch(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;

    memset(&h, 0, sizeof h);
    setup(&p, &h, io, &vb, HWCLOCK_FRAME);
    w32(io, HWR_IE, 1u << 3);
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 100));
    w16(io, HWR_TM0CNT_L + 2, 0x80u | 0x40u);
    hwpump_run(&p, HWP_NOWAIT);
    hwpump_run(&p, HWP_WAIT);
    check(h.sleeps == 0 && h.waits == 0, "WAIT with a timer before the vblank: no host sleep, no host wait", h.sleeps);
    check(p.time.virt == 800u && h.disp == 1 && h.last_irq == 3 && vb == 0u, "virt jumped to the timer's due time, IRQ 3 dispatched, no vblank", (long)p.time.virt);
}

static void run_script(HwPump *p, Host *h, uint8_t *io, uint32_t *vb, uint64_t skew)
{
    int i;

    h->skew = skew;
    setup(p, h, io, vb, HWCLOCK_FRAME);
    w32(io, HWR_IE, 1u | (1u << 3));
    w16(io, HWR_TM0CNT_L, (uint16_t)(0x10000 - 3000));
    w16(io, HWR_TM0CNT_L + 2, 0x80u | 0x40u | 1u);   /* prescaler 64 */
    for (i = 0; i < 200; i++) {
        h->now += (uint64_t)(i * 7919) % 3000000u;   /* a jittery host */
        hwpump_run(p, HWP_NOWAIT);
        hwpump_spin(p, 1000u + (uint32_t)i * 37u);
        (void)hwpump_poll(p);
        hwpump_run(p, HWP_WAIT);
        if (i % 9 == 0) {
            hwpump_spin(p, 3000000u);   /* forces a vblank on a later iteration */
        }
    }
}

static void test_determinism(void)
{
    static uint8_t ioa[0x1000], iob[0x1000];
    HwPump pa, pb;
    Host ha, hb;
    uint32_t va, vb;

    memset(&ha, 0, sizeof ha);
    memset(&hb, 0, sizeof hb);
    run_script(&pa, &ha, ioa, &va, 0u);
    run_script(&pb, &hb, iob, &vb, 987654321ull);
    check(pa.time.virt == pb.time.virt && va == vb && pa.dispatched == pb.dispatched && pa.forced_vblanks == pb.forced_vblanks, "two runs, different host clocks: identical virt / vblanks / dispatches / forced", (long)va);
    check(ha.disp == hb.disp && r32(ioa, HWR_IF) == r32(iob, HWR_IF) && pa.next_vblank == pb.next_vblank, "... and identical IRQ counts and next vblank", ha.disp);
    check(pa.overruns == 0u && pb.overruns == 0u, "FRAME never counts a wall overrun", 0);
}

static void test_wall_unchanged(void)
{
    static uint8_t io[0x1000];
    HwPump p;
    Host h;
    uint32_t vb;

    memset(&h, 0, sizeof h);
    setup(&p, &h, io, &vb, HWCLOCK_WALL);
    h.now = 5u * HWT_FRAME_SYS + 123u;
    hwpump_run(&p, HWP_NOWAIT);
    check(vb == 1u && p.overruns == 1u, "WALL: a late host raises the vblank at a NOWAIT yield and counts the overrun", (long)p.overruns);
    check(hwpump_poll(&p) == p.time.virt && p.forced_vblanks == 0u, "WALL: hwpump_poll is hwtime_update", 0);
    {
        uint64_t base = p.time.real_base;
        hwtime_steal(&p.time, 50u);
        check(p.time.real_base == base + 50u, "WALL: steal moves real_base", (long)p.time.real_base);
    }
    hwpump_spin(&p, 100u);
    check(p.forced_vblanks == 0u, "WALL: spins never force", 0);
    /* switching to FRAME after a vblank keeps virtual time */
    {
        uint64_t v = p.time.virt;
        hwpump_set_clock(&p, HWCLOCK_FRAME);
        check(p.time.virt == v && p.time.frame == 1u, "set_clock(FRAME) after a vblank does not rewind virt", (long)p.time.virt);
    }
}

int main(void)
{
    test_waits();
    test_nowait();
    test_spin_timer();
    test_spin_budget();
    test_sleep_branch();
    test_determinism();
    test_wall_unchanged();
    printf("hw_clock_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
