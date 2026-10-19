/* hw_cp.c -- the DS hardware divider and square-root unit, run at the instant the SDK writes the operands (the ARM9 unit took 18-36 and
 * 13 cycles; the port finishes synchronously, so BUSY is never observed set). Pure C over the register block, host-tested.
 * Register images match cp_context.c: DIVCNT 0x280, NUMER 0x290 (64 bit), DENOM 0x298 (64 bit), RESULT 0x2a0, REM 0x2a8, SQRTCNT 0x2b0,
 * SQRT result 0x2b4 (32 bit), SQRT param 0x2b8 (64 bit). Division by zero follows GBATEK / melonDS (see hwcp_divide). */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

static uint16_t rd16(const uint8_t *io, uint32_t off)
{
    uint16_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static uint64_t rd64(const uint8_t *io, uint32_t off)
{
    uint64_t v;
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

static void wr64(uint8_t *io, uint32_t off, uint64_t v)
{
    memcpy(io + off, &v, sizeof v);
}

/* mode 0: both operands are the low 32 bits, sign-extended. Modes 1 and 3: 64-bit numerator, 32-bit denominator sign-extended. Mode 2:
 * 64/64. The quotient is the 64-bit truncating quotient and the remainder takes the numerator's sign.
 *  - denominator 0 (the low 32 bits in modes 0/1/3, all 64 in mode 2): quotient = (numer < 0) ? +1 : -1, remainder = numer; DIV0 set. In
 *    mode 0 the upper 32 bits of the quotient are the inverse of the sign extension of the low word (so -1 reads 0x00000000ffffffff).
 *  - INT64_MIN / -1 does not fit: quotient INT64_MIN, remainder 0. INT_MIN / -1 in mode 0 is plain 64-bit arithmetic: +2^31, remainder 0. */
void hwcp_divide(unsigned mode, int64_t numer, int64_t denom, int64_t *quot, int64_t *rem, int *div0)
{
    int64_t n = numer, d = denom;
    int64_t q, r;

    assert(quot != NULL && rem != NULL && div0 != NULL);
    assert(mode <= 3u);
    if (mode == 0u) {
        n = (int64_t)(int32_t)(uint32_t)(uint64_t)numer;
    }
    if (mode != 2u) {
        d = (int64_t)(int32_t)(uint32_t)(uint64_t)denom;
    }
    *div0 = (d == 0);
    if (d == 0) {
        q = (n < 0) ? 1 : -1;
        r = n;
        if (mode == 0u) {
            uint32_t lo = (uint32_t)(uint64_t)q;
            uint32_t hi = (lo & 0x80000000u) ? 0u : 0xffffffffu;   /* the inverse of the sign extension of lo */
            q = (int64_t)(((uint64_t)hi << 32) | lo);
        }
    } else if (d == -1) {
        q = (n == INT64_MIN) ? INT64_MIN : -n;   /* avoids the C overflow; -INT64_MIN is the hardware's answer too */
        r = 0;
    } else {
        q = n / d;
        r = n % d;
    }
    *quot = q;
    *rem = r;
}

/* floor(sqrt(x)) of a 64-bit value: bit-by-bit, no floating point, 32 iterations */
uint32_t hwcp_isqrt(uint64_t x)
{
    uint64_t root = 0u;
    uint64_t bit = 1ull << 62;
    uint32_t i;

    for (i = 0; i < 32u; i++) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    assert(root <= 0xffffffffull);
    return (uint32_t)root;
}

void hwcp_div_run(uint8_t *io)
{
    uint16_t cnt;
    int64_t q, r;
    int div0;

    assert(io != NULL);
    cnt = rd16(io, HWR_DIVCNT);
    hwcp_divide((unsigned)(cnt & 3u), (int64_t)rd64(io, HWR_DIV_NUMER), (int64_t)rd64(io, HWR_DIV_DENOM), &q, &r, &div0);
    wr64(io, HWR_DIV_RESULT, (uint64_t)q);
    wr64(io, HWR_DIVREM, (uint64_t)r);
    cnt = (uint16_t)(cnt & ~(HWCP_BUSY | HWCP_DIV0));
    wr16(io, HWR_DIVCNT, (uint16_t)(cnt | (div0 ? HWCP_DIV0 : 0u)));
}

void hwcp_sqrt_run(uint8_t *io)
{
    uint16_t cnt;
    uint64_t x;

    assert(io != NULL);
    cnt = rd16(io, HWR_SQRTCNT);
    x = rd64(io, HWR_SQRT_PARAM);
    if (!(cnt & 1u)) {
        x &= 0xffffffffull;   /* mode 0: a 32-bit input */
    }
    wr32(io, HWR_SQRT_RESULT, hwcp_isqrt(x));
    wr16(io, HWR_SQRTCNT, (uint16_t)(cnt & ~HWCP_BUSY));
}
