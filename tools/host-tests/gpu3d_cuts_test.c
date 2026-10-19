/* gpu3d_cuts_test.c -- slice 2.4c: the cuts that changed code paths in gpu3d.c, against what they replaced. The engine source is #included so the static helpers can be reached.
 *  - div_trunc_exact (FPU divide) against the 64-bit integer divide over sweeps, including the extremes the DS allows (|a| up to 2^62, divisors of +-1, +-2^31, quotients next
 *    to an integer);
 *  - clip_verdict: REJECT means clip_polygon returns 0, INSIDE means it returns n and touches nothing, over random polygons (3 and 4 vertices, start 0 and 2, far plane allowed
 *    or not) with coordinates chosen so every outcome shows up;
 *  - the lighting memo: a stream with the memo against a twin whose memo is cleared before every NORMAL, over random material / light / matrix / normal commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu3d.c"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 20) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t s_rng = 0x2545f4914f6cdd1dull;

static uint64_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static void test_div(void)
{
    static const int32_t divs[] = { 1, -1, 2, -2, 3, 7, 4096, -4096, 0x7fffff, 0x1000000, 0x7fffffff, (int32_t)0x80000000, (int32_t)0x80000001, -0x1000001, 12345679 };
    static const int64_t nums[] = { 0, 1, -1, 4095, 4096, 0x3fffll * 0x7fffffffll, -0x3fffll * 0x7fffffffll, ((int64_t)1 << 52) - 1, -(((int64_t)1 << 52) - 1), (int64_t)1 << 52, (int64_t)1 << 62,
                                    -((int64_t)1 << 62), 0x7fffffffffffffffll, ((int64_t)1 << 45) };
    uint32_t i, k, n;

    for (i = 0u; i < sizeof divs / sizeof divs[0]; i++) {
        for (k = 0u; k < sizeof nums / sizeof nums[0]; k++) {
            CHECK(div_trunc_exact(nums[k], divs[i]) == nums[k] / divs[i]);
        }
    }
    for (n = 0u; n < 2000000u; n++) {
        int32_t d = (int32_t)rnd();
        int64_t a;
        uint32_t shape = (uint32_t)(rnd() % 5u);

        if (d == 0) {
            continue;
        }
        if (shape == 0u) {
            a = (int64_t)(int32_t)rnd() * 0x4000;   /* the z-depth divide */
        } else if (shape == 1u) {
            a = (int64_t)(int32_t)rnd() * (int64_t)(int32_t)rnd();   /* the lerp products */
        } else if (shape == 2u) {
            a = (int64_t)d * (int64_t)(int32_t)rnd() + (int64_t)((int32_t)(rnd() % 3u) - 1);   /* next to a multiple */
        } else if (shape == 3u) {
            a = (int64_t)d * ((int64_t)(rnd() & 0xfffffffffull) - 0x800000000ll) + (int64_t)((int32_t)(rnd() % 3u) - 1);
        } else {
            a = (int64_t)rnd() >> (rnd() % 40u);
        }
        CHECK(div_trunc_exact(a, d) == a / d);
    }
}

static int32_t coord(int32_t w)
{
    switch (rnd() % 6u) {
    case 0: return w;                      /* on the plane: inside */
    case 1: return -w;
    case 2: return (int32_t)(rnd() % 3u) - 1;
    case 3: return w + 1 + (int32_t)(rnd() % 3u);   /* just outside */
    case 4: return -w - 1 - (int32_t)(rnd() % 3u);
    default: return (int32_t)((int64_t)(int32_t)rnd() % (2 * (int64_t)w + 3)) ;
    }
}

static void test_verdict(void)
{
    uint32_t n, k, hist[3] = { 0, 0, 0 };

    for (n = 0u; n < 400000u; n++) {
        Gpu3dVtx v[CLIP_MAX], c[CLIP_MAX];
        uint32_t nv = 3u + (uint32_t)(rnd() & 1u), start = (rnd() % 4u) == 0u ? 2u : 0u, verdict, r, over = 0u;
        int far_ok = (int)(rnd() & 1u);
        int32_t wmax = (rnd() & 1u) != 0u ? 1000 : 0x7fffff;

        memset(v, 0, sizeof v);
        for (k = 0u; k < nv; k++) {
            int32_t w = (rnd() % 16u) == 0u ? (int32_t)(rnd() % 3u) - 1 : (int32_t)(rnd() % (uint32_t)wmax);

            v[k].pos[3] = w;
            v[k].pos[0] = coord(w);
            v[k].pos[1] = coord(w);
            v[k].pos[2] = coord(w);
            if ((rnd() % 3u) == 0u && k > 0u) {   /* a common outside side for most vertices: whole-polygon rejections */
                v[k].pos[2] = v[0].pos[2] > v[0].pos[3] ? v[k].pos[3] + 1 + (int32_t)(rnd() % 5u) : v[k].pos[2];
            }
            v[k].col[0] = v[k].col[1] = v[k].col[2] = (int32_t)(((rnd() % 32u) << 12) + 0xfffu);
            v[k].oc = outcode(v[k].pos);
        }
        if (start == 2u) {
            for (k = 0u; k < 2u; k++) {   /* carried vertices are inside already */
                v[k].pos[3] = 100;
                v[k].pos[0] = v[k].pos[1] = v[k].pos[2] = (int32_t)(rnd() % 201u) - 100;
                v[k].oc = outcode(v[k].pos);
            }
        }
        verdict = clip_verdict(v, nv, start);
        hist[verdict]++;
        memcpy(c, v, sizeof c);
        r = clip_polygon(c, nv, start, 1, far_ok, &over);
        if (verdict == CLIP_REJECT) {
            CHECK(r == 0u);
        } else if (verdict == CLIP_INSIDE) {
            CHECK(r == nv);
            CHECK(memcmp(c, v, nv * sizeof v[0]) == 0);
        }
    }
    CHECK(hist[CLIP_REJECT] > 1000u && hist[CLIP_INSIDE] > 1000u && hist[CLIP_NEEDED] > 1000u);   /* every outcome was exercised */
    printf("  verdicts: reject %u, inside %u, needed %u\n", hist[CLIP_REJECT], hist[CLIP_INSIDE], hist[CLIP_NEEDED]);
}

static Gpu3d s_a, s_b;

static void both(uint32_t op, uint32_t w0, uint32_t w1, uint32_t w2)
{
    uint32_t off = 0x400u + 4u * op;

    gpu3d_port(&s_a, off, w0);
    gpu3d_port(&s_b, off, w0);
    if (op == 0x1bu) {   /* MTX_SCALE: three parameters */
        gpu3d_port(&s_a, off, w1);
        gpu3d_port(&s_b, off, w1);
        gpu3d_port(&s_a, off, w2);
        gpu3d_port(&s_b, off, w2);
    }
}

static void test_memo(void)
{
    uint32_t n, normals = 0u, hits;

    gpu3d_reset(&s_a);
    gpu3d_reset(&s_b);
    both(0x10u, 2u, 0, 0);   /* MTX_MODE position + vector */
    for (n = 0u; n < 300000u; n++) {
        uint32_t pick = (uint32_t)(rnd() % 64u), w = (uint32_t)rnd();

        if (pick < 40u) {   /* NORMAL, from a small set so the memo hits */
            uint32_t nv = (uint32_t)(rnd() % 6u);
            uint32_t word = (nv * 0x111u) | ((nv * 0x2a5u) << 10) | ((nv * 0x13u) << 20);

            memset(s_b.light_memo, 0, sizeof s_b.light_memo);   /* the twin recomputes every time */
            memset(&s_b.st_lit_memo, 0, sizeof s_b.st_lit_memo);
            gpu3d_port(&s_a, 0x400u + 4u * 0x21u, word);
            gpu3d_port(&s_b, 0x400u + 4u * 0x21u, word);
            normals++;
            CHECK(memcmp(s_a.vcolor, s_b.vcolor, 3) == 0);
        } else if (pick < 44u) {
            both(0x30u, w & 0x7fffu, 0, 0);
        } else if (pick < 48u) {
            both(0x31u, w & 0x7fffu, 0, 0);
        } else if (pick < 52u) {
            both(0x32u, (w & 0x3fffffffu) | ((w >> 30) << 30), 0, 0);
        } else if (pick < 56u) {
            both(0x33u, (w & 0x7fffu) | ((rnd() & 3u) << 30), 0, 0);
        } else if (pick < 58u) {
            both(0x29u, (uint32_t)rnd() & 0x3f10ffu, 0, 0);
            both(0x40u, (uint32_t)(rnd() % 4u), 0, 0);
        } else if (pick < 60u) {
            both(0x1bu, 0x800u + (w & 0x1fffu), 0x1000u, 0x1000u);
        } else if (pick < 62u) {
            both(0x2au, (uint32_t)(rnd() % 4u) << 30, 0, 0);
        } else {
            both(0x33u, (w & 0x7fffu) | ((rnd() & 3u) << 30), 0, 0);
        }
    }
    hits = s_a.st_lit_memo;
    CHECK(normals > 100000u && s_a.st_lit == s_b.st_lit);
    printf("  lighting memo: %u NORMALs, %u answered from the memo\n", normals, hits);
}

int main(void)
{
    test_div();
    test_verdict();
    test_memo();
    printf("gpu3d_cuts_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
