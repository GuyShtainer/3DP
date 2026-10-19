/* gpu2d.c -- the 2D engines' per-frame input record (bet 2.3 slice (a)) and the renderer over it (slice (b)): see gpu2d.h. Pure C (<assert.h> / <stdint.h> / <string.h>).
 *
 * The renderer is written from GBATEK's "DS Video" chapter in this file's own structure (the oracle, melonDS, was consulted only to settle behaviours, never copied):
 *   "DS Video: LCD I/O Display Control" (DISPCNT: BG enables, BG mode, 3D-as-BG0, display mode, forced blank, tile / map base of engine A, OBJ mapping + boundary,
 *     extended palette enables), "LCD I/O BG Control" (BGCNT priority, char / screen base, 256-colour, screen size), "LCD I/O BG Scrolling" (BGxHOFS / VOFS, 9 bits),
 *   "LCD VRAM Text Background Screens" + "LCD Text BG Data / Tile Data" (map entries, 4 x 32x32 screens, H / V flip, 4 / 8 bit tiles), "DS Extended Palettes" (BG slot
 *     select by BGCNT bit 13, OBJ), "LCD OBJ - OAM Attributes" + "LCD OBJ - Overview / VRAM Character Mapping" (1D / 2D tile mapping, boundary, the size table, priority
 *     and OAM-order rule), "LCD Color Special Effects" (BLDCNT, BLDALPHA: alpha blend (a*EVA + b*EVB) / 16 with EV capped at 16), "LCD I/O Master Brightness"
 *     (MASTER_BRIGHT up / down), "DS 3D Display Control / 3D as BG0" (the 3D layer's own 5-bit alpha blends against the layer below).
 * Pixel pipeline: per engine, per line: backdrop, then BG priorities 3..0 (BG3..BG0 within a priority, then the OBJs of that priority) painted into a TWO-DEEP stack
 * (top + the layer it covered), then the colour special effect picks top / blend, then master brightness. Colours are 6 bits per channel (r | g << 8 | b << 16) until the
 * very last step. */
#include <assert.h>
#include <string.h>

#include "gpu2d.h"

static const uint32_t k_voff[GPU2D_WINDOWS] = { GPU2D_VOFF_WIN0, GPU2D_VOFF_WIN1, GPU2D_VOFF_WIN2, GPU2D_VOFF_WIN3 };
static const uint32_t k_vsize[GPU2D_WINDOWS] = { 0x80000u, 0x40000u, 0x20000u, 0x20000u };   /* main BG, main OBJ, sub BG, sub OBJ */

/* Copy ONE extended-palette slot into the record: the live view when bound (NULL = nothing mapped), the record's copy is what the renderer reads. */
static const uint8_t *snap_ext(Gpu2dFrame *f, uint32_t off, const uint8_t *live)
{
    if (live == NULL) {
        return NULL;
    }
    memcpy(f->vram + off, live, GPU2D_EXT_SLOT);
    f->vram_bytes += GPU2D_EXT_SLOT;
    return f->vram + off;
}

/* The record's own copy of the VRAM the engines can address (review B1): per window the 16 KB chunks an enabled bank covers (runs are copied with one memcpy), a chunk
   that is no longer bound is zeroed (an unmapped DS read is 0), and every bound ext-palette slot. */
static void snap_vram(Gpu2dFrame *f, const Gpu2dSrc *src)
{
    uint32_t w, e, s, c, n;

    f->vram_bytes = 0u;
    for (w = 0u; w < GPU2D_WINDOWS; w++) {
        const uint8_t *live = src->window(w);
        uint8_t *dst = f->vram + k_voff[w];
        uint32_t chunks = k_vsize[w] / GPU2D_VCHUNK;
        uint64_t m = live != NULL ? (uint64_t)src->window_chunks(w) : 0u;

        m &= ((uint64_t)1u << chunks) - 1u;
        for (c = 0u; c < chunks; c++) {
            if ((m >> c & 1u) == 0u && (f->vchunks[w] >> c & 1u) != 0u) {
                memset(dst + c * GPU2D_VCHUNK, 0, GPU2D_VCHUNK);
            }
        }
        for (c = 0u; c < chunks; c = n) {
            n = c + 1u;
            if ((m >> c & 1u) == 0u) {
                continue;
            }
            while (n < chunks && (m >> n & 1u) != 0u) {
                n++;
            }
            memcpy(dst + c * GPU2D_VCHUNK, live + c * GPU2D_VCHUNK, (size_t)(n - c) * GPU2D_VCHUNK);
            f->vram_bytes += (n - c) * GPU2D_VCHUNK;
        }
        f->vchunks[w] = (uint32_t)m;
        f->win[w] = dst;
    }
    for (e = 0u; e < GPU2D_ENGINES; e++) {
        for (s = 0u; s < GPU2D_BG_EXT_SLOTS; s++) {
            f->bg_ext[e][s] = snap_ext(f, GPU2D_VOFF_BGEXT + (e * GPU2D_BG_EXT_SLOTS + s) * GPU2D_EXT_SLOT, src->bg_ext(e, s));
        }
        f->obj_ext[e] = snap_ext(f, GPU2D_VOFF_OBJEXT + e * GPU2D_EXT_SLOT, src->obj_ext(e));
    }
}

void gpu2d_frame_begin(Gpu2dFrame *f, const Gpu2dSrc *src)
{
    uint32_t e;

    assert(f != NULL && src != NULL && src->io != NULL);
    assert(src->window != NULL && src->bg_ext != NULL && src->obj_ext != NULL && src->window_chunks != NULL);
    f->seq++;
    f->uniform = 0u;
    f->lines = 0u;
    f->layer3d = NULL;
    memset(f->vramcnt, 0, sizeof f->vramcnt);
    memcpy(f->vramcnt, src->io + GPU2D_VRAMCNT_OFF, 10u);   /* VRAMCNT_A..G, WRAMCNT, VRAMCNT_H, VRAMCNT_I */
    for (e = 0u; e < GPU2D_ENGINES; e++) {
        if (src->pltt != NULL) {
            memcpy(f->pltt[e], src->pltt + e * GPU2D_PLTT_LEN, GPU2D_PLTT_LEN);
        } else {
            memset(f->pltt[e], 0, GPU2D_PLTT_LEN);
        }
        if (src->oam != NULL) {
            memcpy(f->oam[e], src->oam + e * GPU2D_OAM_LEN, GPU2D_OAM_LEN);
        } else {
            memset(f->oam[e], 0, GPU2D_OAM_LEN);
        }
    }
    snap_vram(f, src);
}

static void copy_line(Gpu2dFrame *f, const uint8_t *io, uint32_t dst)
{
    memcpy(f->regs[0][dst], io, GPU2D_REGS_LEN);
    memset(f->regs[0][dst] + 4, 0, 4);   /* DISPSTAT, VCOUNT: not render state (gpu2d.h) */
    memcpy(f->regs[1][dst], io + GPU2D_B_BASE, GPU2D_REGS_LEN);
}

void gpu2d_frame_lines(Gpu2dFrame *f, const uint8_t *io, uint32_t first, uint32_t count)
{
    uint32_t i;

    assert(f != NULL && io != NULL && count > 0u && first + count <= GPU2D_LINES);
    if (count == GPU2D_LINES) {
        assert(first == 0u);
        copy_line(f, io, 0u);
        f->uniform = 1u;
        f->lines = GPU2D_LINES;
        return;
    }
    assert(!f->uniform);   /* mixing the short-cut with single lines inside one frame is a caller bug */
    for (i = first; i < first + count; i++) {
        copy_line(f, io, i);
    }
    f->lines += count;
}

const uint8_t *gpu2d_line_regs(const Gpu2dFrame *f, uint32_t engine, uint32_t line)
{
    assert(f != NULL && engine < GPU2D_ENGINES && line < GPU2D_LINES);
    return f->regs[engine][f->uniform ? 0u : line];
}

/* ===================================================================================================================================================================
 * The renderer (bet 2.3 slice (b))
 * =================================================================================================================================================================== */

/* register offsets inside an engine's 0x70-byte image */
enum {
    R_DISPCNT = 0x00, R_BGCNT = 0x08, R_BGOFS = 0x10, R_MOSAIC = 0x4C, R_BLDCNT = 0x50, R_BLDALPHA = 0x52, R_DISPCAP = 0x64, R_MBRIGHT = 0x6C,
    OBJ_PAL = 0x200, OBJ_COUNT = 128, NO_OBJ = 0xFF
};

typedef struct Line {
    uint32_t col[2][GPU2D_W];   /* [0] top, [1] the layer it covers: 6-bit r | g << 8 | b << 16 */
    uint8_t  id[2][GPU2D_W];    /* GPU2D_LID_* (low 3 bits) of the above */
    uint8_t  a3[GPU2D_W];       /* the 5-bit alpha of a top pixel whose id is GPU2D_LID_3D */
    uint32_t objc[GPU2D_W];     /* the sprite pass of this line: colour (6-bit packed) and BG-relative priority; objp == NO_OBJ: none */
    uint8_t  objp[GPU2D_W];
} Line;

typedef struct Eng {
    const Gpu2dFrame *f;
    const uint8_t *r;           /* this line's register image */
    const uint8_t *pal;         /* this engine's 1 KB palette copy */
    const uint8_t *bgv, *objv;  /* BG / OBJ VRAM windows (NULL: reads as zero) */
    uint32_t bgmask, objmask;
    uint32_t e, dispcnt;
    uint32_t *uns;              /* GPU2D_UNS_* accumulator of the call */
} Eng;

static uint32_t rd16(const uint8_t *p, uint32_t o) { return (uint32_t)p[o] | ((uint32_t)p[o + 1u] << 8); }
static uint32_t rd32(const uint8_t *p, uint32_t o) { return rd16(p, o) | (rd16(p, o + 2u) << 16); }

/* 15-bit BGR555 -> the 6-bit-per-channel packed word: each channel doubled (GBATEK: the 2D engines output 6 bits, the 5-bit palette value goes in the upper 5). */
static uint32_t c555(uint32_t c)
{
    return ((c & 31u) << 1) | (((c >> 5) & 31u) << 9) | (((c >> 10) & 31u) << 17);
}

static uint32_t expand6(uint32_t c) { return (c << 2) | (c >> 4); }

static uint32_t out_word(uint32_t c6)
{
    return 0xFF000000u | (expand6(c6 & 63u) << 16) | (expand6((c6 >> 8) & 63u) << 8) | expand6((c6 >> 16) & 63u);   /* packed r | g << 8 | b << 16 -> 0xFFRRGGBB */
}

static void push(Line *l, uint32_t x, uint32_t c, uint32_t id)
{
    l->col[1][x] = l->col[0][x];
    l->id[1][x] = l->id[0][x];
    l->col[0][x] = c;
    l->id[0][x] = (uint8_t)id;
}

/* ---- text backgrounds (BG mode 0) ------------------------------------------------------------------------------------------------------------------------------ */

/* The palette and tile address of the map entry `tile` at tile row `ty` (0..7). */
static void bg_tile(const Eng *g, uint32_t tile, uint32_t ty, uint32_t col256, uint32_t slot, uint32_t tbase, uint32_t *pix, const uint8_t **palp, uint32_t *pbase)
{
    uint32_t row = (tile & 0x800u) != 0u ? 7u - ty : ty;

    if (col256 != 0u) {
        *pix = tbase + ((tile & 0x3FFu) << 6) + (row << 3);
        if ((g->dispcnt & 0x40000000u) != 0u) {   /* extended BG palettes: 8 KB slot, 16 palettes of 256 colours */
            const uint8_t *ext = g->f->bg_ext[g->e][slot];
            *palp = ext;
            *pbase = (tile >> 12) * 512u;
        } else {
            *palp = g->pal;
            *pbase = 0u;
        }
    } else {
        *pix = tbase + ((tile & 0x3FFu) << 5) + (row << 2);
        *palp = g->pal;
        *pbase = (tile >> 12) * 32u;
    }
}

static void draw_bg_text(const Eng *g, Line *l, uint32_t bg, uint32_t line)
{
    const uint8_t *v = g->bgv;
    uint32_t cnt, hofs, y, tbase, mbase, widex, col256, slot, ty, i, tile = 0u, pix = 0u, pbase = 0u;
    const uint8_t *palp = NULL;

    assert(bg < 4u && line < GPU2D_LINES);
    if (v == NULL) {
        return;   /* nothing mapped: every tile byte is 0, colour index 0 is transparent */
    }
    cnt = rd16(g->r, R_BGCNT + 2u * bg);
    hofs = rd16(g->r, R_BGOFS + 4u * bg);
    y = (rd16(g->r, R_BGOFS + 4u * bg + 2u) + line) & 0xFFFFu;
    tbase = ((cnt >> 2) & 0xFu) << 14;
    mbase = ((cnt >> 8) & 0x1Fu) << 11;
    if (g->e == 0u) {   /* engine A adds DISPCNT's 64 KB-unit character and screen bases */
        tbase += ((g->dispcnt >> 24) & 7u) << 16;
        mbase += ((g->dispcnt >> 27) & 7u) << 16;
    }
    if ((cnt & 0x8000u) != 0u) {   /* tall: two screens of 256 rows (or four if wide) */
        mbase += (y & 0x1F8u) << 3;
        mbase += ((cnt & 0x4000u) != 0u) ? ((y & 0x100u) << 3) : 0u;
    } else {
        mbase += (y & 0xF8u) << 3;
    }
    widex = ((cnt & 0x4000u) != 0u) ? 0x100u : 0u;
    col256 = cnt & 0x80u;
    slot = (bg < 2u && (cnt & 0x2000u) != 0u) ? 2u + bg : bg;
    ty = y & 7u;
    for (i = 0u; i < GPU2D_W; i++) {
        uint32_t xp = (hofs + i) & 0xFFFFu, tx, idx, a;

        if (i == 0u || (xp & 7u) == 0u) {
            a = (mbase + ((xp & 0xF8u) >> 2) + ((xp & widex) << 3)) & g->bgmask;
            tile = (uint32_t)v[a] | ((uint32_t)v[a + 1u] << 8);
            bg_tile(g, tile, ty, col256, slot, tbase, &pix, &palp, &pbase);
        }
        tx = (tile & 0x400u) != 0u ? 7u - (xp & 7u) : (xp & 7u);
        if (col256 != 0u) {
            idx = v[(pix + tx) & g->bgmask];
        } else {
            idx = v[(pix + (tx >> 1)) & g->bgmask];
            idx = ((tx & 1u) != 0u) ? (idx >> 4) : (idx & 15u);
        }
        if (idx != 0u) {
            push(l, i, palp != NULL ? c555(rd16(palp, pbase + idx * 2u)) : 0u, GPU2D_LID_BG0 + bg);
        }
    }
}

/* ---- the 3D layer as engine A's BG0 ------------------------------------------------------------------------------------------------------------------------------ */

static void draw_bg_3d(const Eng *g, Line *l, uint32_t line)
{
    const uint32_t *src = g->f->layer3d;
    uint32_t hofs, i;
    int32_t shift;

    assert(line < GPU2D_LINES);
    if (src == NULL) {
        return;
    }
    hofs = rd16(g->r, R_BGOFS) & 0x1FFu;
    shift = (hofs < 256u) ? (int32_t)hofs : (int32_t)hofs - 512;   /* BG0HOFS as the 3D unit's 9-bit signed scroll: output i shows layer pixel i + shift, outside = transparent */
    for (i = 0u; i < GPU2D_W; i++) {
        int32_t j = (int32_t)i + shift;
        uint32_t c;

        if (j < 0 || j >= (int32_t)GPU2D_W) {
            continue;
        }
        c = src[line * GPU2D_W + (uint32_t)j];
        if ((c >> 24) == 0u) {
            continue;   /* alpha 0: no 3D pixel */
        }
        push(l, i, c & 0x3F3F3Fu, GPU2D_LID_3D);
        l->a3[i] = (uint8_t)((c >> 24) & 0x1Fu);
    }
}

/* ---- sprites (one pass per line, results resolved by BG priority later) ----------------------------------------------------------------------------------------------- */

static const uint8_t k_obj_w[16] = { 8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8 };    /* index = shape | size << 2: square, wide, tall, (invalid) */
static const uint8_t k_obj_h[16] = { 8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8 };

static void obj_pixels(const Eng *g, Line *l, uint32_t a2, int32_t x, uint32_t w, uint32_t ypos)
{
    uint32_t tile = a2 & 0x3FFu, prio = (a2 >> 10) & 3u, base, xo, palo = OBJ_PAL + ((a2 >> 12) << 5);
    uint32_t xstart = (x < 0) ? (uint32_t)(-x) : 0u;

    if ((g->dispcnt & 0x10u) != 0u) {   /* 1D mapping: boundary shift, then rows of w / 8 tiles */
        base = (tile << ((g->dispcnt >> 20) & 3u)) + (ypos >> 3) * (w >> 3);
    } else {                            /* 2D mapping: 32 tiles per row */
        base = tile + (ypos >> 3) * 32u;
    }
    base = (base << 5) + ((ypos & 7u) << 2);
    for (xo = xstart; xo < w; xo++) {
        uint32_t sx = (uint32_t)(x + (int32_t)xo), b, idx;

        if (sx >= GPU2D_W) {
            break;
        }
        b = g->objv[(base + ((xo & (w - 8u)) << 2) + ((xo & 7u) >> 1)) & g->objmask];
        idx = ((xo & 1u) != 0u) ? (b >> 4) : (b & 15u);
        if (idx != 0u && prio < l->objp[sx]) {
            l->objc[sx] = c555(rd16(g->pal, palo + idx * 2u));
            l->objp[sx] = (uint8_t)prio;
        }
    }
}

/* One texel of a 16-colour sprite of w x h pixels at (tx, ty) inside the sprite (the addressing of obj_pixels, one pixel at a time): colour index 0..15. */
static uint32_t obj_texel(const Eng *g, uint32_t a2, uint32_t w, uint32_t tx, uint32_t ty)
{
    uint32_t tile = a2 & 0x3FFu, base, b;

    if ((g->dispcnt & 0x10u) != 0u) {
        base = (tile << ((g->dispcnt >> 20) & 3u)) + (ty >> 3) * (w >> 3);
    } else {
        base = tile + (ty >> 3) * 32u;
    }
    base = (base << 5) + ((ty & 7u) << 2);
    b = g->objv[(base + ((tx & (w - 8u)) << 2) + ((tx & 7u) >> 1)) & g->objmask];
    return ((tx & 1u) != 0u) ? (b >> 4) : (b & 15u);
}

/* A rotation / scaling sprite (attr0 bit 8, 16 colours, normal mode; bet 2.3 slice (d): the X menu's cursor icon is one). GBATEK "LCD OBJ - OAM Rotation/Scaling
   Parameters": group (attr1 bits 9..13) selects four s16 8.8 values PA, PB, PC, PD, stored in the unused attribute-3 halfwords (byte 6) of OAM entries 4 * group .. +3. The
   sprite is w x h, centred in its bounding box (double size: twice as large); screen pixel (xo, yo) of the box (offsets from the box centre dx, dy) samples the sprite at
   (PA * dx + PB * dy) / 256 + w / 2, (PC * dx + PD * dy) / 256 + h / 2 (arithmetic shift, floor), transparent outside 0..w-1 / 0..h-1. */
static void obj_affine(const Eng *g, Line *l, uint32_t a1, uint32_t a2, int32_t x, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t ypos)
{
    uint32_t prio = (a2 >> 10) & 3u, palo = OBJ_PAL + ((a2 >> 12) << 5), xo, xstart = (x < 0) ? (uint32_t)(-x) : 0u;
    int32_t pa, pb, pc, pd, dy = (int32_t)ypos - (int32_t)(bh >> 1);
    const uint8_t *pr = g->f->oam[g->e] + ((a1 >> 9) & 31u) * 32u + 6u;

    pa = (int16_t)rd16(pr, 0u);
    pb = (int16_t)rd16(pr, 8u);
    pc = (int16_t)rd16(pr, 16u);
    pd = (int16_t)rd16(pr, 24u);
    for (xo = xstart; xo < bw; xo++) {
        uint32_t sx = (uint32_t)(x + (int32_t)xo), idx;
        int32_t dx = (int32_t)xo - (int32_t)(bw >> 1), tx, ty;

        if (sx >= GPU2D_W) {
            break;
        }
        tx = ((pa * dx + pb * dy) >> 8) + (int32_t)(w >> 1);
        ty = ((pc * dx + pd * dy) >> 8) + (int32_t)(h >> 1);
        if (tx < 0 || ty < 0 || tx >= (int32_t)w || ty >= (int32_t)h) {
            continue;
        }
        idx = obj_texel(g, a2, w, (uint32_t)tx, (uint32_t)ty);
        if (idx != 0u && prio < l->objp[sx]) {
            l->objc[sx] = c555(rd16(g->pal, palo + idx * 2u));
            l->objp[sx] = (uint8_t)prio;
        }
    }
}

static void obj_one(const Eng *g, Line *l, const uint8_t *oam, uint32_t line)
{
    uint32_t a0 = rd16(oam, 0), a1 = rd16(oam, 2), a2 = rd16(oam, 4);
    uint32_t sz = (a0 >> 14) | ((a1 & 0xC000u) >> 12), w = k_obj_w[sz], h = k_obj_h[sz], bw = w, bh = h;
    int32_t x = (int32_t)(a1 & 0x1FFu);
    uint32_t affine = a0 & 0x100u;

    if (affine == 0u && (a0 & 0x200u) != 0u) {
        return;   /* disabled */
    }
    if (affine != 0u && (a0 & 0x200u) != 0u) {
        bw <<= 1;   /* rotation / scaling, double size: the bounding box */
        bh <<= 1;
    }
    if (((line - (a0 & 0xFFu)) & 0xFFu) >= bh) {
        return;
    }
    if (x >= 256) {
        x -= 512;
    }
    if (x <= -(int32_t)bw) {
        return;
    }
    if (((a0 >> 10) & 3u) != 0u) {
        *g->uns |= GPU2D_UNS_SPR_MODE;
    } else if ((a0 & 0x2000u) != 0u) {
        *g->uns |= GPU2D_UNS_SPR_256;
    } else {
        if (affine == 0u && (a1 & 0x3000u) != 0u) {
            *g->uns |= GPU2D_UNS_SPR_FLIP;
        }
        if ((a0 & 0x1000u) != 0u && ((rd16(g->r, R_MOSAIC) >> 8) & 0xFFu) != 0u) {
            *g->uns |= GPU2D_UNS_OBJ_MOSAIC;
        }
        if (g->objv != NULL) {
            if (affine != 0u) {
                obj_affine(g, l, a1, a2, x, w, h, bw, bh, (line - (a0 & 0xFFu)) & 0xFFu);
            } else {
                obj_pixels(g, l, a2, x, w, (line - (a0 & 0xFFu)) & 0xFFu);
            }
        }
    }
}

static void build_obj_line(const Eng *g, Line *l, uint32_t line)
{
    uint32_t n;

    memset(l->objp, NO_OBJ, sizeof l->objp);
    if ((g->dispcnt & 0x1000u) == 0u) {
        return;
    }
    for (n = 0u; n < OBJ_COUNT; n++) {
        obj_one(g, l, g->f->oam[g->e] + n * 8u, line);
    }
}

/* ---- colour special effect + master brightness -------------------------------------------------------------------------------------------------------------------------- */

static const uint8_t k_bld_bit[7] = { 5, 0, 1, 2, 3, 4, 0 };   /* layer ID -> BLDCNT target bit (backdrop 5, BG0..3 0..3, OBJ 4, 3D = BG0) */

static uint32_t mix(uint32_t a, uint32_t b, uint32_t ea, uint32_t eb, uint32_t sh)
{
    uint32_t c, v, rnd = 1u << (sh - 1u), out = 0u;

    for (c = 0u; c < 24u; c += 8u) {
        v = (((a >> c) & 63u) * ea + ((b >> c) & 63u) * eb + rnd) >> sh;
        out |= ((v > 63u) ? 63u : v) << c;
    }
    return out;
}

static uint32_t effect(const Eng *g, const Line *l, uint32_t x, uint32_t *id)
{
    uint32_t bld = rd16(g->r, R_BLDCNT), top = l->id[0][x], sec = l->id[1][x], tb = k_bld_bit[top], sb = 8u + k_bld_bit[sec];
    uint32_t alpha = rd16(g->r, R_BLDALPHA), eva = alpha & 0x1Fu, evb = (alpha >> 8) & 0x1Fu;

    *id = top;
    if (top == GPU2D_LID_3D) {   /* the 3D layer blends by its own alpha against whatever it covers, when that layer is a second target */
        uint32_t a = (uint32_t)l->a3[x] + 1u;

        if ((bld & (1u << sb)) == 0u || a >= 32u) {
            return l->col[0][x];
        }
        *id = top | GPU2D_LID_BLENDED | (sec << GPU2D_LID_SECOND_SHIFT);
        return mix(l->col[0][x], l->col[1][x], a, 32u - a, 5u);
    }
    if (((bld >> 6) & 3u) == 1u && (bld & (1u << tb)) != 0u && (bld & (1u << sb)) != 0u) {
        *id = top | GPU2D_LID_BLENDED | (sec << GPU2D_LID_SECOND_SHIFT);
        return mix(l->col[0][x], l->col[1][x], (eva > 16u) ? 16u : eva, (evb > 16u) ? 16u : evb, 4u);
    }
    return l->col[0][x];
}

static uint32_t bright(uint32_t c6, uint32_t mode, uint32_t fac)
{
    uint32_t sh, out = 0u, v;

    for (sh = 0u; sh < 24u; sh += 8u) {
        v = (c6 >> sh) & 63u;
        v = (mode == 1u) ? v + (((63u - v) * fac) >> 4) : v - (((v * fac) + 15u) >> 4);
        out |= v << sh;
    }
    return out;
}

/* ---- one line of one engine ---------------------------------------------------------------------------------------------------------------------------------- */

static void layers_to_stack(const Eng *g, Line *l, uint32_t line)
{
    uint32_t p, bg, x, bd = c555(rd16(g->pal, 0u));

    for (x = 0u; x < GPU2D_W; x++) {
        l->col[0][x] = l->col[1][x] = bd;
        l->id[0][x] = l->id[1][x] = (uint8_t)GPU2D_LID_BACKDROP;
    }
    build_obj_line(g, l, line);
    for (p = 4u; p-- > 0u;) {
        for (bg = 4u; bg-- > 0u;) {
            if ((g->dispcnt & (0x100u << bg)) == 0u || (rd16(g->r, R_BGCNT + 2u * bg) & 3u) != p) {
                continue;
            }
            if ((rd16(g->r, R_BGCNT + 2u * bg) & 0x40u) != 0u && (rd16(g->r, R_MOSAIC) & 0xFFu) != 0u) {
                *g->uns |= GPU2D_UNS_BG_MOSAIC;
            }
            if (bg == 0u && g->e == 0u && (g->dispcnt & 8u) != 0u) {
                draw_bg_3d(g, l, line);
            } else {
                draw_bg_text(g, l, bg, line);
            }
        }
        for (x = 0u; x < GPU2D_W; x++) {
            if (l->objp[x] == p) {
                push(l, x, l->objc[x], GPU2D_LID_OBJ);
            }
        }
    }
}

static void count_line_features(const Eng *g)
{
    uint32_t bld = rd16(g->r, R_BLDCNT), dm = (g->dispcnt >> 16) & 3u;

    *g->uns |= ((g->dispcnt & 0xE000u) != 0u) ? GPU2D_UNS_WINDOW : 0u;
    *g->uns |= ((g->dispcnt & 7u) != 0u) ? GPU2D_UNS_BG_MODE : 0u;
    *g->uns |= (dm >= 2u) ? GPU2D_UNS_DISP_MODE : 0u;
    *g->uns |= (((bld >> 6) & 3u) >= 2u && (bld & 0x1Fu) != 0u) ? GPU2D_UNS_BLD_BRIGHT : 0u;
    *g->uns |= (g->e == 0u && (rd32(g->r, R_DISPCAP) >> 31) != 0u) ? GPU2D_UNS_CAPTURE : 0u;
}

static void render_line(const Eng *g, Line *l, Gpu2dOut *out, uint32_t line)
{
    uint32_t x, dm = (g->dispcnt >> 16) & ((g->e != 0u) ? 1u : 3u), mb = rd16(g->r, R_MBRIGHT), mode = mb >> 14, fac = mb & 0x1Fu;
    uint32_t *px = &out->pix[g->e][line * GPU2D_W];
    uint8_t *id = &out->lid[g->e][line * GPU2D_W];

    if (dm == 0u) {   /* display mode 0: the LCD shows white, brightness does not apply */
        for (x = 0u; x < GPU2D_W; x++) {
            px[x] = 0xFFFFFFFFu;
            id[x] = (uint8_t)GPU2D_LID_BACKDROP;
        }
        return;
    }
    if ((g->dispcnt & 0x80u) != 0u) {   /* forced blank: white */
        for (x = 0u; x < GPU2D_W; x++) {
            l->col[0][x] = 0x3F3F3Fu;
            l->id[0][x] = (uint8_t)GPU2D_LID_BACKDROP;
        }
    } else {
        count_line_features(g);
        layers_to_stack(g, l, line);
    }
    if (fac > 16u) {
        fac = 16u;
    }
    for (x = 0u; x < GPU2D_W; x++) {
        uint32_t c, lid = l->id[0][x];

        c = ((g->dispcnt & 0x80u) != 0u) ? l->col[0][x] : effect(g, l, x, &lid);
        if (mode == 1u || mode == 2u) {
            c = bright(c, mode, fac);
        }
        px[x] = out_word(c);
        id[x] = (uint8_t)lid;
    }
}

static Line s_line_buf;   /* the line buffers: one set, reused (not re-entrant; the port calls the renderer from one thread) */

static void render_engines(const Gpu2dFrame *f, Gpu2dOut *out, uint32_t e0, uint32_t e1, uint32_t *uns)
{
    uint32_t e, line;
    Eng g;

    for (e = e0; e < e1; e++) {
        g.f = f;
        g.e = e;
        g.pal = f->pltt[e];
        g.bgv = f->win[e == 0u ? 0u : 2u];
        g.objv = f->win[e == 0u ? 1u : 3u];
        g.bgmask = (e == 0u) ? 0x7FFFFu : 0x1FFFFu;
        g.objmask = (e == 0u) ? 0x3FFFFu : 0x1FFFFu;
        g.uns = uns;
        for (line = 0u; line < GPU2D_LINES; line++) {
            g.r = gpu2d_line_regs(f, e, line);
            g.dispcnt = rd32(g.r, R_DISPCNT);
            if (e != 0u) {
                g.dispcnt &= 0xC0B1FFF7u;   /* engine B has no 3D bit, no tile-OBJ bits above its own, no capture-source bits */
            }
            render_line(&g, &s_line_buf, out, line);
        }
    }
}

void gpu2d_render(Gpu2dFrame *f, Gpu2dOut *out)
{
    uint32_t uns = 0u, n;

    assert(f != NULL && out != NULL);
    assert(f->lines == GPU2D_LINES);
    render_engines(f, out, 0u, GPU2D_ENGINES, &uns);
    out->unsupported_mask = uns;
    out->unsupported = 0u;
    for (n = 0u; n < 32u; n++) {
        out->unsupported += (uns >> n) & 1u;
    }
    f->unsupported += out->unsupported;
}

void gpu2d_render_a(const Gpu2dFrame *f, Gpu2dOut *out)
{
    uint32_t uns = 0u;

    assert(f != NULL && out != NULL);
    assert(f->lines == GPU2D_LINES);
    render_engines(f, out, 0u, 1u, &uns);   /* the features met were counted by the gpu2d_render of the same record */
}

/* ---- bet 2.8: the wide top screen ------------------------------------------------------------------------------------------------------------------------------ */

void gpu2d_compose_wide(const Gpu2dFrame *f, const Gpu2dOut *out, const uint32_t *layer_w, uint32_t *dst)
{
    static Line one;   /* one pixel of a line: index 0 only */
    uint32_t x, y, uns = 0u;
    Eng g;

    assert(f != NULL && out != NULL && dst != NULL);
    memset(&g, 0, sizeof g);
    g.f = f;
    g.e = 0u;
    g.pal = f->pltt[0];
    g.uns = &uns;
    for (y = 0u; y < GPU2D_WIDE_H; y++) {
        uint32_t line = y < GPU2D_WIDE_Y0 ? 0u : (y - GPU2D_WIDE_Y0 >= GPU2D_LINES ? GPU2D_LINES - 1u : y - GPU2D_WIDE_Y0);
        uint32_t dm, mb, mode, fac, bd, hofs, inside_y = y >= GPU2D_WIDE_Y0 && y < GPU2D_WIDE_Y0 + GPU2D_LINES;
        int32_t shift;
        uint32_t *row = dst + y * GPU2D_WIDE_W;

        if (inside_y) {
            memcpy(row + GPU2D_WIDE_X0, &out->pix[0][(y - GPU2D_WIDE_Y0) * GPU2D_W], GPU2D_W * sizeof *row);
        }
        g.r = gpu2d_line_regs(f, 0u, line);
        g.dispcnt = rd32(g.r, R_DISPCNT);
        dm = (g.dispcnt >> 16) & 3u;
        mb = rd16(g.r, R_MBRIGHT);
        mode = mb >> 14;
        fac = (mb & 0x1Fu) > 16u ? 16u : (mb & 0x1Fu);
        bd = c555(rd16(g.pal, 0u));
        hofs = rd16(g.r, R_BGOFS) & 0x1FFu;
        shift = (hofs < 256u) ? (int32_t)hofs : (int32_t)hofs - 512;
        for (x = 0u; x < GPU2D_WIDE_W; x++) {
            uint32_t c, lid;

            if (inside_y && x == GPU2D_WIDE_X0) {
                x += GPU2D_W - 1u;   /* the DS frame: engine A's own composite, copied above */
                continue;
            }
            if (dm == 0u) {   /* the same rules as render_line: display mode 0 is white without brightness, forced blank is white with it */
                row[x] = 0xFFFFFFFFu;
                continue;
            }
            if ((g.dispcnt & 0x80u) != 0u) {
                c = 0x3F3F3Fu;
            } else {
                int32_t j = (int32_t)x + shift;

                /* the border has no 2D layer (the DS 2D layers are 1:1 in the centre): the 3D layer (engine A BG0, when it is on) over the backdrop */
                one.col[0][0] = one.col[1][0] = bd;
                one.id[0][0] = one.id[1][0] = (uint8_t)GPU2D_LID_BACKDROP;
                if (layer_w != NULL && (g.dispcnt & 0x108u) == 0x108u && j >= 0 && j < (int32_t)GPU2D_WIDE_W) {
                    uint32_t w3 = layer_w[y * GPU2D_WIDE_W + (uint32_t)j];

                    if ((w3 >> 24) != 0u) {
                        push(&one, 0u, w3 & 0x3F3F3Fu, GPU2D_LID_3D);
                        one.a3[0] = (uint8_t)((w3 >> 24) & 0x1Fu);
                    }
                }
                c = effect(&g, &one, 0u, &lid);
            }
            if (mode == 1u || mode == 2u) {
                c = bright(c, mode, fac);
            }
            row[x] = out_word(c);
        }
    }
}
