/* gpu3d_rec.c -- see gpu3d_rec.h. Pure C. */
#include "gpu3d_rec.h"

#include <string.h>

#include "gpu3d_list.h"
#include "gpu3d_tex.h"

#define TEXSLOT_BYTES 0x20000u   /* texture slots 0-3 (VRAM banks A-D) */
#define PALSLOT_BYTES 0x4000u    /* texture palette slots 0-5 */

void gpu3d_rec_mirror_init(G3rMirror *m)
{
    memset(m, 0, sizeof *m);
}

void gpu3d_rec_reset(Gpu3dRec *r)
{
    r->list_len = 0u;
    r->key = 0u;
    r->npolys = 0u;
    r->ntex = 0u;
    r->texel_used = 0u;
    memset(&r->st, 0, sizeof r->st);
}

/* copy [off, off + len) of the flat texture (pal 0) or palette (pal 1) memory out of the slot views into the scratch image at the same offsets */
static void gather(const G3rSrc *s, int pal, uint32_t off, uint32_t len)
{
    uint8_t *dst = s->scratch + (pal ? GPU3D_TEXMEM_BYTES : 0u);
    uint32_t slot_bytes = pal ? PALSLOT_BYTES : TEXSLOT_BYTES, nslots = pal ? 6u : 4u;

    while (len != 0u) {
        uint32_t k = off / slot_bytes, so = off % slot_bytes, n = slot_bytes - so;
        const uint8_t *v = NULL;

        if (n > len) {
            n = len;
        }
        if (k < nslots) {
            v = pal ? s->tex_pltt(k) : s->tex_slot(k);
        }
        if (v != NULL) {
            memcpy(dst + off, v + so, n);
        } else {
            memset(dst + off, 0, n);
        }
        off += n;
        len -= n;
    }
}

static uint64_t tick(const G3rSrc *s)
{
    return s->clock != NULL ? s->clock() : 0u;
}

int gpu3d_rec_textures(Gpu3dRec *r, const G3rMirror *committed, const G3rSrc *src)
{
    Gpu3dList l;
    uint32_t i, last = G3R_NO_TEX;
    int err;
    const uint8_t *tm, *pm;

    r->mirror = *committed;
    r->mirror.frame++;
    r->ntex = 0u;
    r->texel_used = 0u;
    r->npolys = 0u;
    memset(&r->st, 0, sizeof r->st);
    err = r->list_len != 0u ? gpu3d_list_open(&l, r->list, r->list_len) : GPU3D_ERR_SHORT;
    if (err != GPU3D_ERR_OK) {
        return err;
    }
    r->npolys = l.npolys;
    for (i = 0u; i < l.npolys; i++) {
        r->poly_tex[i] = G3R_NO_TEX;
    }
    if ((l.disp3dcnt & GPU3D_DISP_TEXTURE) == 0u) {
        return 0;
    }
    if (src->flat_tex != NULL && src->flat_pal != NULL) {
        tm = src->flat_tex;
        pm = src->flat_pal;
    } else {
        tm = src->scratch;
        pm = src->scratch + GPU3D_TEXMEM_BYTES;
    }
    for (i = 0u; i < l.npolys; i++) {
        Gpu3dPoly p;
        uint32_t fmt, tpm, k, toff, tlen, poff, plen, slot, w, h;
        uint64_t key, t0, t1;
        G3rTex *t;
        G3rResident *res;

        gpu3d_list_poly(&l, i, &p);
        fmt = gpu3d_tex_format(p.texparam);
        if (fmt == 0u) {
            continue;
        }
        if (fmt == 5u) {
            r->st.unsupported++;
            continue;
        }
        r->st.lookups++;
        tpm = p.texparam & 0x3FF0FFFFu;
        /* the same texture as an earlier polygon of this frame: neighbours in the list usually share it, so the last hit is tried first */
        if (last != G3R_NO_TEX && (r->tex[last].texparam & 0x3FF0FFFFu) == tpm && r->tex[last].texpalette == p.texpalette) {
            r->poly_tex[i] = (uint16_t)last;
            r->st.hits++;
            continue;
        }
        for (k = 0u; k < r->ntex; k++) {
            if ((r->tex[k].texparam & 0x3FF0FFFFu) == tpm && r->tex[k].texpalette == p.texpalette) {
                break;
            }
        }
        if (k < r->ntex) {
            r->poly_tex[i] = (uint16_t)k;
            last = k;
            r->st.hits++;
            continue;
        }
        if (r->ntex >= G3R_TEX_MAX) {
            r->st.tex_full++;
            continue;
        }
        w = gpu3d_tex_width(p.texparam);
        h = gpu3d_tex_height(p.texparam);
        /* the content key: gather the referenced bytes out of the slot views, then hash them (the work the dirty bitmap could skip: 2.1(b) built the bits) */
        t0 = tick(src);
        if (gpu3d_tex_ranges(p.texparam, p.texpalette, &toff, &tlen, &poff, &plen) != GPU3D_TEX_OK) {
            r->st.unsupported++;
            continue;
        }
        if (src->flat_tex == NULL) {
            gather(src, 0, toff, tlen);
            if (plen != 0u) {
                gather(src, 1, poff, plen);
            }
        }
        key = gpu3d_tex_key(p.texparam, p.texpalette, tm, GPU3D_TEXMEM_BYTES, pm, GPU3D_TEXPAL_BYTES);
        t1 = tick(src);
        r->st.hash_ticks += t1 - t0;
        r->st.keys++;
        r->st.hashed_bytes += (uint64_t)tlen + plen;
        for (slot = 0u; slot < G3R_SLOTS; slot++) {
            res = &r->mirror.slot[slot];
            if (res->used && res->key == key && res->w == w && res->h == h) {
                break;
            }
        }
        t = &r->tex[r->ntex];
        t->key = key;
        t->texparam = p.texparam;
        t->texpalette = p.texpalette;
        t->w = (uint16_t)w;
        t->h = (uint16_t)h;
        t->upload = 0u;
        t->texel_off = 0u;
        if (slot < G3R_SLOTS) {
            r->mirror.slot[slot].last_use = r->mirror.frame;
            t->slot = (uint16_t)slot;
            r->st.hits++;
        } else {
            uint32_t victim = G3R_SLOTS, words = w * h;

            for (slot = 0u; slot < G3R_SLOTS; slot++) {
                const G3rResident *c = &r->mirror.slot[slot];

                if (!c->used) {
                    victim = slot;
                    break;
                }
                if (c->last_use != r->mirror.frame && (victim == G3R_SLOTS || c->last_use < r->mirror.slot[victim].last_use)) {
                    victim = slot;
                }
            }
            if (victim == G3R_SLOTS) {
                r->st.cache_full++;
                continue;
            }
            if (r->texel_used + words > r->texel_cap) {
                r->st.pool_full++;
                continue;
            }
            t0 = tick(src);
            if (gpu3d_tex_decode(p.texparam, p.texpalette, tm, GPU3D_TEXMEM_BYTES, pm, GPU3D_TEXPAL_BYTES, r->texels + r->texel_used) != GPU3D_TEX_OK) {
                r->st.unsupported++;
                continue;
            }
            r->st.decode_ticks += tick(src) - t0;
            t->slot = (uint16_t)victim;
            t->upload = 1u;
            t->texel_off = r->texel_used;
            r->texel_used += words;
            r->st.decodes++;
            r->st.texel_bytes += (uint64_t)words * 4u;
            res = &r->mirror.slot[victim];
            res->used = 1u;
            res->key = key;
            res->w = (uint16_t)w;
            res->h = (uint16_t)h;
            res->last_use = r->mirror.frame;
        }
        r->poly_tex[i] = (uint16_t)r->ntex;
        last = r->ntex;
        r->ntex++;
    }
    return 0;
}

uint64_t gpu3d_rec_bytes(const Gpu3dRec *r)
{
    return (uint64_t)r->list_len + (uint64_t)r->texel_used * 4u + (uint64_t)r->npolys * 2u + (uint64_t)r->ntex * sizeof(G3rTex);
}
