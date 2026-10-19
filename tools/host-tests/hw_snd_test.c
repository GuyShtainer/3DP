/* hw_snd_test.c -- bet 1.5, host half: the mailbox and the cadence of source/hw/hw_snd.c, and its wiring into the loopback ARM7 (hw_pxi_arm7.c).
 * The ARM7 driver is a fake (HwSndOps) that logs what the slice asks of it; the real driver over this unit is the private emulator gate `emu-snd-init`.
 * Native, one thread, no SDK, no libctru, -Wall -Wextra -Werror. Prints "hw_snd_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_log.h"
#include "hw_pure.h"

/* hw_snd.c / hw_pxi_arm7.c count their sites through hw_log.c; this test links without it */
void hwcount_hit(HwCountSite *site) { (void)site; }
void hwcount_add(HwCountSite *site, uint32_t n) { (void)site; (void)n; }

#define IO_BYTES 0x1000u
#define P        HWSND_PERIOD_SYS

static long g_checks, g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

/* ---- the fake ARM7 driver ------------------------------------------------------------------------------------------------------ */
typedef struct Fake {
    uint32_t boots;
    uint32_t recv_log[64];
    uint32_t recvs;
    uint32_t pending_wakes;        /* what the ARM7 PXI callback queued for the thread */
    uint32_t pending_cmds;         /* command lists queued for SND_CommandProc */
    uint32_t cmds_done;            /* command lists SND_CommandProc consumed */
    uint32_t runs_wake, runs_periodic;
    uint32_t order[64];            /* 1 = wake pass, 2 = periodic pass, in call order */
    uint32_t norder;
    uint32_t spu_write;            /* if non-zero, each pass writes this value to SOUND0CNT */
    HwSnd *reenter;                /* if set, a pass calls hwsnd_pump on it (the driver's OS call re-entering a pump) */
    uint64_t reenter_now;
    uint8_t *io;
    uint32_t depth, max_depth, enters, leaves;   /* the slice's enter/leave bracket (HWOS_Arm7Enter/Leave in the real glue) */
    uint32_t outside;              /* boot/recv/run calls made while depth == 0: must stay 0 */
} Fake;

static void fk_enter(void *u)
{
    Fake *f = (Fake *)u;

    f->depth++;
    f->enters++;
    if (f->depth > f->max_depth) {
        f->max_depth = f->depth;
    }
}

static void fk_leave(void *u)
{
    Fake *f = (Fake *)u;

    assert(f->depth > 0u);
    f->depth--;
    f->leaves++;
}

static void fk_boot(void *u)
{
    Fake *f = (Fake *)u;

    f->boots++;
    f->outside += f->depth == 0u;
    memcpy(f->io + HWSND_SPU_OFF + 0x100u, &(uint32_t){ 0x80007fu }, 4);   /* SOUNDCNT/MAIN: the driver's start-up writes */
}

static void fk_recv(void *u, uint32_t data, unsigned err)
{
    Fake *f = (Fake *)u;

    assert(err == 0u);
    f->outside += f->depth == 0u;
    if (f->recvs < 64u) {
        f->recv_log[f->recvs] = data;
    }
    f->recvs++;
    if (data == 0u) {
        f->pending_wakes++;
    } else {
        f->pending_cmds++;
    }
}

static int fk_run(void *u, int periodic)
{
    Fake *f = (Fake *)u;
    int woke = f->pending_wakes > 0u;

    f->outside += f->depth == 0u;
    if (woke) {
        f->pending_wakes--;
    }
    if (!woke && !periodic) {
        return 0;
    }
    f->cmds_done += f->pending_cmds;   /* SND_CommandProc drains the whole command queue every pass */
    f->pending_cmds = 0u;
    if (periodic) {
        f->runs_periodic++;
    } else {
        f->runs_wake++;
    }
    if (f->norder < 64u) {
        f->order[f->norder++] = periodic ? 2u : 1u;
    }
    if (f->spu_write != 0u) {
        memcpy(f->io + HWSND_SPU_OFF, &f->spu_write, 4);
    }
    if (f->reenter != NULL) {
        hwsnd_pump(f->reenter, f->reenter_now);
    }
    return 1;
}

static uint8_t g_io[IO_BYTES];
static HwPxi g_pxi;
static HwSnd g_snd;
static Fake g_fake;

static void fresh(void)
{
    HwSndOps ops;

    memset(g_io, 0, sizeof g_io);
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.io = g_io;
    ops.enter = fk_enter;
    ops.leave = fk_leave;
    ops.boot = fk_boot;
    ops.recv = fk_recv;
    ops.run = fk_run;
    ops.user = &g_fake;
    hwpxi_init(&g_pxi, g_io);
    hwsnd_init(&g_snd, &ops, g_io, &g_pxi);
}

/* ---- cases --------------------------------------------------------------------------------------------------------------------- */
static void test_constants(void)
{
    check(HWSND_PERIOD_TICKS == 0xAA8u && HWSND_PERIOD_TICKS == 2728u, "SND_PROC_INTERVAL is 0xAA8 = 2728 OS ticks", (long)HWSND_PERIOD_TICKS);
    check(HWSND_PERIOD_SYS == 1396736u, "...= 1 396 736 sys ticks", (long)HWSND_PERIOD_SYS);
    check((uint64_t)HWSND_PERIOD_SYS * 1000000u / HWT_SYS_HZ == 5209u, "...= 5.209 ms (floor, microseconds)", (long)((uint64_t)HWSND_PERIOD_SYS * 1000000u / HWT_SYS_HZ));
}

static void test_boot_and_mail(void)
{
    fresh();
    hwsnd_send(&g_snd, 0x00123456u, 0u);
    hwsnd_send(&g_snd, 0u, 0u);
    hwsnd_send(&g_snd, 0x00654321u, 0u);
    check(g_fake.recvs == 0u && g_snd.mail_count == 3u, "words are queued, not run, at send time", (long)g_fake.recvs);
    check(g_snd.cmd_words == 2u && g_snd.wake_words == 1u, "pointer words and wake-ups are counted apart", (long)g_snd.cmd_words);
    hwsnd_pump(&g_snd, 1000u);
    check(g_fake.boots == 1u, "the driver boots at the first slice", (long)g_fake.boots);
    check(g_fake.recvs == 3u && g_fake.recv_log[0] == 0x123456u && g_fake.recv_log[1] == 0u && g_fake.recv_log[2] == 0x654321u,
          "all three words reach the ARM7 callback in order", (long)g_fake.recvs);
    check(g_snd.mail_count == 0u, "the mailbox is empty after the slice", (long)g_snd.mail_count);
    check(g_fake.runs_wake == 1u && g_fake.cmds_done == 2u, "the wake-up pass ran once and processed both command lists", (long)g_fake.cmds_done);
    hwsnd_pump(&g_snd, 2000u);
    check(g_fake.boots == 1u && g_fake.runs_wake == 1u && g_fake.runs_periodic == 0u, "a second slice without mail or a period does nothing", (long)g_fake.runs_wake);
    hwsnd_send(&g_snd, 0x11u, 1u);
    check(g_snd.mail_count == 0u, "a word with the error flag is not a command", (long)g_snd.mail_count);
}

static void test_cadence(void)
{
    fresh();
    hwsnd_pump(&g_snd, 5000u);   /* boot: anchor = 5000 */
    check(g_snd.last_sys == 5000u && g_fake.boots == 1u, "the cadence anchors at the boot slice", (long)g_snd.last_sys);
    hwsnd_pump(&g_snd, 5000u + P - 1u);
    check(g_fake.runs_periodic == 0u, "one sys tick short of a period: no step", (long)g_fake.runs_periodic);
    hwsnd_pump(&g_snd, 5000u + P);
    check(g_fake.runs_periodic == 1u && g_snd.catchup_steps == 0u, "exactly one period: one step, no catch-up", (long)g_fake.runs_periodic);
    check(g_snd.last_sys == 5000u + P, "the anchor advances by one period, not to `now`", (long)g_snd.last_sys);
    hwsnd_pump(&g_snd, 5000u + P + P / 2u);
    hwsnd_pump(&g_snd, 5000u + 3u * P + 7u);   /* 2 more periods are due (boundaries 2P and 3P) */
    check(g_fake.runs_periodic == 3u && g_snd.catchup_steps == 1u, "two periods late in one slice: two steps, the second is a catch-up", (long)g_snd.catchup_steps);
    check(g_snd.last_sys == 5000u + 3u * P, "...and the anchor sits on the last accounted boundary (the remainder is kept)", (long)g_snd.last_sys);
    hwsnd_pump(&g_snd, 5000u + 4u * P);
    check(g_fake.runs_periodic == 4u, "the kept remainder completes the next period on time", (long)g_fake.runs_periodic);
    check(g_snd.overruns == 0u && g_snd.periodic_steps == 4u && g_snd.steps == 4u, "no overrun so far; counters agree", (long)g_snd.steps);
}

static void test_catchup_cap(void)
{
    uint64_t t0 = 100u;

    fresh();
    hwsnd_pump(&g_snd, t0);
    hwsnd_pump(&g_snd, t0 + HWSND_CATCHUP_MAX * P);   /* exactly the cap: all caught up, no overrun */
    check(g_fake.runs_periodic == HWSND_CATCHUP_MAX && g_snd.overruns == 0u, "a backlog of exactly 8 periods is caught up in full", (long)g_fake.runs_periodic);
    check(g_snd.catchup_steps == HWSND_CATCHUP_MAX - 1u, "...7 of the 8 steps are catch-up steps", (long)g_snd.catchup_steps);
    hwsnd_pump(&g_snd, t0 + HWSND_CATCHUP_MAX * P + 20u * P + 3u);   /* 20 periods behind */
    check(g_fake.runs_periodic == 2u * HWSND_CATCHUP_MAX && g_snd.overruns == 1u, "20 periods behind: 8 steps run, one overrun counted", (long)g_snd.overruns);
    check(g_snd.last_sys == t0 + HWSND_CATCHUP_MAX * P + 20u * P + 3u, "...the dropped backlog re-anchors the cadence at `now`", (long)g_snd.last_sys);
    hwsnd_pump(&g_snd, g_snd.last_sys + P);
    check(g_fake.runs_periodic == 2u * HWSND_CATCHUP_MAX + 1u && g_snd.overruns == 1u, "after the overrun the cadence is one step per period again", (long)g_fake.runs_periodic);
}

static void test_order_and_reentry(void)
{
    fresh();
    hwsnd_pump(&g_snd, 0u);
    hwsnd_send(&g_snd, 0u, 0u);
    hwsnd_pump(&g_snd, 2u * P);
    check(g_fake.norder == 3u && g_fake.order[0] == 1u && g_fake.order[1] == 2u && g_fake.order[2] == 2u,
          "within a slice the wake-up pass runs before the periodic passes", (long)g_fake.norder);
    fresh();
    g_fake.reenter = &g_snd;
    g_fake.reenter_now = 99u * P;
    hwsnd_pump(&g_snd, 0u);
    hwsnd_pump(&g_snd, P);
    check(g_snd.reentry == 1u && g_fake.runs_periodic == 1u, "a pump called from inside a pass is refused and counted; no recursion", (long)g_snd.reentry);
    check(g_snd.in_pump == 0u, "...and the guard is released", (long)g_snd.in_pump);
}

static void test_registers(void)
{
    uint32_t v = 0x8000007fu;

    fresh();
    hwsnd_pump(&g_snd, 0u);   /* boot wrote one word (fk_boot) */
    check(g_snd.spu_words_changed == 1u && g_snd.spu_slices_changed == 1u, "the boot's register write is counted (diff of the SPU window)", (long)g_snd.spu_words_changed);
    g_fake.spu_write = v;
    hwsnd_pump(&g_snd, P);
    check(g_snd.spu_words_changed == 2u, "a pass that stores a new value into SOUND0CNT is counted", (long)g_snd.spu_words_changed);
    hwsnd_pump(&g_snd, 2u * P);
    check(g_snd.spu_words_changed == 2u, "rewriting the same value is not (documented limit of the diff)", (long)g_snd.spu_words_changed);
    {
        uint32_t back;

        memcpy(&back, g_io + HWSND_SPU_OFF, 4);
        check(back == v, "the stored register reads back", (long)back);
    }
    g_io[0x3ffu] = 0x55u;   /* below the window */
    g_io[0x520u] = 0x55u;   /* above it */
    hwsnd_pump(&g_snd, 3u * P);
    check(g_snd.spu_words_changed == 2u, "bytes just outside the 0x400..0x51f window are not counted", (long)g_snd.spu_words_changed);
}

static void test_reply(void)
{
    uint32_t i;
    int rc = 0;

    fresh();
    check(hwsnd_reply(&g_snd, 0x0101u) == 0 && g_snd.replies == 1u, "an ARM7 alarm word is queued for the ARM9", (long)g_snd.replies);
    check(g_pxi.count == 1u && (g_pxi.recv[0] & 31u) == HWPXI_TAG_SOUND && (g_pxi.recv[0] >> 6) == 0x0101u, "...on the SOUND tag with its data", (long)g_pxi.recv[0]);
    for (i = 1; i < HWPXI_FIFO_DEPTH; i++) {
        rc |= hwsnd_reply(&g_snd, 0x200u + i);
    }
    check(rc == 0 && g_pxi.count == HWPXI_FIFO_DEPTH && g_snd.reply_deferred == 0u, "the ARM9 FIFO fills without a deferral", (long)g_snd.reply_deferred);
    /* F1: snd_alarm.c:120 spins `while (PXI_SendWordByFifo(...) < 0)` inside the ARM9 timer IRQ; a full FIFO must never be reported as -1 */
    for (i = 0; i < 5u; i++) {
        rc |= hwsnd_reply(&g_snd, 0x300u + i);
    }
    check(rc == 0, "a reply into a FULL ARM9 FIFO returns 0 (never -1: the alarm handler would spin forever)", rc);
    check(g_snd.reply_deferred == 5u && g_snd.reply_count == 5u && g_snd.reply_overflow == 0u, "...the five words wait in the ARM7-side ring, nothing is lost", (long)g_snd.reply_count);
    hwsnd_pump(&g_snd, 0u);
    check(g_snd.reply_count == 5u, "a slice with the FIFO still full keeps them (no spin, no drop)", (long)g_snd.reply_count);
    for (i = 0; i < 3u; i++) {
        (void)hwpxi_recv(&g_pxi);   /* the ARM9 takes three words */
    }
    hwsnd_pump(&g_snd, 1u);
    check(g_snd.reply_count == 2u && g_pxi.count == HWPXI_FIFO_DEPTH, "the next slice drains as many as fit, oldest first", (long)g_snd.reply_count);
    for (i = 0; i < HWPXI_FIFO_DEPTH; i++) {
        (void)hwpxi_recv(&g_pxi);
    }
    hwsnd_pump(&g_snd, 2u);
    check(g_snd.reply_count == 0u && g_pxi.count == 2u, "...and the rest when the ARM9 has drained", (long)g_pxi.count);
    check((g_pxi.recv[g_pxi.head] >> 6) == 0x303u && (g_pxi.recv[(g_pxi.head + 1u) % HWPXI_FIFO_DEPTH] >> 6) == 0x304u, "...in the order they were sent", (long)g_pxi.recv[g_pxi.head]);
}

static void test_reset(void)
{
    fresh();
    hwsnd_send(&g_snd, 0u, 0u);
    hwsnd_pump(&g_snd, 3u * P);
    check(g_snd.slices == 1u && g_fake.boots == 1u, "(precondition) a slice ran", (long)g_snd.slices);
    {
        HwSndOps ops = g_snd.ops;

        hwsnd_init(&g_snd, &ops, g_io, &g_pxi);
    }
    check(g_snd.slices == 0u && g_snd.steps == 0u && g_snd.mail_count == 0u && g_snd.booted == 0u, "hwsnd_init (a soft reset) zeroes counters, mailbox and boot state", (long)g_snd.slices);
    hwsnd_pump(&g_snd, 7u);
    check(g_fake.boots == 2u, "...and the driver boots again at the next slice", (long)g_fake.boots);
    hwsnd_pump(&g_snd, 3u);   /* the pump clock restarted below the anchor */
    check(g_snd.rebases == 1u && g_snd.last_sys == 3u, "time going backwards re-anchors instead of underflowing", (long)g_snd.rebases);
    hwsnd_pump(&g_snd, 3u + P);
    check(g_fake.runs_periodic == 1u, "...and the cadence resumes", (long)g_fake.runs_periodic);
}

static void test_mail_overflow(void)
{
    uint32_t i;

    fresh();
    for (i = 0; i < HWSND_MAIL_DEPTH; i++) {
        hwsnd_send(&g_snd, 0x1000u + i, 0u);
    }
    check(g_snd.mail_count == HWSND_MAIL_DEPTH && g_snd.mail_forced == 0u, "the ring holds HWSND_MAIL_DEPTH words", (long)g_snd.mail_count);
    hwsnd_send(&g_snd, 0x2000u, 0u);
    check(g_snd.mail_forced == 1u && g_fake.recvs == HWSND_MAIL_DEPTH && g_snd.mail_count == 1u, "one more forces a drain: nothing is lost", (long)g_fake.recvs);
    check(g_fake.recv_log[0] == 0x1000u && g_fake.recv_log[HWSND_MAIL_DEPTH - 1u] == 0x1000u + HWSND_MAIL_DEPTH - 1u, "...in order", (long)g_fake.recv_log[0]);
}

/* F5: wake words coalesce, a command-list pointer is never dropped */
static void test_wake_coalesce_and_no_drop(void)
{
    uint32_t i;

    fresh();
    for (i = 0; i < 100u; i++) {
        hwsnd_send(&g_snd, 0u, 0u);
    }
    check(g_snd.mail_count == 1u && g_snd.wake_words == 1u && g_snd.wake_coalesced == 99u, "100 wake-ups queue as one (a wake is a flag)", (long)g_snd.wake_coalesced);
    for (i = 0; i < 8u; i++) {
        hwsnd_send(&g_snd, 0x4000u + i, 0u);
        hwsnd_send(&g_snd, 0u, 0u);   /* SND_WaitForCommandProc after every flush */
    }
    check(g_snd.mail_count == 9u && g_snd.cmd_words == 8u && g_snd.mail_forced == 0u, "8 command lists + their wake-ups stay within 9 queued words", (long)g_snd.mail_count);
    hwsnd_pump(&g_snd, 0u);
    check(g_fake.recvs == 9u && g_fake.recv_log[0] == 0u && g_fake.recv_log[1] == 0x4000u && g_fake.recv_log[8] == 0x4007u, "every pointer word reaches the ARM7, in order", (long)g_fake.recvs);
    check(g_fake.runs_wake == 1u && g_fake.cmds_done == 8u, "...one wake pass processes all eight lists", (long)g_fake.cmds_done);
    /* the ring full of pointers, then one more pointer from the ARM9: the slice runs at once, nothing is dropped (this is the old drop-oldest path) */
    fresh();
    for (i = 0; i < HWSND_MAIL_DEPTH + 3u; i++) {
        hwsnd_send(&g_snd, 0x5000u + i, 0u);
    }
    hwsnd_pump(&g_snd, 0u);
    check(g_fake.recvs == HWSND_MAIL_DEPTH + 3u && g_fake.recv_log[0] == 0x5000u, "35 pointers through a 32-word ring: all delivered, none dropped", (long)g_fake.recvs);
}

/* B1: the slice is bracketed by enter/leave, and every driver call happens inside it */
static void test_depth(void)
{
    fresh();
    hwsnd_send(&g_snd, 0x77u, 0u);
    hwsnd_send(&g_snd, 0u, 0u);
    hwsnd_pump(&g_snd, 0u);
    hwsnd_pump(&g_snd, 3u * P);
    check(g_fake.enters == 2u && g_fake.leaves == 2u && g_fake.depth == 0u, "each slice is bracketed by one enter/leave pair", (long)g_fake.enters);
    check(g_fake.max_depth == 1u && g_fake.outside == 0u, "boot, recv and run only ever run inside the bracket", (long)g_fake.outside);
    fresh();
    g_fake.reenter = &g_snd;
    g_fake.reenter_now = 99u * P;
    hwsnd_pump(&g_snd, 0u);
    hwsnd_pump(&g_snd, P);
    check(g_fake.enters == g_fake.leaves && g_fake.max_depth == 1u && g_fake.depth == 0u, "a refused re-entrant pump does not nest or unbalance the bracket", (long)g_fake.max_depth);
}

/* F2: the ARM7's private alarms, driven by the slice's virtual time */
static uint32_t g_alarm_calls[4], g_alarm_args[4], g_alarm_flood;

static void alarm_cb(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;

    g_alarm_calls[id]++;
    g_alarm_args[id] = id;
    if (id == 3u) {
        uint32_t i;

        for (i = 0; i < g_alarm_flood; i++) {
            (void)hwsnd_reply(&g_snd, 0x1100u + id);
        }
    }
}

static void test_alarms(void)
{
    static int key[4];
    const uint64_t T = HWT_SYS_PER_TICK;

    memset(g_alarm_calls, 0, sizeof g_alarm_calls);
    g_alarm_flood = 0u;
    fresh();
    hwsnd_pump(&g_snd, 1000u * T);   /* boot at tick 1000 */
    check(hwsnd_now_tick(&g_snd) == 1000u, "OS_GetTick of the ARM7 code is the slice's virtual time in OS ticks", (long)hwsnd_now_tick(&g_snd));
    hwsnd_alarm_set(&g_snd, &key[0], hwsnd_now_tick(&g_snd) + 50u, 0u, alarm_cb, (void *)(uintptr_t)0);   /* one shot */
    hwsnd_alarm_set(&g_snd, &key[1], 1100u, 40u, alarm_cb, (void *)(uintptr_t)1);                          /* periodic, absolute start */
    hwsnd_pump(&g_snd, 1049u * T);
    check(g_alarm_calls[0] == 0u && g_alarm_calls[1] == 0u, "nothing fires before its tick", (long)g_alarm_calls[0]);
    hwsnd_pump(&g_snd, 1050u * T);
    check(g_alarm_calls[0] == 1u && g_alarm_calls[1] == 0u, "the one-shot fires exactly at its tick", (long)g_alarm_calls[0]);
    hwsnd_pump(&g_snd, 1500u * T);
    check(g_alarm_calls[0] == 1u, "...and never again", (long)g_alarm_calls[0]);
    check(g_alarm_calls[1] == 8u && g_snd.alarm_backlog_dropped == 1u, "a periodic alarm 10 periods late fires the catch-up cap (8) and drops the rest", (long)g_alarm_calls[1]);
    hwsnd_pump(&g_snd, 1540u * T);
    check(g_alarm_calls[1] == 9u, "...then keeps its phase on the period grid (1100 + 40n)", (long)g_alarm_calls[1]);
    hwsnd_alarm_cancel(&g_snd, &key[1]);
    hwsnd_pump(&g_snd, 2000u * T);
    check(g_alarm_calls[1] == 9u, "a cancelled alarm stays quiet", (long)g_alarm_calls[1]);
    /* re-arming the same OSAlarm replaces it (no second slot) */
    hwsnd_alarm_set(&g_snd, &key[2], 2100u, 0u, alarm_cb, (void *)(uintptr_t)2);
    hwsnd_alarm_set(&g_snd, &key[2], 2200u, 0u, alarm_cb, (void *)(uintptr_t)2);
    hwsnd_pump(&g_snd, 2150u * T);
    check(g_alarm_calls[2] == 0u, "re-arming replaces the previous setting", (long)g_alarm_calls[2]);
    hwsnd_pump(&g_snd, 2200u * T);
    check(g_alarm_calls[2] == 1u, "...which fires once at the new tick", (long)g_alarm_calls[2]);
    /* a handler that sends more replies than the FIFO holds: the F1 case through the alarm path */
    {
        uint32_t i;

        g_alarm_flood = HWPXI_FIFO_DEPTH + 4u;
        hwsnd_alarm_set(&g_snd, &key[3], 2300u, 0u, alarm_cb, (void *)(uintptr_t)3);
        for (i = 0; i < HWPXI_FIFO_DEPTH; i++) {
            (void)hwpxi_recv(&g_pxi);   /* empty FIFO */
        }
        hwsnd_pump(&g_snd, 2300u * T);
        check(g_alarm_calls[3] == 1u && g_snd.reply_deferred == 4u && g_snd.reply_overflow == 0u, "an alarm handler flooding the FIFO returns (4 words wait in the ring)", (long)g_snd.reply_deferred);
    }
    /* the ARM9's OS alarm system is not involved: nothing here links against it, and a soft reset clears the list */
    {
        HwSndOps ops = g_snd.ops;

        hwsnd_init(&g_snd, &ops, g_io, &g_pxi);
        check(g_snd.alarm[0].key == NULL && g_snd.alarms_set == 0u, "hwsnd_init (soft reset) clears the alarm list", 0);
    }
}

/* the wiring through the loopback ARM7: a word the ARM9 sends on the SOUND tag lands in the mailbox; without a mailbox it is counted and dropped */
static void test_through_pxi(void)
{
    HwArm7 a;
    HwArm7Ops aops;
    uint32_t word = HWPXI_TAG_SOUND | (0x00abcdefu << 6);

    fresh();
    memset(&aops, 0, sizeof aops);
    aops.snd = &g_snd;
    hwarm7_init(&a, &g_pxi, &aops);
    check((g_pxi.handle_bits & (1u << HWPXI_TAG_SOUND)) != 0u, "the SOUND tag's handle bit is seeded (PXI_IsCallbackReady)", (long)g_pxi.handle_bits);
    hwpxi_send(&g_pxi, word);
    check(g_snd.mail_count == 1u && g_snd.cmd_words == 1u && a.snd_swallowed == 0u, "hwpxi_send -> snd_responder -> hwsnd_send", (long)g_snd.mail_count);
    hwsnd_pump(&g_snd, 0u);
    check(g_fake.recvs == 1u && g_fake.recv_log[0] == 0x00abcdefu, "...and the slice delivers the 26-bit payload unchanged", (long)g_fake.recv_log[0]);
    hwpxi_send(&g_pxi, HWPXI_TAG_SOUND);
    hwsnd_pump(&g_snd, 1u);
    check(g_snd.wake_words == 1u && g_fake.runs_wake == 1u, "word 0 on the tag is the wake-up request: one wake pass", (long)g_snd.wake_words);

    memset(&aops, 0, sizeof aops);
    hwarm7_init(&a, &g_pxi, &aops);
    hwpxi_send(&g_pxi, word);
    check(a.snd_swallowed == 1u && g_snd.cmd_words == 1u, "with ops.snd == NULL the word is swallowed (the pre-1.5 behaviour, card_fs.3dsx)", (long)a.snd_swallowed);
}

/* ---- the channel / capture busy bits ----------------------------------------------------------------------------------------------- */
static void set_chan(unsigned ch, uint32_t cnt, uint32_t sad, uint32_t tmr, uint32_t pnt, uint32_t len)
{
    uint32_t w[4];

    w[0] = cnt;
    w[1] = sad;
    w[2] = (pnt << 16) | tmr;
    w[3] = len;
    memcpy(g_io + HWSND_SPU_OFF + 0x10u * ch, w, 16);
}

static uint32_t chan_cnt(unsigned ch)
{
    uint32_t v;

    memcpy(&v, g_io + HWSND_SPU_OFF + 0x10u * ch, 4);
    return v;
}

static void test_busy_bits(void)
{
    const uint32_t ONE = 0x90000000u, LOOP = 0x88000000u;   /* busy + repeat 2 (one-shot) / repeat 1 (loop), PCM8 */
    uint64_t t0 = 10u * P;

    check(hwsnd_chan_ticks(ONE, 0xff00u, 0u, 2u) == 8u * 4096u, "PCM8 one-shot: 2 words = 8 samples x (0x100 x 16) sys ticks", (long)hwsnd_chan_ticks(ONE, 0xff00u, 0u, 2u));
    check(hwsnd_chan_ticks(ONE | 0x20000000u, 0xff00u, 1u, 1u) == 4u * 4096u, "PCM16: (pnt+len) words x 2 samples", 0);
    check(hwsnd_chan_ticks(ONE | 0x40000000u, 0xff00u, 1u, 3u) == (4u * 8u - 8u) * 4096u, "ADPCM: 8 samples a word minus the 8-sample header", 0);
    check(hwsnd_chan_ticks(LOOP, 0xff00u, 0u, 2u) == 0u, "loop mode never ends", 0);
    check(hwsnd_chan_ticks(ONE | 0x60000000u, 0xff00u, 0u, 2u) == 0u, "PSG / noise never end", 0);
    check(hwsnd_chan_ticks(ONE, 0u, 0u, 1u) == 4u * 65536u * 16u, "TMR 0 is the slowest rate (65536 timer ticks a sample)", 0);
    check(hwsnd_cap_ticks(0x84u, 1u) == 2u * 8192u && hwsnd_cap_ticks(0x8cu, 1u) == 4u * 8192u, "capture one-shot: PCM16 2 / PCM8 4 samples a word at 8192 sys ticks", 0);
    check(hwsnd_cap_ticks(0x80u, 5u) == 0u, "capture loop mode never ends", 0);

    fresh();
    hwsnd_pump(&g_snd, t0);   /* boot */
    set_chan(2u, ONE, 0x02000000u, 0xff00u, 0u, 2u);
    hwsnd_pump(&g_snd, t0 + 1u);   /* the end of this slice sees the key-on at t0+1 */
    check((chan_cnt(2u) & 0x80000000u) != 0u && g_snd.chan_keyons == 1u, "a one-shot key-on starts busy", (long)g_snd.chan_keyons);
    hwsnd_pump(&g_snd, t0 + 1u + 32767u);
    check((chan_cnt(2u) & 0x80000000u) != 0u, "still busy one tick before its end", 0);
    hwsnd_pump(&g_snd, t0 + 1u + 32768u);
    check((chan_cnt(2u) & 0x80000000u) == 0u && g_snd.chan_expired == 1u, "bit 31 clears when the length has played", (long)chan_cnt(2u));
    check((chan_cnt(2u) & 0x7fffffffu) == (ONE & 0x7fffffffu), "only bit 31 changes", (long)chan_cnt(2u));
    hwsnd_pump(&g_snd, t0 + 1u + 40000u);
    check(g_snd.chan_keyons == 1u && g_snd.chan_expired == 1u, "an expired channel is not keyed again", 0);

    fresh();
    hwsnd_pump(&g_snd, t0);
    set_chan(3u, LOOP, 0x02000000u, 0xff00u, 0u, 2u);
    set_chan(9u, ONE | 0x60000000u, 0u, 0xff00u, 0u, 2u);
    hwsnd_pump(&g_snd, t0 + 1u);
    hwsnd_pump(&g_snd, t0 + 1000000u);
    check((chan_cnt(3u) & 0x80000000u) != 0u && (chan_cnt(9u) & 0x80000000u) != 0u && g_snd.chan_unbounded == 2u, "loop and PSG stay busy for ever", (long)g_snd.chan_unbounded);
    g_io[HWSND_SPU_OFF + 0x30u + 3u] = 0;   /* the driver stops channel 3: SOUNDxCNT = 0 */
    memset(g_io + HWSND_SPU_OFF + 0x30u, 0, 4);
    hwsnd_pump(&g_snd, t0 + 1000001u);
    set_chan(3u, ONE, 0x02000000u, 0xff00u, 0u, 1u);
    hwsnd_pump(&g_snd, t0 + 1000002u);
    check(g_snd.chan_keyons == 3u, "a stopped channel keyed on again starts a new clock", (long)g_snd.chan_keyons);
    set_chan(3u, ONE, 0x02000000u, 0xff00u, 0u, 8u);   /* parameters rewritten while busy: re-keyed from now */
    hwsnd_pump(&g_snd, t0 + 1000003u);
    check(g_snd.chan_keyons == 4u, "rewriting the parameters of a busy channel re-keys it", (long)g_snd.chan_keyons);

    fresh();
    hwsnd_pump(&g_snd, t0);
    g_io[HWSND_SPU_OFF + 0x108u] = 0x84u;   /* SNDCAP0CNT: start, one-shot, PCM16 */
    g_io[HWSND_SPU_OFF + 0x114u] = 2u;      /* SNDCAP0LEN = 2 words = 4 samples */
    g_io[HWSND_SPU_OFF + 0x109u] = 0x80u;   /* SNDCAP1CNT: loop capture */
    g_io[HWSND_SPU_OFF + 0x11cu] = 2u;
    hwsnd_pump(&g_snd, t0 + 1u);
    hwsnd_pump(&g_snd, t0 + 1u + 4u * 8192u - 1u);
    check((g_io[HWSND_SPU_OFF + 0x108u] & 0x80u) != 0u, "capture 0 busy before its end", 0);
    hwsnd_pump(&g_snd, t0 + 1u + 4u * 8192u);
    check((g_io[HWSND_SPU_OFF + 0x108u] & 0x80u) == 0u && (g_io[HWSND_SPU_OFF + 0x108u] & 0x7fu) == 4u && g_snd.cap_expired == 1u, "capture 0 bit 7 clears at its end", (long)g_io[HWSND_SPU_OFF + 0x108u]);
    check((g_io[HWSND_SPU_OFF + 0x109u] & 0x80u) != 0u, "a loop capture stays busy", 0);
}

int main(void)
{
    test_constants();
    test_boot_and_mail();
    test_cadence();
    test_catchup_cap();
    test_order_and_reentry();
    test_registers();
    test_reply();
    test_reset();
    test_mail_overflow();
    test_wake_coalesce_and_no_drop();
    test_depth();
    test_alarms();
    test_through_pxi();
    test_busy_bits();
    printf("hw_snd_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
