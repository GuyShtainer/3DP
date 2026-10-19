/* hw_budget_test.c -- bet 2.7a: the budget histogram on hand-computed distributions. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hw_budget.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

int main(void)
{
    static HwBud b;
    uint32_t i;

    memset(&b, 0, sizeof b);
    CHECK(hwbud_avg_ms(&b) == 0.0 && hwbud_p95_ms(&b) == 0.0 && hwbud_max_ms(&b) == 0.0);

    /* 100 samples: 95 of 1 ms, 5 of 10 ms -> avg 1.45, p95 = the 95th sample = 1 ms bin, max 10 */
    for (i = 0; i < 95u; i++) { hwbud_add(&b, (uint32_t)HWBUD_TICKS_PER_MS); }
    for (i = 0; i < 5u; i++) { hwbud_add(&b, (uint32_t)(10 * HWBUD_TICKS_PER_MS)); }
    CHECK(b.n == 100u);
    CHECK(near(hwbud_avg_ms(&b), 1.45, 0.001));
    CHECK(near(hwbud_max_ms(&b), 10.0, 0.001));
    CHECK(near(hwbud_p95_ms(&b), 1.0, 0.07));      /* within one 1/16 ms bin */
    hwbud_add(&b, (uint32_t)(10 * HWBUD_TICKS_PER_MS));   /* 101 samples: rank 96 is a 10 ms sample */
    CHECK(near(hwbud_p95_ms(&b), 10.0, 0.07));

    /* the range is 64 ms: a 40 ms sample is in range, a 70 ms one lands in the overflow bin */
    memset(&b, 0, sizeof b);
    hwbud_add(&b, (uint32_t)(40 * HWBUD_TICKS_PER_MS));
    CHECK(b.hist[HWBUD_BINS] == 0u && !hwbud_p95_over(&b));
    CHECK(near(hwbud_p95_ms(&b), 40.0, 0.001));
    memset(&b, 0, sizeof b);
    hwbud_add(&b, (uint32_t)(70 * HWBUD_TICKS_PER_MS));
    CHECK(b.hist[HWBUD_BINS] == 1u && hwbud_p95_over(&b));
    /* more than 5 % past 64 ms -> over; 5 % or fewer -> the p95 is still resolved in range */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 94u; i++) { hwbud_add(&b, (uint32_t)(2 * HWBUD_TICKS_PER_MS)); }
    for (i = 0; i < 6u; i++) { hwbud_add(&b, (uint32_t)(100 * HWBUD_TICKS_PER_MS)); }
    CHECK(hwbud_p95_over(&b));
    memset(&b, 0, sizeof b);
    for (i = 0; i < 95u; i++) { hwbud_add(&b, (uint32_t)(2 * HWBUD_TICKS_PER_MS)); }
    for (i = 0; i < 5u; i++) { hwbud_add(&b, (uint32_t)(100 * HWBUD_TICKS_PER_MS)); }
    CHECK(!hwbud_p95_over(&b) && near(hwbud_p95_ms(&b), 2.0, 0.07));
    /* 40 ms used to be past a 32 ms range: p95 of a distribution at 40 ms is now 40, not the max-clamped overflow */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 100u; i++) { hwbud_add(&b, (uint32_t)((20 + i % 3) * HWBUD_TICKS_PER_MS)); }
    CHECK(!hwbud_p95_over(&b) && near(hwbud_p95_ms(&b), 22.0, 0.07));

    /* a single sample: p95 never exceeds the max */
    memset(&b, 0, sizeof b);
    hwbud_add(&b, 100u);
    CHECK(hwbud_p95_ms(&b) <= hwbud_max_ms(&b) + 1e-9);
    CHECK(near(hwbud_ms(268111856u), 1000.0, 0.001));

    if (fails) { printf("hw_budget_test: %d FAILED\n", fails); return 1; }
    printf("hw_budget_test: all passed\n");
    return 0;
}
