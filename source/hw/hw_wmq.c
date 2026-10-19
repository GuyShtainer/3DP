/* hw_wmq.c -- see hw_wmq.h. Pure C99. */
#include <assert.h>
#include <string.h>

#include "hw_wmq.h"

void hwwmq_init(HwWmQ *q)
{
    assert(q != NULL);
    memset(q, 0, sizeof *q);
}

static void commit(volatile uint16_t *control, void *dst, const void *reply)
{
    memcpy(dst, reply, HWWMQ_REPLY_BYTES);
    *control = (uint16_t)(*control | HWWMQ_BUSY_BIT);
}

int hwwmq_post(HwWmQ *q, volatile uint16_t *control, void *dst, const void *reply, uint32_t delay)
{
    unsigned slot;

    assert(q != NULL && control != NULL && dst != NULL && reply != NULL);
    q->posted++;
    if (delay == 0u && q->n == 0u && (*control & HWWMQ_BUSY_BIT) == 0u) {
        commit(control, dst, reply);
        q->delivered++;
        return 1;
    }
    if (q->n >= HWWMQ_DEPTH) {
        q->dropped++;
        return -1;
    }
    slot = (q->head + q->n) % HWWMQ_DEPTH;
    memcpy(q->e[slot].data, reply, HWWMQ_REPLY_BYTES);
    q->e[slot].due = q->frame + delay;
    q->n++;
    q->deferred++;
    return 0;
}

int hwwmq_tick(HwWmQ *q, volatile uint16_t *control, void *dst)
{
    assert(q != NULL && control != NULL && dst != NULL);
    q->frame++;
    if (q->n == 0u || q->e[q->head].due > q->frame || (*control & HWWMQ_BUSY_BIT) != 0u) {
        return 0;
    }
    if (q->frame - q->e[q->head].due > q->max_wait) {
        q->max_wait = q->frame - q->e[q->head].due;
    }
    commit(control, dst, q->e[q->head].data);
    q->head = (q->head + 1u) % HWWMQ_DEPTH;
    q->n--;
    q->delivered++;
    return 1;
}

unsigned hwwmq_pending(const HwWmQ *q)
{
    assert(q != NULL);
    return q->n;
}

uint32_t hwwmq_scan_frames(unsigned channels, unsigned max_channel_time)
{
    uint32_t ms = (uint32_t)(channels > 14u ? 14u : channels) * (max_channel_time != 0u ? max_channel_time : 30u);
    uint32_t frames = (ms * 60u + 999u) / 1000u;

    return frames != 0u ? frames : 1u;
}
