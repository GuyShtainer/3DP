/* gpu3d_selfcheck_test.c -- bet 2.7a: the synthetic check frames (gpu3d_selfcheck.c) against a software model of what the PICA path reads back. The model fills the logical depth/stencil
 * image and the layer from the LIST ITSELF (the screen box of each quad through gpu3d_viewport_xy, the depth through gpu3d_depth24_z), so it also proves the quads land where the
 * probes sit. Then: the correct image is OK; the previous frame's image is STALE; byte-order and transposed images are LAYOUT failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu3d_edge.h"
#include "gpu3d_list.h"
#include "gpu3d_selfcheck.h"
#include "gpu3d_tex.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t g_ds[G3E_PIXELS], g_layer[G3E_PIXELS];

/* what a correct PICA readback of frame n holds */
static int model(uint32_t n)
{
    static uint8_t buf[G3S_LIST_BYTES];
    Gpu3dList l;
    uint32_t i, x, y;
    size_t len = g3s_build(buf, sizeof buf, n);

    if (len == 0u || gpu3d_list_open(&l, buf, len) != GPU3D_ERR_OK) {
        return 0;
    }
    for (i = 0; i < G3E_PIXELS; i++) {
        g_ds[i] = (((l.clear_color >> 24) & 0x3Fu) << 24) | g3e_clear_code(l.clear_depth);
        g_layer[i] = 0;
    }
    for (i = 0; i < l.npolys; i++) {
        Gpu3dPoly p;
        Gpu3dVert v0, v2;
        int32_t sx0, sy0, sx1, sy1;

        gpu3d_list_poly(&l, i, &p);
        gpu3d_list_vert(&l, p.first, &v0);
        gpu3d_list_vert(&l, p.first + 2u, &v2);
        gpu3d_viewport_xy(l.viewport, v0.x, v0.y, v0.w, &sx0, &sy0);
        gpu3d_viewport_xy(l.viewport, v2.x, v2.y, v2.w, &sx1, &sy1);
        for (y = (uint32_t)sy0; y < (uint32_t)sy1; y++) {
            for (x = (uint32_t)sx0; x < (uint32_t)sx1; x++) {
                uint32_t code = g3e_depth_from_ds(gpu3d_depth24_z(v0.z, v0.w)), old = g_ds[y * G3E_W + x] & 0xFFFFFFu;

                if (code < old) {   /* depth test: nearer passes */
                    g_ds[y * G3E_W + x] = (0x80u | ((p.attr >> 24) & 0x3Fu)) << 24 | code;
                    g_layer[y * G3E_W + x] = gpu3d_c5_to_c6(v0.r) | (gpu3d_c5_to_c6(v0.g) << 8) | (gpu3d_c5_to_c6(v0.b) << 16);
                }
            }
        }
    }
    return 1;
}

/* what a correct readback of the translucent frame G3S_TRANS holds, from the list itself and the DS blend rules: an opaque polygon draws into the stencil/depth/colour image; the
   frame splits at the first translucent polygon, which takes a copy of the image (the first copy: IDs and depth of the opaque polygons); a translucent polygon passing the depth
   test BLENDS over a covered pixel and STORES over an empty one, and writes its depth (attr bit 11); the readback is the first copy with the final depth merged in. */
static int model_trans(void)
{
    static uint8_t buf[G3S_LIST_BYTES];
    static uint32_t first[G3E_PIXELS], depth[G3E_PIXELS];
    static uint8_t c5[3][G3E_PIXELS];   /* the colour in 5-bit channels, as the DS blends it */
    Gpu3dList l;
    uint32_t i, x, y;
    size_t len = g3s_build(buf, sizeof buf, G3S_TRANS);
    int split = 0;

    if (len == 0u || gpu3d_list_open(&l, buf, len) != GPU3D_ERR_OK) {
        return 0;
    }
    for (i = 0; i < G3E_PIXELS; i++) {
        g_ds[i] = (((l.clear_color >> 24) & 0x3Fu) << 24) | g3e_clear_code(l.clear_depth);
        depth[i] = g_ds[i] & 0xFFFFFFu;
        g_layer[i] = 0;
    }
    for (i = 0; i < l.npolys; i++) {
        Gpu3dPoly p;
        Gpu3dVert v0, v2;
        int32_t sx0, sy0, sx1, sy1;
        int tr;

        gpu3d_list_poly(&l, i, &p);
        tr = gpu3d_is_translucent(p.attr, 0u);
        if (tr && !split) {
            memcpy(first, g_ds, sizeof first);
            split = 1;
        }
        gpu3d_list_vert(&l, p.first, &v0);
        gpu3d_list_vert(&l, p.first + 2u, &v2);
        gpu3d_viewport_xy(l.viewport, v0.x, v0.y, v0.w, &sx0, &sy0);
        gpu3d_viewport_xy(l.viewport, v2.x, v2.y, v2.w, &sx1, &sy1);
        for (y = (uint32_t)sy0; y < (uint32_t)sy1; y++) {
            for (x = (uint32_t)sx0; x < (uint32_t)sx1; x++) {
                uint32_t k = y * G3E_W + x, code = g3e_depth_from_ds(gpu3d_depth24_z(v0.z, v0.w));

                if (code >= depth[k]) {
                    continue;
                }
                if (!tr) {
                    g_ds[k] = (0x80u | ((p.attr >> 24) & 0x3Fu)) << 24 | code;
                    depth[k] = code;
                    c5[0][k] = (uint8_t)v0.r; c5[1][k] = (uint8_t)v0.g; c5[2][k] = (uint8_t)v0.b;
                    g_layer[k] = gpu3d_c5_to_c6(v0.r) | (gpu3d_c5_to_c6(v0.g) << 8) | (gpu3d_c5_to_c6(v0.b) << 16);
                } else {
                    uint32_t covered = (g_ds[k] >> 31) & 1u, r = v0.r, g = v0.g, b = v0.b;
                    uint32_t f = ((p.attr >> 16) & 31u) + 1u;   /* the source's share, in 32nds */

                    if (covered) {   /* blend in 5 bits, as the DS does */
                        r = (r * f + c5[0][k] * (32u - f)) / 32u;
                        g = (g * f + c5[1][k] * (32u - f)) / 32u;
                        b = (b * f + c5[2][k] * (32u - f)) / 32u;
                    }
                    c5[0][k] = (uint8_t)r; c5[1][k] = (uint8_t)g; c5[2][k] = (uint8_t)b;
                    g_layer[k] = gpu3d_c5_to_c6(r) | (gpu3d_c5_to_c6(g) << 8) | (gpu3d_c5_to_c6(b) << 16);
                    if ((p.attr >> 11) & 1u) {
                        depth[k] = code;
                    }
                }
            }
        }
    }
    for (i = 0; i < G3E_PIXELS; i++) {   /* the readback: the first copy's stencil, the final depth */
        g_ds[i] = (first[i] & 0xFF000000u) | depth[i];
    }
    return 1;
}

/* F-2: the chain frames' polygons are opaque (alpha 31) with no depth-write bit, whatever the stack held (the quads' alpha/depthwrite were once left uninitialised) */
static void __attribute__((noinline)) poison_stack(void)
{
    volatile uint8_t junk[4096];
    uint32_t i;
    for (i = 0; i < sizeof junk; i++) junk[i] = 0xA5u;
}

static void attr_clean(void)
{
    static uint8_t buf[G3S_LIST_BYTES];
    Gpu3dList l;
    uint32_t n, i;
    for (n = 0; n < G3S_FRAMES; n++) {
        poison_stack();
        CHECK(g3s_build(buf, sizeof buf, n) != 0u && gpu3d_list_open(&l, buf, sizeof buf) == GPU3D_ERR_OK, "frame %u: attr_clean: the list does not open", (unsigned)n);
        for (i = 0; i < 2u; i++) {
            Gpu3dPoly p;
            gpu3d_list_poly(&l, i, &p);
            CHECK(((p.attr >> 16) & 31u) == 31u && ((p.attr >> 11) & 1u) == 0u && (p.attr & 0x0000FFFFu) == 0xC0u, "frame %u poly %u: attr %08x is not opaque/no-depth-write", (unsigned)n, (unsigned)i, (unsigned)p.attr);
        }
    }
}

int main(void)
{
    static uint32_t prev_ds[G3E_PIXELS], prev_layer[G3E_PIXELS], bad[G3E_PIXELS];
    G3sVerdict v;
    uint32_t n, i, x, y;
    static uint8_t tiny[16];

    attr_clean();
    CHECK(g3s_build(tiny, sizeof tiny, 0) == 0u, "a too small buffer is refused");
    CHECK(g3s_build(tiny, 4096, G3S_TOTAL) == 0u, "frame 9 is out of range");
    for (n = 0; n < G3S_FRAMES; n++) {
        CHECK(model(n), "frame %u: the list does not open", (unsigned)n);
        CHECK(g3s_judge(g_ds, g_layer, n, &v) == G3S_OK, "frame %u: the model image is judged %d (%s %s got %x want %x)", (unsigned)n, v.code, v.probe, v.what, (unsigned)v.got, (unsigned)v.want);
        if (n > 0u) {
            CHECK(g3s_judge(prev_ds, prev_layer, n, &v) == G3S_STALE, "frame %u: the previous frame's image is judged %d, not STALE", (unsigned)n, v.code);
        }
        /* layout faults: depth and stencil bytes swapped; the image transposed (the de-tile walked the wrong way) */
        for (i = 0; i < G3E_PIXELS; i++) {
            bad[i] = (g_ds[i] << 8) | (g_ds[i] >> 24);
        }
        CHECK(g3s_judge(bad, g_layer, n, &v) == G3S_LAYOUT, "frame %u: rotated words judged %d", (unsigned)n, v.code);
        for (y = 0; y < G3E_H; y++) {
            for (x = 0; x < G3E_W; x++) {
                bad[y * G3E_W + x] = g_ds[(x % G3E_H) * G3E_W + y];
            }
        }
        CHECK(g3s_judge(bad, g_layer, n, &v) == G3S_LAYOUT, "frame %u: transposed image judged %d", (unsigned)n, v.code);
        memcpy(prev_ds, g_ds, sizeof prev_ds);
        memcpy(prev_layer, g_layer, sizeof prev_layer);
    }
    /* the frame-0 stale case does not exist (nothing came before it) */
    model(0);
    memset(bad, 0, sizeof bad);
    CHECK(g3s_judge(bad, g_layer, 0, &v) == G3S_LAYOUT, "a zeroed depth/stencil image is a LAYOUT failure");
    CHECK(g3s_judge(g_ds, g_layer, G3S_TOTAL, &v) == G3S_LAYOUT, "frame 9 refused by the judge");
    /* N4: the translucent frame. The model image is OK; a readback without the depth merge, one that stored instead of blended, one that blended over the empty plane, and a stale
       stencil (the translucent ID where the opaque one belongs) are LAYOUT failures. */
    CHECK(model_trans(), "the translucent frame's list does not open");
    CHECK(g3s_judge(g_ds, g_layer, G3S_TRANS, &v) == G3S_OK, "translucent frame: the model image is judged %d (%s %s got %x want %x)", v.code, v.probe, v.what, (unsigned)v.got, (unsigned)v.want);
    CHECK((g_ds[100 * G3E_W + 100] >> 24) == (0x80u | 9u) && (g_ds[100 * G3E_W + 150] >> 24) == 5u && (g_ds[100 * G3E_W + 40] >> 24) == (0x80u | 9u),
          "model stencils at O+T, T, O: %x %x %x", (unsigned)(g_ds[100 * G3E_W + 100] >> 24), (unsigned)(g_ds[100 * G3E_W + 150] >> 24), (unsigned)(g_ds[100 * G3E_W + 40] >> 24));
    CHECK((g_layer[100 * G3E_W + 100] & 63u) == gpu3d_c5_to_c6(12u), "the O+T colour is the 50 %% blend: red %u", (unsigned)(g_layer[100 * G3E_W + 100] & 63u));
    memcpy(bad, g_ds, sizeof bad);   /* the merge copy missing: the depth stays the first copy's (O's at O+T, the clear at T) */
    for (y = 0; y < G3E_H; y++) {
        for (x = 0; x < G3E_W; x++) {
            uint32_t k = y * G3E_W + x;

            if (x >= 80 && x < 176 && y >= 48 && y < 144) {
                bad[k] = (bad[k] & 0xFF000000u) | (x < 128 ? (g_ds[100 * G3E_W + 40] & 0xFFFFFFu) : g3e_clear_code(0x7FFFu));
            }
        }
    }
    CHECK(g3s_judge(bad, g_layer, G3S_TRANS, &v) == G3S_LAYOUT && strcmp(v.what, "depth") == 0, "no merge: judged %d (%s %s)", v.code, v.probe, v.what);
    memcpy(prev_layer, g_layer, sizeof prev_layer);   /* stored instead of blended */
    prev_layer[100 * G3E_W + 100] = gpu3d_c5_to_c6(20u) | (gpu3d_c5_to_c6(10u) << 8) | (gpu3d_c5_to_c6(1u) << 16);
    CHECK(g3s_judge(g_ds, prev_layer, G3S_TRANS, &v) == G3S_LAYOUT && strcmp(v.what, "colour") == 0, "stored not blended: judged %d (%s %s)", v.code, v.probe, v.what);
    memcpy(prev_layer, g_layer, sizeof prev_layer);   /* blended over the empty plane (darkened) */
    prev_layer[100 * G3E_W + 150] = gpu3d_c5_to_c6(12u) | (gpu3d_c5_to_c6(6u) << 8) | (gpu3d_c5_to_c6(2u) << 16);
    CHECK(g3s_judge(g_ds, prev_layer, G3S_TRANS, &v) == G3S_LAYOUT && strcmp(v.probe, "T") == 0, "blended over empty: judged %d (%s %s)", v.code, v.probe, v.what);
    memcpy(bad, g_ds, sizeof bad);
    bad[100 * G3E_W + 100] = (bad[100 * G3E_W + 100] & 0xFFFFFFu) | ((0x80u | 22u) << 24);
    CHECK(g3s_judge(bad, g_layer, G3S_TRANS, &v) == G3S_LAYOUT && strcmp(v.what, "stencil") == 0, "the translucent ID in the stencil: judged %d (%s %s)", v.code, v.probe, v.what);
    for (i = 0; i < G3E_PIXELS; i++) {
        bad[i] = (g_ds[i] << 8) | (g_ds[i] >> 24);
    }
    CHECK(g3s_judge(bad, g_layer, G3S_TRANS, &v) == G3S_LAYOUT, "translucent frame: rotated words judged %d", v.code);
    /* the overlap probe really is the nearer quad's */
    model(3);
    CHECK((g_ds[100 * G3E_W + 100] >> 24) == (0x80u | 23u) && (g_ds[100 * G3E_W + 40] >> 24) == (0x80u | 4u) && (g_ds[5 * G3E_W + 5] >> 24) == 5u,
          "model stencils at the probes: %x %x %x", (unsigned)(g_ds[100 * G3E_W + 100] >> 24), (unsigned)(g_ds[100 * G3E_W + 40] >> 24), (unsigned)(g_ds[5 * G3E_W + 5] >> 24));
    printf("gpu3d_selfcheck_test: %d checks, %d failures\n", checks, fails);
    return fails != 0 ? 1 : 0;
}
