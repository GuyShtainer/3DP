/* asm_to_c_test.c -- host tests for slice 1.2b (NitroSDK mwcc asm bodies translated to C).
 *
 * Built and run by tools/host-tests/build.sh (`make host-tests`). The translated SDK functions are compiled
 * on the build machine (see build.sh for the shadow headers) and compared against independent reference
 * implementations written here from the documented semantics (GBATEK BIOS decompression formats, RC4, the
 * fx32 12-bit fixed point rules), never from the C under test. Prints "N checks, 0 failures"; exits non-zero
 * on any failure. Rules: ~/.claude/c-golden-rules.md (no goto, bounded loops, small functions, asserts).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nitro/types.h>
#include <nitro/fx/fx_mtx22.h>
#include <nitro/fx/fx_mtx33.h>
#include <nitro/fx/fx_mtx43.h>
#include <nitro/fx/fx_mtx44.h>
#include <nitro/mi/memory.h>
#include <nitro/mi/swap.h>
#include <nitro/mi/uncompress.h>
#include <nitro/math/qsort.h>
#include <nitro/os.h>
#include <nitro/cp/context.h>
#include <crypto/rc4.h>
#include <hw_os_hooks.h>
#include <gxasm.h>
#include "hw_gx.h"

/* ---- host stand-ins for the DS address space and the HWOS_* hooks -------------------------------------- */
HwRegs g_hwregs;
u8 g_hwmainmem[0x800000] __attribute__((aligned(4096)));
u8 g_hwsys[0x1000] __attribute__((aligned(4096)));
u8 g_hwitcm[0x8000], g_hwwram[0x8000], g_hwpltt[0x800], g_hwvram_bg[0x80000], g_hwvram_dbbg[0x20000], g_hwvram_obj[0x40000];
u8 g_hwvram_dbobj[0x20000], g_hwvram_lcdc[0xA4000] __attribute__((aligned(4096))), g_hwoam[0x800], g_hwctrdg_rom[16], g_hwbios[0x8000];
u32 SDK_AUTOLOAD_DTCM_START[0x1000];
#define host_ioreg (g_hwregs.b)
void HWOS_SpinWait(uint32_t cycle) { (void)cycle; }
/* the divider / sqrt restart hooks cp_context.c calls (source/hw/hw_devs.c on the 3DS): here straight onto the pure unit */
void hwcp_div_run(uint8_t *io);
void hwcp_sqrt_run(uint8_t *io);
void HWDEV_CpDivRun(void) { hwcp_div_run((uint8_t *)host_ioreg); }
void HWG3_Sync(void) {}
void HWG3_StatWrite(uint32_t off) { (void)off; }
void HWDEV_CpSqrtRun(void) { hwcp_sqrt_run((uint8_t *)host_ioreg); }

static uint32_t g_cpsr;
static uint32_t g_cp15[HWOS_CP15_COUNT];

uint32_t HWOS_CpsrRead(void) { return g_cpsr; }
void HWOS_CpsrWriteC(uint32_t value) { g_cpsr = (g_cpsr & ~0xffu) | (value & 0xffu); }
uint32_t HWOS_Cp15Read(HWOSCp15Reg reg) { assert((int)reg >= 0 && reg < HWOS_CP15_COUNT); return g_cp15[reg]; }
void HWOS_Cp15Write(HWOSCp15Reg reg, uint32_t value) { assert((int)reg >= 0 && reg < HWOS_CP15_COUNT); g_cp15[reg] = value; }

/* ---- harness ------------------------------------------------------------------------------------------- */
static long g_checks;
static long g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    assert(g_checks >= 0);
    g_checks++;
    if (!ok) {
        g_fails++;
        if (g_fails <= 40) {
            fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
        }
    }
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    assert(g_rng != 0);
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    assert(g_rng != 0);
    return g_rng;
}

/* ============================ fx matrices ================================================================= */
typedef enum { K_IDENT, K_COPY, K_TRANS, K_SCALE, K_ROTX, K_ROTY, K_ROTZ } FxKind;
typedef void (*FxAdapter)(s32 *out, const s32 *in, s32 a, s32 b, s32 c);

typedef struct {
    const char *name;
    FxKind kind;
    FxAdapter fn;
    int sr, sc;  /* source rows/cols (0 for none) */
    int dr, dc;  /* destination rows/cols */
} FxCase;

#define AD_ID(adapter, fn, T) static void adapter(s32 *o, const s32 *i, s32 a, s32 b, s32 c) { (void)i; (void)a; (void)b; (void)c; fn((T *)o); }
#define AD_CP(adapter, fn, TS, TD) static void adapter(s32 *o, const s32 *i, s32 a, s32 b, s32 c) { (void)a; (void)b; (void)c; fn((const TS *)i, (TD *)o); }
#define AD_SC2(adapter, fn, T) static void adapter(s32 *o, const s32 *i, s32 a, s32 b, s32 c) { (void)i; (void)c; fn((T *)o, a, b); }
#define AD_SC3(adapter, fn, T) static void adapter(s32 *o, const s32 *i, s32 a, s32 b, s32 c) { (void)i; fn((T *)o, a, b, c); }

AD_ID(a_id22, MTX_Identity22_, MtxFx22)
AD_ID(a_id33, MTX_Identity33_, MtxFx33)
AD_ID(a_id43, MTX_Identity43_, MtxFx43)
AD_ID(a_id44, MTX_Identity44_, MtxFx44)
AD_CP(a_c33_43, MTX_Copy33To43_, MtxFx33, MtxFx43)
AD_CP(a_c33_44, MTX_Copy33To44_, MtxFx33, MtxFx44)
AD_CP(a_c43_44, MTX_Copy43To44_, MtxFx43, MtxFx44)
AD_CP(a_c44_33, MTX_Copy44To33_, MtxFx44, MtxFx33)
AD_CP(a_c44_43, MTX_Copy44To43_, MtxFx44, MtxFx43)
AD_CP(a_t22, MTX_Transpose22_, MtxFx22, MtxFx22)
AD_CP(a_t33, MTX_Transpose33_, MtxFx33, MtxFx33)
AD_CP(a_t43, MTX_Transpose43_, MtxFx43, MtxFx43)
AD_CP(a_t44, MTX_Transpose44_, MtxFx44, MtxFx44)
AD_SC2(a_s22, MTX_Scale22_, MtxFx22)
AD_SC3(a_s33, MTX_Scale33_, MtxFx33)
AD_SC3(a_s43, MTX_Scale43_, MtxFx43)
AD_SC3(a_s44, MTX_Scale44_, MtxFx44)
AD_SC2(a_r22, MTX_Rot22_, MtxFx22)
AD_SC2(a_rx33, MTX_RotX33_, MtxFx33)
AD_SC2(a_ry33, MTX_RotY33_, MtxFx33)
AD_SC2(a_rz33, MTX_RotZ33_, MtxFx33)
AD_SC2(a_rx43, MTX_RotX43_, MtxFx43)
AD_SC2(a_ry43, MTX_RotY43_, MtxFx43)
AD_SC2(a_rz43, MTX_RotZ43_, MtxFx43)
AD_SC2(a_rx44, MTX_RotX44_, MtxFx44)
AD_SC2(a_ry44, MTX_RotY44_, MtxFx44)
AD_SC2(a_rz44, MTX_RotZ44_, MtxFx44)

static const FxCase k_fx[] = {
    {"Identity22", K_IDENT, a_id22, 0, 0, 2, 2}, {"Identity33", K_IDENT, a_id33, 0, 0, 3, 3},
    {"Identity43", K_IDENT, a_id43, 0, 0, 4, 3}, {"Identity44", K_IDENT, a_id44, 0, 0, 4, 4},
    {"Copy33To43", K_COPY, a_c33_43, 3, 3, 4, 3}, {"Copy33To44", K_COPY, a_c33_44, 3, 3, 4, 4},
    {"Copy43To44", K_COPY, a_c43_44, 4, 3, 4, 4}, {"Copy44To33", K_COPY, a_c44_33, 4, 4, 3, 3},
    {"Copy44To43", K_COPY, a_c44_43, 4, 4, 4, 3},
    {"Transpose22", K_TRANS, a_t22, 2, 2, 2, 2}, {"Transpose33", K_TRANS, a_t33, 3, 3, 3, 3},
    {"Transpose43", K_TRANS, a_t43, 4, 3, 4, 3}, {"Transpose44", K_TRANS, a_t44, 4, 4, 4, 4},
    {"Scale22", K_SCALE, a_s22, 0, 0, 2, 2}, {"Scale33", K_SCALE, a_s33, 0, 0, 3, 3},
    {"Scale43", K_SCALE, a_s43, 0, 0, 4, 3}, {"Scale44", K_SCALE, a_s44, 0, 0, 4, 4},
    {"Rot22", K_ROTZ, a_r22, 0, 0, 2, 2},
    {"RotX33", K_ROTX, a_rx33, 0, 0, 3, 3}, {"RotY33", K_ROTY, a_ry33, 0, 0, 3, 3}, {"RotZ33", K_ROTZ, a_rz33, 0, 0, 3, 3},
    {"RotX43", K_ROTX, a_rx43, 0, 0, 4, 3}, {"RotY43", K_ROTY, a_ry43, 0, 0, 4, 3}, {"RotZ43", K_ROTZ, a_rz43, 0, 0, 4, 3},
    {"RotX44", K_ROTX, a_rx44, 0, 0, 4, 4}, {"RotY44", K_ROTY, a_ry44, 0, 0, 4, 4}, {"RotZ44", K_ROTZ, a_rz44, 0, 0, 4, 4},
};

static s32 neg32(s32 v) { return (s32)(0u - (u32)v); }  /* two's complement wrap, as the asm rsb does */

static s32 ref_diag(int r, s32 x, s32 y, s32 z)
{
    assert(r >= 0 && r < 4);
    return r == 0 ? x : (r == 1 ? y : (r == 2 ? z : 4096));
}

/* Reference for one destination cell. src is row-major with the case's source column count. */
static s32 ref_cell(const FxCase *c, const s32 *src, int r, int cc, s32 a, s32 b, s32 z)
{
    assert(c != NULL);
    assert(r >= 0 && r < c->dr && cc >= 0 && cc < c->dc);
    switch (c->kind) {
    case K_IDENT: return r == cc ? 4096 : 0;
    case K_COPY: return (r < c->sr && cc < c->sc) ? src[r * c->sc + cc] : (r == cc ? 4096 : 0);
    case K_TRANS: return (cc < c->sr && r < c->sc) ? src[cc * c->sc + r] : 0;
    case K_SCALE: return r == cc ? ref_diag(r, a, b, z) : 0;
    case K_ROTX: /* a = sin, b = cos; identity with the 1-2 plane rotated */
        if (r == 1 && cc == 1) return b;
        if (r == 1 && cc == 2) return a;
        if (r == 2 && cc == 1) return neg32(a);
        if (r == 2 && cc == 2) return b;
        return r == cc ? 4096 : 0;
    case K_ROTY:
        if (r == 0 && cc == 0) return b;
        if (r == 0 && cc == 2) return neg32(a);
        if (r == 2 && cc == 0) return a;
        if (r == 2 && cc == 2) return b;
        return r == cc ? 4096 : 0;
    default: /* K_ROTZ */
        if (r == 0 && cc == 0) return b;
        if (r == 0 && cc == 1) return a;
        if (r == 1 && cc == 0) return neg32(a);
        if (r == 1 && cc == 1) return b;
        return r == cc ? 4096 : 0;
    }
}

static void fx_run_case(const FxCase *c, const s32 *src, s32 a, s32 b, s32 z)
{
    s32 out[24];
    s32 in[16];
    int r, cc, n, bad = 0;

    assert(c != NULL && src != NULL);
    memcpy(in, src, sizeof in);
    for (n = 0; n < 24; n++) out[n] = 0x5A5A5A5A;
    c->fn(out, in, a, b, z);
    for (r = 0; r < c->dr; r++) {
        for (cc = 0; cc < c->dc; cc++) {
            if (out[r * c->dc + cc] != ref_cell(c, in, r, cc, a, b, z)) bad++;
        }
    }
    for (n = c->dr * c->dc; n < 24; n++) {
        if (out[n] != 0x5A5A5A5A) bad++;  /* wrote past the matrix */
    }
    check(bad == 0, c->name, bad);
}

static void fx_inplace_transposes(void)
{
    s32 m[16], ref[16];
    int i, r, c;

    assert(sizeof(MtxFx44) == 64 && sizeof(MtxFx43) == 48);
    for (i = 0; i < 16; i++) m[i] = (s32)rnd();
    memcpy(ref, m, sizeof m);
    MTX_Transpose44_((const MtxFx44 *)m, (MtxFx44 *)m);
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) if (m[r * 4 + c] != ref[c * 4 + r]) i = -1;
    check(i != -1, "Transpose44 in place", 0);
    for (i = 0; i < 16; i++) m[i] = (s32)rnd();
    memcpy(ref, m, sizeof m);
    MTX_Transpose33_((const MtxFx33 *)m, (MtxFx33 *)m);
    for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) if (m[r * 3 + c] != ref[c * 3 + r]) i = -1;
    check(i != -1, "Transpose33 in place", 0);
    for (i = 0; i < 16; i++) m[i] = (s32)rnd();
    memcpy(ref, m, sizeof m);
    MTX_Transpose43_((const MtxFx43 *)m, (MtxFx43 *)m);
    for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) if (m[r * 3 + c] != ref[c * 3 + r]) i = -1;
    check(i != -1 && m[9] == 0 && m[10] == 0 && m[11] == 0, "Transpose43 in place", 0);
}

static void test_fx_matrices(void)
{
    static const s32 trig[] = {0, 1, -1, 4096, -4096, 2896, 0x7fffffff, (s32)0x80000000, 0x12345678};
    s32 src[16];
    size_t k;
    int t, i;

    assert(sizeof k_fx / sizeof k_fx[0] == 27);
    assert(sizeof(MtxFx22) == 16 && sizeof(MtxFx33) == 36);
    for (k = 0; k < sizeof k_fx / sizeof k_fx[0]; k++) {
        for (t = 0; t < (int)(sizeof trig / sizeof trig[0]); t++) {
            for (i = 0; i < 16; i++) src[i] = (i < 3) ? trig[(t + i) % 9] : (s32)rnd();
            fx_run_case(&k_fx[k], src, trig[t], trig[(t + 3) % 9], trig[(t + 5) % 9]);
        }
    }
    fx_inplace_transposes();
}

/* fx32 products: the translated MTX_ScaleApply22 / MTX_Concat22 go through mul64 and a 64-bit sum. Reference
   uses explicit floor division so it does not depend on the shift operator. */
static s32 ref_floor_div4096(int64_t p)
{
    int64_t q = p / 4096;
    if (p % 4096 < 0) q -= 1;
    return (s32)q;
}

static void test_fx_products(void)
{
    static const s32 v[] = {0, 1, -1, 4095, 4096, 4097, -4097, 0x7fffffff, (s32)0x80000000, 0x40000000, -0x3fffffff, 123456, -654321};
    MtxFx22 a, d;
    int i, j, bad = 0;
    s32 x, y;

    assert(sizeof v / sizeof v[0] == 13);
    for (i = 0; i < 13; i++) {
        for (j = 0; j < 13; j++) {
            x = v[i]; y = v[j];
            a._00 = v[(i + j) % 13]; a._01 = v[(i + 2 * j) % 13]; a._10 = v[(3 * i + j) % 13]; a._11 = v[(i + 5) % 13];
            MTX_ScaleApply22(&a, &d, x, y);
            if (d._00 != ref_floor_div4096((int64_t)x * a._00) || d._01 != ref_floor_div4096((int64_t)x * a._01) ||
                d._10 != ref_floor_div4096((int64_t)y * a._10) || d._11 != ref_floor_div4096((int64_t)y * a._11)) bad++;
        }
    }
    check(bad == 0, "ScaleApply22 fx32 products (overflow edges, negative rounding)", bad);
    check(ref_floor_div4096(-1) == -1 && ref_floor_div4096(-4096) == -1 && ref_floor_div4096(-4097) == -2, "floor ref sanity", 0);
}

static void test_fx_concat(void)
{
    static const s32 v[] = {0, 1, -1, 4095, 4096, -4097, 0x3fffffff, -0x40000000, 77777, -88888};
    MtxFx22 a, b, ab, alias;
    int i, j, bad = 0;

    assert(sizeof v / sizeof v[0] == 10);
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            a._00 = v[i]; a._01 = v[j]; a._10 = v[(i + j) % 10]; a._11 = v[(i * 3 + 1) % 10];
            b._00 = v[(j + 2) % 10]; b._01 = v[(i + 7) % 10]; b._10 = v[(i + 4) % 10]; b._11 = v[j];
            MTX_Concat22(&a, &b, &ab);
            alias = b;
            MTX_Concat22(&a, &alias, &alias);
            if (ab._00 != ref_floor_div4096((int64_t)a._00 * b._00 + (int64_t)a._01 * b._10) ||
                ab._01 != ref_floor_div4096((int64_t)a._00 * b._01 + (int64_t)a._01 * b._11) ||
                ab._10 != ref_floor_div4096((int64_t)a._10 * b._00 + (int64_t)a._11 * b._10) ||
                ab._11 != ref_floor_div4096((int64_t)a._10 * b._01 + (int64_t)a._11 * b._11)) bad++;
            if (memcmp(&ab, &alias, sizeof ab) != 0) bad++;
        }
    }
    check(bad == 0, "Concat22 products and ab==b aliasing", bad);
}

/* ============================ mi_memory / mi_swap ========================================================= */
#define BUFSZ 256
typedef enum { M_COPY16, M_COPY32, M_COPYFAST, M_SEND16, M_SEND32, M_COPY8 } CopyKind;

static void do_copy(CopyKind k, const u8 *s, u8 *d, u32 n)
{
    assert(s != NULL && d != NULL);
    switch (k) {
    case M_COPY16: MIi_CpuCopy16(s, d, n); break;
    case M_COPY32: MIi_CpuCopy32(s, d, n); break;
    case M_COPYFAST: MIi_CpuCopyFast(s, d, n); break;
    case M_SEND16: MIi_CpuSend16(s, (volatile void *)d, n); break;
    case M_SEND32: MIi_CpuSend32(s, (volatile void *)d, n); break;
    default: MI_CpuCopy8(s, d, n); break;
    }
}

/* every (srcOff, dstOff, length) combination for one unit size; the whole destination buffer is compared
   so an off-by-one over- or under-write shows up. */
static void copy_sweep(CopyKind k, u32 unit, const char *name)
{
    static u8 src[BUFSZ] __attribute__((aligned(16)));
    static u8 dst[BUFSZ] __attribute__((aligned(16)));
    static u8 ref[BUFSZ] __attribute__((aligned(16)));
    u32 so, dof, n, i, bad = 0, cases = 0;

    assert(unit == 1 || unit == 2 || unit == 4);
    assert(BUFSZ >= 3 * 64);
    for (i = 0; i < BUFSZ; i++) src[i] = (u8)(rnd() >> 7);
    for (so = 0; so < 32; so += unit) {
        for (dof = 0; dof < 32; dof += unit) {
            for (n = 0; n <= 64; n += unit) {
                for (i = 0; i < BUFSZ; i++) dst[i] = ref[i] = (u8)(0xA0 + i);
                if (k == M_SEND16 || k == M_SEND32) {
                    /* Send = FIFO write: the destination address is fixed, so only the last unit survives. */
                    for (i = 0; n > 0 && i < unit; i++) ref[96 + dof + i] = src[64 + so + n - unit + i];
                } else {
                    for (i = 0; i < n; i++) ref[96 + dof + i] = src[64 + so + i];
                }
                do_copy(k, src + 64 + so, dst + 96 + dof, n);
                bad += memcmp(dst, ref, BUFSZ) != 0;
                cases++;
            }
        }
    }
    check(bad == 0 && cases > 0, name, bad);
}

typedef enum { F_16, F_32, F_FAST, F_8 } FillKind;

static void do_fill(FillKind k, u8 *d, u32 data, u32 n)
{
    assert(d != NULL);
    switch (k) {
    case F_16: MIi_CpuClear16((u16)data, d, n); break;
    case F_32: MIi_CpuClear32(data, d, n); break;
    case F_FAST: MIi_CpuClearFast(data, d, n); break;
    default: MI_CpuFill8(d, (u8)data, n); break;
    }
}

static void fill_sweep(FillKind k, u32 unit, const char *name)
{
    static u8 dst[BUFSZ] __attribute__((aligned(16)));
    static u8 ref[BUFSZ] __attribute__((aligned(16)));
    u32 dof, n, i, bad = 0, data = 0xC3A5F00Du;
    u32 mask = (unit == 1) ? 0xffu : (unit == 2 ? 0xffffu : 0xffffffffu);

    assert(unit == 1 || unit == 2 || unit == 4);
    assert(BUFSZ >= 128 + 64 + 32);
    for (dof = 0; dof < 32; dof += unit) {
        for (n = 0; n <= 64; n += unit) {
            for (i = 0; i < BUFSZ; i++) dst[i] = ref[i] = (u8)(0x30 + i);
            for (i = 0; i < n; i++) ref[64 + dof + i] = (u8)(((data & mask) >> (8 * (i % unit))) & 0xff);
            do_fill(k, dst + 64 + dof, data & mask, n);
            bad += memcmp(dst, ref, BUFSZ) != 0;
        }
    }
    check(bad == 0, name, bad);
}

typedef void (*CopyBlk)(const void *, void *);
typedef void (*ZeroBlk)(void *);

static void block_copy_case(CopyBlk f, u32 n, const char *name)
{
    static u32 srcw[64], dstw[96], refw[96];
    u8 *d = (u8 *)dstw, *r = (u8 *)refw;
    u32 off, i, bad = 0;

    assert(n % 4 == 0 && n <= 128);
    assert(sizeof dstw >= 128 + 32);
    for (off = 0; off < 16; off += 4) {
        for (i = 0; i < 64; i++) srcw[i] = rnd();
        for (i = 0; i < 96; i++) dstw[i] = refw[i] = 0x11223344u + i;
        memcpy(r + 32 + off, srcw, n);
        f(srcw, d + 32 + off);
        bad += memcmp(d, r, sizeof dstw) != 0;
    }
    check(bad == 0, name, bad);
}

static void block_zero_case(ZeroBlk f, u32 n, const char *name)
{
    static u32 dstw[64], refw[64];
    u32 off, i, bad = 0;

    assert(n % 4 == 0 && n <= 64);
    assert(sizeof dstw >= 64 + 32);
    for (off = 0; off < 16; off += 4) {
        for (i = 0; i < 64; i++) dstw[i] = refw[i] = 0xDEADBEEFu + i;
        memset((u8 *)refw + 16 + off, 0, n);
        f((u8 *)dstw + 16 + off);
        bad += memcmp(dstw, refw, sizeof dstw) != 0;
    }
    check(bad == 0, name, bad);
}

static void test_mi_memory(void)
{
    copy_sweep(M_COPY16, 2, "MIi_CpuCopy16 all alignments, len 0..64");
    copy_sweep(M_COPY32, 4, "MIi_CpuCopy32 all alignments, len 0..64");
    copy_sweep(M_COPYFAST, 4, "MIi_CpuCopyFast all alignments, len 0..64");
    copy_sweep(M_SEND16, 2, "MIi_CpuSend16 all alignments, len 0..64");
    copy_sweep(M_SEND32, 4, "MIi_CpuSend32 all alignments, len 0..64");
    copy_sweep(M_COPY8, 1, "MI_CpuCopy8 all alignments, len 0..64");
    fill_sweep(F_16, 2, "MIi_CpuClear16 (fill) alignments, len 0..64");
    fill_sweep(F_32, 4, "MIi_CpuClear32 (fill) alignments, len 0..64");
    fill_sweep(F_FAST, 4, "MIi_CpuClearFast (fill) alignments, len 0..64");
    fill_sweep(F_8, 1, "MI_CpuFill8 alignments, len 0..64");
    block_copy_case(MI_Copy16B, 16, "MI_Copy16B");
    block_copy_case(MI_Copy32B, 32, "MI_Copy32B");
    block_copy_case(MI_Copy36B, 36, "MI_Copy36B");
    block_copy_case(MI_Copy48B, 48, "MI_Copy48B");
    block_copy_case(MI_Copy64B, 64, "MI_Copy64B");
    block_copy_case(MI_Copy128B, 128, "MI_Copy128B");
    block_zero_case(MI_Zero32B, 32, "MI_Zero32B");
    block_zero_case(MI_Zero36B, 36, "MI_Zero36B");
    block_zero_case(MI_Zero48B, 48, "MI_Zero48B");
    block_zero_case(MI_Zero64B, 64, "MI_Zero64B");
}

static void test_mi_swap(void)
{
    volatile u32 w = 0x11223344u;
    volatile u8 bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    u32 oldw;
    u8 oldb;
    int i, ok = 1;

    assert(sizeof w == 4);
    assert(sizeof bytes == 8);
    oldw = MI_SwapWord(0xAABBCCDDu, &w);
    check(oldw == 0x11223344u && w == 0xAABBCCDDu, "MI_SwapWord returns old, stores new", oldw);
    oldw = MI_SwapWord(0, &w);
    check(oldw == 0xAABBCCDDu && w == 0, "MI_SwapWord second swap", oldw);
    for (i = 0; i < 8; i++) {
        oldb = MI_SwapByte((u32)(0xF0 + i), &bytes[i]);
        if (oldb != (u8)(i + 1) || bytes[i] != (u8)(0xF0 + i)) ok = 0;
    }
    check(ok, "MI_SwapByte on every byte of a word", 0);
    oldb = MI_SwapByte(0x1234u, &bytes[3]);
    check(oldb == 0xF3 && bytes[3] == 0x34 && bytes[2] == 0xF2 && bytes[4] == 0xF4, "MI_SwapByte stores low byte only", bytes[3]);
}

/* ============================ qsort ======================================================================= */
typedef struct { s32 key; s32 id; } Rec;
static s32 cmp_key(void *a, void *b)
{
    s32 x = ((Rec *)a)->key, y = ((Rec *)b)->key;
    assert(a != NULL && b != NULL);
    return x < y ? -1 : (x > y ? 1 : 0);
}
static s32 cmp_s16(void *a, void *b) { assert(a && b); return (s32)*(s16 *)a - (s32)*(s16 *)b; }

static int recs_sorted_perm(const Rec *r, int n)
{
    static unsigned char seen[4096];
    int i;

    assert(n >= 0 && n <= 4096);
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        if (i > 0 && r[i - 1].key > r[i].key) return 0;
        if (r[i].id < 0 || r[i].id >= n || seen[r[i].id]) return 0;
        seen[r[i].id] = 1;
    }
    return 1;
}

static void qsort_case(int n, int mode, int useStack, const char *name)
{
    static Rec r[4096];
    static u32 stk[128];
    int i;

    assert(n >= 0 && n <= 4096);
    assert(mode >= 0 && mode < 5);
    for (i = 0; i < n; i++) {
        r[i].id = i;
        switch (mode) {
        case 0: r[i].key = (s32)(rnd() % 7); break;            /* heavy duplicates */
        case 1: r[i].key = (s32)rnd(); break;                   /* full range, negatives */
        case 2: r[i].key = i; break;                            /* already sorted */
        case 3: r[i].key = n - i; break;                        /* reversed */
        default: r[i].key = 5; break;                           /* all equal */
        }
    }
    if (getenv("HOST_TEST_TRACE")) fprintf(stderr, "qs n=%d mode=%d stack=%d\n", n, mode, useStack);
    MATH_QSort(r, (u32)n, sizeof(Rec), cmp_key, useStack ? stk : NULL);
    check(recs_sorted_perm(r, n), name, n);
}

static void test_qsort(void)
{
    static const int sizes[] = {0, 1, 2, 3, 4, 5, 8, 17, 64, 100, 513, 2000, 4096};
    int s, mode;
    s16 w[9] = {5, -3, 5, 0, -3, 9, 5, 1, -3};
    s16 sorted[9] = {-3, -3, -3, 0, 1, 5, 5, 5, 9};
    char nm[64];

    assert(sizeof(void *) * 64 <= 128 * sizeof(u32));  /* caller stack: 64 pointer slots (host pointers are 8 bytes) */
    assert(sizeof(Rec) == 8);
    for (s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++) {
        for (mode = 0; mode < 5; mode++) {
            snprintf(nm, sizeof nm, "qsort n=%d mode=%d caller stack", sizes[s], mode);
            qsort_case(sizes[s], mode, 1, nm);
            snprintf(nm, sizeof nm, "qsort n=%d mode=%d local stack", sizes[s], mode);
            qsort_case(sizes[s], mode, 0, nm);
        }
    }
    MATH_QSort(w, 9, sizeof(s16), cmp_s16, NULL);
    check(memcmp(w, sorted, sizeof w) == 0, "qsort s16 width 2 with duplicates", w[0]);
}

/* ============================ RC4 ========================================================================= */
static void test_rc4(void)
{
    static const u8 key[] = "Key";
    static const u8 plain[] = "Plaintext";
    static const u8 expect[9] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};  /* Wikipedia RC4 vector */
    CRYPTORC4Context c1, c2;
    CRYPTORC4FastContext f1;
    u8 out[64], big[300], o1[300], o2[300], o3[300];
    u32 i;

    assert(sizeof(CRYPTORC4Context) == 260);
    assert(sizeof(CRYPTORC4FastContext) == 8 + 1024);
    CRYPTO_RC4Init(&c1, key, 3);
    CRYPTO_RC4Encrypt(&c1, plain, 9, out);
    check(memcmp(out, expect, 9) == 0, "RC4 known vector Key/Plaintext", out[0]);
    CRYPTO_RC4Init(&c1, key, 3);
    CRYPTO_RC4Decrypt(&c1, expect, 9, out);
    check(memcmp(out, plain, 9) == 0, "RC4 decrypt round trip", out[0]);
    CRYPTO_RC4Init(&c1, key, 3);
    CRYPTO_RC4Encrypt(&c1, plain, 4, out);
    CRYPTO_RC4Encrypt(&c1, plain + 4, 0, out + 4);
    CRYPTO_RC4Encrypt(&c1, plain + 4, 5, out + 4);
    check(memcmp(out, expect, 9) == 0, "RC4 chunked 4+0+5 keeps ctx state", out[4]);
    for (i = 0; i < sizeof big; i++) big[i] = (u8)rnd();
    CRYPTO_RC4Init(&c1, "0123456789abcdef", 16);
    CRYPTO_RC4Encrypt(&c1, big, 300, o1);
    CRYPTO_RC4Init(&c2, "0123456789abcdef", 16);
    CRYPTO_RC4Encrypt(&c2, big, 77, o2);
    CRYPTO_RC4Encrypt(&c2, big + 77, 223, o2 + 77);
    check(memcmp(o1, o2, 300) == 0 && c1.i == c2.i && c1.j == c2.j, "RC4 300 bytes whole vs split", c1.i);
    CRYPTO_RC4FastInit(&f1, "0123456789abcdef", 16);
    CRYPTO_RC4FastEncrypt(&f1, big, 300, o3);
    check(memcmp(o1, o3, 300) == 0, "RC4 fast matches plain RC4 over 300 bytes", o3[0]);
    CRYPTO_RC4FastInit(&f1, key, 3);
    CRYPTO_RC4FastEncrypt(&f1, plain, 4, out);
    CRYPTO_RC4FastEncrypt(&f1, plain + 4, 5, out + 4);
    check(memcmp(out, expect, 9) == 0 && (f1.i & 0x00ffffff) == 0, "RC4 fast known vector, chunked, i/j in top byte", f1.i);
}

/* ============================ decompression ================================================================
   Test-side encoders for the BIOS formats (GBATEK "BIOS Decompression Functions"), written from the format
   description. Every stream starts with the 4-byte header: low byte = type | param, upper 24 bits = size. */
#define SRCMAX 4096
#define STREAMMAX 12288

static void put_header(u8 *o, u32 type, u32 size)
{
    assert(o != NULL);
    assert(size < (1u << 24));
    o[0] = (u8)type;
    o[1] = (u8)size;
    o[2] = (u8)(size >> 8);
    o[3] = (u8)(size >> 16);
}

static u32 enc_rl(const u8 *s, u32 n, u8 *out)
{
    u32 i = 0, o = 4, run, lit;

    assert(s != NULL && out != NULL);
    assert(n <= SRCMAX);
    put_header(out, 0x30, n);
    while (i < n) {
        for (run = 1; i + run < n && s[i + run] == s[i] && run < 130; run++) {}
        if (run >= 3) {
            out[o++] = (u8)(0x80 | (run - 3));
            out[o++] = s[i];
            i += run;
        } else {
            for (lit = 0; i + lit < n && lit < 128; lit++) {
                if (lit >= 2 && s[i + lit] == s[i + lit - 1] && s[i + lit] == s[i + lit - 2]) { lit -= 2; break; }
            }
            if (lit == 0) lit = 1;
            out[o++] = (u8)(lit - 1);
            memcpy(out + o, s + i, lit);
            o += lit;
            i += lit;
        }
    }
    return o;
}

/* greedy LZ77: window 4096, min match 3, disp >= minDisp. ext = the 0x11 variant (header param 1). */
static u32 lz_best(const u8 *s, u32 i, u32 n, u32 minDisp, u32 maxLen, u32 *dispOut)
{
    u32 best = 0, d, l, lo = (i > 4096) ? i - 4096 : 0;

    assert(dispOut != NULL && minDisp >= 1);
    assert(i <= n);
    for (d = minDisp; d <= i - lo; d++) {
        for (l = 0; i + l < n && l < maxLen && s[i + l] == s[i + l - d]; l++) {}
        if (l > best) { best = l; *dispOut = d; }
    }
    return best >= 3 ? best : 0;
}

static u32 emit_ref(u8 *out, u32 o, u32 len, u32 disp, int ext)
{
    u32 dv = disp - 1, v;

    assert(len >= 3 && disp >= 1 && disp <= 4096);
    if (!ext) {
        out[o++] = (u8)(((len - 3) << 4) | (dv >> 8));
    } else if (len <= 16) {
        out[o++] = (u8)(((len - 1) << 4) | (dv >> 8));
    } else if (len <= 0x110) {
        v = len - 0x11;
        out[o++] = (u8)(v >> 4);
        out[o++] = (u8)(((v & 0xF) << 4) | (dv >> 8));
    } else {
        v = len - 0x111;
        out[o++] = (u8)(0x10 | (v >> 12));
        out[o++] = (u8)(v >> 4);
        out[o++] = (u8)(((v & 0xF) << 4) | (dv >> 8));
    }
    out[o++] = (u8)dv;
    return o;
}

static u32 enc_lz(const u8 *s, u32 n, u8 *out, int ext, u32 minDisp)
{
    u32 i = 0, o = 4, flagPos = 0, bit = 8, len, disp = 0, maxLen = ext ? 0x10110u : 18u;

    assert(s != NULL && out != NULL);
    assert(n <= SRCMAX);
    put_header(out, 0x10 | (ext ? 1 : 0), n);
    while (i < n) {
        if (bit == 8) { flagPos = o++; out[flagPos] = 0; bit = 0; }
        len = lz_best(s, i, n, minDisp, maxLen, &disp);
        if (len) {
            out[flagPos] |= (u8)(0x80 >> bit);
            o = emit_ref(out, o, len, disp, ext);
            i += len;
        } else {
            out[o++] = s[i++];
        }
        bit++;
    }
    return o;
}

/* ---- Huffman: codes given as (code, length) per symbol; tree laid out breadth first ------------------ */
typedef struct { u16 code; u8 len; } HCode;
typedef struct { int child[2]; int sym; } HNode;

static int hn_insert(HNode *t, int *cnt, const HCode *c, int sym)
{
    int node = 0, b;

    assert(t != NULL && cnt != NULL);
    assert(c->len >= 1 && c->len <= 12);
    for (b = c->len - 1; b >= 0; b--) {
        int bit = (c->code >> b) & 1;
        if (t[node].child[bit] == 0) { t[node].child[bit] = ++*cnt; t[*cnt].child[0] = t[*cnt].child[1] = 0; t[*cnt].sym = -1; }
        node = t[node].child[bit];
    }
    t[node].sym = sym;
    return node;
}

static u32 huff_tree(const HCode *codes, int nsym, u8 *tbl)
{
    HNode t[600];
    int cnt = 0, q[600], slot[600], head = 0, tail = 0, pair = 2, i;

    assert(codes != NULL && tbl != NULL);
    assert(nsym >= 2 && nsym <= 256);
    t[0].child[0] = t[0].child[1] = 0; t[0].sym = -1;
    for (i = 0; i < nsym; i++) if (codes[i].len) hn_insert(t, &cnt, &codes[i], i);
    memset(tbl, 0, 520);
    q[tail] = 0; slot[tail++] = 1;
    while (head < tail) {
        int n = q[head], idx = slot[head++], k, off, flagsv = 0;
        off = (pair - (idx & ~1) - 2) / 2;
        assert(off >= 0 && off < 64);
        for (k = 0; k < 2; k++) {
            int ch = t[n].child[k];
            if (t[ch].sym >= 0) { tbl[pair + k] = (u8)t[ch].sym; flagsv |= k ? 0x40 : 0x80; }
            else { q[tail] = ch; slot[tail++] = pair + k; }
        }
        tbl[idx] = (u8)(off | flagsv);
        pair += 2;
    }
    tbl[0] = (u8)(pair / 2 - 1);
    return (u32)pair;
}

static u32 enc_huff(const u8 *s, u32 nbytes, int bits, const HCode *codes, int nsym, u8 *out)
{
    u32 o = 4, tsz, nsymb = nbytes * 8 / (u32)bits, i, acc = 0, nacc = 0, sym;

    assert(s != NULL && out != NULL);
    assert(bits == 4 || bits == 8);
    put_header(out, 0x20 | (u32)bits, nbytes);
    tsz = huff_tree(codes, nsym, out + o);
    o += tsz;
    for (i = 0; i < nsymb; i++) {
        int b;
        sym = (bits == 8) ? s[i] : ((i & 1) ? (u32)(s[i / 2] >> 4) : (u32)(s[i / 2] & 15));
        for (b = codes[sym].len - 1; b >= 0; b--) {
            acc |= (u32)((codes[sym].code >> b) & 1) << (31 - nacc);
            if (++nacc == 32) { memcpy(out + o, &acc, 4); o += 4; acc = 0; nacc = 0; }
        }
    }
    if (nacc) { memcpy(out + o, &acc, 4); o += 4; }
    return o;
}

/* ---- sample data ------------------------------------------------------------------------------------- */
static u32 make_sample(int kind, u8 *d, u32 n)
{
    u32 i;

    assert(d != NULL);
    assert(n <= SRCMAX);
    for (i = 0; i < n; i++) {
        switch (kind) {
        case 0: d[i] = 0; break;
        case 1: d[i] = "abcabcabd"[i % 9]; break;
        case 2: d[i] = (u8)rnd(); break;
        case 3: d[i] = (u8)(((i / 37) % 3 == 0) ? 7 : (rnd() & 3)); break;
        default: d[i] = (u8)((i / 5) * 3 + (i % 5 == 0 ? 0 : 1)); break;
        }
    }
    return n;
}

static void dec_check(const char *what, const u8 *got, const u8 *want, u32 n, u32 guardFrom)
{
    u32 i, guardBad = 0;

    assert(got != NULL && want != NULL);
    for (i = guardFrom; i < guardFrom + 8; i++) guardBad += got[i] != 0xEE;
    check(memcmp(got, want, n) == 0 && guardBad == 0, what, (long)n);
}

/* The streaming readers (MI_ReadUncomp*, mi_uncomp_stream.c) are trap stubs since release P1: nothing in the game calls
   them, so only the whole-buffer decoders are tested here. */
static void rl_lz_family(int kind, u32 n)
{
    static u8 src[SRCMAX], enc[STREAMMAX], dest[SRCMAX + 64];
    u32 len;
    char nm[80];

    assert(n <= SRCMAX && n % 2 == 0);
    assert(kind >= 0 && kind < 5);
    make_sample(kind, src, n);

    len = enc_rl(src, n, enc);
    assert(len > 4 && len <= STREAMMAX);
    memset(dest, 0xEE, sizeof dest); MI_UncompressRL8(enc, dest);
    snprintf(nm, sizeof nm, "UncompressRL8 sample %d n=%u", kind, n); dec_check(nm, dest, src, n, n);
    memset(dest, 0xEE, sizeof dest); MI_UncompressRL16(enc, dest);
    snprintf(nm, sizeof nm, "UncompressRL16 sample %d n=%u", kind, n); dec_check(nm, dest, src, n, n);

    len = enc_lz(src, n, enc, 0, 1);
    assert(len > 4 && len <= STREAMMAX);
    memset(dest, 0xEE, sizeof dest); MI_UncompressLZ8(enc, dest);
    snprintf(nm, sizeof nm, "UncompressLZ8 sample %d n=%u", kind, n); dec_check(nm, dest, src, n, n);
    len = enc_lz(src, n, enc, 1, 1);
    assert(len > 4 && len <= STREAMMAX);
    memset(dest, 0xEE, sizeof dest); MI_UncompressLZ8(enc, dest);
    snprintf(nm, sizeof nm, "UncompressLZ8 ext sample %d n=%u", kind, n); dec_check(nm, dest, src, n, n);
}

static void lz16_family(int kind, u32 n)
{
    static u8 src[SRCMAX], enc[STREAMMAX], dest[SRCMAX + 64];
    u32 len, ext;
    char nm[80];

    assert(n <= SRCMAX && n % 2 == 0);
    make_sample(kind, src, n);
    for (ext = 0; ext < 2; ext++) {
        len = enc_lz(src, n, enc, (int)ext, 2);  /* LZ16 (VRAM-safe) streams never use displacement 1 */
        assert(len > 4 && len <= STREAMMAX);
        memset(dest, 0xEE, sizeof dest); MI_UncompressLZ16(enc, dest);
        snprintf(nm, sizeof nm, "UncompressLZ16 ext=%u sample %d n=%u", ext, kind, n); dec_check(nm, dest, src, n, n);
    }
}

static void test_rl_lz(void)
{
    static const u32 sizes[] = {2, 4, 6, 30, 130, 258, 600, 1024};
    int kind;
    u32 s;

    assert(sizeof sizes / sizeof sizes[0] == 8);
    assert(SRCMAX >= 1024);
    for (kind = 0; kind < 5; kind++) {
        for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            rl_lz_family(kind, sizes[s]);
            lz16_family(kind, sizes[s]);
        }
    }
}

static void test_lz_ext_forms(void)
{
    static u8 src[SRCMAX], enc[STREAMMAX], dest[SRCMAX + 64];
    u32 len, n = 3000;

    assert(n <= SRCMAX);
    assert(sizeof enc >= 2 * SRCMAX);
    memset(src, 0x42, n);  /* one long run: forces the 3-byte (len >= 0x111) extended form */
    len = enc_lz(src, n, enc, 1, 1);
    check(len < 40, "LZ ext encoder emitted the long form (sample is tiny)", len);
    memset(dest, 0xEE, sizeof dest); MI_UncompressLZ8(enc, dest);
    dec_check("UncompressLZ8 ext 3-byte length form", dest, src, n, n);
    src[0] = 1; src[1] = 2;
    len = enc_lz(src, n, enc, 1, 2);
    memset(dest, 0xEE, sizeof dest); MI_UncompressLZ16(enc, dest);
    dec_check("UncompressLZ16 ext long form, disp 2", dest, src, n, n);
}

/* ---- Huffman ----------------------------------------------------------------------------------------- */
static void huff_case(const HCode *codes, int nsym, int bits, int symUsed, u32 nbytes, const char *name)
{
    static u8 src[SRCMAX], enc[STREAMMAX], dest[SRCMAX + 64];
    u32 i, len, nsymb = nbytes * 8 / (u32)bits;

    assert(nbytes % 4 == 0 && nbytes <= SRCMAX);
    assert(symUsed >= 2 && symUsed <= nsym);
    memset(src, 0, sizeof src);
    for (i = 0; i < nsymb; i++) {
        u32 s = rnd() % (u32)symUsed;
        if (bits == 8) src[i] = (u8)s;
        else src[i / 2] |= (u8)(s << ((i & 1) * 4));
    }
    len = enc_huff(src, nbytes, bits, codes, nsym, enc);
    assert(len > 4 && len <= STREAMMAX);
    memset(dest, 0xEE, sizeof dest);
    MI_UncompressHuffman(enc, dest);
    dec_check(name, dest, src, nbytes, nbytes);
}

static void test_huffman(void)
{
    HCode bal4[16], skew4[4] = {{0, 1}, {2, 2}, {6, 3}, {7, 3}}, skew8[5] = {{0, 1}, {2, 2}, {6, 3}, {14, 4}, {15, 4}}, bal8[16];
    int i;

    assert(sizeof bal4 / sizeof bal4[0] == 16);
    assert(sizeof skew8 / sizeof skew8[0] == 5);
    for (i = 0; i < 16; i++) { bal4[i].code = (u16)i; bal4[i].len = 4; bal8[i].code = (u16)i; bal8[i].len = 4; }
    huff_case(bal4, 16, 4, 16, 4, "Huffman 4-bit balanced, 4 bytes");
    huff_case(bal4, 16, 4, 16, 64, "Huffman 4-bit balanced, 64 bytes");
    huff_case(bal4, 16, 4, 16, 1024, "Huffman 4-bit balanced, 1024 bytes");
    huff_case(skew4, 4, 4, 4, 8, "Huffman 4-bit skewed, 8 bytes");
    huff_case(skew4, 4, 4, 4, 512, "Huffman 4-bit skewed, 512 bytes");
    huff_case(skew8, 5, 8, 5, 4, "Huffman 8-bit skewed, 4 bytes");
    huff_case(skew8, 5, 8, 5, 400, "Huffman 8-bit skewed, 400 bytes");
    huff_case(bal8, 16, 8, 16, 256, "Huffman 8-bit 16 symbols balanced, 256 bytes");
}

/* ---- Diff filters and UnpackBits --------------------------------------------------------------------- */
static void test_diff(void)
{
    static u8 src[SRCMAX], enc[SRCMAX + 8], out[SRCMAX + 16];
    u32 n, i;

    assert(sizeof enc > 64);
    assert(sizeof out > 64);
    for (n = 4; n <= 512; n *= 2) {
        make_sample(2, src, n);
        put_header(enc, 0x81, n);
        enc[4] = src[0];
        for (i = 1; i < n; i++) enc[4 + i] = (u8)(src[i] - src[i - 1]);
        memset(out, 0xEE, sizeof out); MI_UnfilterDiff8(enc, out);
        dec_check("UnfilterDiff8", out, src, n, n);
        put_header(enc, 0x82, n);
        for (i = 0; i < n; i += 2) {
            u16 cur = (u16)(src[i] | (src[i + 1] << 8)), prev = i ? (u16)(src[i - 2] | (src[i - 1] << 8)) : 0;
            u16 dlt = (u16)(cur - prev);
            enc[4 + i] = (u8)dlt;
            enc[5 + i] = (u8)(dlt >> 8);
        }
        memset(out, 0xEE, sizeof out); MI_UnfilterDiff16(enc, out);
        dec_check("UnfilterDiff16 (halfword differences)", out, src, n, n);
    }
}

static u32 ref_unpack(const u8 *s, u32 nbytes, u32 sbits, u32 dbits, u32 offs, u32 zflag, u32 *w)
{
    uint64_t acc = 0;
    u32 bitpos = 0, nw = 0, i, k;

    assert(sbits == 1 || sbits == 2 || sbits == 4 || sbits == 8);
    assert(dbits >= 1 && dbits <= 32);
    for (i = 0; i < nbytes; i++) {
        for (k = 0; k < 8; k += sbits) {
            u32 v = (u32)(s[i] >> k) & ((1u << sbits) - 1u);
            if (v != 0 || zflag) v += offs;
            acc |= (uint64_t)v << bitpos;
            bitpos += dbits;
            if (bitpos >= 32) { w[nw++] = (u32)acc; acc >>= 32; bitpos -= 32; }
        }
    }
    return nw;
}

static void unpack_case(u32 nbytes, u32 sbits, u32 dbits, u32 offs, u32 zflag)
{
    static u8 src[64] __attribute__((aligned(4)));
    static u32 want[512], got[520];
    MIUnpackBitsParam p;
    u32 i, nw;
    char nm[80];

    assert(nbytes <= 64 && nbytes > 0);
    assert(sizeof got / sizeof got[0] >= 8 * 64 + 2);  /* 1-bit source units to 32-bit fields: 512 words */
    for (i = 0; i < nbytes; i++) src[i] = (u8)(rnd() >> 5);
    nw = ref_unpack(src, nbytes, sbits, dbits, offs, zflag, want);
    for (i = 0; i < 520; i++) got[i] = 0xEEEEEEEEu;
    memset(&p, 0, sizeof p);
    p.srcNum = (u16)nbytes; p.srcBitNum = (u16)sbits; p.destBitNum = (u16)dbits;
    p.destOffset = offs; p.destOffset0_on = zflag;
    MI_UnpackBits(src, got, &p);
    snprintf(nm, sizeof nm, "UnpackBits %u->%u bits, offset %u zflag %u, %u bytes", sbits, dbits, offs, zflag, nbytes);
    check(memcmp(got, want, nw * 4) == 0 && got[nw] == 0xEEEEEEEEu, nm, nw);
}

static void test_unpack(void)
{
    static const u32 combos[][2] = {{1, 4}, {1, 8}, {1, 16}, {1, 32}, {2, 8}, {2, 4}, {4, 8}, {4, 16}, {4, 32}, {8, 16}, {8, 32}};  /* BIOS destination widths: 1,2,4,8,16,32 */
    u32 i;

    assert(sizeof combos / sizeof combos[0] == 11);
    assert(sizeof(MIUnpackBitsParam) == 8);
    for (i = 0; i < 11; i++) {
        unpack_case(8, combos[i][0], combos[i][1], 0, 0);
        unpack_case(24, combos[i][0], combos[i][1], 0, 0);
        unpack_case(16, combos[i][0], combos[i][1], 0, 1);
        if (combos[i][1] >= 8 && combos[i][0] <= 4) {
            unpack_case(16, combos[i][0], combos[i][1], 0x10, 0);
            unpack_case(16, combos[i][0], combos[i][1], 0x10, 1);
        }
    }
    unpack_case(1, 1, 4, 0, 0);
    unpack_case(3, 8, 16, 0x100, 1);
}

/* ============================ cp, gx, os ================================================================= */
static void test_cp_context(void)
{
    CPContext c, d;
    int i;

    assert(sizeof(CPContext) == 28);   /* DS ABI: u64 aligned to 4 (nitro/types.h SDK_GCC), 3*8 + 2*2 */
    assert((size_t)REG_DIV_NUMER_ADDR == (size_t)host_ioreg + 0x290);
    memset(host_ioreg, 0, 0x400);
    for (i = 0; i < 4; i++) ((u32 *)(host_ioreg + 0x290))[i] = 0x1000u * (u32)(i + 1) + 0x11u;
    for (i = 4; i < 6; i++) ((u32 *)(host_ioreg + 0x2b8))[i - 4] = 0x1000u * (u32)(i + 1) + 0x11u;
    *(u16 *)(host_ioreg + 0x280) = 0x8003u;
    *(u16 *)(host_ioreg + 0x2b0) = 0x8001u;
    memset(&c, 0xAA, sizeof c);
    CP_SaveContext(&c);
    check(c.div_mode == (0x8003u & REG_CP_DIVCNT_MODE_MASK), "CP_SaveContext div mode masked", c.div_mode);
    check(c.sqrt_mode == (0x8001u & REG_CP_SQRTCNT_MODE_MASK), "CP_SaveContext sqrt mode masked", c.sqrt_mode);
    check(c.div_numer == (((u64)0x2011u << 32) | 0x1011u) && c.div_denom == (((u64)0x4011u << 32) | 0x3011u), "CP_SaveContext div words", 0);
    check(c.sqrt == (((u64)0x6011u << 32) | 0x5011u), "CP_SaveContext sqrt words", 0);
    memset(host_ioreg, 0, 0x400);
    d = c;
    CPi_RestoreContext(&d);
    for (i = 0; i < 4; i++) if (((u32 *)(host_ioreg + 0x290))[i] != 0x1000u * (u32)(i + 1) + 0x11u) break;
    for (i += 0; i < 6 && i >= 4; i++) if (((u32 *)(host_ioreg + 0x2b8))[i - 4] != 0x1000u * (u32)(i + 1) + 0x11u) break;
    check(i == 6, "CPi_RestoreContext writes the six data words", i);
    check(*(u16 *)(host_ioreg + 0x280) == c.div_mode && *(u16 *)(host_ioreg + 0x2B0) == c.sqrt_mode, "CPi_RestoreContext writes both control halfwords", 0);
}

void G3X_ClearFifo(void);  /* declared in <nitro/gx/g3x.h>, which pulls in the whole GX header tree */

static u32 sum4(const u32 *a)
{
    return a[0] + a[1] + a[2] + a[3];
}

static void test_gx_fifo(void)
{
    HwGxStats gs;
    u32 src[32], dst[40];
    u32 k, i;

    assert(sizeof src == 128);
    assert(sizeof dst >= 128);
    for (i = 0; i < 32; i++) src[i] = 0x1000u + i;
    /* the FIFO port is one address: each chunk is stored to the same words d[0..chunk-1], so the final window
       holds the last chunk. 48B = 4 chunks of 3, 64B = 2 of 8, 128B = 4 of 8. */
    for (k = 0; k < 40; k++) dst[k] = 0xEE;
    GX_SendFifo48B(src, dst);
    check(dst[0] == src[9] && dst[1] == src[10] && dst[2] == src[11] && dst[3] == 0xEE, "GX_SendFifo48B last chunk in port window", dst[0]);
    for (k = 0; k < 40; k++) dst[k] = 0xEE;
    GX_SendFifo64B(src, dst);
    check(dst[0] == src[8] && dst[7] == src[15] && dst[8] == 0xEE, "GX_SendFifo64B last chunk in port window", dst[0]);
    for (k = 0; k < 40; k++) dst[k] = 0xEE;
    GX_SendFifo128B(src, dst);
    check(dst[0] == src[24] && dst[7] == src[31] && dst[8] == 0xEE, "GX_SendFifo128B last chunk in port window", dst[0]);
    /* bet 2.1a: a store into the GX FIFO / a command port is a hook call (hw_gx.c), never a store into the I/O block */
    memset(host_ioreg, 0xAA, 0x600);   /* GXSTAT (0x600) stays clear: G3X_ClearFifo spins on its busy bit */
    HWGX_Init(NULL, NULL, 0, NULL);
    G3X_ClearFifo();
    HWGX_Stats(&gs);
    check(gs.total[HWGX_PATH_SENDFIFO] == 128 && sum4(gs.total) == 128, "GXi_NopClearFifo128_ sends 128 NOP words to the hook (via G3X_ClearFifo)", gs.total[2]);
    GX_SendFifo36B(src, (void *)(host_ioreg + 0x400));
    GX_SendFifo48B(src, (void *)(host_ioreg + 0x400));
    GX_SendFifo64B(src, (void *)(host_ioreg + 0x404));
    GX_SendFifo128B(src, (void *)(host_ioreg + 0x400));
    HWGX_Stats(&gs);
    check(gs.total[HWGX_PATH_SENDFIFO] == 128 + 9 + 12 + 16 + 32 && sum4(gs.total) == gs.total[2], "GX_SendFifo36B/48B/64B/128B into the FIFO count as SENDFIFO", gs.total[2]);
    MI_CpuSend32(src, (void *)(host_ioreg + 0x400), 16);
    MI_CpuCopy32(src, (void *)(host_ioreg + 0x440), 8);
    MI_CpuCopyFast(src, (void *)(host_ioreg + 0x448), 8);
    HWGX_Stats(&gs);
    check(gs.total[HWGX_PATH_CPUSEND] == 4 + 2 + 2, "MI_CpuSend32 / MI_CpuCopy32 / MI_CpuCopyFast into the FIFO and ports count as CPUSEND", gs.total[1]);
    check(HWGX_PortLast(0x440) == src[0] && HWGX_PortLast(0x444) == src[1] && HWGX_PortLast(0x448) == src[0] && HWGX_PortLast(0x44c) == src[1],
          "MI_CpuCopy32 walks consecutive ports", HWGX_PortLast(0x44c));
    for (i = 0x400; i < 0x600 && host_ioreg[i] == 0xAA; i++) {}
    check(i == 0x600 && host_ioreg[0x3ff] == 0xAA, "none of those stores touched the I/O block", i);
    for (k = 0; k < 40; k++) dst[k] = 0xEE;
    MI_CpuCopy32(src, dst, 16);
    MI_CpuSend32(src, dst + 20, 8);
    check(dst[3] == src[3] && dst[4] == 0xEE && dst[20] == src[1] && dst[21] == 0xEE, "MI_CpuCopy32 / MI_CpuSend32 to ordinary memory are unchanged", dst[3]);
}

static void test_os_interrupts(void)
{
    OSIntrMode m;

    assert(HWOS_CpsrRead() == g_cpsr);
    g_cpsr = 0xF000001Fu;  /* flags set, SYS mode, IRQ and FIQ enabled */
    m = OS_DisableInterrupts();
    check(m == 0 && g_cpsr == 0xF000009Fu, "OS_DisableInterrupts keeps flags, returns old I bit 0", g_cpsr);
    m = OS_DisableInterrupts();
    check(m == 0x80, "OS_DisableInterrupts when already disabled returns 0x80", m);
    m = OS_RestoreInterrupts(0);
    check(m == 0x80 && g_cpsr == 0xF000001Fu, "OS_RestoreInterrupts(enable)", g_cpsr);
    m = OS_EnableInterrupts();
    check(m == 0 && g_cpsr == 0xF000001Fu, "OS_EnableInterrupts when enabled", g_cpsr);
    m = OS_DisableInterrupts_IrqAndFiq();
    check(m == 0 && g_cpsr == 0xF00000DFu, "OS_DisableInterrupts_IrqAndFiq sets I and F", g_cpsr);
    check(OS_GetCpsrIrq() == 0x80 && OS_GetProcMode() == 0x1F, "OS_GetCpsrIrq / OS_GetProcMode", g_cpsr);
    m = OS_RestoreInterrupts_IrqAndFiq(0x40);
    check(m == 0xC0 && g_cpsr == 0xF000005Fu, "OS_RestoreInterrupts_IrqAndFiq(FIQ off only)", g_cpsr);
    m = OS_EnableInterrupts_IrqAndFiq();
    check(m == 0x40 && g_cpsr == 0xF000001Fu, "OS_EnableInterrupts_IrqAndFiq", g_cpsr);
    g_cpsr = 0x13;  /* SVC mode */
    check(OS_GetProcMode() == 0x13, "OS_GetProcMode in SVC", OS_GetProcMode());
    OS_SpinWait(0); OS_SpinWait(3); OS_SpinWait(4); OS_SpinWait(1000);
    check(1, "OS_SpinWait returns for 0, 3, 4, 1000", 0);
}

static void test_os_cp15(void)
{
    u32 base;
    int n;

    memset(g_cp15, 0, sizeof g_cp15);
    g_cp15[HWOS_CP15_CONTROL] = 0x00012078u;
    base = g_cp15[HWOS_CP15_CONTROL];
    assert(HWOS_CP15_COUNT == 16);
    assert(base != 0);
    OS_EnableITCM();
    check(g_cp15[HWOS_CP15_CONTROL] == (base | HW_C1_ITCM_ENABLE), "OS_EnableITCM sets only its bit", g_cp15[HWOS_CP15_CONTROL]);
    OS_DisableITCM();
    check(g_cp15[HWOS_CP15_CONTROL] == (base & ~(u32)HW_C1_ITCM_ENABLE), "OS_DisableITCM clears only its bit", g_cp15[HWOS_CP15_CONTROL]);
    OS_EnableDTCM(); OS_EnableProtectionUnit();
    check((g_cp15[HWOS_CP15_CONTROL] & (HW_C1_DTCM_ENABLE | HW_C1_PROTECT_UNIT_ENABLE)) == (HW_C1_DTCM_ENABLE | HW_C1_PROTECT_UNIT_ENABLE), "OS_EnableDTCM + EnableProtectionUnit", 0);
    OS_DisableDTCM(); OS_DisableProtectionUnit();
    check((g_cp15[HWOS_CP15_CONTROL] & (HW_C1_DTCM_ENABLE | HW_C1_PROTECT_UNIT_ENABLE)) == 0, "OS_DisableDTCM + DisableProtectionUnit", 0);
    OS_SetITCMParam(0xFFFFFFFFu);
    check(g_cp15[HWOS_CP15_ITCM] == HW_C9_TCMR_SIZE_MASK && OS_GetITCMParam() == HW_C9_TCMR_SIZE_MASK, "ITCM param masked to the size field", g_cp15[HWOS_CP15_ITCM]);
    OS_SetDTCMParam(0xFFFFFFFFu);
    check(OS_GetDTCMParam() == (HW_C9_TCMR_BASE_MASK | HW_C9_TCMR_SIZE_MASK), "DTCM param masked to base+size", OS_GetDTCMParam());
    OS_SetDTCMParam(0x00000014u | 0x27000000u);
    OS_SetDTCMAddress(0x0B000000u);
    check(OS_GetDTCMAddress() == (0x0B000000u & HW_C9_TCMR_BASE_MASK) && (OS_GetDTCMParam() & HW_C9_TCMR_SIZE_MASK) == (0x14u & HW_C9_TCMR_SIZE_MASK), "OS_SetDTCMAddress keeps the size field", OS_GetDTCMParam());
    for (n = 0; n < 4; n++) {
        OS_SetIPermissionsForProtectionRegion(0xF << (4 * n), 0x3u << (4 * n));
        OS_SetDPermissionsForProtectionRegion(0xF << (4 * n), 0x1u << (4 * n));
    }
    check(OS_GetIPermissionsForProtectionRegion() == 0x3333u && OS_GetDPermissionsForProtectionRegion() == 0x1111u, "protection permissions set/get (bic then orr)", 0);
}

typedef void (*SetReg)(u32);
typedef u32 (*GetReg)(void);

static void test_os_regions(void)
{
    static const SetReg set[8] = {OS_SetProtectionRegion0, OS_SetProtectionRegion1, OS_SetProtectionRegion2, OS_SetProtectionRegion3, OS_SetProtectionRegion4, OS_SetProtectionRegion5, OS_SetProtectionRegion6, OS_SetProtectionRegion7};
    static const GetReg get[8] = {OS_GetProtectionRegion0, OS_GetProtectionRegion1, OS_GetProtectionRegion2, OS_GetProtectionRegion3, OS_GetProtectionRegion4, OS_GetProtectionRegion5, OS_GetProtectionRegion6, OS_GetProtectionRegion7};
    int n, k, bad = 0;

    assert(HWOS_CP15_REGION1 == HWOS_CP15_REGION0 + 1);
    assert(HWOS_CP15_REGION7 == HWOS_CP15_REGION0 + 7);
    memset(g_cp15, 0, sizeof g_cp15);
    for (n = 0; n < 8; n++) {
        set[n](0x01000000u * (u32)(n + 1) + 0x35u);
        for (k = 0; k < 8; k++) {
            u32 want = (k <= n) ? 0x01000000u * (u32)(k + 1) + 0x35u : 0;
            if (g_cp15[HWOS_CP15_REGION0 + k] != want) bad++;
        }
    }
    for (n = 0; n < 8; n++) if (get[n]() != 0x01000000u * (u32)(n + 1) + 0x35u) bad++;
    check(bad == 0, "OS_Set/GetProtectionRegion0..7 hit region N only", bad);
    memset(g_cp15, 0, sizeof g_cp15);
    OS_EnableICacheForProtectionRegion(0x3); OS_EnableDCacheForProtectionRegion(0x5); OS_EnableWriteBufferForProtectionRegion(0x9);
    OS_DisableICacheForProtectionRegion(0x1); OS_DisableDCacheForProtectionRegion(0x4); OS_DisableWriteBufferForProtectionRegion(0x8);
    check(OS_GetICacheEnableFlagsForProtectionRegion() == 0x2 && OS_GetDCacheEnableFlagsForProtectionRegion(0) == 0x1 && OS_GetWriteBufferEnableFlagsForProtectionRegion(0) == 0x1, "cache / write-buffer enable and disable flags", 0);
}

static void test_os_context(void)
{
    OSContext c;
    int i, zero = 1;

    assert(sizeof(c.r) == 13 * 4);
    assert(sizeof(OSContext) >= 4 * 4 + 52);
    memset(&c, 0xCC, sizeof c);
    OS_InitContext(&c, 0x02000000u, 0x023FFF00u);
    for (i = 0; i < 13; i++) if (c.r[i] != 0) zero = 0;
    check(c.pc_plus4 == 0x02000004u && c.cpsr == 0x1Fu && c.lr == 0 && zero, "OS_InitContext ARM entry: pc+4, SYS mode, regs zero", c.cpsr);
    check(c.sp_svc == 0x023FFF00u && c.sp == (0x023FFF00u - HW_SVC_STACK_SIZE), "OS_InitContext sp_svc and sp = top - svc stack", c.sp);
    OS_InitContext(&c, 0x02000001u, 0x023FFF04u);
    check(c.pc_plus4 == 0x02000005u && c.cpsr == 0x3Fu, "OS_InitContext Thumb entry sets T bit", c.cpsr);
    check(c.sp == ((0x023FFF04u - HW_SVC_STACK_SIZE) & ~4u) && (c.sp & 4) == 0, "OS_InitContext sp 8-byte aligned (bit 2 cleared by -4)", c.sp);
}

static void test_os_vram_and_locks(void)
{
    u32 *flag = (u32 *)(g_hwsys + 0xfb0);
    s32 id;
    int i, ok = 1;

    assert((uintptr_t)flag == (uintptr_t)HW_LOCK_ID_FLAG_MAIN);
    assert(OS_MAINP_LOCK_ID_START == 0x40);
    OSi_InitVramExclusive();
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_A | OS_VRAM_BANK_ID_B, 5) == TRUE, "TryLockVram A|B id 5", 0);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_B, 6) == FALSE, "TryLockVram B by id 6 refused", 0);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_B, 5) == TRUE, "TryLockVram B by owner 5", 0);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_C, 6) == TRUE, "TryLockVram free bank C id 6", 0);
    OSi_UnlockVram(OS_VRAM_BANK_ID_A, 6);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_A, 9) == FALSE, "UnlockVram with wrong id leaves bank locked", 0);
    OSi_UnlockVram(OS_VRAM_BANK_ID_A | OS_VRAM_BANK_ID_B, 5);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_A | OS_VRAM_BANK_ID_B, 9) == TRUE, "UnlockVram frees A and B", 0);
    check(OSi_TryLockVram(OS_VRAM_BANK_ID_ALL, 9) == FALSE, "TryLockVram ALL fails while C is held by 6", 0);
    flag[0] = 0xFFFFFFFFu; flag[1] = 0x0000FFFFu;
    for (i = 0; i < 32; i++) { id = OS_GetLockID(); if (id != 0x40 + i) ok = 0; }
    check(ok && flag[0] == 0, "OS_GetLockID hands out 0x40..0x5f MSB first", ok);
    for (i = 0; i < 16; i++) { id = OS_GetLockID(); if (id != 0x60 + 16 + i) ok = 0; }
    check(ok && flag[1] == 0, "OS_GetLockID continues in the second flag word (0x70..0x7f)", ok);
    check(OS_GetLockID() == OS_LOCK_ID_ERROR, "OS_GetLockID returns OS_LOCK_ID_ERROR when exhausted", 0);
    OS_ReleaseLockID(0x45);
    OS_ReleaseLockID(0x7f);
    OS_ReleaseLockID(0x10);   /* below the pool: ignored */
    OS_ReleaseLockID(0x80);   /* past the pool: ignored */
    check(flag[0] == (0x80000000u >> 5) && flag[1] == 0x00000001u, "OS_ReleaseLockID sets the matching bit; out-of-pool ids ignored", flag[0]);
    check(OS_GetLockID() == 0x45 && OS_GetLockID() == 0x7f && OS_GetLockID() == OS_LOCK_ID_ERROR, "released ids are reissued", 0);
}

extern BOOL OSi_IsThreadInitialized;  /* defined in os_thread.c, no header declares it */

static void test_os_thread_flag(void)
{
    assert(sizeof(OSi_IsThreadInitialized) >= 4);
    assert(TRUE != FALSE);
    OSi_IsThreadInitialized = 0;
    check(OS_IsThreadAvailable() == FALSE, "OS_IsThreadAvailable reads the init flag (0)", 0);
    OSi_IsThreadInitialized = 1;
    check(OS_IsThreadAvailable() == TRUE, "OS_IsThreadAvailable reads the init flag (1)", 1);
    OSi_IsThreadInitialized = 0;
}

static void test_os_unlock_wrappers(void)
{
    OSLockWord *card = (OSLockWord *)(g_hwsys + 0xfe0);
    OSLockWord *ctrdg = (OSLockWord *)(g_hwsys + 0xfe8);

    assert((uintptr_t)card == (uintptr_t)HW_CARD_LOCK_BUF);
    assert((uintptr_t)ctrdg == (uintptr_t)HW_CTRDG_LOCK_BUF);
    card->lockFlag = 0x41; card->ownerID = 0x41;
    check(OS_UnLockCard(0x42) == OS_UNLOCK_ERROR && card->lockFlag == 0x41, "OS_UnLockCard wrong owner -> error, lock kept", 0);
    ctrdg->lockFlag = 0x43; ctrdg->ownerID = 0x43;
    check(OS_UnLockCartridge(0x44) == OS_UNLOCK_ERROR && ctrdg->lockFlag == 0x43, "OS_UnLockCartridge wrong owner -> error, lock kept", 0);
}

static void step(const char *name)
{
    assert(name != NULL);
    if (getenv("HOST_TEST_TRACE") != NULL) {
        fprintf(stderr, "[%s]\n", name);
    }
}

int main(void)
{
    g_cpsr = 0x1F;
    step("test_fx_matrices"); test_fx_matrices();
    step("test_fx_products"); test_fx_products();
    step("test_fx_concat"); test_fx_concat();
    step("test_mi_memory"); test_mi_memory();
    step("test_mi_swap"); test_mi_swap();
    step("test_qsort"); test_qsort();
    step("test_rc4"); test_rc4();
    step("test_rl_lz"); test_rl_lz();
    step("test_lz_ext_forms"); test_lz_ext_forms();
    step("test_huffman"); test_huffman();
    step("test_diff"); test_diff();
    step("test_unpack"); test_unpack();
    step("test_cp_context"); test_cp_context();
    step("test_gx_fifo"); test_gx_fifo();
    step("test_os_interrupts"); test_os_interrupts();
    step("test_os_cp15"); test_os_cp15();
    step("test_os_regions"); test_os_regions();
    step("test_os_context"); test_os_context();
    step("test_os_vram_and_locks"); test_os_vram_and_locks();
    step("test_os_thread_flag"); test_os_thread_flag();
    step("test_os_unlock_wrappers"); test_os_unlock_wrappers();
    printf("%ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
