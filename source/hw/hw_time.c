/* hw_time.c -- virtual time for the event model. Pure C. */
#include <assert.h>

#include "hw_pure.h"

void hwtime_init(HwTime *t, uint64_t real_now)
{
    assert(t != NULL);
    assert(HWT_SYS_PER_BUS * 64u == HWT_SYS_PER_TICK);   /* the OS tick is bus/64 */
    t->virt = 0;
    t->real_base = real_now;
    t->tick_off = 0;
    t->frame = 0;
}

uint64_t hwtime_update(HwTime *t, uint64_t real_now)
{
    uint64_t r;

    assert(t != NULL);
    assert(t->virt < UINT64_MAX / 2u);
    if (t->frame) {
        return t->virt;   /* FRAME: only charges and the pump's frame step move virtual time */
    }
    r = real_now >= t->real_base ? real_now - t->real_base : 0;
    if (r > t->virt) {
        t->virt = r;
    }
    return t->virt;
}

void hwtime_steal(HwTime *t, uint64_t ticks)
{
    assert(t != NULL);
    assert(ticks < UINT64_MAX / 4u && t->real_base < UINT64_MAX / 2u);
    if (t->frame) {
        return;
    }
    t->real_base += ticks;
}

void hwtime_spin_cycles(HwTime *t, uint32_t arm9_cycles)
{
    assert(t != NULL);
    assert(HWT_SYS_PER_CYCLE == 4u);
    t->virt += (uint64_t)arm9_cycles * HWT_SYS_PER_CYCLE;
}

uint64_t hwtime_os_tick(const HwTime *t)
{
    assert(t != NULL);
    assert(HWT_SYS_PER_TICK == 512u);
    return (uint64_t)((int64_t)(t->virt / HWT_SYS_PER_TICK) + t->tick_off);
}

void hwtime_set_os_tick(HwTime *t, uint64_t tick)
{
    assert(t != NULL);
    assert(tick < (uint64_t)INT64_MAX);
    t->tick_off = (int64_t)tick - (int64_t)(t->virt / HWT_SYS_PER_TICK);
}
