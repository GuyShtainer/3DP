/* hw_budget.c -- see hw_budget.h. */
#include <assert.h>
#include <string.h>

#include "hw_budget.h"

void hwbud_add(HwBud *b, uint32_t ticks)
{
    uint32_t bin = ticks / HWBUD_BIN_TICKS;

    assert(b != NULL);
    if (bin > HWBUD_BINS) {
        bin = HWBUD_BINS;
    }
    b->n++;
    b->sum += ticks;
    if (ticks > b->max) {
        b->max = ticks;
    }
    b->hist[bin]++;
}

double hwbud_ms(uint64_t ticks)
{
    return (double)ticks / HWBUD_TICKS_PER_MS;
}

double hwbud_avg_ms(const HwBud *b)
{
    assert(b != NULL);
    return b->n == 0u ? 0.0 : hwbud_ms(b->sum) / (double)b->n;
}

double hwbud_max_ms(const HwBud *b)
{
    assert(b != NULL);
    return hwbud_ms(b->max);
}

static uint32_t p95_bin(const HwBud *b)
{
    uint64_t need, seen = 0;
    uint32_t i;

    need = ((uint64_t)b->n * 95u + 99u) / 100u;   /* ceil(0.95 n): the sample at that rank */
    for (i = 0; i < HWBUD_BINS; i++) {
        seen += b->hist[i];
        if (seen >= need) {
            return i;
        }
    }
    return HWBUD_BINS;
}

int hwbud_p95_over(const HwBud *b)
{
    assert(b != NULL);
    return b->n != 0u && p95_bin(b) >= HWBUD_BINS;
}

double hwbud_p95_ms(const HwBud *b)
{
    uint32_t i;
    double edge;

    assert(b != NULL);
    if (b->n == 0u) {
        return 0.0;
    }
    i = p95_bin(b);
    edge = hwbud_ms((uint64_t)(i + 1u) * HWBUD_BIN_TICKS);
    if (i >= HWBUD_BINS || edge > hwbud_max_ms(b)) {
        edge = hwbud_max_ms(b);
    }
    return edge;
}
