/* hw_timer.c -- DS timers 0-3 emulated from the register contents at yield points. Pure C.
 * The SDK writes CNT_L (reload) and CNT_H (control) as plain memory. At each advance the module notices an enable edge (latch the
 * reload), a software rewrite of the reload (CNT_L differs from what it wrote back), runs the prescaler / cascade arithmetic over
 * the elapsed bus ticks, writes the counter back to CNT_L and reports the overflow IRQs. */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

static uint16_t rd16(const uint8_t *io, uint32_t off)
{
    uint16_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static void wr16(uint8_t *io, uint32_t off, uint16_t v)
{
    memcpy(io + off, &v, sizeof v);
}

static uint32_t prescale(uint16_t ctl)
{
    static const uint32_t ps[4] = { 1, 64, 256, 1024 };
    return ps[ctl & 3u];
}

void hwtimer_init(HwTimers *t, uint64_t bus_now)
{
    assert(t != NULL);
    assert(bus_now < UINT64_MAX);
    memset(t, 0, sizeof *t);
    t->last_bus = bus_now;
}

void hwtimer_ctl_write(HwTimers *t, unsigned n)
{
    assert(t != NULL);
    assert(n < 4u);
    if (n < 4u) {
        t->running[n] = 0;   /* the next advance sees a fresh enable edge and latches the reload */
    }
}

uint32_t hwtimer_advance(HwTimers *t, uint8_t *io, uint64_t bus_now)
{
    uint64_t dt = bus_now >= t->last_bus ? bus_now - t->last_bus : 0;
    uint64_t carry = 0;
    uint32_t iflags = 0;
    uint32_t n;

    assert(t != NULL && io != NULL);
    assert(bus_now >= t->last_bus || dt == 0u);
    t->last_bus = bus_now;
    for (n = 0; n < 4; n++) {
        uint32_t off = HWR_TM0CNT_L + 4u * n;
        uint16_t cnt = rd16(io, off);
        uint16_t ctl = rd16(io, off + 2);
        uint64_t steps, total, overflows = 0;
        uint32_t ps;

        if (!(ctl & 0x80u)) {
            t->running[n] = 0;
            carry = 0;
            continue;
        }
        if (!t->running[n]) {
            t->running[n] = 1;
            t->reload[n] = cnt;
            t->counter[n] = cnt;
            t->acc[n] = 0;
            t->last_written[n] = cnt;
        } else if (cnt != t->last_written[n]) {
            t->reload[n] = cnt;   /* software wrote a new reload; the running counter is not touched */
        }
        ps = prescale(ctl);
        if (n > 0 && (ctl & 4u)) {
            steps = carry;
        } else {
            t->acc[n] += dt;
            steps = t->acc[n] / ps;
            t->acc[n] %= ps;
        }
        total = (uint64_t)t->counter[n] + steps;
        if (total > 0xffffu) {
            uint64_t period = 0x10000u - (uint64_t)t->reload[n];
            uint64_t over = total - 0x10000u;
            overflows = 1 + over / period;
            t->counter[n] = (uint16_t)(t->reload[n] + over % period);
        } else {
            t->counter[n] = (uint16_t)total;
        }
        carry = overflows;
        if (overflows && (ctl & 0x40u)) {
            iflags |= 1u << (HWIRQ_TIMER0 + n);
        }
        wr16(io, off, t->counter[n]);
        t->last_written[n] = t->counter[n];
    }
    return iflags;
}

uint64_t hwtimer_next_irq(const HwTimers *t, const uint8_t *io)
{
    uint64_t best = UINT64_MAX;
    uint32_t n;

    assert(t != NULL && io != NULL);
    assert(t->last_bus < UINT64_MAX);

    for (n = 0; n < 4; n++) {
        uint32_t off = HWR_TM0CNT_L + 4u * n;
        uint16_t ctl = rd16(io, off + 2);
        uint64_t steps, bus;

        if (!(ctl & 0x80u) || !(ctl & 0x40u) || (n > 0 && (ctl & 4u))) {
            continue;
        }
        steps = 0x10000u - (uint64_t)(t->running[n] ? t->counter[n] : rd16(io, off));
        bus = t->last_bus + steps * prescale(ctl) - (t->running[n] ? t->acc[n] : 0);
        if (bus < best) {
            best = bus;
        }
    }
    return best;
}
