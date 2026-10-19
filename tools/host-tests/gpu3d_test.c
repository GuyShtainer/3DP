/* gpu3d_test.c -- bet 2.4a: source/hw/gpu3d.c on hand-built streams. Every expected value is worked out by hand in 20.12 fixed point (1.0 = 0x1000), not taken from the
 * code under test. The differential gate against the real traces is `make ge-replay`; this file pins the corners the traces never reach (stack overflow, rounding, ...). */
#include <stdio.h>
#include <string.h>
#include "gpu3d.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { uint32_t a_ = (uint32_t)(a), b_ = (uint32_t)(b); g_checks++; if (a_ != b_) { g_fail++; \
    printf("FAIL %s:%d: %s = 0x%08x, want 0x%08x\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* one command through its command port (offset 0x400 + 4 * opcode), one store per parameter */
static void cmd(Gpu3d *g, uint32_t op, const uint32_t *p, uint32_t n)
{
    uint32_t i;

    if (n == 0u) {
        gpu3d_port(g, 0x400u + 4u * op, 0u);
        return;
    }
    for (i = 0u; i < n; i++) {
        gpu3d_port(g, 0x400u + 4u * op, p[i]);
    }
}
static void cmd1(Gpu3d *g, uint32_t op, uint32_t a) { cmd(g, op, &a, 1u); }
static void cmd0(Gpu3d *g, uint32_t op) { cmd(g, op, NULL, 0u); }
static void trans(Gpu3d *g, int32_t x, int32_t y, int32_t z) { uint32_t p[3]; p[0] = (uint32_t)x; p[1] = (uint32_t)y; p[2] = (uint32_t)z; cmd(g, 0x1Cu, p, 3u); }
static uint32_t clipw(Gpu3d *g, uint32_t i) { return gpu3d_read(g, GPU3D_REG_CLIPMTX + 4u * i); }
static uint32_t vecw(Gpu3d *g, uint32_t i) { return gpu3d_read(g, GPU3D_REG_VECMTX + 4u * i); }
static uint32_t stat(Gpu3d *g) { return gpu3d_read(g, GPU3D_REG_GXSTAT); }

static void test_reset_and_trans(void)
{
    Gpu3d g;
    uint32_t i;

    gpu3d_reset(&g);
    for (i = 0u; i < 16u; i++) {
        CHECK_EQ(clipw(&g, i), (i % 5u == 0u) ? 0x1000u : 0u);
    }
    for (i = 0u; i < 9u; i++) {
        CHECK_EQ(vecw(&g, i), (i % 4u == 0u) ? 0x1000u : 0u);
    }
    CHECK_EQ(stat(&g), GPU3D_GXSTAT_HALF | GPU3D_GXSTAT_EMPTY);
    cmd1(&g, 0x10u, 1u);                       /* MTX_MODE position */
    trans(&g, 0x1000, 0x2000, -0x3000);
    CHECK_EQ(clipw(&g, 12), 0x1000u);
    CHECK_EQ(clipw(&g, 13), 0x2000u);
    CHECK_EQ(clipw(&g, 14), (uint32_t)-0x3000);
    CHECK_EQ(clipw(&g, 15), 0x1000u);
    CHECK_EQ(vecw(&g, 0), 0x1000u);            /* mode 1 leaves the vector matrix alone */
    CHECK_EQ(g.done, 2u);
}

static void test_scale_keeps_translation(void)
{
    Gpu3d g;
    uint32_t p[3] = { 0x2000u, 0x2000u, 0x2000u };

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 2u);                       /* position + vector */
    trans(&g, 0x1000, 0x2000, 0x3000);
    cmd(&g, 0x1Bu, p, 3u);                     /* MTX_SCALE x2: operand x current, so the translation row stays */
    CHECK_EQ(clipw(&g, 0), 0x2000u);
    CHECK_EQ(clipw(&g, 5), 0x2000u);
    CHECK_EQ(clipw(&g, 10), 0x2000u);
    CHECK_EQ(clipw(&g, 12), 0x1000u);
    CHECK_EQ(clipw(&g, 13), 0x2000u);
    CHECK_EQ(clipw(&g, 14), 0x3000u);
    CHECK_EQ(vecw(&g, 0), 0x1000u);            /* SCALE never touches the vector matrix */
}

static void test_mult_rounding(void)
{
    Gpu3d g;
    uint32_t cur[16] = { 0u }, op[9] = { 0u };

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 2u);
    cur[0] = (uint32_t)-3; cur[5] = 0x1000u; cur[10] = 0x1000u; cur[15] = 0x1000u;
    cmd(&g, 0x16u, cur, 16u);                  /* MTX_LOAD_4x4, m00 = -3/4096 */
    op[0] = 3u; op[4] = 0x1000u; op[8] = 0x1000u;
    cmd(&g, 0x1Au, op, 9u);                    /* MTX_MULT_3x3, m00 = 3/4096: -9 >> 12 floors to -1 */
    CHECK_EQ(clipw(&g, 0), 0xFFFFFFFFu);
    CHECK_EQ(clipw(&g, 5), 0x1000u);
    CHECK_EQ(clipw(&g, 10), 0x1000u);
    CHECK_EQ(clipw(&g, 15), 0x1000u);
    CHECK_EQ(vecw(&g, 0), 0xFFFFFFFFu);        /* mode 2: the vector matrix took the same product */
    op[0] = 0x1000u;
    cur[0] = 3u;
    cmd(&g, 0x16u, cur, 16u);
    op[0] = 3u;
    cmd(&g, 0x1Au, op, 9u);                    /* 3 * 3 = 9 >> 12 = 0 (positive truncates to 0) */
    CHECK_EQ(clipw(&g, 0), 0u);
    /* MULT_4x3: three values per row, the fourth column is (0,0,0,1); 1.5 * 1.5 = 2.25 */
    {
        uint32_t m43[12] = { 0u }, one[16] = { 0u };

        one[0] = 0x1800u; one[5] = 0x1000u; one[10] = 0x1000u; one[15] = 0x1000u;
        cmd(&g, 0x16u, one, 16u);
        m43[0] = 0x1800u; m43[4] = 0x1000u; m43[8] = 0x1000u; m43[9] = 0x0400u;   /* row 3 = translation (0, 0, 0) ... see below */
        m43[9] = 0u; m43[10] = 0u; m43[11] = 0x0400u;                              /* translation z = 0.25 */
        cmd(&g, 0x19u, m43, 12u);
        CHECK_EQ(clipw(&g, 0), 0x2400u);
        CHECK_EQ(clipw(&g, 14), 0x0400u);
        CHECK_EQ(clipw(&g, 15), 0x1000u);
    }
}

static void test_stacks(void)
{
    Gpu3d g;
    uint32_t i;
    uint32_t p6 = 1u;

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    trans(&g, 0x1000, 0, 0);
    cmd0(&g, 0x11u);                           /* PUSH */
    CHECK_EQ((stat(&g) >> 8) & 31u, 1u);
    trans(&g, 0x1000, 0, 0);
    CHECK_EQ(clipw(&g, 12), 0x2000u);
    cmd1(&g, 0x12u, p6);                       /* POP 1 */
    CHECK_EQ(clipw(&g, 12), 0x1000u);
    CHECK_EQ((stat(&g) >> 8) & 31u, 0u);
    cmd1(&g, 0x13u, 5u);                       /* STORE 5 */
    trans(&g, 0x500, 0, 0);
    CHECK_EQ(clipw(&g, 12), 0x1500u);
    cmd1(&g, 0x14u, 5u);                       /* RESTORE 5 */
    CHECK_EQ(clipw(&g, 12), 0x1000u);
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_ERROR, 0u);

    /* the position stack takes 31 pushes; the 32nd raises the error flag */
    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    for (i = 0u; i < 31u; i++) {
        cmd0(&g, 0x11u);
    }
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_ERROR, 0u);
    CHECK_EQ((stat(&g) >> 8) & 31u, 31u);
    CHECK_EQ(g.pos_high, 31u);
    cmd0(&g, 0x11u);
    CHECK((stat(&g) & GPU3D_GXSTAT_ERROR) != 0u);
    CHECK_EQ(g.overflows, 1u);
    gpu3d_write_gxstat(&g, GPU3D_GXSTAT_ERROR); /* writing 1 acknowledges */
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_ERROR, 0u);

    /* underflow: a pop below zero */
    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    cmd1(&g, 0x12u, 1u);
    CHECK((stat(&g) & GPU3D_GXSTAT_ERROR) != 0u);

    /* STORE / RESTORE past 30 */
    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    cmd1(&g, 0x13u, 31u);
    CHECK((stat(&g) & GPU3D_GXSTAT_ERROR) != 0u);
}

static void test_projection_stack(void)
{
    Gpu3d g;

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 0u);
    trans(&g, 0x1000, 0, 0);
    cmd0(&g, 0x11u);                           /* depth 1 */
    CHECK((stat(&g) & (1u << 13)) != 0u);
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_ERROR, 0u);
    trans(&g, 0x1000, 0, 0);
    cmd0(&g, 0x11u);                           /* a second push overflows */
    CHECK((stat(&g) & GPU3D_GXSTAT_ERROR) != 0u);
    CHECK_EQ(stat(&g) & (1u << 13), 0u);       /* the 1-bit pointer wrapped */
    gpu3d_write_gxstat(&g, GPU3D_GXSTAT_ERROR);
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_ERROR, 0u);
    cmd0(&g, 0x11u);
    cmd1(&g, 0x12u, 1u);
    CHECK_EQ(stat(&g) & (1u << 13), 0u);
    CHECK_EQ(g.proj_high, 1u);
}

static void test_clip_recompute(void)
{
    Gpu3d g;
    uint32_t pj[16] = { 0u };

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 0u);
    pj[0] = 0x2000u; pj[5] = 0x2000u; pj[10] = 0x2000u; pj[15] = 0x1000u;
    cmd(&g, 0x16u, pj, 16u);                   /* projection diag(2,2,2,1) */
    cmd1(&g, 0x10u, 1u);
    trans(&g, 0x1000, 0, 0);                   /* clip = position x projection */
    CHECK_EQ(clipw(&g, 12), 0x2000u);
    CHECK_EQ(clipw(&g, 15), 0x1000u);
    cmd1(&g, 0x10u, 0u);                       /* changing the projection recomputes it again */
    cmd0(&g, 0x15u);
    CHECK_EQ(clipw(&g, 12), 0x1000u);
    CHECK_EQ(clipw(&g, 0), 0x1000u);
}

static uint32_t box(Gpu3d *g, int x, int y, int z, int w, int h, int d)
{
    uint32_t p[3];

    p[0] = ((uint32_t)x & 0xffffu) | ((uint32_t)y << 16);
    p[1] = ((uint32_t)z & 0xffffu) | ((uint32_t)w << 16);
    p[2] = ((uint32_t)h & 0xffffu) | ((uint32_t)d << 16);
    cmd(g, 0x70u, p, 3u);
    return (stat(g) >> 1) & 1u;
}

static void test_box_test(void)
{
    Gpu3d g;

    gpu3d_reset(&g);
    CHECK_EQ(box(&g, -0x400, -0x400, -0x400, 0x800, 0x800, 0x800), 1u);   /* inside the unit cube */
    CHECK_EQ(box(&g, 0x4000, 0, 0, 0x100, 0x100, 0x100), 0u);             /* far to the right of the frustum */
    CHECK_EQ(box(&g, -0x7000, -0x7000, -0x7000, 0x7fff, 0x7fff, 0x7fff), 1u);   /* encloses the whole view */
    CHECK_EQ(g.box_tests, 3u);
    CHECK_EQ(g.box_in_view, 2u);
    CHECK_EQ(box(&g, 0x4000, 0, 0, 0x100, 0x100, 0x100), 0u);             /* the result bit is rewritten every test */
}

static void test_pos_vec_test(void)
{
    Gpu3d g;
    uint32_t p2[2];

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    trans(&g, 0x1000, 0, 0);
    p2[0] = 0x0123u | (0xfff0u << 16);         /* x = 0x123, y = -16 */
    p2[1] = 0x0456u;
    cmd(&g, 0x71u, p2, 2u);                    /* POS_TEST */
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_POS_RESULT), 0x1123u);
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_POS_RESULT + 4u), 0xfffffff0u);
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_POS_RESULT + 8u), 0x456u);
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_POS_RESULT + 12u), 0x1000u);
    CHECK_EQ(g.pos_tests, 1u);
    cmd1(&g, 0x72u, 0x100u | (0x200u << 10) | (0x3ffu << 20));   /* VEC_TEST: x = 0x100/512, y = 0x200 (-1.0), z = 0x3ff (-1/512) */
    /* identity vector matrix: r = n * 0x1000 >> 9 = n * 8 -> 0x800, -0x1000 (0xf000 sign-extended 13-bit: 0x1000 & 0x1000 set), -8 */
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_VEC_RESULT) & 0xffffu, 0x0800u);
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_VEC_RESULT) >> 16, 0xf000u);
    CHECK_EQ(gpu3d_read(&g, GPU3D_REG_VEC_RESULT + 4u) & 0xffffu, 0xfff8u);
    CHECK_EQ(g.vec_tests, 1u);
}

static void test_gxstat(void)
{
    Gpu3d g;

    gpu3d_reset(&g);
    cmd1(&g, 0x50u, 0u);                       /* SWAP_BUFFERS: busy until the vertical blank */
    CHECK((stat(&g) & GPU3D_GXSTAT_BUSY) != 0u);
    gpu3d_vblank(&g);
    CHECK_EQ(stat(&g) & GPU3D_GXSTAT_BUSY, 0u);
    CHECK_EQ(gpu3d_read(&g, 0x700u), 0u);      /* outside the register block */
}

static void test_param_table(void)
{
    static const uint8_t want[256] = {
        [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
        [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
        [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32, [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
    };
    static const uint8_t listed[] = { 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                      0x2A, 0x2B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41, 0x50, 0x60, 0x70, 0x71, 0x72 };
    uint32_t i, known = 0u;

    for (i = 0u; i < 256u; i++) {
        CHECK_EQ(gpu3d_nparams(i), want[i]);
    }
    for (i = 0u; i < sizeof listed; i++) {
        CHECK(gpu3d_known(listed[i]) != 0);
        known++;
    }
    CHECK_EQ(known, 38u);
    for (i = 0u; i < 256u; i++) {
        int in = 0;
        uint32_t k;

        for (k = 0u; k < sizeof listed; k++) {
            in |= (listed[k] == i);
        }
        CHECK_EQ(gpu3d_known(i) != 0, in);
    }
}

static void test_unknown_and_skip(void)
{
    Gpu3d g;
    uint32_t i, n;

    gpu3d_reset(&g);
    cmd1(&g, 0x10u, 1u);
    trans(&g, 0x1000, 0, 0);
    /* packed word 0x15, 0x01: an unlisted opcode takes no parameters and is counted; IDENTITY then runs */
    n = gpu3d_fifo(&g, 0x00001501u);
    CHECK_EQ(n, 2u);
    CHECK_EQ(g.unknown[0x01], 1u);
    CHECK_EQ(g.unknown_total, 1u);
    CHECK_EQ(g.count[0x15], 1u);
    CHECK_EQ(clipw(&g, 12), 0u);
    /* SHININESS (0x34) takes 32 parameter words; the opcode packed behind it (TRANS-less IDENTITY) must wait for them */
    trans(&g, 0x1000, 0, 0);
    gpu3d_fifo(&g, 0x00001534u);
    for (i = 0u; i < 31u; i++) {
        CHECK_EQ(gpu3d_fifo(&g, 0xabcdef00u + i), 0u);
        CHECK_EQ(clipw(&g, 12), 0x1000u);
    }
    n = gpu3d_fifo(&g, 0x12345678u);
    CHECK_EQ(n, 2u);
    CHECK_EQ(clipw(&g, 12), 0u);
    /* an unlisted opcode between two parameterised ones consumes nothing */
    gpu3d_fifo(&g, 0x00731510u);               /* 0x10 MTX_MODE (1 param), 0x15, 0x73 (unlisted) */
    gpu3d_fifo(&g, 1u);
    CHECK_EQ(g.unknown[0x73], 1u);
    CHECK_EQ(g.mode, 1u);
}


/* ===== slice (b): vertices, lighting, texgen, the clipper, culling, the sealed list. Expected values are derived by hand in the comments. ===== */
static Gpu3d G;   /* ~1 MB: static, never on the stack */
#define ATTR_FRONT 0x001f0080u   /* alpha 31, render the front only */
#define ATTR_BACK  0x001f0040u
#define ATTR_BOTH  0x001f00c0u
#define VP_FULL    0xbfff0000u   /* VIEWPORT 0,0 .. 255,191 */

static void fresh(void)
{
    gpu3d_reset(&G);
    cmd1(&G, 0x60u, VP_FULL);
}
static void vtx16(int32_t x, int32_t y, int32_t z)
{
    uint32_t p[2];

    p[0] = ((uint32_t)x & 0xffffu) | ((uint32_t)y << 16);
    p[1] = (uint32_t)z & 0xffffu;
    cmd(&G, 0x23u, p, 2u);
}
static void begin(uint32_t attr, uint32_t prim)
{
    cmd1(&G, 0x29u, attr);
    cmd1(&G, 0x40u, prim);
}
/* the screen row of y (4.12) at w = 1.0 in the 192-line viewport: (w - y) * 192 / (2 w) */
static int32_t row_of(int32_t y) { return (int32_t)(((int64_t)(0x1000 - y) * 192) / 0x2000); }
static int32_t col_of(int32_t x) { return (int32_t)(((int64_t)(x + 0x1000) * 256) / 0x2000); }
static void swap_and_show(uint32_t param)
{
    cmd1(&G, 0x50u, param);
    gpu3d_vblank(&G);
}

static void test_vertex_transform(void)
{
    const Gpu3dPoly *p;
    const Gpu3dVtx *v;

    fresh();
    cmd1(&G, 0x10u, 1u);
    trans(&G, 0x800, 0, 0);                        /* position matrix: translate x by +0.5 */
    cmd1(&G, 0x20u, 0x7c00u);                      /* colour: blue 31 */
    begin(ATTR_BOTH, 0u);
    vtx16(0, 0, 0);                                /* -> (0.5, 0, 0, 1.0) */
    vtx16(0x400, 0x400, 0x400);                    /* -> (0.75, .25, .25, 1.0) */
    vtx16(0, 0x400, 0);
    swap_and_show(0u);
    CHECK_EQ(gpu3d_render_count(&G), 1u);
    p = gpu3d_render_poly(&G, 0);
    v = gpu3d_render_vtx(&G, p, 0);
    CHECK_EQ(v->pos[0], 0x800u); CHECK_EQ(v->pos[1], 0u); CHECK_EQ(v->pos[2], 0u); CHECK_EQ(v->pos[3], 0x1000u);
    CHECK_EQ(v->col[2], (31u << 12) + 0xfffu);     /* colours are kept as (c << 12) + 0xFFF */
    CHECK_EQ(v->col[0], 0xfffu);
    v = gpu3d_render_vtx(&G, p, 1);
    CHECK_EQ(v->pos[0], 0xc00u); CHECK_EQ(v->pos[1], 0x400u); CHECK_EQ(v->pos[2], 0x400u);
    /* scale 2.0 in the position matrix, then the same vertex: 64-bit sums, 12-bit shift */
    fresh();
    cmd1(&G, 0x10u, 1u);
    { uint32_t sc[3] = { 0x2000u, 0x2000u, 0x2000u }; cmd(&G, 0x1bu, sc, 3u); }
    begin(ATTR_BOTH, 0u);
    vtx16(0x400, 0x400, 0x400);
    vtx16(-0x400, 0x400, 0x400);
    vtx16(0, -0x400, 0x400);
    swap_and_show(0u);
    p = gpu3d_render_poly(&G, 0);
    v = gpu3d_render_vtx(&G, p, 0);
    CHECK_EQ(v->pos[0], 0x800u); CHECK_EQ(v->pos[1], 0x800u); CHECK_EQ(v->pos[2], 0x800u); CHECK_EQ(v->pos[3], 0x1000u);
    /* the other vertex commands: VTX_10 (<< 6), VTX_XY keeps z, VTX_XZ keeps y, VTX_YZ keeps x, VTX_DIFF adds the raw signed 10-bit delta */
    fresh();
    begin(ATTR_BOTH, 0u);
    cmd1(&G, 0x24u, 0x01u | (0x02u << 10) | (0x03u << 20));   /* (1,2,3) << 6 */
    CHECK_EQ(G.cur_vtx[0], 0x40); CHECK_EQ(G.cur_vtx[1], 0x80); CHECK_EQ(G.cur_vtx[2], 0xc0);
    cmd1(&G, 0x25u, 0x00200010u);   CHECK_EQ(G.cur_vtx[0], 0x10); CHECK_EQ(G.cur_vtx[1], 0x20); CHECK_EQ(G.cur_vtx[2], 0xc0);
    cmd1(&G, 0x26u, 0x00400030u);   CHECK_EQ(G.cur_vtx[0], 0x30); CHECK_EQ(G.cur_vtx[1], 0x20); CHECK_EQ(G.cur_vtx[2], 0x40);
    cmd1(&G, 0x27u, 0x00600050u);   CHECK_EQ(G.cur_vtx[0], 0x30); CHECK_EQ(G.cur_vtx[1], 0x50); CHECK_EQ(G.cur_vtx[2], 0x60);
    cmd1(&G, 0x28u, 0x3ffu | (0x005u << 10));   /* x -= 1 (10-bit -1), y += 5, z += 0 */
    CHECK_EQ(G.cur_vtx[0], 0x2f); CHECK_EQ(G.cur_vtx[1], 0x55); CHECK_EQ(G.cur_vtx[2], 0x60);
}

static void test_screen_and_depth(void)
{
    const Gpu3dPoly *p;
    const Gpu3dVtx *v;

    fresh();
    begin(ATTR_FRONT, 0u);
    vtx16(-0x800, -0x800, 0);     /* counter-clockwise on the screen = the front */
    vtx16(0x800, -0x800, 0);
    vtx16(0, 0x800, 0);
    swap_and_show(0u);
    CHECK_EQ(gpu3d_render_count(&G), 1u);
    p = gpu3d_render_poly(&G, 0);
    CHECK_EQ(p->nverts, 3u);
    v = gpu3d_render_vtx(&G, p, 0); CHECK_EQ(v->sx, 64); CHECK_EQ(v->sy, 144);    /* (-.5: (-0x800 + 0x1000) * 256 / 0x2000 = 64; (0x1000 + 0x800) * 192 / 0x2000 = 144) */
    v = gpu3d_render_vtx(&G, p, 1); CHECK_EQ(v->sx, 192); CHECK_EQ(v->sy, 144);
    v = gpu3d_render_vtx(&G, p, 2); CHECK_EQ(v->sx, 128); CHECK_EQ(v->sy, 48);
    CHECK_EQ(p->ytop, 48); CHECK_EQ(p->ybottom, 144);
    CHECK_EQ(p->sortkey, (144u << 8) | 48u);
    CHECK_EQ(p->zdepth[0], 0x7ffe00u);     /* z = 0: ((0 * 0x4000) / w + 0x3fff) * 0x200 */
    CHECK_EQ(p->wnorm[0], 0x1000u);        /* w = 0x1000 needs 13 bits; the 16-bit normalisation shifts in steps of 4: wsize 16, no shift */
    CHECK((p->flags & GPU3D_PF_FACING) != 0u);
    CHECK_EQ(row_of(0x800), 48); CHECK_EQ(col_of(-0x800), 64);   /* the helpers agree with the numbers above */
    /* the W-buffer swap flag selects w as the depth; the flag in force while a polygon is built is the one of the PREVIOUS swap (the oracle's behaviour) */
    fresh();
    cmd1(&G, 0x50u, 2u);
    gpu3d_vblank(&G);
    begin(ATTR_FRONT, 0u);
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    swap_and_show(2u);
    p = gpu3d_render_poly(&G, 0);
    CHECK_EQ(p->zdepth[0], 0x1000u);
    CHECK((p->flags & GPU3D_PF_WBUFFER) != 0u);
}

static void test_culling(void)
{
    fresh();
    begin(ATTR_FRONT, 0u);                     /* front only: the counter-clockwise triangle stays, the clockwise one goes */
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    vtx16(-0x800, -0x800, 0); vtx16(0, 0x800, 0); vtx16(0x800, -0x800, 0);
    CHECK_EQ(G.npoly, 1u);
    CHECK_EQ(G.st_culled, 1u);
    fresh();
    begin(ATTR_BACK, 0u);                      /* back only: the other way round */
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    vtx16(-0x800, -0x800, 0); vtx16(0, 0x800, 0); vtx16(0x800, -0x800, 0);
    CHECK_EQ(G.npoly, 1u);
    CHECK_EQ(G.st_culled, 1u);
    swap_and_show(0u);
    CHECK((gpu3d_render_poly(&G, 0)->flags & GPU3D_PF_FACING) == 0u);
    fresh();
    begin(0x001f0000u, 0u);                    /* neither bit: nothing is drawn */
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    CHECK_EQ(G.npoly, 0u);
}

static void test_one_dot_and_far_plane(void)
{
    fresh();
    gpu3d_write_1dot(&G, 0u);                  /* any vertex with w above 0x1ff counts as far */
    begin(ATTR_BOTH, 0u);                      /* three vertices a few units apart: one pixel */
    vtx16(0, 0, 0); vtx16(1, 0, 0); vtx16(0, -1, 0);
    CHECK_EQ(G.npoly, 0u);
    CHECK_EQ(G.st_dot, 1u);
    begin(ATTR_BOTH | (1u << 13), 0u);         /* attribute bit 13 keeps it */
    vtx16(0, 0, 0); vtx16(1, 0, 0); vtx16(0, -1, 0);
    CHECK_EQ(G.npoly, 1u);
    fresh();                                   /* the power-on 1-dot depth is the far limit: kept */
    begin(ATTR_BOTH, 0u);
    vtx16(0, 0, 0); vtx16(1, 0, 0); vtx16(0, -1, 0);
    CHECK_EQ(G.npoly, 1u);
    /* far plane: a polygon crossing it is dropped unless attribute bit 12 */
    fresh();
    begin(ATTR_BOTH, 0u);
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0x2000);
    CHECK_EQ(G.npoly, 0u);
    CHECK_EQ(G.st_clipped_out, 1u);
    fresh();
    begin(ATTR_BOTH | (1u << 12), 0u);
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0x2000);
    CHECK_EQ(G.npoly, 1u);
    CHECK_EQ(G.pram[G.cur_bank][0].nverts, 4u);   /* the far vertex became two cut vertices */
}

static void test_clipper_interpolation(void)
{
    const Gpu3dPoly *p;
    const Gpu3dVtx *v;

    /* A (0,0,0) black, B (2.0,0,0) red 31, C (0,.5,0) black: B lies beyond the x = +w plane (w = 1.0). The edges B-A and B-C cross it halfway / at t = 0.5:
       numerator w_B - x_B = 0x1000 - 0x2000 = -0x1000, denominator = numerator - (w_A - x_A) = -0x2000, so t = 1/2.
       B-A: (x,y,z,w) = (0x1000, 0, 0, 0x1000) and the colour 0x1FFFF + (0xFFF - 0x1FFFF) / 2 = 0x107FF, snapped to (c << 12) + 0xFFF = 0x10FFF (c = 16);
       B-C: y = 0 + 0x800 / 2 = 0x400. Result in order A, cut(B,A), cut(B,C), C. Screen: sx 256 for both cuts; rows 96 and (0x1000 - 0x400) * 192 / 0x2000 = 72. */
    fresh();
    begin(ATTR_FRONT, 0u);
    cmd1(&G, 0x20u, 0u);       vtx16(0, 0, 0);
    cmd1(&G, 0x20u, 0x001fu);  vtx16(0x2000, 0, 0);
    cmd1(&G, 0x20u, 0u);       vtx16(0, 0x800, 0);
    swap_and_show(0u);
    CHECK_EQ(gpu3d_render_count(&G), 1u);
    p = gpu3d_render_poly(&G, 0);
    CHECK_EQ(p->nverts, 4u);
    v = gpu3d_render_vtx(&G, p, 0); CHECK_EQ(v->clipped, 0u); CHECK_EQ(v->pos[0], 0u);
    v = gpu3d_render_vtx(&G, p, 1);
    CHECK_EQ(v->clipped, 1u);
    CHECK_EQ(v->pos[0], 0x1000u); CHECK_EQ(v->pos[1], 0u); CHECK_EQ(v->pos[2], 0u); CHECK_EQ(v->pos[3], 0x1000u);
    CHECK_EQ(v->col[0], 0x10fffu); CHECK_EQ(v->col[1], 0xfffu);
    CHECK_EQ(v->sx, 256); CHECK_EQ(v->sy, 96);
    v = gpu3d_render_vtx(&G, p, 2);
    CHECK_EQ(v->pos[0], 0x1000u); CHECK_EQ(v->pos[1], 0x400u); CHECK_EQ(v->col[0], 0x10fffu);
    CHECK_EQ(v->sy, 72);
    v = gpu3d_render_vtx(&G, p, 3); CHECK_EQ(v->clipped, 0u); CHECK_EQ(v->pos[1], 0x800u);
    /* a polygon wholly outside is gone */
    fresh();
    begin(ATTR_BOTH, 0u);
    vtx16(0x3000, 0, 0); vtx16(0x4000, 0, 0); vtx16(0x3000, 0x800, 0);
    CHECK_EQ(G.npoly, 0u);
    CHECK_EQ(G.st_clipped_out, 1u);
}

static void test_primitives(void)
{
    fresh();
    begin(ATTR_FRONT, 1u);                     /* quads: 4 vertices = 1 polygon; 8 = 2 */
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0x800, 0x800, 0); vtx16(-0x800, 0x800, 0);
    CHECK_EQ(G.npoly, 1u); CHECK_EQ(G.nvtx, 4u);
    vtx16(-0x400, -0x400, 0); vtx16(0x400, -0x400, 0); vtx16(0x400, 0x400, 0); vtx16(-0x400, 0x400, 0);
    CHECK_EQ(G.npoly, 2u); CHECK_EQ(G.nvtx, 8u);
    fresh();
    begin(ATTR_FRONT, 2u);                     /* triangle strip: 4 vertices = 2 triangles sharing two vertices (4 stored, not 6) */
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(-0x800, 0x800, 0); vtx16(0x800, 0x800, 0);
    CHECK_EQ(G.npoly, 2u); CHECK_EQ(G.nvtx, 4u);
    vtx16(0, 0xc00, 0);                        /* a third triangle: one more vertex */
    CHECK_EQ(G.nvtx, 5u);
    fresh();
    begin(ATTR_FRONT, 3u);                     /* quad strip: 6 vertices = 2 quads sharing an edge */
    vtx16(-0x800, 0x800, 0); vtx16(-0x800, -0x800, 0); vtx16(0, 0x800, 0); vtx16(0, -0x800, 0); vtx16(0x800, 0x800, 0); vtx16(0x800, -0x800, 0);
    CHECK_EQ(G.npoly, 2u); CHECK_EQ(G.nvtx, 6u);
    CHECK_EQ(G.pram[G.cur_bank][1].nverts, 4u);
    CHECK_EQ(G.pram[G.cur_bank][1].vtx[0], G.pram[G.cur_bank][0].vtx[3]);   /* the second quad starts on the first quad's last edge */
    CHECK_EQ(G.pram[G.cur_bank][1].vtx[1], G.pram[G.cur_bank][0].vtx[2]);
}

static void test_lighting(void)
{
    uint32_t l;

    /* diffuse white (31,31,31), no ambient; light l white, direction (0,0,-1) so the light shines onto a normal of +z. NORMAL (0,0,511): the vector matrix is the identity, so
       the transformed normal is 511 (1 sign + 9 fraction bits), the stored direction (negated) is +512, dot = (512 * 511) >> 9 = 511.
       Diffuse = 31 * 31 * 511 = 491071, cut to 20 bits (fits), and the colour is that >> 14 = 29 (29.97 truncated). Specular and ambient terms are zero. */
    for (l = 0u; l < 4u; l++) {
        fresh();
        cmd1(&G, 0x30u, 0x7fffu);                                   /* diffuse 31, ambient 0 */
        cmd1(&G, 0x31u, 0u);                                        /* specular 0, emission 0 */
        cmd1(&G, 0x32u, (l << 30) | (0x200u << 20));                /* direction z = -512 (-1.0) */
        cmd1(&G, 0x33u, (l << 30) | 0x7fffu);                       /* white */
        begin(ATTR_BOTH | (1u << l), 0u);                           /* only light l is on */
        cmd1(&G, 0x21u, 0x1ffu << 20);
        CHECK_EQ(G.vcolor[0], 29u); CHECK_EQ(G.vcolor[1], 29u); CHECK_EQ(G.vcolor[2], 29u);
        begin(ATTR_BOTH | (1u << ((l + 1u) & 3u)), 0u);              /* another light on: this one is not used, and that one has no colour: black */
        cmd1(&G, 0x21u, 0x1ffu << 20);
        CHECK_EQ(G.vcolor[0], 0u);
    }
    /* a normal facing away: no diffuse; ambient 10 * light 31: ((10 << 9) * 31) >> 14 = 9 (9.69); emission 7 adds 7 */
    fresh();
    cmd1(&G, 0x30u, (10u << 16) | (10u << 21) | (10u << 26) | 0x7fffu);
    cmd1(&G, 0x31u, (7u << 16) | (7u << 21) | (7u << 26));
    cmd1(&G, 0x32u, 0x200u << 20);
    cmd1(&G, 0x33u, 0x7fffu);
    begin(ATTR_BOTH | 1u, 0u);
    cmd1(&G, 0x21u, 0x201u << 20);                                  /* z = -511 */
    CHECK_EQ(G.vcolor[0], 9u + 7u); CHECK_EQ(G.vcolor[1], 16u); CHECK_EQ(G.vcolor[2], 16u);
    /* no light on: only the emission */
    fresh();
    cmd1(&G, 0x31u, (7u << 16) | (7u << 21) | (7u << 26));
    begin(ATTR_BOTH, 0u);
    cmd1(&G, 0x21u, 0x1ffu << 20);
    CHECK_EQ(G.vcolor[0], 7u);
    /* the saturation: emission 31 + diffuse is clamped at 31 */
    fresh();
    cmd1(&G, 0x30u, 0x7fffu);
    cmd1(&G, 0x31u, (31u << 16) | (31u << 21) | (31u << 26));
    cmd1(&G, 0x32u, 0x200u << 20);
    cmd1(&G, 0x33u, 0x7fffu);
    begin(ATTR_BOTH | 1u, 0u);
    cmd1(&G, 0x21u, 0x1ffu << 20);
    CHECK_EQ(G.vcolor[0], 31u);
}

static void test_texgen(void)
{
    const Gpu3dPoly *p;

    /* mode 1 (TEXCOORD through the texture matrix): translation (1.0, 2.0) in the matrix adds 1 and 2 (1/16 texel units): s' = (s * 0x1000 + 0x1000) >> 12 */
    fresh();
    cmd1(&G, 0x10u, 3u);
    trans(&G, 0x1000, 0x2000, 0);
    cmd1(&G, 0x2au, 1u << 30);
    cmd1(&G, 0x22u, 0x02000100u);                                   /* s = 0x100, t = 0x200 */
    CHECK_EQ(G.texc[0], 0x101); CHECK_EQ(G.texc[1], 0x202);
    /* mode 0: the coordinates as sent */
    cmd1(&G, 0x2au, 0u);
    cmd1(&G, 0x22u, 0x02000100u);
    CHECK_EQ(G.texc[0], 0x100); CHECK_EQ(G.texc[1], 0x200);
    /* mode 2 (the normal): scale the texture matrix by 64.0 (0x40000); normal (256,0,0): 256 * 0x40000 >> 21 = 32 added to the raw s (0x10): 0x30; t unchanged (0x20) */
    fresh();
    cmd1(&G, 0x10u, 3u);
    { uint32_t sc[3] = { 0x40000u, 0x1000u, 0x1000u }; cmd(&G, 0x1bu, sc, 3u); }
    cmd1(&G, 0x2au, 2u << 30);
    begin(ATTR_BOTH, 0u);
    cmd1(&G, 0x22u, 0x00200010u);
    cmd1(&G, 0x21u, 0x100u);
    CHECK_EQ(G.texc[0], 0x30); CHECK_EQ(G.texc[1], 0x20);
    /* mode 3 (the vertex): x = 1.0 through tex[0] = 64.0: (2^12 * 2^18) >> 24 = 64, plus the raw s 0x10 = 0x50 */
    fresh();
    cmd1(&G, 0x10u, 3u);
    { uint32_t sc[3] = { 0x40000u, 0x1000u, 0x1000u }; cmd(&G, 0x1bu, sc, 3u); }
    cmd1(&G, 0x2au, 3u << 30);
    begin(ATTR_BOTH, 0u);
    cmd1(&G, 0x22u, 0x00200010u);
    vtx16(0x1000, 0, 0);
    CHECK_EQ(G.tmpv[0].tex[0], 0x50); CHECK_EQ(G.tmpv[0].tex[1], 0x20);
    (void)p;
}

static void tri_at(uint32_t attr, int32_t y0, int32_t y1)
{
    begin(attr, 0u);
    vtx16(-0x400, y0, 0); vtx16(0x400, y0, 0); vtx16(0, y1, 0);   /* counter-clockwise when y1 > y0 */
}

static void test_translucency_and_sort(void)
{
    const Gpu3dPoly *p;

    /* alpha 31 opaque, 0 (wireframe) opaque, 1..30 translucent; texture format 1 (A3I5) and 6 (A5I3) translucent whatever the alpha */
    fresh();
    tri_at(0x001f0080u, 0, 0x400); CHECK_EQ(G.pram[G.cur_bank][0].flags & GPU3D_PF_TRANSLUCENT, 0u);
    tri_at(0x00000080u, 0, 0x400); CHECK_EQ(G.pram[G.cur_bank][1].flags & GPU3D_PF_TRANSLUCENT, 0u);
    tri_at(0x000f0080u, 0, 0x400); CHECK((G.pram[G.cur_bank][2].flags & GPU3D_PF_TRANSLUCENT) != 0u);
    tri_at(0x001e0080u, 0, 0x400); CHECK((G.pram[G.cur_bank][3].flags & GPU3D_PF_TRANSLUCENT) != 0u);
    cmd1(&G, 0x2au, 1u << 26);     tri_at(0x001f0080u, 0, 0x400); CHECK((G.pram[G.cur_bank][4].flags & GPU3D_PF_TRANSLUCENT) != 0u);
    cmd1(&G, 0x2au, 6u << 26);     tri_at(0x001f0080u, 0, 0x400); CHECK((G.pram[G.cur_bank][5].flags & GPU3D_PF_TRANSLUCENT) != 0u);
    cmd1(&G, 0x2au, 2u << 26);     tri_at(0x001f0080u, 0, 0x400); CHECK_EQ(G.pram[G.cur_bank][6].flags & GPU3D_PF_TRANSLUCENT, 0u);
    CHECK_EQ(G.nopaque, 3u);
    /* sort: A lower on the screen (y -0x800..-0x400), B higher (0x400..0x800), C1 translucent low, C2 translucent high. The key is (bottom << 8) | top, +0x10000 if translucent.
       Auto: opaque by key (B then A), then the translucent by key (C2 then C1). Manual (bit 0): the translucent keep their order (C1, C2). */
    fresh();
    tri_at(0x001f0080u, -0x800, -0x400);   /* A */
    tri_at(0x001f0080u, 0x400, 0x800);     /* B */
    tri_at(0x000f0080u, -0x800, -0x400);   /* C1 */
    tri_at(0x000f0080u, 0x400, 0x800);     /* C2 */
    cmd1(&G, 0x50u, 0u);
    gpu3d_vblank(&G);
    CHECK_EQ(gpu3d_render_count(&G), 4u);
    CHECK_EQ(gpu3d_render_poly(&G, 0)->sortkey, (uint32_t)((uint32_t)row_of(0x400) << 8 | (uint32_t)row_of(0x800)));   /* B: bottom row_of(0x400), top row_of(0x800) */
    CHECK_EQ(gpu3d_render_poly(&G, 1)->sortkey, (uint32_t)((uint32_t)row_of(-0x800) << 8 | (uint32_t)row_of(-0x400)));  /* A */
    p = gpu3d_render_poly(&G, 2); CHECK_EQ(p->sortkey, 0x10000u | ((uint32_t)row_of(0x400) << 8) | (uint32_t)row_of(0x800));   /* C2 */
    p = gpu3d_render_poly(&G, 3); CHECK_EQ(p->sortkey, 0x10000u | ((uint32_t)row_of(-0x800) << 8) | (uint32_t)row_of(-0x400)); /* C1 */
    fresh();
    tri_at(0x001f0080u, -0x800, -0x400);
    tri_at(0x001f0080u, 0x400, 0x800);
    tri_at(0x000f0080u, -0x800, -0x400);
    tri_at(0x000f0080u, 0x400, 0x800);
    cmd1(&G, 0x50u, 1u);
    gpu3d_vblank(&G);
    CHECK_EQ(gpu3d_render_poly(&G, 0)->sortkey & 0x10000u, 0u);
    CHECK_EQ(gpu3d_render_poly(&G, 2)->sortkey, 0x10000u | ((uint32_t)row_of(-0x800) << 8) | (uint32_t)row_of(-0x400));   /* C1 first */
    CHECK_EQ(gpu3d_render_poly(&G, 3)->sortkey, 0x10000u | ((uint32_t)row_of(0x400) << 8) | (uint32_t)row_of(0x800));
    /* a stable sort: two equal keys stay in submission order */
    fresh();
    tri_at(0x001f0080u, 0, 0x400);
    tri_at(0x001f0080u, 0, 0x400);
    G.pram[G.cur_bank][1].texpalette = 0x77u;
    cmd1(&G, 0x50u, 0u);
    gpu3d_vblank(&G);
    CHECK_EQ(gpu3d_render_poly(&G, 1)->texpalette, 0x77u);
}

static void test_swap_latch(void)
{
    fresh();
    tri_at(0x001f0080u, 0, 0x400);
    CHECK_EQ(gpu3d_render_count(&G), 0u);          /* nothing shows before the swap */
    cmd1(&G, 0x50u, 0u);
    CHECK_EQ(gpu3d_render_count(&G), 0u);          /* ... nor before the vertical blank */
    CHECK((stat(&G) & GPU3D_GXSTAT_BUSY) != 0u);   /* the engine is busy after a swap */
    gpu3d_vblank(&G);
    CHECK_EQ(gpu3d_render_count(&G), 1u);
    CHECK_EQ(stat(&G) & GPU3D_GXSTAT_BUSY, 0u);
    gpu3d_vblank(&G);                              /* no swap since: the list on show stays */
    CHECK_EQ(gpu3d_render_count(&G), 1u);
    cmd1(&G, 0x50u, 0u);                           /* an empty swap shows an empty list */
    gpu3d_vblank(&G);
    CHECK_EQ(gpu3d_render_count(&G), 0u);
}

static void test_list_limits(void)
{
    uint32_t i;

    /* 2048 polygons / 6144 vertices: 1536 quads use all the vertices; the 1537th does not fit and raises the RAM-overflow flag (DISP3DCNT bit 13), sticky until acknowledged */
    fresh();
    begin(ATTR_FRONT, 1u);
    for (i = 0u; i < 1536u; i++) {
        vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0x800, 0x800, 0); vtx16(-0x800, 0x800, 0);
    }
    CHECK_EQ(G.npoly, 1536u); CHECK_EQ(G.nvtx, 6144u);
    CHECK_EQ(gpu3d_disp3dcnt_flags(&G), 0u);
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0x800, 0x800, 0); vtx16(-0x800, 0x800, 0);
    CHECK_EQ(G.npoly, 1536u);
    CHECK_EQ(gpu3d_disp3dcnt_flags(&G), 1u << 13);
    gpu3d_write_disp3dcnt(&G, 1u << 13);           /* write 1 to clear */
    CHECK_EQ(gpu3d_disp3dcnt_flags(&G), 0u);
    /* 2048 triangles fit both limits (6144 vertices); the 2049th is dropped */
    fresh();
    begin(ATTR_FRONT, 0u);
    for (i = 0u; i < 2048u; i++) {
        vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    }
    CHECK_EQ(G.npoly, 2048u);
    CHECK_EQ(gpu3d_disp3dcnt_flags(&G), 0u);
    vtx16(-0x800, -0x800, 0); vtx16(0x800, -0x800, 0); vtx16(0, 0x800, 0);
    CHECK_EQ(G.npoly, 2048u);
    CHECK_EQ(gpu3d_disp3dcnt_flags(&G), 1u << 13);
}

int main(void)
{
    test_reset_and_trans();
    test_scale_keeps_translation();
    test_mult_rounding();
    test_stacks();
    test_projection_stack();
    test_clip_recompute();
    test_box_test();
    test_pos_vec_test();
    test_gxstat();
    test_param_table();
    test_unknown_and_skip();
    test_vertex_transform();
    test_screen_and_depth();
    test_culling();
    test_one_dot_and_far_plane();
    test_clipper_interpolation();
    test_primitives();
    test_lighting();
    test_texgen();
    test_translucency_and_sort();
    test_swap_latch();
    test_list_limits();
    printf("gpu3d_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
