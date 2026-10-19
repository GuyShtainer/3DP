/* gpu3d_edge_test.c -- bet 2.5c: the edge rule on synthetic ID / depth buffers, the 8x8 Morton de-tile round trip, the depth reconstruction on known vertices (Z and W mode),
 * the stencil carrier and the same-ID translucent rule against a model of the stencil unit. Build: tools/host-tests/build.sh. */
#include "gpu3d_edge.h"
#include "gpu3d_list.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_ds[G3E_PIXELS], g_layer[G3E_PIXELS];

static uint32_t DS(uint32_t covered, uint32_t id, uint32_t z) { return ((covered ? 0x80u : 0u) | id) << 24 | (z & 0xFFFFFFu); }

static G3eParams params(uint32_t clear_id, uint32_t clear_z)
{
    G3eParams p;
    uint32_t i;
    memset(&p, 0, sizeof p);
    p.clear_id = clear_id;
    p.clear_z = clear_z;
    for (i = 0; i < 8; i++) p.edge_color[i] = (uint16_t)(i * 0x0421u + 1u);   /* distinct per table entry */
    return p;
}

static void fill(uint32_t id, uint32_t z)
{
    uint32_t i;
    for (i = 0; i < G3E_PIXELS; i++) g_ds[i] = DS(1, id, g3e_depth_from_ds(z));
}

/* ---- the model of the PICA stencil unit: a pixel with a depth-test outcome meets a pass ---- */
typedef struct Px { uint32_t stencil; int drawn_store, drawn_blend; } Px;

static void meet(Px *px, const G3ePass *p, int zpass)
{
    if (!g3e_pass_stencil_ok(p, px->stencil)) return;
    if (!zpass) return;                    /* stencil op on depth fail: keep */
    if (p->blend) px->drawn_blend++; else px->drawn_store++;
    px->stencil = g3e_pass_stencil_new(p, px->stencil);
}

static void run_passes(Px *px, const G3ePass *ps, uint32_t n, int zpass)
{
    uint32_t i;
    for (i = 0; i < n; i++) meet(px, &ps[i], zpass);
}

int main(void)
{
    uint32_t x, y, i;

    /* ---- carrier choice and clear values ---- */
    CHECK(g3e_carrier(1u << 5) == G3E_CARRIER_EDGE && g3e_carrier(0x39u) == G3E_CARRIER_EDGE && g3e_carrier(0x19u) == G3E_CARRIER_TRANS && g3e_carrier(0) == G3E_CARRIER_TRANS, "carrier");
    CHECK(g3e_clear_stencil(G3E_CARRIER_EDGE, 0x3F000000u) == 63u && g3e_clear_stencil(G3E_CARRIER_EDGE, 0x05000000u) == 5u && g3e_clear_stencil(G3E_CARRIER_EDGE, 0xC0000000u) == 0u, "edge clear stencil (ID, bit 7 clear)");
    CHECK(g3e_clear_stencil(G3E_CARRIER_TRANS, 0x3F000000u) == 0u, "trans clear stencil is empty");
    { G3ePass o; g3e_opaque_pass(G3E_CARRIER_EDGE, 9u << 24, &o); CHECK(o.ref == 0x89 && o.wmask == 0xFF && o.replace && !o.blend && g3e_pass_stencil_new(&o, 3) == 0x89, "opaque edge pass id 9");
      g3e_opaque_pass(G3E_CARRIER_TRANS, 9u << 24, &o); CHECK(g3e_pass_stencil_new(&o, 0xC5) == 0x80, "opaque trans pass clears the blend ID"); }

    /* ---- Morton: indices, bijection, round trip ---- */
    CHECK(g3e_tiled_index(0, 0, 192) == 0 && g3e_tiled_index(1, 0, 192) == 1 && g3e_tiled_index(0, 1, 192) == 2 && g3e_tiled_index(1, 1, 192) == 3 && g3e_tiled_index(2, 0, 192) == 4
          && g3e_tiled_index(0, 2, 192) == 8 && g3e_tiled_index(4, 0, 192) == 16 && g3e_tiled_index(0, 4, 192) == 32 && g3e_tiled_index(7, 7, 192) == 63 && g3e_tiled_index(8, 0, 192) == 64
          && g3e_tiled_index(0, 8, 192) == 24 * 64 && g3e_tiled_index(191, 255, 192) == 192 * 256 - 1, "tiled index");
    {
        static uint8_t seen[G3E_RAW_W * G3E_RAW_H];
        uint32_t dup = 0, oob = 0;
        memset(seen, 0, sizeof seen);
        for (y = 0; y < G3E_RAW_H; y++)
            for (x = 0; x < G3E_RAW_W; x++) {
                uint32_t t = g3e_tiled_index(x, y, G3E_RAW_W);
                if (t >= G3E_RAW_W * G3E_RAW_H) oob++;
                else if (seen[t]++) dup++;
            }
        CHECK(dup == 0 && oob == 0, "tiled index is a bijection (dup %u, out of range %u)", dup, oob);
    }
    {
        static uint32_t raw[G3E_RAW_W * G3E_RAW_H], back[G3E_PIXELS];
        uint32_t bad = 0;
        /* build the hardware buffer from a logical image through the documented turn (logical (x, y) is hardware pixel (191 - y, x)), then de-tile */
        for (y = 0; y < G3E_H; y++)
            for (x = 0; x < G3E_W; x++) raw[g3e_tiled_index(G3E_RAW_W - 1u - y, x, G3E_RAW_W)] = (x * 7u + y * 131u) ^ (x << 16) ^ (y << 8);
        g3e_detile_ds(raw, back);
        for (y = 0; y < G3E_H; y++)
            for (x = 0; x < G3E_W; x++)
                if (back[y * G3E_W + x] != ((x * 7u + y * 131u) ^ (x << 16) ^ (y << 8))) bad++;
        CHECK(bad == 0, "de-tile round trip (%u wrong)", bad);
    }

    /* ---- depth reconstruction ---- */
    {
        uint32_t d, worst = 0, exact_low = 0, nonmono = 0, prev = 0;
        for (d = 0; d <= 0xFFFFFFu; d += 1u) {
            uint32_t code = g3e_depth_from_ds(d), back = g3e_depth_to_ds(code), diff = back > d ? back - d : d - back;
            if (diff > worst) worst = diff;
            if (d < 0x7FFFFFu && back != d) exact_low++;
            if (d && back < prev) nonmono++;
            prev = back;
            if (code > 0xFFFFFFu) worst = 99;
        }
        CHECK(worst <= 1u, "ds -> code -> ds within one code over all 2^24 values (worst %u)", worst);
        CHECK(exact_low == 0u, "exact below 0x7FFFFF (%u wrong)", exact_low);
        CHECK(nonmono == 0u, "monotonic (%u steps back)", nonmono);
    }
    {   /* known vertices, Z-buffer mode: depth24_z of z/w -> PICA code -> reconstruction, order kept */
        static const int32_t zs[6][2] = { { -4096, 4096 }, { 0, 4096 }, { 2048, 4096 }, { 4096, 4096 }, { 100, 8192 }, { 4095, 4096 } };
        uint32_t prev = 0, bad = 0, k;
        for (k = 0; k < 6; k++) {
            uint32_t d24 = gpu3d_depth24_z(zs[k][0], zs[k][1]), rec = g3e_depth_to_ds(g3e_depth_from_ds(d24));
            if ((rec > d24 ? rec - d24 : d24 - rec) > 1u) bad++;
        }
        CHECK(bad == 0, "Z mode reconstruction on known vertices (%u wrong)", bad);
        CHECK(gpu3d_depth24_z(-4096, 4096) == 0u && gpu3d_depth24_z(0, 4096) == 0x7FFE00u && gpu3d_depth24_z(4096, 4096) == 0xFFFE00u, "known Z-mode vertex depths");
        for (k = 0; k < 4; k++) {   /* nearer vertex -> smaller reconstructed depth */
            uint32_t r = g3e_depth_to_ds(g3e_depth_from_ds(gpu3d_depth24_z(zs[k][0], zs[k][1])));
            CHECK(k == 0 || r > prev, "Z mode order kept at %u", k);
            prev = r;
        }
    }
    {   /* W-buffer mode: w with the low bits dropped, w = 4096 (1.0) .. 0xFFFFFF */
        int32_t w[3] = { 0x1000, 0x40000, 0xFFFFF0 };
        uint32_t drop = gpu3d_w_drop(w, 3), k, bad = 0;
        CHECK(drop == 8u, "w drop for a 24-bit w is 8 (got %u)", drop);
        for (k = 0; k < 3; k++) {
            uint32_t d24 = gpu3d_depth24_w(w[k], drop), rec = g3e_depth_to_ds(g3e_depth_from_ds(d24));
            if ((rec > d24 ? rec - d24 : d24 - rec) > 1u) bad++;
        }
        CHECK(bad == 0, "W mode reconstruction on known vertices (%u wrong)", bad);
        CHECK(g3e_depth_to_ds(g3e_depth_from_ds(gpu3d_depth24_w(w[0], drop))) < g3e_depth_to_ds(g3e_depth_from_ds(gpu3d_depth24_w(w[2], drop))), "W mode: larger w is farther");
    }
    CHECK(g3e_clear_depth_ds(0x7FFFu) == 0xFFFFFFu && g3e_clear_depth_ds(0) == 0x1FFu && g3e_clear_depth_ds(0x8000u) == 0x1FFu, "rear-plane depth");
    CHECK(g3e_depth_to_ds(g3e_clear_code(0x7FFFu)) == 0xFFFFFFu && g3e_depth_to_ds(g3e_clear_code(0x1234u)) == g3e_clear_depth_ds(0x1234u), "clear code reads back as the rear-plane depth");

    /* ---- edge colour ---- */
    CHECK(g3e_edge_rgb(0) == 0u && g3e_edge_rgb(0x7FFFu) == (63u | (63u << 8) | (63u << 16)) && g3e_edge_rgb(1) == 3u && g3e_edge_rgb(0x20u) == (3u << 8) && g3e_edge_rgb(0x400u) == (3u << 16), "edge colour 5 -> 6 bits");

    /* ---- edge rule ---- */
    {
        G3eParams p = params(63, 0xFFFFFFu);
        uint32_t marked;
        /* a vertical boundary at x = 100: ID 5 (z 1000) on the left, ID 9 (z 2000) on the right: the nearer side (left, x = 99) is marked, the farther is not */
        fill(5, 1000);
        for (y = 0; y < G3E_H; y++) for (x = 100; x < G3E_W; x++) g_ds[y * G3E_W + x] = DS(1, 9, g3e_depth_from_ds(2000));
        memset(g_layer, 0, sizeof g_layer);
        for (i = 0; i < G3E_PIXELS; i++) g_layer[i] = (31u << 24) | 0x123456u;
        marked = g3e_edge_pass(g_layer, g_ds, &p);
        CHECK(g3e_edge_pixel(g_ds, &p, 99, 50) == 1 && g3e_edge_pixel(g_ds, &p, 100, 50) == 0 && g3e_edge_pixel(g_ds, &p, 98, 50) == 0 && g3e_edge_pixel(g_ds, &p, 101, 50) == 0, "boundary: only the nearer side");
        /* plus the screen ring: both IDs are not 63 and nearer than the rear plane, so the outermost ring is marked */
        CHECK(marked == 192u + (2u * 256u + 2u * 190u) - 2u, "marked count %u (boundary column 192 + ring 892, minus the 2 pixels in both)", marked);
        {   /* the edge pixel is drawn at coverage 16 over the colour behind it: here the farther neighbour (0x123456), weights 17 and 15 of 32; alpha kept */
            uint32_t e = g3e_edge_rgb(p.edge_color[0]), bl = 0x123456u, m = 0;
            int c;
            for (c = 0; c < 3; c++) m |= ((((e >> (8 * c)) & 63u) * 17u + ((bl >> (8 * c)) & 63u) * 15u) >> 5) << (8 * c);
            CHECK((g_layer[50 * G3E_W + 99] & 0x00FFFFFFu) == m && (g_layer[50 * G3E_W + 99] >> 24) == 31u, "ID 5: EDGE_COLOR[0] at coverage 16 over the farther pixel, alpha kept");
        }
        CHECK((g_layer[0 * G3E_W + 200] >> 24) == 16u, "ring pixel with nothing behind: the edge colour at alpha 16");
        CHECK((g_layer[0 * G3E_W + 200] & 0x00FFFFFFu) == g3e_edge_rgb(p.edge_color[1]), "ring pixel of ID 9 uses EDGE_COLOR[1]");
        CHECK(g_layer[50 * G3E_W + 100] == ((31u << 24) | 0x123456u) && g_layer[100 * G3E_W + 128] == ((31u << 24) | 0x123456u), "farther side and interior untouched");
        /* equal IDs: no mark; equal depth: no mark; farther than the neighbour: no mark */
        fill(5, 1000);
        for (x = 100; x < G3E_W; x++) g_ds[50 * G3E_W + x] = DS(1, 5, g3e_depth_from_ds(1000));
        CHECK(g3e_edge_pixel(g_ds, &p, 99, 50) == 0 && g3e_edge_pixel(g_ds, &p, 100, 50) == 0, "same ID: no edge");
        g_ds[50 * G3E_W + 100] = DS(1, 7, g3e_depth_from_ds(1000));
        CHECK(g3e_edge_pixel(g_ds, &p, 99, 50) == 0 && g3e_edge_pixel(g_ds, &p, 100, 50) == 0 && g3e_edge_pixel(g_ds, &p, 101, 50) == 0, "different ID at equal depth: no edge on either side (strict less-than)");
        g_ds[50 * G3E_W + 100] = DS(1, 7, g3e_depth_from_ds(999));
        CHECK(g3e_edge_pixel(g_ds, &p, 99, 50) == 0 && g3e_edge_pixel(g_ds, &p, 100, 50) == 1 && g3e_edge_pixel(g_ds, &p, 101, 50) == 0, "the nearer pixel is marked, its farther neighbours are not");
        /* 4-neighbour only: a diagonal neighbour of another ID does not mark */
        fill(5, 1000);
        g_ds[51 * G3E_W + 101] = DS(1, 7, g3e_depth_from_ds(2000));
        CHECK(g3e_edge_pixel(g_ds, &p, 100, 50) == 0 && g3e_edge_pixel(g_ds, &p, 101, 50) == 1 && g3e_edge_pixel(g_ds, &p, 100, 51) == 1 && g3e_edge_pixel(g_ds, &p, 102, 51) == 1 && g3e_edge_pixel(g_ds, &p, 101, 52) == 1,
              "neighbours are the four of the cross");
        /* an empty (clear-plane) pixel is never marked, and it counts as the rear plane for its neighbours: ID 63 / clear depth */
        fill(5, 1000);
        g_ds[60 * G3E_W + 60] = DS(0, 63, g3e_clear_code(0x7FFFu));
        CHECK(g3e_edge_pixel(g_ds, &p, 60, 60) == 0 && g3e_edge_pixel(g_ds, &p, 61, 60) == 1 && g3e_edge_pixel(g_ds, &p, 60, 59) == 1 && g3e_edge_pixel(g_ds, &p, 62, 60) == 0, "the rear plane is a neighbour");
        /* the screen ring: the outermost pixel against the off-screen rear plane. ID 63 on the ring is the rear plane's own ID: no mark. A pixel at the clear depth: no mark. */
        fill(5, 1000);
        CHECK(g3e_edge_pixel(g_ds, &p, 0, 0) == 1 && g3e_edge_pixel(g_ds, &p, 255, 191) == 1 && g3e_edge_pixel(g_ds, &p, 0, 100) == 1 && g3e_edge_pixel(g_ds, &p, 128, 191) == 1 && g3e_edge_pixel(g_ds, &p, 1, 1) == 0, "ring marked");
        fill(63, 1000);
        CHECK(g3e_edge_pixel(g_ds, &p, 0, 0) == 0 && g3e_edge_pixel(g_ds, &p, 128, 0) == 0, "ring pixel of the rear-plane ID is not marked");
        fill(5, 0xFFFFFF);
        CHECK(g3e_edge_pixel(g_ds, &p, 0, 0) == 0, "a pixel at the rear-plane depth is not nearer than the off-screen plane");
        p = params(5, 0xFFFFFFu);
        fill(5, 1000);
        CHECK(g3e_edge_pixel(g_ds, &p, 0, 0) == 0, "ring of the polygon's own ID = the rear plane's ID");
        /* the pass equals the reference on a deterministic pseudo-random buffer */
        {
            uint32_t s = 12345u, bad = 0, n_pass, n_ref = 0;
            p = params(17, 0xFFFFFFu);
            for (i = 0; i < G3E_PIXELS; i++) {
                s = s * 1664525u + 1013904223u;
                g_ds[i] = DS((s >> 28) != 0, (s >> 8) & 3u, g3e_depth_from_ds((s >> 12) & 0xFFFu));
                g_layer[i] = 0x1F000000u;
            }
            n_pass = g3e_edge_pass(g_layer, g_ds, &p);
            for (y = 0; y < G3E_H; y++)
                for (x = 0; x < G3E_W; x++) {
                    int m = g3e_edge_pixel(g_ds, &p, x, y);
                    n_ref += (uint32_t)m;
                    if ((g_layer[y * G3E_W + x] != 0x1F000000u) != m) bad++;
                }
            CHECK(bad == 0 && n_pass == n_ref && n_pass > 1000, "pass == reference on noise (%u wrong, %u marked)", bad, n_pass);
        }
    }

    /* ---- the stencil carrier and the same-ID rule: the pass lists against a model of the stencil unit ---- */
    {
        uint32_t car, id, other, seen, st, zp, bad = 0, cases = 0;
        for (car = 0; car < 2; car++)
            for (seen = 0; seen < 2; seen++)
                for (id = 0; id < 64; id += 21)      /* ID 0, 21, 42, 63 */
                    for (other = 0; other < 64; other += 9) {
                        uint32_t attr = id << 24, s_j = 0xC0u | id;
                        G3ePass ps[3];
                        uint32_t n = g3e_trans_passes(car, attr, seen, ps);
                        /* pixel states of the layout: empty / opaque covered / latest blend from `other` / latest blend from this ID */
                        uint32_t states[4], ns = 0;
                        if (car == G3E_CARRIER_TRANS) { states[0] = 0x00; states[1] = 0x80; states[2] = 0xC0u | other; states[3] = s_j; ns = 4; }
                        else { states[0] = 5u; states[1] = 0x80u | 5u; states[2] = 0x80u | other; states[3] = 63u; ns = 4; }
                        for (st = 0; st < ns; st++)
                            for (zp = 0; zp < 2; zp++) {
                                Px px;
                                uint32_t init = states[st], covered, same_id;
                                int want_blend = 0, want_store = 0;
                                px.stencil = init; px.drawn_store = px.drawn_blend = 0;
                                run_passes(&px, ps, n, (int)zp);
                                covered = car == G3E_CARRIER_TRANS ? (init & 0x80u) != 0 : (init & 0x80u) != 0;
                                same_id = car == G3E_CARRIER_TRANS && init == s_j;
                                if (zp) {
                                    if (!covered) want_store = 1;
                                    else if (!(seen && same_id)) want_blend = 1;
                                }
                                cases++;
                                if (px.drawn_blend != want_blend || px.drawn_store != want_store) { bad++; printf("  state %02x car %u seen %u zp %u: blend %d store %d (want %d %d)\n", init, car, seen, zp, px.drawn_blend, px.drawn_store, want_blend, want_store); }
                                if (!zp && px.stencil != init) bad++;
                                if (zp && (want_blend || want_store)) {
                                    uint32_t want_st = car == G3E_CARRIER_TRANS ? s_j : (init | 0x80u);
                                    if (px.stencil != want_st) { bad++; printf("  state %02x car %u: stencil %02x want %02x\n", init, car, px.stencil, want_st); }
                                }
                                if (zp && !(want_blend || want_store) && px.stencil != init) bad++;
                            }
                    }
        CHECK(bad == 0, "stencil model: %u wrong of %u cases (every pixel drawn at most once, same-ID pixels skipped only in the trans layout)", bad, cases);
        CHECK(g3e_trans_passes(G3E_CARRIER_TRANS, 0, 1, (G3ePass[3]){{0}}) == 3 && g3e_trans_passes(G3E_CARRIER_TRANS, 0, 0, (G3ePass[3]){{0}}) == 2 && g3e_trans_passes(G3E_CARRIER_EDGE, 0, 1, (G3ePass[3]){{0}}) == 2, "pass counts");
        /* opaque over a blend state: the opaque pass wipes the translucent ID (DS: an opaque store clears the translucent attribute bits) */
        { G3ePass o, ps[3]; uint32_t n, st2; Px px;
          g3e_opaque_pass(G3E_CARRIER_TRANS, 7u << 24, &o);
          st2 = g3e_pass_stencil_new(&o, 0xC0u | 7u);
          CHECK(st2 == 0x80u, "opaque pass over a blend state");
          n = g3e_trans_passes(G3E_CARRIER_TRANS, 7u << 24, 1, ps);
          px.stencil = st2; px.drawn_blend = px.drawn_store = 0;
          run_passes(&px, ps, n, 1);
          CHECK(px.drawn_blend == 1 && px.stencil == (0xC0u | 7u), "a translucent polygon of the same ID blends over an opaque pixel that replaced its earlier blend"); }
        /* a second translucent polygon of the SAME ID over the first one's pixel is skipped; a different ID blends */
        { G3ePass a[3], b[3]; uint32_t na, nb; Px px;
          na = g3e_trans_passes(G3E_CARRIER_TRANS, 12u << 24, 0, a);
          px.stencil = 0x80; px.drawn_blend = px.drawn_store = 0;
          run_passes(&px, a, na, 1);
          CHECK(px.drawn_blend == 1 && px.stencil == (0xC0u | 12u), "first translucent polygon of ID 12");
          nb = g3e_trans_passes(G3E_CARRIER_TRANS, 12u << 24, 1, b);
          px.drawn_blend = px.drawn_store = 0;
          run_passes(&px, b, nb, 1);
          CHECK(px.drawn_blend == 0 && px.drawn_store == 0 && px.stencil == (0xC0u | 12u), "second translucent polygon of ID 12 skips the pixel");
          nb = g3e_trans_passes(G3E_CARRIER_TRANS, 13u << 24, 1, b);
          run_passes(&px, b, nb, 1);
          CHECK(px.drawn_blend == 1 && px.stencil == (0xC0u | 13u), "a polygon of another ID blends and takes over the ID"); }
    }

    /* ---- bet 2.5(d), review B3: the stencil rewrite of a split frame (EDGE layout -> TRANS layout) and the depth-only merge de-tile ---- */
    {
        G3ePass rw, ps[3];
        uint32_t id, n, bad = 0, cases = 0;
        g3e_rewrite_pass(&rw);
        CHECK(rw.rel == G3E_REL_ALWAYS && rw.wmask == 0x7Fu && rw.ref == 0u && rw.replace && !rw.blend, "rewrite pass shape");
        for (id = 0; id < 64u; id++) {
            uint32_t covered = 0x80u | id, empty = id;      /* an opaque pixel of the EDGE layout, and a pixel still holding the rear-plane ID */
            Px px;
            cases += 2u;
            px.stencil = covered;
            if (!g3e_pass_stencil_ok(&rw, px.stencil) || g3e_pass_stencil_new(&rw, px.stencil) != G3E_TRANS_OPAQUE) bad++;
            px.stencil = empty;
            if (!g3e_pass_stencil_ok(&rw, px.stencil) || g3e_pass_stencil_new(&rw, px.stencil) != G3E_TRANS_EMPTY) bad++;
            /* after the rewrite the TRANS passes behave as on a TRANS-cleared frame: a translucent polygon over a rewritten pixel */
            px.stencil = g3e_pass_stencil_new(&rw, covered);
            px.drawn_blend = px.drawn_store = 0;
            n = g3e_trans_passes(G3E_CARRIER_TRANS, (id ^ 5u) << 24, 0, ps);
            run_passes(&px, ps, n, 1);
            cases++;
            if (px.drawn_blend != 1 || px.drawn_store != 0 || px.stencil != (0xC0u | ((id ^ 5u) & 63u))) bad++;
            px.stencil = g3e_pass_stencil_new(&rw, empty);
            px.drawn_blend = px.drawn_store = 0;
            run_passes(&px, ps, n, 1);
            cases++;
            if (px.drawn_blend != 0 || px.drawn_store != 1 || px.stencil != (0xC0u | ((id ^ 5u) & 63u))) bad++;
        }
        CHECK(bad == 0, "rewrite pass: %u wrong of %u cases (covered -> 0x80, empty -> 0x00, then the TRANS passes blend over the first and store over the second)", bad, cases);
    }
    {
        static uint32_t raw[G3E_RAW_W * G3E_RAW_H], io[G3E_PIXELS], io0[G3E_PIXELS];
        uint32_t bad = 0;
        for (y = 0; y < G3E_H; y++)
            for (x = 0; x < G3E_W; x++) {
                raw[g3e_tiled_index(G3E_RAW_W - 1u - y, x, G3E_RAW_W)] = 0xEE000000u | ((x * 3u + y * 17u) & 0xFFFFFFu);      /* a second copy: its stencil byte (0xEE) must NOT arrive */
                io0[y * G3E_W + x] = io[y * G3E_W + x] = ((0x80u | (x & 63u)) << 24) | 0x123456u;
            }
        g3e_detile_depth_merge(raw, io);
        for (y = 0; y < G3E_H; y++)
            for (x = 0; x < G3E_W; x++) {
                uint32_t w = io[y * G3E_W + x];
                if ((w >> 24) != (io0[y * G3E_W + x] >> 24) || (w & 0xFFFFFFu) != ((x * 3u + y * 17u) & 0xFFFFFFu)) bad++;
            }
        CHECK(bad == 0, "depth merge de-tile: %u wrong (stencil byte kept from the first copy, depth from the second)", bad);
    }

    printf("gpu3d_edge_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
