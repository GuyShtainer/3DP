/* gpu3d_export_fixture.c -- the engine side of the PLY1 export test (see gpu3d_export_fixture.h). */
#include <string.h>

#include "gpu3d.h"
#include "gpu3d_export_fixture.h"

static void cmd(Gpu3d *g, uint32_t op, uint32_t a)
{
    gpu3d_port(g, 0x400u + 4u * op, a);
}

static void vtx16(Gpu3d *g, int x, int y, int z)
{
    gpu3d_port(g, 0x400u + 4u * 0x23u, (uint32_t)(x & 0xffff) | ((uint32_t)(y & 0xffff) << 16));
    gpu3d_port(g, 0x400u + 4u * 0x23u, (uint32_t)(z & 0xffff));
}

size_t fixture_run(uint8_t *buf, size_t cap, Fixture *fx)
{
    static Gpu3d g;
    Gpu3dExportRegs regs;
    size_t n;
    uint32_t i, k, first = 0u;

    gpu3d_reset(&g);
    memset(&regs, 0, sizeof regs);
    cmd(&g, 0x60u, 0xbfff0000u);                 /* VIEWPORT: the full 256 x 192 */
    cmd(&g, 0x10u, 0u);
    cmd(&g, 0x15u, 0u);                          /* projection = identity */
    cmd(&g, 0x10u, 1u);
    cmd(&g, 0x15u, 0u);                          /* position = identity */
    cmd(&g, 0x29u, 0x1f00c0u);                   /* opaque, both faces */
    cmd(&g, 0x2au, 0x00010000u | 0x20u);         /* a TEXIMAGE_PARAM word to carry through */
    cmd(&g, 0x2bu, 0x123u);
    cmd(&g, 0x40u, 0u);                          /* triangles */
    cmd(&g, 0x20u, 0x1fu);                       /* red */
    cmd(&g, 0x22u, 0x00200010u);
    vtx16(&g, -0x800, -0x800, 0);
    cmd(&g, 0x20u, 0x3e0u);                      /* green */
    vtx16(&g, 0x800, -0x800, 0);
    cmd(&g, 0x20u, 0x7c00u);                     /* blue */
    vtx16(&g, 0, 0x800, 0);
    cmd(&g, 0x29u, 0x1000c0u | (16u << 16));     /* alpha 16: translucent */
    cmd(&g, 0x40u, 1u);                          /* a quad that leaves the view volume on +x: the clipper adds vertices */
    cmd(&g, 0x20u, 0x7fffu);
    vtx16(&g, 0x400, -0x400, 0);
    vtx16(&g, 0xc00, -0x400, 0);
    vtx16(&g, 0x1800, 0x400, 0);
    vtx16(&g, 0x400, 0x400, 0);
    cmd(&g, 0x50u, 0u);                          /* SWAP_BUFFERS: auto sort */
    gpu3d_vblank(&g);
    regs.key = 77u;
    regs.disp3dcnt = 0x3001u;
    regs.clear_color = 0x1f001fu;
    regs.clear_depth = 0x7fffu;
    regs.alpha_ref = 5u;
    regs.edge_color[3] = 0x1234u;
    regs.toon[31] = 0x7fffu;
    regs.tex_hash_lo = 0xdeadbeefu;
    regs.tex_hash_hi = 0x01020304u;
    n = gpu3d_export_list(&g, &regs, buf, cap);
    memset(fx, 0, sizeof *fx);
    fx->npolys = gpu3d_render_count(&g);
    fx->viewport_cmd = g.viewport_cmd;
    for (i = 0u; i < fx->npolys && i < 16u; i++) {
        const Gpu3dPoly *p = gpu3d_render_poly(&g, i);

        fx->poly[i] = (FixPoly){ p->attr, p->texparam, p->texpalette, p->nverts, p->flags, p->ytop, p->ybottom, p->sortkey, first };
        for (k = 0u; k < p->nverts && first < 64u; k++, first++) {
            const Gpu3dVtx *v = gpu3d_render_vtx(&g, p, k);
            uint32_t c;

            for (c = 0u; c < 4u; c++) {
                fx->vtx[first].pos[c] = v->pos[c];
            }
            for (c = 0u; c < 3u; c++) {
                fx->vtx[first].rgb[c] = (uint8_t)((uint32_t)v->col[c] >> 12);
            }
            fx->vtx[first].s = v->tex[0];
            fx->vtx[first].t = v->tex[1];
            fx->vtx[first].sx = v->sx;
            fx->vtx[first].sy = v->sy;
            fx->vtx[first].depth = (uint32_t)p->zdepth[k];
        }
    }
    fx->nverts = first;
    return n;
}
