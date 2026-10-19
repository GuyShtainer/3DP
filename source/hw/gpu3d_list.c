/* gpu3d_list.c -- reader / writer of the DS polygon list (see gpu3d_list.h and the private design notes). Pure C. */
#include "gpu3d_list.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

size_t gpu3d_list_size(uint32_t npolys, uint32_t nverts)
{
    return GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * npolys + (size_t)GPU3D_VERT_BYTES * nverts;
}

int gpu3d_list_open(Gpu3dList *l, const uint8_t *buf, size_t len)
{
    uint32_t i;
    if (len < GPU3D_HDR_BYTES) return GPU3D_ERR_SHORT;
    if (memcmp(buf, "PLY1", 4) != 0) return GPU3D_ERR_MAGIC;
    if (rd32(buf + 4) != 1u || rd32(buf + 12) != GPU3D_HDR_BYTES) return GPU3D_ERR_VERSION;
    memset(l, 0, sizeof *l);
    l->buf = buf;
    l->len = len;
    l->key = rd32(buf + 8);
    l->disp3dcnt = rd32(buf + 0x10);
    l->clear_color = rd32(buf + 0x14);
    l->clear_depth = rd32(buf + 0x18);
    l->swap_bits = rd32(buf + 0x1C);
    l->viewport = rd32(buf + 0x20);
    l->npolys = rd32(buf + 0x24);
    l->nverts = rd32(buf + 0x28);
    l->tex_hash = (uint64_t)rd32(buf + 0x2C) | ((uint64_t)rd32(buf + 0x30) << 32);
    l->alpha_ref = rd32(buf + 0x34);
    for (i = 0; i < 8; i++) l->edge_color[i] = rd16(buf + 0x38 + 2 * i);
    l->fog_color = rd32(buf + 0x48);
    l->fog_offset = rd32(buf + 0x4C);
    for (i = 0; i < 32; i++) l->toon[i] = rd16(buf + 0x50 + 2 * i);
    l->render_count = rd32(buf + 0x90);
    l->degenerate_count = rd32(buf + 0x94);
    l->flags = rd32(buf + 0x98);
    l->wide = rd32(buf + 0x9C);
    l->st_dist = (int32_t)rd32(buf + 0xA0);
    l->st_m00 = (int32_t)rd32(buf + 0xA4);
    l->st_guard = (int32_t)rd32(buf + 0xA8);
    if ((l->wide & GPU3D_LW_WIDE) != 0u) {
        if (l->npolys > GPU3D_LIST_WIDE_POLYS || l->nverts > GPU3D_LIST_WIDE_VERTS) return GPU3D_ERR_COUNTS;
    } else if (l->npolys > GPU3D_MAX_POLYS || l->nverts > GPU3D_MAX_VERTS) {
        return GPU3D_ERR_COUNTS;
    }
    if (len < gpu3d_list_size(l->npolys, l->nverts)) return GPU3D_ERR_SHORT;
    for (i = 0; i < l->npolys; i++) {
        const uint8_t *p = buf + GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * i;
        uint32_t nv = p[10], first = rd32(p + 12);
        if (nv < 3u || nv > GPU3D_MAX_PV || first > l->nverts || nv > l->nverts - first) return GPU3D_ERR_RANGE;
    }
    return GPU3D_ERR_OK;
}

const char *gpu3d_list_strerror(int err)
{
    switch (err) {
    case GPU3D_ERR_OK: return "ok";
    case GPU3D_ERR_SHORT: return "polygon list: buffer too short";
    case GPU3D_ERR_MAGIC: return "polygon list: bad magic";
    case GPU3D_ERR_VERSION: return "polygon list: unsupported version";
    case GPU3D_ERR_COUNTS: return "polygon list: counts exceed the DS polygon / vertex RAM";
    case GPU3D_ERR_RANGE: return "polygon list: polygon vertex range out of bounds";
    default: return "polygon list: unknown error";
    }
}

void gpu3d_list_poly(const Gpu3dList *l, uint32_t i, Gpu3dPoly *out)
{
    const uint8_t *p;
    memset(out, 0, sizeof *out);
    if (i >= l->npolys) return;
    p = l->buf + GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * i;
    out->attr = rd32(p);
    out->texparam = rd32(p + 4);
    out->texpalette = rd16(p + 8);
    out->nverts = p[10];
    out->flags = p[11];
    out->first = rd32(p + 12);
    out->ytop = (int16_t)rd16(p + 16);
    out->ybottom = (int16_t)rd16(p + 18);
    out->sortkey = rd32(p + 20);
    out->wtag = p[24];
}

void gpu3d_list_vert(const Gpu3dList *l, uint32_t i, Gpu3dVert *out)
{
    const uint8_t *p;
    memset(out, 0, sizeof *out);
    if (i >= l->nverts) return;
    p = l->buf + GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * l->npolys + (size_t)GPU3D_VERT_BYTES * i;
    out->x = (int32_t)rd32(p);
    out->y = (int32_t)rd32(p + 4);
    out->z = (int32_t)rd32(p + 8);
    out->w = (int32_t)rd32(p + 12);
    out->r = p[16];
    out->g = p[17];
    out->b = p[18];
    out->s = (int16_t)rd16(p + 20);
    out->t = (int16_t)rd16(p + 22);
    out->sx = (int16_t)rd16(p + 24);
    out->sy = (int16_t)rd16(p + 26);
    out->depth24 = rd32(p + 28);
}

void gpu3d_list_put_header(uint8_t *buf, const Gpu3dList *l)
{
    uint32_t i;
    memset(buf, 0, GPU3D_HDR_BYTES);
    memcpy(buf, "PLY1", 4);
    wr32(buf + 4, 1u);
    wr32(buf + 8, l->key);
    wr32(buf + 12, GPU3D_HDR_BYTES);
    wr32(buf + 0x10, l->disp3dcnt);
    wr32(buf + 0x14, l->clear_color);
    wr32(buf + 0x18, l->clear_depth);
    wr32(buf + 0x1C, l->swap_bits);
    wr32(buf + 0x20, l->viewport);
    wr32(buf + 0x24, l->npolys);
    wr32(buf + 0x28, l->nverts);
    wr32(buf + 0x2C, (uint32_t)(l->tex_hash & 0xFFFFFFFFu));
    wr32(buf + 0x30, (uint32_t)(l->tex_hash >> 32));
    wr32(buf + 0x34, l->alpha_ref);
    for (i = 0; i < 8; i++) wr16(buf + 0x38 + 2 * i, l->edge_color[i]);
    wr32(buf + 0x48, l->fog_color);
    wr32(buf + 0x4C, l->fog_offset);
    for (i = 0; i < 32; i++) wr16(buf + 0x50 + 2 * i, l->toon[i]);
    wr32(buf + 0x90, l->render_count);
    wr32(buf + 0x94, l->degenerate_count);
    wr32(buf + 0x98, l->flags);
    wr32(buf + 0x9C, l->wide);
    wr32(buf + 0xA0, (uint32_t)l->st_dist);
    wr32(buf + 0xA4, (uint32_t)l->st_m00);
    wr32(buf + 0xA8, (uint32_t)l->st_guard);
}

void gpu3d_list_put_poly(uint8_t *buf, uint32_t i, const Gpu3dPoly *q, uint32_t npolys)
{
    uint8_t *p = buf + GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * i;
    (void)npolys;
    memset(p, 0, GPU3D_POLY_BYTES);
    wr32(p, q->attr);
    wr32(p + 4, q->texparam);
    wr16(p + 8, q->texpalette);
    p[10] = q->nverts;
    p[11] = q->flags;
    wr32(p + 12, q->first);
    wr16(p + 16, (uint32_t)(uint16_t)q->ytop);
    wr16(p + 18, (uint32_t)(uint16_t)q->ybottom);
    wr32(p + 20, q->sortkey);
    p[24] = q->wtag;
}

void gpu3d_list_put_vert(uint8_t *buf, uint32_t i, const Gpu3dVert *v, uint32_t npolys)
{
    uint8_t *p = buf + GPU3D_HDR_BYTES + (size_t)GPU3D_POLY_BYTES * npolys + (size_t)GPU3D_VERT_BYTES * i;
    memset(p, 0, GPU3D_VERT_BYTES);
    wr32(p, (uint32_t)v->x);
    wr32(p + 4, (uint32_t)v->y);
    wr32(p + 8, (uint32_t)v->z);
    wr32(p + 12, (uint32_t)v->w);
    p[16] = v->r;
    p[17] = v->g;
    p[18] = v->b;
    wr16(p + 20, (uint32_t)(uint16_t)v->s);
    wr16(p + 22, (uint32_t)(uint16_t)v->t);
    wr16(p + 24, (uint32_t)(uint16_t)v->sx);
    wr16(p + 26, (uint32_t)(uint16_t)v->sy);
    wr32(p + 28, v->depth24);
}

int gpu3d_is_translucent(uint32_t attr, uint32_t texparam)
{
    uint32_t alpha = (attr >> 16) & 31u, fmt = (texparam >> 26) & 7u;
    return (alpha >= 1u && alpha <= 30u) || fmt == 1u || fmt == 6u;
}

void gpu3d_viewport_xy(uint32_t viewport, int32_t x, int32_t y, int32_t w, int32_t *sx, int32_t *sy)
{
    int32_t x1 = (int32_t)(viewport & 0xFFu), y1 = (int32_t)((viewport >> 8) & 0xFFu), x2 = (int32_t)((viewport >> 16) & 0xFFu), y2 = (int32_t)((viewport >> 24) & 0xFFu);
    int32_t width = x2 - x1 + 1, height = y2 - y1 + 1, top = 191 - y2;
    int64_t px, py, den;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (top < 0) top = 0;
    if (w <= 0) {
        *sx = x1;
        *sy = top;
        return;
    }
    px = (int64_t)x + w;
    py = (int64_t)w - y;
    den = (int64_t)w * 2;
    *sx = (int32_t)((px * width) / den) + x1;
    *sy = (int32_t)((py * height) / den) + top;
    if (*sx < 0) *sx = 0;
    if (*sx > 256) *sx = 256;
    if (*sy < 0) *sy = 0;
    if (*sy > 192) *sy = 192;
}

void gpu3d_viewport_wide_xy(int32_t x, int32_t y, int32_t w, int32_t *sx, int32_t *sy)
{
    int64_t ww = (int64_t)((uint32_t)w & 0x00FFFFFFu);
    if (w <= 0 || ww == 0) {
        *sx = 0;
        *sy = 0;
        return;
    }
    {
        int64_t nx = ((int64_t)x + ww) * 256, ny = (ww - (int64_t)y) * 192, d = 2 * ww;
        int64_t qx = nx / d, qy = ny / d;

        qx -= (nx % d != 0 && nx < 0) ? 1 : 0;   /* floor: left of / above the DS frame the numerator is negative */
        qy -= (ny % d != 0 && ny < 0) ? 1 : 0;
        *sx = (int32_t)qx + 72;
        *sy = (int32_t)qy + 24;
    }
}

uint32_t gpu3d_depth24_z(int32_t z, int32_t w)
{
    int64_t v;
    if (w <= 0) return 0x7FFE00u;
    v = ((int64_t)z * 0x4000) / w + 0x3FFF;
    if (v < 0) v = 0;
    if (v > 0x7FFF) v = 0x7FFF;
    return (uint32_t)v * 0x200u;
}

uint32_t gpu3d_depth24_w(int32_t w, uint32_t drop)
{
    uint32_t v;
    if (w < 0) return 0;
    v = (uint32_t)w & 0xFFFFFFu;
    return (v >> drop) << drop;
}

uint32_t gpu3d_w_drop(const int32_t *w, uint32_t n)
{
    uint32_t i, bits = 0;
    for (i = 0; i < n; i++) {
        uint32_t v = (uint32_t)w[i] & 0xFFFFFFu, b = 0;
        while ((v >> b) != 0u && b < 32u) b += 4u;
        if (b > bits) bits = b;
    }
    return bits > 16u ? bits - 16u : 0u;
}
