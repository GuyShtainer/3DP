/* gpu3d_edge.c -- see gpu3d_edge.h. Written from GBATEK ("DS 3D Video": Edge Marking, Depth Buffering) and the PICA framebuffer layout; no emulator source. */
#include "gpu3d_edge.h"

#include <assert.h>
#include <string.h>

uint32_t g3e_carrier(uint32_t disp3dcnt) { return (disp3dcnt & (1u << 5)) ? G3E_CARRIER_EDGE : G3E_CARRIER_TRANS; }

uint32_t g3e_clear_stencil(uint32_t carrier, uint32_t clear_color)
{
    assert(carrier == G3E_CARRIER_EDGE || carrier == G3E_CARRIER_TRANS);
    return carrier == G3E_CARRIER_EDGE ? ((clear_color >> 24) & G3E_EDGE_ID_MASK) : G3E_TRANS_EMPTY;
}

static void put_pass(G3ePass *p, uint32_t blend, uint32_t rel, uint32_t ref, uint32_t mask, uint32_t wmask, uint32_t replace)
{
    assert(p != NULL && rel <= G3E_REL_STENCIL_GT);
    assert(ref <= 0xFFu && mask <= 0xFFu && wmask <= 0xFFu);
    p->blend = (uint8_t)blend;
    p->rel = (uint8_t)rel;
    p->ref = (uint8_t)ref;
    p->mask = (uint8_t)mask;
    p->wmask = (uint8_t)wmask;
    p->replace = (uint8_t)replace;
}

void g3e_opaque_pass(uint32_t carrier, uint32_t attr, G3ePass *out)
{
    assert(out != NULL);
    assert(carrier == G3E_CARRIER_EDGE || carrier == G3E_CARRIER_TRANS);
    put_pass(out, 0, G3E_REL_ALWAYS, carrier == G3E_CARRIER_EDGE ? (G3E_EDGE_COVERED | ((attr >> 24) & G3E_EDGE_ID_MASK)) : G3E_TRANS_OPAQUE, 0xFFu, 0xFFu, 1);
}

uint32_t g3e_trans_passes(uint32_t carrier, uint32_t attr, int seen, G3ePass out[3])
{
    assert(out != NULL);
    assert(carrier == G3E_CARRIER_EDGE || carrier == G3E_CARRIER_TRANS);
    if (carrier == G3E_CARRIER_EDGE) {
        /* covered: blend, stencil untouched; empty: store and set bit 7 (the polygon ID bits stay the rear plane's / the opaque polygon's). Same-ID rule not applied. */
        put_pass(&out[0], 1, G3E_REL_EQ, G3E_EDGE_COVERED, G3E_EDGE_COVERED, 0, 0);
        put_pass(&out[1], 0, G3E_REL_NE, G3E_EDGE_COVERED, G3E_EDGE_COVERED, G3E_EDGE_COVERED, 1);
        return 2;
    } else {
        uint32_t s = G3E_TRANS_BLEND | ((attr >> 24) & G3E_EDGE_ID_MASK);
        if (!seen) {
            /* no pixel can hold this ID yet: covered (bit 7) blends, then empty stores; both leave `s` behind */
            put_pass(&out[0], 1, G3E_REL_EQ, s, 0x80u, 0xFFu, 1);
            put_pass(&out[1], 0, G3E_REL_NE, s, 0x80u, 0xFFu, 1);
            return 2;
        }
        /* the empty pixels first (they leave `s` behind, so the two passes after cannot draw them again), then the covered ones below `s` and above `s`, which skips the pixels
           whose latest blend was this ID. An empty pixel that failed the depth test in the first pass fails it again, so the two blend passes never draw it. */
        put_pass(&out[0], 0, G3E_REL_NE, s, 0x80u, 0xFFu, 1);
        put_pass(&out[1], 1, G3E_REL_STENCIL_LT, s, 0xFFu, 0xFFu, 1);
        put_pass(&out[2], 1, G3E_REL_STENCIL_GT, s, 0xFFu, 0xFFu, 1);
        return 3;
    }
}

void g3e_rewrite_pass(G3ePass *out)
{
    /* every pixel passes (ALWAYS, the depth test is switched to ALWAYS by the caller), the write mask keeps only bit 7: covered (0x80 | ID) -> 0x80, empty (rear-plane ID, bit 7 clear) -> 0x00 */
    assert(out != NULL);
    put_pass(out, 0, G3E_REL_ALWAYS, 0u, 0xFFu, 0x7Fu, 1);
}

int g3e_pass_stencil_ok(const G3ePass *p, uint32_t stencil)
{
    assert(p != NULL);
    uint32_t a = stencil & p->mask, b = p->ref & p->mask;
    switch (p->rel) {
    case G3E_REL_EQ: return a == b;
    case G3E_REL_NE: return a != b;
    case G3E_REL_STENCIL_LT: return a < b;
    case G3E_REL_STENCIL_GT: return a > b;
    default: return 1;
    }
}

uint32_t g3e_pass_stencil_new(const G3ePass *p, uint32_t stencil)
{
    assert(p != NULL);
    return p->replace ? ((stencil & ~(uint32_t)p->wmask) | (p->ref & p->wmask)) & 0xFFu : stencil;
}

uint32_t g3e_tiled_index(uint32_t x, uint32_t y, uint32_t w)
{
    assert(w != 0u && (w & 7u) == 0u);
    assert(x < w);
    uint32_t in = (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64u + in;
}

/* one tile walk for both de-tiles: keep_hi != 0 merges (only the depth, the low 24 bits, replaces the word's), else the whole word is written. raw_w x raw_h is the hardware
   buffer (the logical image turned: logical width = raw_h); the narrow callers pass the G3E_RAW_* constants (bet 2.8 added the wide 240 x 400 buffer) */
static void detile_walk(const uint32_t *tiled, uint32_t *out, uint32_t keep_hi, uint32_t raw_w, uint32_t raw_h)
{
    /* walk the raw buffer in memory order (one 8x8 tile after the other, z-order inside): reads are sequential, only the writes scatter */
    static const uint8_t ix[64] = { 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3,4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7,0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3,4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7 };
    static const uint8_t iy[64] = { 0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3,0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3,4,4,5,5,4,4,5,5,6,6,7,7,6,6,7,7,4,4,5,5,4,4,5,5,6,6,7,7,6,6,7,7 };
    uint32_t tx, ty, k;
    assert(raw_w % 8u == 0u && raw_h % 8u == 0u);
    for (ty = 0; ty < raw_h / 8u; ty++)
        for (tx = 0; tx < raw_w / 8u; tx++) {
            const uint32_t *t = tiled + (ty * (raw_w / 8u) + tx) * 64u;
            for (k = 0; k < 64u; k++) {
                uint32_t rx = tx * 8u + ix[k], ry = ty * 8u + iy[k];
                uint32_t *o = out + (raw_w - 1u - rx) * raw_h + ry;
                *o = keep_hi ? ((*o & 0xFF000000u) | (t[k] & 0xFFFFFFu)) : t[k];
            }
        }
}

void g3e_detile_ds(const uint32_t *tiled, uint32_t *out)
{
    assert(tiled != NULL && out != NULL);
    assert(tiled != out);
    detile_walk(tiled, out, 0u, G3E_RAW_W, G3E_RAW_H);
}

void g3e_detile_depth_merge(const uint32_t *tiled, uint32_t *io)
{
    assert(tiled != NULL && io != NULL);
    assert(tiled != io);
    detile_walk(tiled, io, 1u, G3E_RAW_W, G3E_RAW_H);
}

void g3e_detile_ds_wh(const uint32_t *tiled, uint32_t *out, uint32_t w, uint32_t h, int merge)
{
    assert(tiled != NULL && out != NULL);
    assert(tiled != out);
    assert(w <= G3E_WMAX && h <= G3E_WMAX);
    detile_walk(tiled, out, merge ? 1u : 0u, h, w);
}

uint32_t g3e_depth_to_ds(uint32_t code) { return code + (code >> 23); }

uint32_t g3e_depth_from_ds(uint32_t d24)
{
    uint64_t v = ((uint64_t)d24 * 0xFFFFFFu + 0x800000u) >> 24;
    return (uint32_t)v;
}

uint32_t g3e_clear_depth_ds(uint32_t clear_depth) { return (clear_depth & 0x7FFFu) * 0x200u + 0x1FFu; }

uint32_t g3e_clear_code(uint32_t clear_depth) { return g3e_depth_from_ds(g3e_clear_depth_ds(clear_depth)); }

uint32_t g3e_edge_rgb(uint16_t c555)
{
    assert(c555 <= 0x7FFFu);
    uint32_t r = ((uint32_t)c555 << 1) & 0x3Eu, g = ((uint32_t)c555 >> 4) & 0x3Eu, b = ((uint32_t)c555 >> 9) & 0x3Eu;
    if (r) r++;
    if (g) g++;
    if (b) b++;
    return r | (g << 8) | (b << 16);
}

/* the polygon ID and the DS-scale depth of the pixel at (x, y); a pixel outside the image is the rear plane */
static void sample(const uint32_t *ds, const G3eParams *p, int x, int y, uint32_t *id, uint32_t *z)
{
    assert(ds != NULL && p != NULL && id != NULL && z != NULL);
    if (x < 0 || y < 0 || x >= (int)G3E_W || y >= (int)G3E_H) {
        *id = p->clear_id;
        *z = p->clear_z;
    } else {
        uint32_t w = ds[(uint32_t)y * G3E_W + (uint32_t)x];
        *id = (w >> 24) & G3E_EDGE_ID_MASK;
        *z = g3e_depth_to_ds(w & 0xFFFFFFu);
    }
}

int g3e_edge_pixel(const uint32_t *ds, const G3eParams *p, uint32_t x, uint32_t y)
{
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    uint32_t w, id, z, k;
    assert(ds != NULL && p != NULL);
    assert(x < G3E_W && y < G3E_H);
    w = ds[y * G3E_W + x];
    if (!(w & ((uint32_t)G3E_EDGE_COVERED << 24))) return 0;
    id = (w >> 24) & G3E_EDGE_ID_MASK;
    z = g3e_depth_to_ds(w & 0xFFFFFFu);
    for (k = 0; k < 4; k++) {
        uint32_t nid, nz;
        sample(ds, p, (int)x + dx[k], (int)y + dy[k], &nid, &nz);
        if (nid != id && z < nz) return 1;
    }
    return 0;
}

/* the colour of an edge pixel: the edge colour at coverage 16 of 31 (the DS gives an edge pixel half coverage, drawn with the usual (alpha + 1) / 32 weight) over `below`, the colour of
   the polygon behind it; with nothing behind (a clear pixel or the screen border) the edge colour stays as it is and the alpha becomes 16 */
static uint32_t edge_mix(uint32_t edge, uint32_t below, uint32_t keep_alpha)
{
    uint32_t r, g, b;
    assert((edge >> 24) == 0u);
    assert((below >> 24) <= 31u);
    r = ((edge & 63u) * 17u + (below & 63u) * 15u) >> 5;
    g = (((edge >> 8) & 63u) * 17u + ((below >> 8) & 63u) * 15u) >> 5;
    b = (((edge >> 16) & 63u) * 17u + ((below >> 16) & 63u) * 15u) >> 5;
    return (keep_alpha & 0xFF000000u) | r | (g << 8) | (b << 16);
}

static uint32_t edge_pass_wh(uint32_t *layer, const uint32_t *ds, const G3eParams *p, const uint32_t W, const uint32_t H)
{
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    uint32_t rgb[8], n = 0, x, y, i;
    uint32_t prev[G3E_WMAX], cur[G3E_WMAX];          /* the layer rows y - 1 and y as they were before any edge colour went in (the blend reads the pixel behind, not an edge pixel) */
    assert(layer != NULL && ds != NULL && p != NULL);
    assert((const void *)layer != (const void *)ds);
    assert(W >= 3u && W <= G3E_WMAX && H >= 3u);
    for (i = 0; i < 8; i++) rgb[i] = g3e_edge_rgb(p->edge_color[i]);
    for (x = 0; x < W; x++) prev[x] = 0;
    for (y = 0; y < H; y++) {
        const uint32_t *row = ds + y * W;
        uint32_t *lrow = layer + y * W;
        memcpy(cur, lrow, W * sizeof cur[0]);
        for (x = 0; x < W; x++) {
            uint32_t w = row[x], id, z = 0, k, best_z = 0, best = 4u, bz, nid;
            if (!(w >> 31)) continue;
            id = (w >> 24) & G3E_EDGE_ID_MASK;
            if (y > 0 && y < H - 1u && x > 0 && x < W - 1u) {   /* interior: almost every pixel has four neighbours of its own ID, which one XOR test finds */
                const uint32_t *c = row + x;
                if ((((c[-1] ^ w) | (c[1] ^ w) | (c[-(int)W] ^ w) | (c[W] ^ w)) & ((uint32_t)G3E_EDGE_ID_MASK << 24)) == 0u) continue;
            }
            for (k = 0; k < 4u; k++) {
                int nx = (int)x + dx[k], ny = (int)y + dy[k];
                if (nx < 0 || ny < 0 || nx >= (int)W || ny >= (int)H) { nid = p->clear_id; bz = p->clear_z; }
                else {
                    uint32_t nw = ds[(uint32_t)ny * W + (uint32_t)nx];
                    nid = (nw >> 24) & G3E_EDGE_ID_MASK;
                    if (nid == id) continue;
                    bz = g3e_depth_to_ds(nw & 0xFFFFFFu);
                }
                if (nid == id) continue;
                if (!z) z = g3e_depth_to_ds(w & 0xFFFFFFu) + 1u;       /* +1 so that depth 0 still reads as "computed" */
                if (z - 1u < bz && (best == 4u || bz > best_z)) { best = k; best_z = bz; }
            }
            if (best == 4u) continue;
            {
                int nx = (int)x + dx[best], ny = (int)y + dy[best];
                uint32_t edge = rgb[id >> 3], below;
                if (nx < 0 || ny < 0 || nx >= (int)W || ny >= (int)H) below = 0;
                else below = ny < (int)y ? prev[nx] : (ny > (int)y ? lrow[nx + (int)W] : (nx < (int)x ? cur[nx] : lrow[nx]));
                if ((below >> 24) == 0u) lrow[x] = (16u << 24) | edge;
                else lrow[x] = edge_mix(edge, below, lrow[x]);
                n++;
            }
        }
        memcpy(prev, cur, W * sizeof prev[0]);
    }
    return n;
}

uint32_t g3e_edge_pass(uint32_t *layer, const uint32_t *ds, const G3eParams *p)
{
    return edge_pass_wh(layer, ds, p, G3E_W, G3E_H);
}

uint32_t g3e_edge_pass_wh(uint32_t *layer, const uint32_t *ds, const G3eParams *p, uint32_t w, uint32_t h)
{
    return edge_pass_wh(layer, ds, p, w, h);
}
