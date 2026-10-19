/* gpu3d_export.c -- the sealed render list as PLY1 bytes (bet 2.4 slice (c)); the format is the private design notes of bet 2.5. Pure C. No include of the 2.5
 * lane's gpu3d_list.h: that header declares Gpu3dPoly / GPU3D_MAX_* too, so the two cannot meet in one translation unit; the host test reads this file's bytes through it. */
#include <string.h>

#include "gpu3d.h"

#define HDR_BYTES  256u
#define POLY_BYTES 32u
#define VERT_BYTES 32u

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xffffu);
    put16(p + 2, v >> 16);
}

size_t gpu3d_export_size(const Gpu3d *g)
{
    uint32_t i, nv = 0u, n = gpu3d_render_count(g);

    for (i = 0u; i < n; i++) {
        nv += gpu3d_render_poly(g, i)->nverts;
    }
    return HDR_BYTES + (size_t)n * POLY_BYTES + (size_t)nv * VERT_BYTES;
}

size_t gpu3d_export_list(const Gpu3d *g, const Gpu3dExportRegs *regs, uint8_t *buf, size_t cap)
{
    static const Gpu3dExportRegs zero;
    uint32_t n = gpu3d_render_count(g), i, k, nv = 0u, degenerate = 0u, wbuf = 0u;
    size_t need = gpu3d_export_size(g);
    uint8_t *ph, *pp, *pv;

    if (regs == NULL) {
        regs = &zero;
    }
    if (buf == NULL || cap < need) {
        return 0u;
    }
    memset(buf, 0, need);
    for (i = 0u; i < n; i++) {
        const Gpu3dPoly *p = gpu3d_render_poly(g, i);

        nv += p->nverts;
        degenerate += (p->flags & GPU3D_PF_DEGENERATE) != 0u ? 1u : 0u;
        wbuf |= (p->flags & GPU3D_PF_WBUFFER) != 0u ? 2u : 0u;
    }
    if (n == 0u) {
        wbuf = g->flush_attr & 2u;
    }
    ph = buf;
    pp = buf + HDR_BYTES;
    pv = pp + (size_t)n * POLY_BYTES;
    memcpy(ph, "PLY1", 4);
    put32(ph + 0x04, 1u);
    put32(ph + 0x08, regs->key);
    put32(ph + 0x0c, HDR_BYTES);
    put32(ph + 0x10, regs->disp3dcnt);
    put32(ph + 0x14, regs->clear_color);
    put32(ph + 0x18, regs->clear_depth);
    put32(ph + 0x1c, wbuf);
    put32(ph + 0x20, g->viewport_cmd);
    put32(ph + 0x24, n);
    put32(ph + 0x28, nv);
    put32(ph + 0x2c, regs->tex_hash_lo);
    put32(ph + 0x30, regs->tex_hash_hi);
    put32(ph + 0x34, regs->alpha_ref);
    for (k = 0u; k < 8u; k++) {
        put16(ph + 0x38 + 2u * k, regs->edge_color[k]);
    }
    put32(ph + 0x48, regs->fog_color);
    put32(ph + 0x4c, regs->fog_offset);
    for (k = 0u; k < 32u; k++) {
        put16(ph + 0x50 + 2u * k, regs->toon[k]);
    }
    put32(ph + 0x90, n);
    put32(ph + 0x94, degenerate);
    if (g->wide_on != 0u) {   /* bet 2.8: the wide word and the stereo parameters (zero, as before, on a narrow engine) */
        int32_t dist, m00;

        gpu3d_render_stereo(g, &dist, &m00);
        put32(ph + 0x9c, 1u | (g->stereo_on != 0u ? 2u : 0u));
        put32(ph + 0xa0, (uint32_t)dist);
        put32(ph + 0xa4, (uint32_t)m00);
        put32(ph + 0xa8, (uint32_t)g->xguard);
    }
    nv = 0u;
    for (i = 0u; i < n; i++) {
        const Gpu3dPoly *p = gpu3d_render_poly(g, i);
        uint8_t *r = pp + (size_t)i * POLY_BYTES;

        put32(r + 0, p->attr);
        put32(r + 4, p->texparam);
        put16(r + 8, p->texpalette);
        r[10] = p->nverts;
        r[11] = p->flags;
        put32(r + 12, nv);
        put16(r + 16, (uint32_t)(uint16_t)p->ytop);
        put16(r + 18, (uint32_t)(uint16_t)p->ybottom);
        put32(r + 20, p->sortkey);
        r[24] = p->wtag;
        for (k = 0u; k < p->nverts; k++) {
            const Gpu3dVtx *v = gpu3d_render_vtx(g, p, k);
            uint8_t *o = pv + (size_t)(nv + k) * VERT_BYTES;
            uint32_t c;

            put32(o + 0, (uint32_t)v->pos[0]);
            put32(o + 4, (uint32_t)v->pos[1]);
            put32(o + 8, (uint32_t)v->pos[2]);
            put32(o + 12, (uint32_t)v->pos[3]);
            for (c = 0u; c < 3u; c++) {
                o[16 + c] = (uint8_t)((uint32_t)v->col[c] >> 12);
            }
            put16(o + 20, (uint32_t)(uint16_t)v->tex[0]);
            put16(o + 22, (uint32_t)(uint16_t)v->tex[1]);
            put16(o + 24, (uint32_t)(uint16_t)v->sx);
            put16(o + 26, (uint32_t)(uint16_t)v->sy);
            put32(o + 28, (uint32_t)p->zdepth[k]);
        }
        nv += p->nverts;
    }
    return need;
}
