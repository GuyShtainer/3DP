/* gpu3d_clip_test.c -- slice 2.4(d), bet-2.4 review B1: the clipper never hands back more than GPU3D_MAX_PV (10) vertices.
 *
 * A convex planar quad clips to at most 10 vertices. A skew quad (a plane can cut it four times) gained two per plane and reached 11-13, which wrote past Gpu3dPoly.vtx[10], zdepth[10] and
 * wnorm[10]. melonDS has the same 10-entry buffers and no bound check (GPU3D.cpp: `Vertex temp[10]` in ClipAgainstPlane, `Vertex clippedvertices[10]` in SubmitPolygon,
 * `Polygon::Vertices[10]`): past 10 it is a stack overflow and the harness hangs, so the >10 case has no oracle evidence; the engine drops the vertices that do not fit and counts the polygon.
 *
 * This test drives the PUBLIC API (identity matrices, VIEWPORT, POLYGON_ATTR 0x001F10C0 = both faces, BEGIN_VTXS quads, four VTX_16) with:
 *   - the reviewer's n=12 quad, the n=11 / n=12 / n=13 quads of the private oracle's clip-trace generator, and an ordinary control quad;
 *   - 100 000 random quads (the review's search): every sealed polygon has 3..10 vertices, every index is in range, st_clip_over equals the polygons that were cut short (>0, < polygons).
 * Built twice by tools/host-tests/build.sh: plain, and with -fsanitize=bounds,undefined (native clang, never under Rosetta), where an out-of-bounds index is a "runtime error" line the
 * script greps for. Prints "gpu3d_clip_test: N checks, F failures". */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gpu3d.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Gpu3d G;

static void fifo1(uint32_t op, uint32_t a)
{
    gpu3d_fifo(&G, op);
    gpu3d_fifo(&G, a);
}

/* identity matrices, viewport, attribute: the state the private oracle's clip-trace generator sets */
static void setup(void)
{
    gpu3d_reset(&G);
    fifo1(0x60u, 0xBFFF0000u);
    fifo1(0x10u, 0u);
    gpu3d_fifo(&G, 0x15u);
    fifo1(0x10u, 2u);
    gpu3d_fifo(&G, 0x15u);
    fifo1(0x29u, 0x001F10C0u);
}

static void quad(const int16_t q[4][3])
{
    uint32_t i;

    fifo1(0x40u, 1u);
    for (i = 0u; i < 4u; i++) {
        gpu3d_fifo(&G, 0x23u);
        gpu3d_fifo(&G, ((uint32_t)(uint16_t)q[i][0]) | ((uint32_t)(uint16_t)q[i][1] << 16));
        gpu3d_fifo(&G, (uint32_t)(uint16_t)q[i][2]);
    }
    gpu3d_fifo(&G, 0x41u);
}

/* every polygon of the sealed list, and everything it indexes, in range; returns the largest vertex count */
static uint32_t check_list(void)
{
    uint32_t n = gpu3d_render_count(&G), i, k, big = 0u;

    for (i = 0u; i < n; i++) {
        const Gpu3dPoly *p = gpu3d_render_poly(&G, i);

        CHECK(p->nverts >= 3u && p->nverts <= GPU3D_MAX_PV);
        big = p->nverts > big ? p->nverts : big;
        for (k = 0u; k < p->nverts && k < GPU3D_MAX_PV; k++) {
            CHECK(p->vtx[k] < GPU3D_MAX_VERTS);
            CHECK(gpu3d_render_vtx(&G, p, k) != NULL);
            CHECK(p->zdepth[k] >= 0 && p->zdepth[k] <= 0xffffff);
        }
        CHECK(gpu3d_render_vtx(&G, p, p->nverts) == NULL);
    }
    return big;
}

static const int16_t Q[][4][3] = {
    { { 5235, 8031, 6416 }, { -10211, -9599, -10595 }, { 9893, -1015, 296 }, { -9883, -10655, -9996 } },   /* review B1: n=12 */
    { { -910, -4392, 9257 }, { -7858, 5191, -8003 }, { 9404, -3381, 13 }, { -6622, 9466, -7734 } },        /* n=11 */
    { { -3258, 162, 5686 }, { -891, -7781, -8004 }, { 6350, 4629, -564 }, { -6230, -7778, -9534 } },       /* n=11 */
    { { -6404, -958, 7527 }, { 201, 9200, -9061 }, { 2448, -5968, 786 }, { 8570, 7287, -7290 } },          /* n=11 */
    { { -6215, 7389, -7040 }, { -2282, -1138, 9437 }, { 549, 7456, -9175 }, { 8194, -9535, 7744 } },       /* n=12 */
    { { -8481, 6866, -8441 }, { 5074, -9659, 8710 }, { -8990, 8836, -6071 }, { 6942, 2140, 404 } },        /* n=12 */
    { { 10462, -7094, -8929 }, { -8774, -2602, 8483 }, { 4421, -4461, -5680 }, { -5835, 5735, 1862 } },    /* n=13 */
};

static void test_known_quads(void)
{
    uint32_t i;

    for (i = 0u; i < sizeof Q / sizeof Q[0]; i++) {
        setup();
        quad(Q[i]);
        fifo1(0x50u, 0u);
        gpu3d_vblank(&G);
        CHECK(gpu3d_render_count(&G) == 1u);   /* the polygon is kept, cut short, not dropped */
        CHECK(check_list() == GPU3D_MAX_PV);   /* all of them needed more than the 10 that fit */
        CHECK(G.st_clip_over == 1u);
    }
}

static void test_control(void)
{
    static const int16_t c[4][3] = { { 0, 0, 0 }, { 2000, 0, 0 }, { 2000, 2000, 0 }, { 0, 2000, 0 } };

    setup();
    quad(c);
    fifo1(0x50u, 0u);
    gpu3d_vblank(&G);
    CHECK(gpu3d_render_count(&G) == 1u);
    CHECK(check_list() == 4u);
    CHECK(G.st_clip_over == 0u);
}

/* the review's search: random skew quads in a box that sticks out of the view volume, w = 4096. Many are rejected (far plane) or culled; those that survive are bounded. */
static void test_random(void)
{
    uint32_t s = 12345u, it, sealed = 0u, big = 0u, over_seen = 0u;
    int16_t q[4][3];

    for (it = 0u; it < 100000u; it++) {
        uint32_t i, k;

        for (i = 0u; i < 4u; i++) {
            for (k = 0u; k < 3u; k++) {
                s = s * 1664525u + 1013904223u;
                q[i][k] = (int16_t)((int32_t)((s >> 8) % 21000u) - 10500);
            }
        }
        setup();
        quad(q);
        fifo1(0x50u, 0u);
        gpu3d_vblank(&G);
        sealed += gpu3d_render_count(&G);
        k = check_list();
        big = k > big ? k : big;
        over_seen += G.st_clip_over;
        CHECK(G.st_clip_over <= 1u);
    }
    CHECK(big == GPU3D_MAX_PV);
    CHECK(over_seen > 10u && over_seen < sealed);
    printf("  random quads: %u sealed polygons, largest %u vertices, %u cut short at %u\n", sealed, big, over_seen, (unsigned)GPU3D_MAX_PV);
}

int main(void)
{
    test_known_quads();
    test_control();
    test_random();
    printf("gpu3d_clip_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0 ? 1 : 0;
}
