/* gpu3d_tex.c -- see gpu3d_tex.h. Written from GBATEK ("DS 3D Textures": formats, TEXIMAGE_PARAM, PLTT_BASE) and the PICA texture layout of libctru's
 * GPU_TEXCOLOR / citro3d's C3D_Tex (8x8 tiles, z-order inside a tile). Pure C. */
#include "gpu3d_tex.h"

uint32_t gpu3d_tex_format(uint32_t tp) { return (tp >> 26) & 7u; }
uint32_t gpu3d_tex_width(uint32_t tp) { return 8u << ((tp >> 20) & 7u); }
uint32_t gpu3d_tex_height(uint32_t tp) { return 8u << ((tp >> 23) & 7u); }
static uint32_t wrap_of(uint32_t rep, uint32_t flip) { return rep ? (flip ? GPU3D_WRAP_MIRROR : GPU3D_WRAP_REPEAT) : GPU3D_WRAP_CLAMP; }
uint32_t gpu3d_tex_wrap_s(uint32_t tp) { return wrap_of((tp >> 16) & 1u, (tp >> 18) & 1u); }
uint32_t gpu3d_tex_wrap_t(uint32_t tp) { return wrap_of((tp >> 17) & 1u, (tp >> 19) & 1u); }
int gpu3d_tex_color0_transparent(uint32_t tp) { return (int)((tp >> 29) & 1u); }

uint32_t gpu3d_c5_to_c6(uint32_t c5) { return c5 ? c5 * 2u + 1u : 0u; }
uint32_t gpu3d_c6_mod8(uint32_t c6) { return ((c6 + 1u) * 255u) / 64u; }     /* rounded DOWN: an 8-bit result shifted right by two must give back c6 when the vertex colour is 1.0 */
uint32_t gpu3d_a5_to_a8(uint32_t a5) { return a5 ? ((a5 + 1u) * 255u * 2u + 32u) / 64u : 0u; }

static uint32_t bits_per_texel(uint32_t fmt)
{
    switch (fmt) {
    case 1: case 4: case 6: return 8;
    case 2: return 2;
    case 3: return 4;
    case 7: return 16;
    default: return 0;
    }
}

int gpu3d_tex_ranges(uint32_t tp, uint32_t pal, uint32_t *toff, uint32_t *tlen, uint32_t *poff, uint32_t *plen)
{
    uint32_t fmt = gpu3d_tex_format(tp), bpt = bits_per_texel(fmt), n, pn;
    *toff = *tlen = *poff = *plen = 0;
    if (bpt == 0) return GPU3D_TEX_UNSUPPORTED;
    *toff = (tp & 0xFFFFu) << 3;
    n = gpu3d_tex_width(tp) * gpu3d_tex_height(tp) * bpt / 8u;
    if (*toff >= GPU3D_TEXMEM_BYTES) n = 0;
    else if (n > GPU3D_TEXMEM_BYTES - *toff) n = GPU3D_TEXMEM_BYTES - *toff;
    *tlen = n;
    switch (fmt) {
    case 2: *poff = (pal & 0xFFFFu) << 3; pn = 4u * 2u; break;
    case 3: *poff = (pal & 0xFFFFu) << 4; pn = 16u * 2u; break;
    case 4: *poff = (pal & 0xFFFFu) << 4; pn = 256u * 2u; break;
    case 1: case 6: *poff = (pal & 0xFFFFu) << 4; pn = (fmt == 1 ? 32u : 8u) * 2u; break;
    default: pn = 0; break;
    }
    if (pn) {
        if (*poff >= GPU3D_TEXPAL_BYTES) pn = 0;
        else if (pn > GPU3D_TEXPAL_BYTES - *poff) pn = GPU3D_TEXPAL_BYTES - *poff;
    }
    *plen = pn;
    return GPU3D_TEX_OK;
}

uint32_t gpu3d_tex_wrap_index(int32_t c, uint32_t size, uint32_t mode)
{
    int32_t s = (int32_t)size;
    int32_t m;
    if (mode == GPU3D_WRAP_CLAMP) return (uint32_t)(c < 0 ? 0 : (c >= s ? s - 1 : c));
    m = c % s;
    if (m < 0) m += s;
    if (mode == GPU3D_WRAP_REPEAT) return (uint32_t)m;
    /* mirror: the period is 2 * size, the second half runs backwards */
    {
        int32_t p = c % (2 * s);
        if (p < 0) p += 2 * s;
        return (uint32_t)(p < s ? p : 2 * s - 1 - p);
    }
}

uint32_t gpu3d_tex_tiled_index(uint32_t x, uint32_t y, uint32_t w)
{
    uint32_t tile = (y >> 3) * (w >> 3) + (x >> 3);
    uint32_t xi = x & 7u, yi = y & 7u;
    uint32_t z = (xi & 1u) | ((yi & 1u) << 1) | ((xi & 2u) << 1) | ((yi & 2u) << 2) | ((xi & 4u) << 2) | ((yi & 4u) << 3);
    return tile * 64u + z;
}

static uint32_t at8(const uint8_t *m, size_t len, size_t i) { return i < len ? m[i] : 0u; }
static uint32_t at16(const uint8_t *m, size_t len, size_t i) { return at8(m, len, i) | (at8(m, len, i + 1) << 8); }

static uint32_t pack(uint32_t c555, uint32_t a8)
{
    uint32_t r = gpu3d_c6_mod8(gpu3d_c5_to_c6(c555 & 31u)), g = gpu3d_c6_mod8(gpu3d_c5_to_c6((c555 >> 5) & 31u)), b = gpu3d_c6_mod8(gpu3d_c5_to_c6((c555 >> 10) & 31u));
    return (r << 24) | (g << 16) | (b << 8) | a8;
}

int gpu3d_tex_decode(uint32_t tp, uint32_t pal, const uint8_t *tm, size_t tl, const uint8_t *pm, size_t pl, uint32_t *out)
{
    uint32_t fmt = gpu3d_tex_format(tp), w = gpu3d_tex_width(tp), h = gpu3d_tex_height(tp), x, y;
    size_t base = (size_t)(tp & 0xFFFFu) << 3, pbase;
    int c0 = gpu3d_tex_color0_transparent(tp);
    if (bits_per_texel(fmt) == 0) return GPU3D_TEX_UNSUPPORTED;
    pbase = (size_t)(pal & 0xFFFFu) << (fmt == 2 ? 3 : 4);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            uint32_t v, idx, a8 = 255u, col;
            switch (fmt) {
            case 1: v = at8(tm, tl, base + i); idx = v & 31u; a8 = gpu3d_a5_to_a8(((v >> 5) * 4u) + ((v >> 5) / 2u)); col = at16(pm, pl, pbase + 2u * idx); break;
            case 6: v = at8(tm, tl, base + i); idx = v & 7u; a8 = gpu3d_a5_to_a8(v >> 3); col = at16(pm, pl, pbase + 2u * idx); break;
            case 2: v = (at8(tm, tl, base + i / 4u) >> (2u * (i & 3u))) & 3u; if (c0 && v == 0) a8 = 0; col = at16(pm, pl, pbase + 2u * v); break;
            case 3: v = (at8(tm, tl, base + i / 2u) >> (4u * (i & 1u))) & 15u; if (c0 && v == 0) a8 = 0; col = at16(pm, pl, pbase + 2u * v); break;
            case 4: v = at8(tm, tl, base + i); if (c0 && v == 0) a8 = 0; col = at16(pm, pl, pbase + 2u * v); break;
            default: col = at16(tm, tl, base + 2u * i); a8 = (col & 0x8000u) ? 255u : 0u; break;     /* 7: direct colour, bit 15 = opaque */
            }
            out[gpu3d_tex_tiled_index(x, h - 1u - y, w)] = pack(col, a8);   /* rows are stored bottom-up: the PICA reads memory row h-1-floor(v*h) (measured, the private design notes) */
        }
    }
    return GPU3D_TEX_OK;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

uint64_t gpu3d_tex_key(uint32_t tp, uint32_t pal, const uint8_t *tm, size_t tl, const uint8_t *pm, size_t pl)
{
    uint32_t toff, tlen, poff, plen;
    uint8_t hdr[8];
    uint64_t h = 0xcbf29ce484222325ULL;
    int i;
    uint32_t full_tp = tp;
    tp &= 0x3FF00000u;      /* size, format, colour-0 bit: the sampler's wrap bits and the offset do not change the decoded texture */
    for (i = 0; i < 4; i++) { hdr[i] = (uint8_t)(tp >> (8 * i)); hdr[4 + i] = 0; }     /* the palette base is not hashed: the palette BYTES are */
    h = fnv(h, hdr, 8);
    if (gpu3d_tex_ranges(full_tp, pal, &toff, &tlen, &poff, &plen) != GPU3D_TEX_OK) return h;
    if (toff < tl) h = fnv(h, tm + toff, tl - toff < tlen ? tl - toff : tlen);
    if (plen && poff < pl) h = fnv(h, pm + poff, pl - poff < plen ? pl - poff : plen);
    return h;
}
