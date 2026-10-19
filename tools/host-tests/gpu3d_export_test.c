/* gpu3d_export_test.c -- slice 2.4c: the engine's PLY1 export read back through the 2.5 lane's own reader (gpu3d_list.h / gpu3d_list.c, read-only from the sibling tree).
 * The scene (gpu3d_export_fixture.c) is a lit-free opaque triangle and a translucent quad that the +x plane cuts. Every field of the header, every polygon and every vertex
 * record that the reader returns is checked against what the engine's accessors hold, and the hand-known values (positions, colours, counts, the draw order) are pinned. */
#include <stdio.h>
#include <string.h>

#include "gpu3d_export_fixture.h"
#include "gpu3d_list.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    static uint8_t buf[65536];
    static Fixture fx;
    Gpu3dList l;
    size_t n = fixture_run(buf, sizeof buf, &fx);
    uint32_t i, k;

    CHECK(n > 256u);
    CHECK(gpu3d_list_open(&l, buf, n) == GPU3D_ERR_OK);
    CHECK(l.len == n && n == gpu3d_list_size(l.npolys, l.nverts));
    CHECK(l.key == 77u && l.disp3dcnt == 0x3001u && l.clear_color == 0x1f001fu && l.clear_depth == 0x7fffu);
    CHECK(l.viewport == 0xbfff0000u && l.viewport == fx.viewport_cmd);
    CHECK(l.npolys == fx.npolys && l.nverts == fx.nverts && l.render_count == fx.npolys);
    CHECK(l.alpha_ref == 5u && l.edge_color[3] == 0x1234u && l.toon[31] == 0x7fffu && l.tex_hash == 0x01020304deadbeefull);
    CHECK(l.swap_bits == 0u && l.flags == 0u);
    CHECK(l.npolys == 2u);                       /* the triangle and the cut quad */
    CHECK(l.nverts > 3u + 4u);                   /* the cut adds vertices */
    for (i = 0u; i < l.npolys; i++) {
        Gpu3dPoly p;

        gpu3d_list_poly(&l, i, &p);
        CHECK(p.attr == fx.poly[i].attr && p.texparam == fx.poly[i].texparam && p.texpalette == fx.poly[i].texpal);
        CHECK(p.nverts == fx.poly[i].nverts && p.flags == fx.poly[i].flags && p.first == fx.poly[i].first);
        CHECK(p.ytop == fx.poly[i].ytop && p.ybottom == fx.poly[i].ybottom && p.sortkey == fx.poly[i].sortkey);
        CHECK(p.nverts >= 3u && p.nverts <= GPU3D_MAX_PV);
        for (k = 0u; k < p.nverts; k++) {
            Gpu3dVert v;
            const FixVtx *e = &fx.vtx[p.first + k];

            gpu3d_list_vert(&l, p.first + k, &v);
            CHECK(v.x == e->pos[0] && v.y == e->pos[1] && v.z == e->pos[2] && v.w == e->pos[3]);
            CHECK(v.r == e->rgb[0] && v.g == e->rgb[1] && v.b == e->rgb[2]);
            CHECK(v.s == e->s && v.t == e->t && v.sx == e->sx && v.sy == e->sy && v.depth24 == e->depth);
            CHECK(gpu3d_is_translucent(p.attr, p.texparam) == ((p.flags & GPU3D_PF_TRANSLUCENT) != 0u));
        }
    }
    {   /* hand-known: opaque first, then the translucent quad */
        Gpu3dPoly a, b;
        Gpu3dVert v0, v1, v2;

        gpu3d_list_poly(&l, 0, &a);
        gpu3d_list_poly(&l, 1, &b);
        CHECK(a.nverts == 3u && (a.flags & GPU3D_PF_TRANSLUCENT) == 0u && (b.flags & GPU3D_PF_TRANSLUCENT) != 0u && b.nverts > 4u);
        gpu3d_list_vert(&l, a.first + 0u, &v0);
        gpu3d_list_vert(&l, a.first + 1u, &v1);
        gpu3d_list_vert(&l, a.first + 2u, &v2);
        CHECK(v0.x == -0x800 && v0.y == -0x800 && v0.z == 0 && v0.w == 0x1000);
        CHECK(v0.r == 31u && v0.g == 0u && v0.b == 0u && v1.g == 31u && v2.b == 31u);
        CHECK(v0.s == 0x10 && v0.t == 0x20);
        CHECK(v0.sx == 64 && v1.sx == 192 && v2.sx == 128);   /* x: (x + w) * 256 / (2 w) */
        CHECK(a.attr == 0x1f00c0u && a.texparam == 0x10020u && a.texpalette == 0x123u);
        CHECK(b.attr == (0x1000c0u | (16u << 16)));
    }
    CHECK(gpu3d_list_open(&l, buf, 100u) == GPU3D_ERR_SHORT);   /* the reader refuses a short buffer */
    printf("gpu3d_export_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
