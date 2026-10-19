/* hw_pump.c -- the event pump of the private design notes. Pure C; every effect goes through HwPumpOps. */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

static uint16_t rd16(const uint8_t *io, uint32_t off)
{
    uint16_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static uint32_t rd32(const uint8_t *io, uint32_t off)
{
    uint32_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static void wr16(uint8_t *io, uint32_t off, uint16_t v)
{
    memcpy(io + off, &v, sizeof v);
}

static void wr32(uint8_t *io, uint32_t off, uint32_t v)
{
    memcpy(io + off, &v, sizeof v);
}

void hwpump_init(HwPump *p, const HwPumpOps *ops, uint8_t *ioreg, uint32_t *vblank_buf)
{
    assert(p != NULL && ops != NULL && ioreg != NULL);
    assert(ops->real_sys_tick != NULL && ops->wait_vblank != NULL && ops->sleep_sys != NULL);
    memset(p, 0, sizeof *p);
    p->ops = *ops;
    p->io = ioreg;
    p->vblank_buf = vblank_buf;
    hwtime_init(&p->time, ops->real_sys_tick(ops->user));
    hwtimer_init(&p->tm, 0);
    p->next_vblank = HWT_FRAME_SYS;
    p->inited = 1;
}

/* VCOUNT / DISPSTAT.vblank from the position inside the frame whose vblank (line 192) is next_vblank - FRAME. */
static void set_lines(HwPump *p, uint64_t now)
{
    uint64_t frame_start;

    assert(p->next_vblank >= HWT_FRAME_SYS);
    assert(p->io != NULL);
    frame_start = p->next_vblank - HWT_FRAME_SYS;   /* time of line 192 of the current frame */
    uint64_t since = now >= frame_start ? now - frame_start : 0;
    uint32_t line = (uint32_t)((192u + since * 263u / HWT_FRAME_SYS) % 263u);
    uint16_t ds = rd16(p->io, HWR_DISPSTAT);

    if (p->hb.in_line) {   /* an HBlank line is running (a handler may poll the tick, which lands here): lines 0-191 are outside the vblank */
        line = p->hb.line;
    }
    wr16(p->io, HWR_VCOUNT, (uint16_t)line);
    ds = (uint16_t)(line >= 192u ? (ds | 1u) : (ds & ~1u));
    wr16(p->io, HWR_DISPSTAT, ds);
}

static void raise_vblank(HwPump *p, uint64_t now)
{
    p->vblanks++;
    p->frame_spin = 0;
    if (p->ops.vblank != NULL) {
        p->ops.vblank(p->ops.user);
    }
    if (p->vblank_buf) {
        (*p->vblank_buf)++;
    }
    if (rd16(p->io, HWR_DISPSTAT) & 8u) {
        wr32(p->io, HWR_IF, rd32(p->io, HWR_IF) | (1u << HWIRQ_VBLANK));
    }
    if (now >= p->next_vblank + HWT_FRAME_SYS) {   /* overrun: the skipped vblanks are not caught up */
        p->overruns++;
        p->next_vblank = now + HWT_FRAME_SYS;
    } else {
        p->next_vblank += HWT_FRAME_SYS;
    }
}

/* steps 1-3 of the pump: timers, then the vblank if it is due */
static void advance(HwPump *p, uint64_t now)
{
    uint32_t f = hwtimer_advance(&p->tm, p->io, now / HWT_SYS_PER_BUS);

    if (f) {
        wr32(p->io, HWR_IF, rd32(p->io, HWR_IF) | f);
    }
    if (now >= p->next_vblank) {
        raise_vblank(p, now);
    }
    set_lines(p, now);
}

static int pending(const HwPump *p)
{
    return (rd32(p->io, HWR_IE) & rd32(p->io, HWR_IF)) != 0;
}

#define HWPUMP_MAX_DISPATCH 64u   /* per pump: a handler that re-raises its own IRQ forever is a bug, not a loop */

static void dispatch_loop(HwPump *p)
{
    uint32_t iter;

    assert(p != NULL && p->ops.dispatch != NULL);
    assert(p->ops.irq_masked != NULL);
    for (iter = 0; iter < HWPUMP_MAX_DISPATCH; iter++) {
        uint32_t pend, n, flags;

        if (!(rd32(p->io, HWR_IME) & 1u) || p->ops.irq_masked(p->ops.user)) {
            break;
        }
        if (p->ops.level_irqs != NULL) {
            p->ops.level_irqs(p->ops.user);
        }
        pend = rd32(p->io, HWR_IE) & rd32(p->io, HWR_IF);
        if (!pend) {
            break;
        }
        for (n = 0; n < 31u && !(pend & (1u << n)); n++) {
        }
        flags = rd32(p->io, HWR_IF);
        wr32(p->io, HWR_IF, flags & ~(1u << n));   /* acknowledge, then run the handler */
        p->dispatched++;
        p->ops.dispatch(p->ops.user, (int)n);
    }
}

uint32_t hwpump_hblank_frame(HwPump *p, const HwPumpLineOps *lo)
{
    uint32_t n, ran = 0u;
    int ds_on, ie_on;

    assert(p != NULL && p->inited && lo != NULL && lo->snap != NULL);
    assert(!p->hb.in_line);   /* a handler must not reach the snapshot point */
    ds_on = (rd16(p->io, HWR_DISPSTAT) & 0x10u) != 0;
    ie_on = (rd32(p->io, HWR_IE) & (1u << HWIRQ_HBLANK)) != 0;
    p->hb.frames++;
    p->hb.ds_frames += ds_on ? 1u : 0u;
    p->hb.ie_frames += ie_on ? 1u : 0u;
    if (!ds_on || !ie_on) {
        lo->snap(lo->user, 0u, HW_LINES);
        p->hb.shortcut++;
        return 0u;
    }
    p->hb.full++;
    for (n = 0; n < HW_LINES; n++) {
        uint16_t ds;

        p->hb.in_line = 1u;
        p->hb.line = n;
        wr16(p->io, HWR_VCOUNT, (uint16_t)n);
        ds = rd16(p->io, HWR_DISPSTAT);
        wr16(p->io, HWR_DISPSTAT, (uint16_t)((ds | 2u) & ~1u));
        if ((rd16(p->io, HWR_DISPSTAT) & 0x10u) && (rd32(p->io, HWR_IE) & (1u << HWIRQ_HBLANK))) {
            wr32(p->io, HWR_IF, rd32(p->io, HWR_IF) | (1u << HWIRQ_HBLANK));
            p->hb.irqs++;
            dispatch_loop(p);
        }
        lo->snap(lo->user, n, 1u);   /* after the callback: a BGxHOFS it wrote for this line is in this line's image */
        wr16(p->io, HWR_DISPSTAT, (uint16_t)(rd16(p->io, HWR_DISPSTAT) & ~2u));
        ran++;
    }
    p->hb.in_line = 0u;
    p->hb.lines += ran;
    set_lines(p, p->time.virt);   /* back to the frame's own VCOUNT / vblank flag */
    return ran;
}

void hwpump_set_clock(HwPump *p, HwClock clock)
{
    assert(p != NULL && p->inited);
    assert(clock == HWCLOCK_WALL || clock == HWCLOCK_FRAME);
    p->time.frame = clock == HWCLOCK_FRAME ? 1u : 0u;
    if (p->time.frame && p->vblanks == 0u) {
        p->time.virt = 0;
        p->next_vblank = HWT_FRAME_SYS;
        p->frame_spin = 0;
    }
}

/* FRAME clock: charge `sys` ticks of game-side waiting. Virtual time is clamped one tick below the next vblank (only a waiting yield may cross it); the charge
   still counts against the frame's spin budget, and once the budget is gone the vblank is forced (virt = next_vblank, forced_vblanks++) so a busy-wait that no
   timer can end becomes a visible number instead of a hang. The caller runs advance() / hwpump_run() next, which raises it. */
static void frame_charge(HwPump *p, uint64_t sys)
{
    assert(p->time.frame && p->next_vblank >= HWT_FRAME_SYS);
    p->frame_spin += sys;
    p->time.virt += sys;
    if (p->time.virt >= p->next_vblank) {
        p->time.virt = p->next_vblank - 1u;
    }
    if (p->frame_spin >= HWT_SPIN_BUDGET_SYS) {
        p->time.virt = p->next_vblank;
        p->forced_vblanks++;
    }
}

uint64_t hwpump_poll(HwPump *p)
{
    assert(p != NULL && p->inited);
    if (!p->time.frame) {
        return hwtime_update(&p->time, p->ops.real_sys_tick(p->ops.user));
    }
    frame_charge(p, (uint64_t)HWT_POLL_CYCLES * HWT_SYS_PER_CYCLE);
    advance(p, p->time.virt);
    return p->time.virt;
}

void hwpump_run(HwPump *p, HwPumpMode mode)
{
    uint64_t now;

    assert(p != NULL && p->inited);
    assert(mode == HWP_DISPATCH_ONLY || mode == HWP_NOWAIT || mode == HWP_WAIT);
    if (mode == HWP_DISPATCH_ONLY) {
        dispatch_loop(p);
        return;
    }
    now = hwtime_update(&p->time, p->ops.real_sys_tick(p->ops.user));
    advance(p, now);
    if (mode == HWP_WAIT && !pending(p)) {
        uint64_t tn = hwtimer_next_irq(&p->tm, p->io);
        uint64_t tsys = tn == UINT64_MAX ? UINT64_MAX : tn * HWT_SYS_PER_BUS;

        if (tsys < p->next_vblank) {
            if (p->time.frame) {
                if (tsys > now) {
                    p->time.virt = now = tsys;   /* FRAME: nothing to wait for on the host, jump to the timer's due time (below next_vblank) */
                }
            } else {
                p->ops.sleep_sys(p->ops.user, tsys > now ? tsys - now : 0);
                now = hwtime_update(&p->time, p->ops.real_sys_tick(p->ops.user));
                if (now < tsys) {
                    p->time.virt = now = tsys;   /* the sleep may return early; the timer was due */
                }
            }
        } else {
            p->ops.wait_vblank(p->ops.user);
            if (p->time.frame) {
                p->time.virt = now = p->next_vblank;   /* FRAME: exactly one frame per waiting yield, whatever the host clock says */
            } else {
                now = hwtime_update(&p->time, p->ops.real_sys_tick(p->ops.user));
                if (now < p->next_vblank) {
                    p->time.virt = now = p->next_vblank;   /* the real vblank has happened: the frame boundary is due */
                }
            }
        }
        advance(p, now);
    }
    if (p->ops.arm7_pump) {
        p->ops.arm7_pump(p->ops.user);
    }
    dispatch_loop(p);
}

void hwpump_sync(HwPump *p)
{
    uint64_t now;

    assert(p != NULL && p->inited);
    now = hwtime_update(&p->time, p->ops.real_sys_tick(p->ops.user));
    advance(p, now);
}

void hwpump_spin(HwPump *p, uint32_t arm9_cycles)
{
    assert(p != NULL && p->inited);
    assert(arm9_cycles > 0u);
    if (p->time.frame) {
        frame_charge(p, (uint64_t)arm9_cycles * HWT_SYS_PER_CYCLE);
    } else {
        hwtime_spin_cycles(&p->time, arm9_cycles);
    }
    hwpump_run(p, HWP_NOWAIT);
}
