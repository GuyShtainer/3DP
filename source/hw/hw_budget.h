/* hw_budget.h -- bet 2.7a: a fixed-size per-bucket time distribution (count, sum, max, histogram) so the hardware session can report avg / p95 / max per
 * frame bucket without a per-frame array. Pure C (no SDK, no libctru). One bucket is written by ONE thread only (no locking); the report reads it after the
 * writers have stopped. Ticks are svcGetSystemTick ticks (268 111 856 Hz); bins are 1/16 ms wide (2.7(b) N1: 1024 bins = 0 .. 64 ms, 4 KB per bucket, 17 buckets = 70 KB of bss). */
#ifndef HW_BUDGET_H
#define HW_BUDGET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWBUD_TICKS_PER_MS   268111.856
#define HWBUD_BIN_TICKS      16757u    /* 1/16 ms */
#define HWBUD_BINS           1024u     /* 0 .. 64 ms; the last bin is the overflow */
#define HWBUD_RANGE_MS       64

typedef struct HwBud {
    uint32_t n;                        /* samples */
    uint64_t sum;                      /* ticks */
    uint32_t max;                      /* ticks */
    uint32_t hist[HWBUD_BINS + 1u];
} HwBud;

void hwbud_add(HwBud *b, uint32_t ticks);
double hwbud_ms(uint64_t ticks);                    /* ticks to milliseconds */
double hwbud_avg_ms(const HwBud *b);                /* 0 when empty */
double hwbud_p95_ms(const HwBud *b);                /* upper edge of the bin holding the 95th percentile, never above the max; 0 when empty; meaningless when hwbud_p95_over */
int hwbud_p95_over(const HwBud *b);                 /* 1 when the 95th percentile lies past HWBUD_RANGE_MS (the report prints ">64", not the max) */
double hwbud_max_ms(const HwBud *b);

#ifdef __cplusplus
}
#endif

#endif
