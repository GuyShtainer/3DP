/* gpu3d_selfcheck.c -- see gpu3d_selfcheck.h. */
#include <assert.h>
#include <string.h>

#include "gpu3d_edge.h"
#include "gpu3d_list.h"
#include "gpu3d_selfcheck.h"
#include "gpu3d_tex.h"

#define VIEWPORT   0xBFFF0000u      /* x 0..255, y 0..191 */
#define CLEAR_ID   5u
#define CLEAR_DEPTH 0x7FFFu
#define DEPTH_TOL  0x1000u          /* codes (1/4096 of the range): a wrong layout is off by far more, the PICA's own rounding by a few */
#define ZA         0x800            /* clip z at w = 0x1000: farther quad */
#define ZB         (-0x800)         /* nearer quad */

typedef struct Quad {
    int32_t sx0, sx1, sy0, sy1;     /* screen pixel box, [sx0, sx1) x [sy0, sy1) */
    int32_t z;
    uint32_t id;
    uint8_t r, g, b;
    uint32_t alpha;                 /* polygon alpha 0..31 (0 = the opaque 31 of the chain frames) */
    uint32_t depthwrite;            /* translucent polygons: attr bit 11 */
} Quad;

/* check frame 8 (G3S_TRANS): O far and opaque, T nearer, translucent (alpha 15 = a 16/32 blend), depth write on, overlapping O's right half and reaching past it */
#define T_IDO   9u
#define T_IDT   22u
#define T_ALPHA 15u
static void quad_trans(Quad *o, Quad *t)
{
    *o = (Quad){ 32, 128, 48, 144, ZA, T_IDO, 4, 2, 3, 0, 0 };
    *t = (Quad){ 80, 176, 48, 144, ZB, T_IDT, 20, 10, 1, T_ALPHA, 1 };
}

static void quad_of(uint32_t n, Quad *a, Quad *b)
{
    memset(a, 0, sizeof *a);   /* F-2: alpha and depthwrite were left as stack garbage (a translucent poly in the chain frames); the opaque 31 comes from alpha 0 */
    memset(b, 0, sizeof *b);
    a->sx0 = 32; a->sx1 = 128; a->sy0 = 48; a->sy1 = 144; a->z = ZA; a->id = 1u + n; a->r = (uint8_t)(4u + n); a->g = 2; a->b = 3;
    b->sx0 = 80; b->sx1 = 176; b->sy0 = 48; b->sy1 = 144; b->z = ZB; b->id = 20u + n; b->r = 20; b->g = (uint8_t)(8u + n); b->b = 1;
}

static void corner(int32_t sx, int32_t sy, int32_t z, Gpu3dVert *v)
{
    memset(v, 0, sizeof *v);
    v->w = 0x1000;
    v->x = (sx - 128) * 32;                       /* sx = (x + w) * 256 / (2w) */
    v->y = 4096 - (sy * 8192) / 192;              /* sy = (w - y) * 192 / (2w) */
    v->z = z;
    v->sx = (int16_t)sx;
    v->sy = (int16_t)sy;
    v->depth24 = gpu3d_depth24_z(z, 0x1000);
}

static void put_quad(uint8_t *buf, uint32_t qi, const Quad *q)
{
    Gpu3dPoly p;
    Gpu3dVert v;
    uint32_t k;
    static const int cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };

    memset(&p, 0, sizeof p);
    p.attr = ((q->alpha != 0u ? q->alpha : 31u) << 16) | (q->id << 24) | 0xC0u | (q->depthwrite << 11);   /* alpha 31 (opaque) unless set, render both faces */
    p.nverts = 4;
    p.first = qi * 4u;
    p.ytop = (int16_t)q->sy0;
    p.ybottom = (int16_t)(q->sy1 - 1);
    gpu3d_list_put_poly(buf, qi, &p, 2);
    for (k = 0; k < 4u; k++) {
        corner(cx[k] ? q->sx1 : q->sx0, cy[k] ? q->sy1 : q->sy0, q->z, &v);
        v.r = q->r;
        v.g = q->g;
        v.b = q->b;
        gpu3d_list_put_vert(buf, qi * 4u + k, &v, 2);
    }
}

size_t g3s_build(uint8_t *buf, size_t cap, uint32_t n)
{
    Gpu3dList l;
    Quad a, b;
    size_t sz = gpu3d_list_size(2, 8);
    uint32_t i;

    assert(buf != NULL);
    if (n > G3S_TRANS || sz > cap) {
        return 0;
    }
    memset(buf, 0, sz);
    memset(&l, 0, sizeof l);
    l.key = 0x7001u + n;
    l.disp3dcnt = GPU3D_DISP_EDGE;
    l.clear_color = (CLEAR_ID << 24) | (31u << 16);
    l.clear_depth = CLEAR_DEPTH;
    l.viewport = VIEWPORT;
    l.npolys = 2;
    l.nverts = 8;
    l.render_count = 2;
    for (i = 0; i < 8u; i++) {
        l.edge_color[i] = 0x7FFFu;
    }
    gpu3d_list_put_header(buf, &l);
    if (n == G3S_TRANS) {
        quad_trans(&a, &b);
    } else {
        quad_of(n, &a, &b);
    }
    put_quad(buf, 0, &a);
    put_quad(buf, 1, &b);
    return sz;
}

typedef struct Probe {
    const char *name;
    uint32_t x, y, stencil, code;
    int has_colour;
    uint32_t r6, g6, b6;
    uint32_t tol;                   /* colour tolerance in 6-bit levels (the 8-bit PICA blend rounds) */
} Probe;

#define NPROBE 4u

static void expect(uint32_t n, Probe p[NPROBE])
{
    Quad a, b;

    quad_of(n, &a, &b);
    p[0] = (Probe){ "A", 40, 100, 0x80u | a.id, g3e_depth_from_ds(gpu3d_depth24_z(a.z, 0x1000)), 1, gpu3d_c5_to_c6(a.r), gpu3d_c5_to_c6(a.g), gpu3d_c5_to_c6(a.b), 1 };
    p[1] = (Probe){ "AB", 100, 100, 0x80u | b.id, g3e_depth_from_ds(gpu3d_depth24_z(b.z, 0x1000)), 1, gpu3d_c5_to_c6(b.r), gpu3d_c5_to_c6(b.g), gpu3d_c5_to_c6(b.b), 1 };
    p[2] = (Probe){ "B", 150, 100, 0x80u | b.id, g3e_depth_from_ds(gpu3d_depth24_z(b.z, 0x1000)), 1, gpu3d_c5_to_c6(b.r), gpu3d_c5_to_c6(b.g), gpu3d_c5_to_c6(b.b), 1 };
    p[3] = (Probe){ "OUT", 5, 5, CLEAR_ID, g3e_clear_code(CLEAR_DEPTH), 0, 0, 0, 0, 1 };
}

/* The translucent frame's probes, written from the DS rules and the port's split design (not from any de-tile): the stencil/depth image is the FIRST copy (taken before the rewrite
   quad, so it still holds the opaque IDs) with only the DEPTH replaced by the final copy (the merge); the colour is the TRANS carrier's: blend over a covered pixel, store over an empty one. */
static void expect_trans(Probe p[NPROBE])
{
    Quad o, t;
    uint32_t zo, zt;

    quad_trans(&o, &t);
    zo = g3e_depth_from_ds(gpu3d_depth24_z(o.z, 0x1000));
    zt = g3e_depth_from_ds(gpu3d_depth24_z(t.z, 0x1000));
    p[0] = (Probe){ "O", 40, 100, 0x80u | o.id, zo, 1, gpu3d_c5_to_c6(o.r), gpu3d_c5_to_c6(o.g), gpu3d_c5_to_c6(o.b), 1 };
    /* O+T: 16/32 of each (alpha 15 -> (15 + 1) / 32), the colour in 5 bits first: (4 + 20) / 2 = 12, (2 + 10) / 2 = 6, (3 + 1) / 2 = 2 */
    p[1] = (Probe){ "OT", 100, 100, 0x80u | o.id, zt, 1, gpu3d_c5_to_c6((o.r + t.r) / 2u), gpu3d_c5_to_c6((o.g + t.g) / 2u), gpu3d_c5_to_c6((o.b + t.b) / 2u), 2 };
    p[2] = (Probe){ "T", 150, 100, CLEAR_ID, zt, 1, gpu3d_c5_to_c6(t.r), gpu3d_c5_to_c6(t.g), gpu3d_c5_to_c6(t.b), 1 };
    p[3] = (Probe){ "OUT", 5, 5, CLEAR_ID, g3e_clear_code(CLEAR_DEPTH), 0, 0, 0, 0, 1 };
}

static uint32_t absdiff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

/* first mismatch of the image against the probes, or 0 (v filled when non-NULL) */
static int mismatch(const uint32_t *ds, const uint32_t *layer, const Probe p[NPROBE], G3sVerdict *v)
{
    uint32_t i;

    for (i = 0; i < NPROBE; i++) {
        uint32_t w = ds[p[i].y * G3E_W + p[i].x], c = layer[p[i].y * G3E_W + p[i].x];
        uint32_t cr = c & 63u, cg = (c >> 8) & 63u, cb = (c >> 16) & 63u;
        const char *what = NULL;
        uint32_t got = 0, want = 0;

        if ((w >> 24) != p[i].stencil) {
            what = "stencil"; got = w >> 24; want = p[i].stencil;
        } else if (absdiff(w & 0xFFFFFFu, p[i].code) > DEPTH_TOL) {
            what = "depth"; got = w & 0xFFFFFFu; want = p[i].code;
        } else if (p[i].has_colour && (absdiff(cr, p[i].r6) > p[i].tol || absdiff(cg, p[i].g6) > p[i].tol || absdiff(cb, p[i].b6) > p[i].tol)) {
            what = "colour"; got = c & 0x3F3F3Fu; want = p[i].r6 | (p[i].g6 << 8) | (p[i].b6 << 16);
        }
        if (what != NULL) {
            if (v != NULL) {
                v->x = p[i].x; v->y = p[i].y; v->got = got; v->want = want; v->what = what; v->probe = p[i].name;
            }
            return 1;
        }
    }
    return 0;
}

int g3s_judge(const uint32_t *ds, const uint32_t *layer, uint32_t n, G3sVerdict *v)
{
    Probe now[NPROBE], prev[NPROBE];
    G3sVerdict local;

    assert(ds != NULL && layer != NULL);
    if (v == NULL) {
        v = &local;
    }
    memset(v, 0, sizeof *v);
    if (n > G3S_TRANS) {
        v->code = G3S_LAYOUT;
        v->what = "frame";
        v->probe = "-";
        return v->code;
    }
    if (n == G3S_TRANS) {   /* no stale case: the previous frame's image is a different scene */
        expect_trans(now);
        v->code = mismatch(ds, layer, now, v) ? G3S_LAYOUT : G3S_OK;
        return v->code;
    }
    expect(n, now);
    if (!mismatch(ds, layer, now, v)) {
        v->code = G3S_OK;
        return v->code;
    }
    v->code = G3S_LAYOUT;
    if (n > 0u) {
        expect(n - 1u, prev);
        if (!mismatch(ds, layer, prev, NULL)) {
            v->code = G3S_STALE;
        }
    }
    return v->code;
}
