/* hw_wide_test.c -- bet 2.8: the widescreen / stereo formulas and the geometry engine's wide path, on synthetic input.
 *
 *  1. The formula: for every fovY with sin x 5/4 < 1, MTX_PerspectiveW's _00 / _11 (the SDK's FX_Div, bit for bit) built with hwwide_sin / hwwide_aspect keep the
 *     per-pixel scale: 200 * _00' == 128 * _00 and 120 * _11' == 96 * _11 to 20.12 rounding (the worst relative difference is printed).
 *  2. A LOAD of a registered widened projection is applied as the DS matrix (CLIPMTX equals the narrow engine's), tagged wide + perspective; an orthographic one (the w
 *     row 0 0 0 1) is tagged wide only: zero parallax.
 *  3. A polygon inside the DS planes lands on the narrow engine's pixels + (72, 24); one beyond the DS x plane but inside the wide one is kept wide and dropped narrow;
 *     with stereo, the guard band keeps a perspective polygon past the wide plane and not an orthographic one. BOX_TEST sees the wide volume.
 *  4. gpu3d_viewport_wide_xy floors left of the DS frame; hwwide_shear is zero at the convergence depth.
 *  5. The wide top screen (gpu2d_compose_wide): the DS frame is engine A's composite 1:1 at (72, 24); a border pixel is the 3D layer over the backdrop through the same
 *     effect and master brightness as the centre (a fade step: border and centre agree), lines above / below take the registers of lines 0 / 191, display mode 0 is white;
 *     hwscr_blit_bgr8_wide's byte layout and the FBW1 dump header.
 * Prints "hw_wide_test: N checks, F failures". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gpu3d.h"
#include "gpu2d.h"
#include "hw_screen.h"
#include "hw_wide.h"

void gpu3d_viewport_wide_xy(int32_t x, int32_t y, int32_t w, int32_t *sx, int32_t *sy);   /* gpu3d_list.h (its Gpu3dPoly clashes with gpu3d.h's) */

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int32_t fx_div(int32_t numer, int32_t denom)
{
    int64_t q = (int64_t)((uint64_t)(int64_t)numer << 32) / (int64_t)denom;

    return (int32_t)((q + (1 << 19)) >> 20);
}

static void fifo(Gpu3d *g, uint32_t op, const uint32_t *p, uint32_t n)
{
    uint32_t i;

    gpu3d_fifo(g, op);
    for (i = 0; i < n; i++) {
        gpu3d_fifo(g, p[i]);
    }
}

static void fifo1(Gpu3d *g, uint32_t op, uint32_t a)
{
    fifo(g, op, &a, 1u);
}

/* projection = m (MTX_LOAD_4x4), position = identity, full-screen viewport, both faces drawn */
static void setup(Gpu3d *g, const int32_t *m)
{
    fifo1(g, 0x60u, 0xBFFF0000u);
    fifo1(g, 0x10u, 0u);
    fifo(g, 0x16u, (const uint32_t *)m, 16u);
    fifo1(g, 0x10u, 2u);
    gpu3d_fifo(g, 0x15u);
    fifo1(g, 0x29u, 0x001F00C0u);
}

static void tri(Gpu3d *g, const int16_t v[3][3])
{
    uint32_t i;

    fifo1(g, 0x40u, 0u);
    for (i = 0u; i < 3u; i++) {
        uint32_t p[2] = { (uint32_t)(uint16_t)v[i][0] | ((uint32_t)(uint16_t)v[i][1] << 16), (uint32_t)(uint16_t)v[i][2] };

        fifo(g, 0x23u, p, 2u);
    }
    gpu3d_fifo(g, 0x41u);
}

static void seal(Gpu3d *g)
{
    fifo1(g, 0x50u, 0u);
    gpu3d_vblank(g);
}

static uint32_t box(Gpu3d *g, int16_t x, int16_t y, int16_t z, int16_t w, int16_t h, int16_t d)
{
    uint32_t p[3] = { (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16), (uint32_t)(uint16_t)z | ((uint32_t)(uint16_t)w << 16),
                      (uint32_t)(uint16_t)h | ((uint32_t)(uint16_t)d << 16) };

    fifo(g, 0x70u, p, 3u);
    return (gpu3d_read(g, GPU3D_REG_GXSTAT) >> 1) & 1u;
}

static void test_formula(void)
{
    int idx, n = 0;
    double worst_x = 0.0, worst_y = 0.0;
    int ok = 0;

    CHECK(hwwide_aspect(4096 * 4 / 3) == 6826);
    CHECK(hwwide_sin(3276, &ok) == 4095 && ok == 1);
    CHECK(hwwide_sin(3277, &ok) == 3277 && ok == 0);   /* 3277 * 5/4 rounds to 4096: not a sine; left narrow */
    for (idx = 16; idx < 16384; idx += 16) {   /* fovY index (65536 = 360 degrees), the sine table's step */
        double a = (double)idx * 2.0 * 3.14159265358979323846 / 65536.0;
        int32_t sn = (int32_t)lround(sin(a) * 4096.0), cs = (int32_t)lround(cos(a) * 4096.0), m11, m00, w11, w00, ws;
        double ex, ey;

        ws = hwwide_sin(sn, &ok);
        if (!ok || sn <= 0) {
            continue;
        }
        m11 = fx_div(cs, sn);
        m00 = fx_div(m11, 4096 * 4 / 3);
        w11 = fx_div(cs, ws);
        w00 = fx_div(w11, hwwide_aspect(4096 * 4 / 3));
        if (idx > 8192 || sn < 256) {
            continue;   /* half-angles 3.6 .. 45 degrees (the game's cameras: the field's is 8.1): outside that the 20.12 sine / matrix has too few bits for 0.2 % */
        }
        ex = fabs(200.0 * w00 - 128.0 * m00) / (128.0 * m00);
        ey = fabs(120.0 * w11 - 96.0 * m11) / (96.0 * m11);
        worst_x = ex > worst_x ? ex : worst_x;
        worst_y = ey > worst_y ? ey : worst_y;
        n++;
    }
    CHECK(n > 400);
    CHECK(worst_x < 2e-3 && worst_y < 2e-3);
    printf("hw_wide_test: per-pixel scale over %d fovY values: worst relative difference x %.2e, y %.2e (20.12 rounding; the field camera's is below 1e-4)\n", n, worst_x,
           worst_y);
}

/* the field camera (src/overlay005/field_camera.c CAMERA_TYPE_DEFAULT): narrow _00 / _11 and the widened ones (the private wide-model harness rebuilds them from the trace) */
static const int32_t NAR[16] = { 21628, 0, 0, 0, 0, 28836, 0, 0, 0, 0, -4096, -4096, 0, 0, -4096, 0 };
static const int32_t WID[16] = { 13842, 0, 0, 0, 0, 23068, 0, 0, 0, 0, -4096, -4096, 0, 0, -4096, 0 };
static const int32_t ORTN[16] = { 2048, 0, 0, 0, 0, 2730, 0, 0, 0, 0, -2048, 0, 0, 0, 0, 4096 };
static const int32_t ORTW[16] = { 1311, 0, 0, 0, 0, 2184, 0, 0, 0, 0, -2048, 0, 0, 0, 0, 4096 };

static void test_engine(void)
{
    Gpu3d *n = (Gpu3d *)calloc(1, sizeof *n), *w = (Gpu3d *)calloc(1, sizeof *w), *s = (Gpu3d *)calloc(1, sizeof *s);
    /* z = -2.0: w = 8192. x = 1000 * 21628 / 4096 = 5280 (inside); 2000 -> 10560 = 1.29 w (wide only); 2500 -> 1.61 w (the stereo guard band only) */
    static const int16_t IN[3][3] = { { -1000, -500, -8192 }, { 1000, -500, -8192 }, { 0, 800, -8192 } };
    static const int16_t WIDE[3][3] = { { 1900, -100, -8192 }, { 2050, -100, -8192 }, { 2000, 100, -8192 } };
    static const int16_t GUARD[3][3] = { { 2450, -100, -8192 }, { 2600, -100, -8192 }, { 2500, 100, -8192 } };
    uint32_t i, k, same = 1u;

    if (n == NULL || w == NULL || s == NULL) {
        CHECK(0);
        return;
    }
    gpu3d_reset(n);
    gpu3d_reset(w);
    gpu3d_reset(s);
    gpu3d_wide_config(w, 1u, 0u, 0);
    gpu3d_wide_config(s, 1u, 1u, HWWIDE_GUARD_Q12);
    gpu3d_wide_register(w, WID, NAR[0], NAR[5], 2731713);
    gpu3d_wide_register(w, ORTW, ORTN[0], ORTN[5], 2731713);
    gpu3d_wide_register(s, WID, NAR[0], NAR[5], 2731713);
    gpu3d_wide_register(s, ORTW, ORTN[0], ORTN[5], 2731713);

    /* 2: the DS matrix is what is applied; the tags */
    setup(n, NAR);
    setup(w, WID);
    for (i = 0; i < 16u; i++) {
        same &= gpu3d_read(n, GPU3D_REG_CLIPMTX + 4u * i) == gpu3d_read(w, GPU3D_REG_CLIPMTX + 4u * i);
    }
    CHECK(same == 1u);
    CHECK(w->proj_tag == (GPU3D_WT_WIDE | GPU3D_WT_PERSP));
    CHECK(w->proj_m00 == NAR[0] && w->proj_dist == 2731713);
    {
        Gpu3d *o = (Gpu3d *)calloc(1, sizeof *o);

        gpu3d_reset(o);
        gpu3d_wide_config(o, 1u, 0u, 0);
        gpu3d_wide_register(o, ORTW, ORTN[0], ORTN[5], 2731713);
        setup(o, ORTW);
        CHECK(o->proj_tag == GPU3D_WT_WIDE);   /* the w row is 0 0 0 1: no perspective, no shear */
        setup(o, NAR);                          /* an unregistered projection: centred, untagged */
        CHECK(o->proj_tag == 0u);
        free(o);
    }

    /* 3: inside -> the DS pixels + (72, 24) */
    tri(n, IN);
    tri(w, IN);
    seal(n);
    seal(w);
    CHECK(gpu3d_render_count(n) == 1u && gpu3d_render_count(w) == 1u);
    if (gpu3d_render_count(n) == 1u && gpu3d_render_count(w) == 1u) {
        const Gpu3dPoly *pn = gpu3d_render_poly(n, 0), *pw = gpu3d_render_poly(w, 0);

        CHECK(pw->wtag == (GPU3D_WT_WIDE | GPU3D_WT_PERSP) && pn->wtag == 0u);
        CHECK(pn->nverts == pw->nverts);
        for (k = 0; k < pn->nverts && k < pw->nverts; k++) {
            const Gpu3dVtx *a = gpu3d_render_vtx(n, pn, k), *b = gpu3d_render_vtx(w, pw, k);
            int32_t sx, sy;

            CHECK(b->sx == a->sx + 72 && b->sy == a->sy + 24);
            gpu3d_viewport_wide_xy(b->pos[0], b->pos[1], b->pos[3], &sx, &sy);
            CHECK(sx == b->sx && sy == b->sy);   /* the list's map (the PICA's) is the engine's */
        }
        CHECK(pw->ytop == pn->ytop + 24);
    }
    /* beyond the DS x plane, inside the wide one */
    setup(n, NAR);
    setup(w, WID);
    tri(n, WIDE);
    tri(w, WIDE);
    seal(n);
    seal(w);
    CHECK(gpu3d_render_count(n) == 0u);
    CHECK(gpu3d_render_count(w) == 1u);
    if (gpu3d_render_count(w) == 1u) {
        const Gpu3dPoly *pw = gpu3d_render_poly(w, 0);

        CHECK(gpu3d_render_vtx(w, pw, 0)->sx > 328 && gpu3d_render_vtx(w, pw, 0)->sx < 400);
    }
    /* the guard band: past the wide plane, kept by the stereo engine for a perspective camera only */
    setup(w, WID);
    setup(s, WID);
    tri(w, GUARD);
    tri(s, GUARD);
    seal(w);
    seal(s);
    CHECK(gpu3d_render_count(w) == 0u);
    CHECK(gpu3d_render_count(s) == 1u);
    CHECK(s->st_guard_polys == 0u);   /* entirely inside the guard planes: not clipped at all */
    {
        int32_t d = 0, m = 0;

        gpu3d_render_stereo(s, &d, &m);
        CHECK(d == 2731713 && m == NAR[0]);
    }
    /* BOX_TEST sees the wide volume: a box at x = 2000..2100 */
    setup(n, NAR);
    setup(w, WID);
    CHECK(box(n, 2000, -100, -8192, 100, 200, 100) == 0u);
    CHECK(box(w, 2000, -100, -8192, 100, 200, 100) == 1u);
    CHECK(box(w, 2500, -100, -8192, 100, 200, 100) == 0u);
    setup(s, WID);
    CHECK(box(s, 2500, -100, -8192, 100, 200, 100) == 1u);
    free(n);
    free(w);
    free(s);
}

static void test_maps(void)
{
    int32_t sx, sy;

    gpu3d_viewport_wide_xy(0, 0, 8192, &sx, &sy);
    CHECK(sx == 200 && sy == 120);
    gpu3d_viewport_wide_xy(-8192, 8192, 8192, &sx, &sy);   /* the DS frame's corner (0, 0) */
    CHECK(sx == 72 && sy == 24);
    gpu3d_viewport_wide_xy(-8193, 8193, 8192, &sx, &sy);   /* just outside: floored, not truncated to the DS corner */
    CHECK(sx == 71 && sy == 23);
    gpu3d_viewport_wide_xy(-12800, -9830, 8192, &sx, &sy); /* the wide frame's edges: x = -1.5625 w, y = -1.2 w */
    CHECK(sx == 0 && sy == 235);
    CHECK(hwwide_shear(1234, 2731713, 2731713, 0.38f) == 1234);
    CHECK(hwwide_shear(0, 2 * 2731713, 2731713, 0.25f) == 682928);
    CHECK(hwwide_shear(0, 2 * 2731713, 2731713, -0.25f) == -682928);
    CHECK(hwwide_sigma(0.0f, 21628, 2731713, HWWIDE_IOD) == 0.0f);
    CHECK(fabsf(hwwide_sigma(1.0f, 21628, 2731713, HWWIDE_IOD) - 0.38f) < 0.001f);
    /* review 2.8 N2: the guard bound sigma |1 - c/w| <= 1.5625 g (g = 512/4096): holds while the shear stays under 0.1953 of w (sigma 0.38 and w >= 0.66 c is the field camera's edge), fails
       for a camera that shears further */
    CHECK(hwwide_shear_in_guard(2731713, 2731713, 0.38f, HWWIDE_GUARD_Q12) == 1);          /* w = c: no shear at all */
    CHECK(hwwide_shear_in_guard(2 * 2731713, 2731713, 0.38f, HWWIDE_GUARD_Q12) == 1);      /* far: 0.38 * 0.5 = 0.19 <= 0.1953 */
    CHECK(hwwide_shear_in_guard(2 * 2731713, 2731713, 0.40f, HWWIDE_GUARD_Q12) == 0);      /* sigma 0.40 * 0.5 = 0.20 > 0.1953 */
    CHECK(hwwide_shear_in_guard(2731713 / 3, 2731713, 0.38f, HWWIDE_GUARD_Q12) == 0);      /* near: w = c/3, 0.38 * 2 = 0.76 */
    CHECK(hwwide_shear_in_guard(2731713 / 3, 2731713, -0.38f, HWWIDE_GUARD_Q12) == 0);     /* the sign does not matter */
    CHECK(hwwide_shear_in_guard(0, 2731713, 0.38f, HWWIDE_GUARD_Q12) == 1);
}

/* engine A, one uniform register image: display mode 1, BG0 on as the 3D layer, the backdrop colour, MASTER_BRIGHT `mb`, BLDCNT `bld`. */
static void compose_frame(Gpu2dFrame *f, uint32_t dispcnt, uint32_t mb, uint32_t bld)
{
    uint8_t io[0x1070];

    memset(io, 0, sizeof io);
    io[0] = (uint8_t)dispcnt; io[1] = (uint8_t)(dispcnt >> 8); io[2] = (uint8_t)(dispcnt >> 16); io[3] = (uint8_t)(dispcnt >> 24);
    io[0x6C] = (uint8_t)mb; io[0x6D] = (uint8_t)(mb >> 8);
    io[0x50] = (uint8_t)bld; io[0x51] = (uint8_t)(bld >> 8);
    io[0x1000] = 1;   /* engine B display mode 1 */
    io[0x1002] = 1;
    gpu2d_frame_lines(f, io, 0u, GPU2D_LINES);
    f->pltt[0][0] = 0x1F; f->pltt[0][1] = 0x40;   /* backdrop: r 31, g 2, b 16 */
}

static void test_compose(void)
{
    Gpu2dFrame *f = (Gpu2dFrame *)calloc(1u, sizeof *f);
    Gpu2dOut *o = (Gpu2dOut *)calloc(1u, sizeof *o);
    uint32_t *lw = (uint32_t *)malloc(400u * 240u * 4u), *crop = (uint32_t *)malloc(256u * 192u * 4u), *dst = (uint32_t *)malloc(400u * 240u * 4u);
    uint8_t *fb = (uint8_t *)malloc(400u * 240u * 3u), *dump = (uint8_t *)malloc(HWSCR_WDUMP_SIZE);
    uint32_t x, y, step, bad = 0u;

    if (f == NULL || o == NULL || lw == NULL || crop == NULL || dst == NULL || fb == NULL || dump == NULL) {
        CHECK(0);
        return;
    }
    /* the wide layer: a word that depends on (x % 64, y % 64) only, with transparent hole columns (x % 64 == 10) and translucent stripes (y % 64 == 5), so every border
       pixel has a centre twin (cx, cy) carrying the same layer word */
    for (y = 0; y < 240u; y++)
        for (x = 0; x < 400u; x++) {
            uint32_t a = (x % 64u == 10u) ? 0u : ((y % 64u == 5u) ? 15u : 31u);
            lw[y * 400u + x] = (x % 64u) | ((y % 64u) << 8) | (40u << 16) | (a << 24);
        }
    for (y = 0; y < 192u; y++) memcpy(crop + y * 256u, lw + (y + 24u) * 400u + 72u, 256u * 4u);
    /* every master-brightness step of a fade, up and down, with the 3D layer blended over the backdrop (BLDCNT 2nd target = backdrop) */
    for (step = 0; step <= 16u; step++) {
        uint32_t m;
        for (m = 1; m <= 2u; m++) {
            compose_frame(f, 0x10108u, (m << 14) | step, 0x2000u);
            f->layer3d = crop;
            gpu2d_render(f, o);
            gpu2d_compose_wide(f, o, lw, dst);
            for (y = 0; y < 192u; y++)   /* the centre is engine A 1:1 */
                if (memcmp(dst + (y + 24u) * 400u + 72u, &o->pix[0][y * 256u], 256u * 4u) != 0) bad++;
            for (y = 0; y < 240u; y++)
                for (x = 0; x < 400u; x++) {
                    uint32_t cx = 128u + x % 64u, cy = 64u + (y % 64u + 64u - 0u) % 64u;
                    if (x >= 72u && x < 328u && y >= 24u && y < 216u) continue;
                    if (dst[y * 400u + x] != dst[cy * 400u + cx]) bad++;
                }
        }
    }
    CHECK(bad == 0u);
    /* a mutant: a border composed without the brightness (MASTER_BRIGHT 0 in its registers) next to the faded centre must be caught by the same twin comparison */
    compose_frame(f, 0x10108u, (2u << 14) | 8u, 0x2000u);
    f->layer3d = crop;
    gpu2d_render(f, o);
    {
        Gpu2dFrame *g = (Gpu2dFrame *)calloc(1u, sizeof *g);
        uint32_t *d2 = (uint32_t *)malloc(400u * 240u * 4u);
        if (g != NULL && d2 != NULL) {
            compose_frame(g, 0x10108u, 0u, 0x2000u);
            gpu2d_compose_wide(g, o, lw, d2);   /* centre: the faded composite; border: unfaded */
            CHECK(d2[0] != d2[64u * 400u + 128u]);
        }
        free(g);
        free(d2);
    }
    /* the 3D layer off (DISPCNT bit 3 clear): the border is the backdrop, as a centre pixel without 3D */
    compose_frame(f, 0x10100u, (1u << 14) | 5u, 0u);
    f->layer3d = crop;
    gpu2d_render(f, o);
    gpu2d_compose_wide(f, o, lw, dst);
    CHECK(dst[0] == o->pix[0][0] && dst[239u * 400u + 399u] == o->pix[0][0]);
    /* display mode 0: white, no brightness */
    compose_frame(f, 0x00108u, (2u << 14) | 16u, 0u);
    gpu2d_render(f, o);
    gpu2d_compose_wide(f, o, lw, dst);
    CHECK(dst[0] == 0xFFFFFFFFu && dst[24u * 400u + 72u] == 0xFFFFFFFFu);
    /* a mutant: a compose that skips the brightness in the border must be caught by the same comparison */
    compose_frame(f, 0x10108u, (2u << 14) | 8u, 0u);
    gpu2d_render(f, o);
    gpu2d_compose_wide(f, o, lw, dst);
    {
        Gpu2dFrame *g = (Gpu2dFrame *)calloc(1u, sizeof *g);
        uint32_t *d2 = (uint32_t *)malloc(400u * 240u * 4u);
        if (g != NULL && d2 != NULL) {
            compose_frame(g, 0x10108u, 0u, 0u);
            g->layer3d = crop;
            gpu2d_compose_wide(g, o, lw, d2);   /* centre: the faded composite; border: unfaded */
            CHECK(d2[0] != dst[64u * 400u + 128u]);   /* the mutant's border no longer matches the centre */
        }
        free(g);
        free(d2);
    }
    /* the framebuffer layout and the dump */
    for (x = 0; x < 400u * 240u; x++) dst[x] = 0xFF000000u | x;
    hwscr_blit_bgr8_wide(dst, fb);
    CHECK(fb[((3u * 240u) + (239u - 7u)) * 3u + 0u] == (uint8_t)(7u * 400u + 3u));   /* pixel (3, 7): B byte */
    compose_frame(f, 0x10108u, (2u << 14) | 8u, 0x2000u);
    hwscr_wdump_pack(dump, 1234u, dst, NULL, lw, NULL, 0x1F000000u, f, 0u, 0.0f);
    CHECK(dump[0] == 'F' && dump[1] == 'B' && dump[2] == 'W' && dump[3] == '1' && dump[12] == 0u && dump[16] == (400u & 255u));
    CHECK(dump[64 + 3 * 5 + 2] == 5u && dump[64 + 400u * 240u * 3u] == 0u);
    CHECK(dump[55] == 0x1Fu && dump[64 + 2u * 400u * 240u * 3u + 400u * 240u * 4u] == 0u);
    CHECK(dump[32] == 8u && dump[33] == 0x80u && dump[37] == 0x80u && dump[41] == 0x20u && dump[44] == 0x08u && dump[48] == 0x1Fu);
    CHECK(memcmp(dump + 64 + 2u * 400u * 240u * 3u + 4u * 777u, &lw[777], 4u) == 0);
    free(f); free(o); free(lw); free(crop); free(dst); free(fb); free(dump);
}

/* the eye's sub-pixel x: at e_sigma 0 its floor is the snapped wide map, and at full shear it stays within one pixel of the snapped sheared vertex */
static void test_shear_sx(void)
{
    int32_t w, x, sx, sy, bad0 = 0, bad1 = 0, n = 0;
    for (w = 4096; w <= (1 << 23); w = w * 3 + 77) {
        for (x = -w * 2; x <= w * 2; x += w / 37 + 1) {
            double f0 = hwwide_shear_sx(x, w, 2731713, 0.0f), f1 = hwwide_shear_sx(x, w, 2731713, 0.38f);
            gpu3d_viewport_wide_xy(x, 0, w, &sx, &sy);
            bad0 += (int32_t)floor(f0) != sx;
            gpu3d_viewport_wide_xy(hwwide_shear(x, w, 2731713, 0.38f), 0, w, &sx, &sy);
            bad1 += fabs(f1 - (double)sx) > 1.0;
            n++;
        }
    }
    CHECK(n > 1000);
    CHECK(bad0 == 0);
    CHECK(bad1 == 0);
    CHECK(hwwide_shear_sx(100, 0, 5, 0.38f) == 0.0 && hwwide_shear_sx(100, -5, 5, 0.38f) == 0.0);
}

int main(void)
{
    test_formula();
    test_engine();
    test_maps();
    test_compose();
    test_shear_sx();
    printf("hw_wide_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
