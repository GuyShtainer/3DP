/* gpu3d_pica.c -- see gpu3d_pica.h and the private design notes. Written from GBATEK ("DS 3D Video", "DS 3D Textures") and the citro3d / libctru headers.
 *
 * Encoding of one DS vertex for the PICA (the CPU does the DS viewport transform and the DS depth quantisation, so the GPU only has to interpolate and test):
 *   position  x' = ndc_x * w', y' = ndc_y * w', z' = (depth01 - 1) * w', w' = w / 4096, where ndc is taken from the INTEGER DS screen position (so the covered pixels follow the
 *             DS snap) and depth01 is the DS 24-bit depth of the vertex / 2^24; with C3D_DepthMap(scale 1, offset 1) the PICA depth is then depth01 itself at the vertices and is
 *             interpolated linearly in screen space between them, as the DS does in Z-buffer mode.
 *   colour    textured: (v6 + 1) / 64 (v6 = the 6-bit expansion of the 5-bit lit colour), so a modulate with the texel fraction (t6 + 1) / 64 gives the DS ((t+1)(v+1)-1)>>6;
 *             untextured: (4 * v6 + 2) / 255, so the 8-bit result shifted right by two is v6.  alpha = (polygon alpha + 1) / 32.
 *   texcoord  (s / 16) / width, (t / 16) / height. */
#include "gpu3d_pica.h"

#include <3ds.h>
#include <citro3d.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gpu3d_edge.h"
#include "gpu3d_list.h"
#include "gpu3d_selfcheck.h"
#include "gpu3d_tex.h"
#include "hw_wide.h"

PicaUnsupported pica_unsupported;
PicaStats pica_stats;

#define VTX_FLOATS 10u                  /* x y z w, s t, r g b a */
#define POOL_SLOTS G3R_SLOTS
#define ORIENT_RAW_W 192u               /* the hardware framebuffer is the logical image turned: 192 wide, 256 high */
#define ORIENT_RAW_H 256u

/* bet 2.8: the geometry of the frame being drawn. Narrow = the DS 256 x 192 (raw 192 x 256); wide = 400 x 240 (raw 240 x 400), made at the first wide list. The narrow
   values are the constants the narrow path always used, so its arithmetic is unchanged (the clip-space x of a vertex is (px - 128) / 128 either way). */
typedef struct FrameDim {
    uint32_t w, h, raw_w, raw_h;
    float hx, hy;                       /* half width / height: the clip-space scale */
} FrameDim;
static const FrameDim k_narrow = { PICA_W, PICA_H, ORIENT_RAW_W, ORIENT_RAW_H, 128.0f, 96.0f };
static const FrameDim k_wide = { HWWIDE_W, HWWIDE_H, HWWIDE_H, HWWIDE_W, 200.0f, 120.0f };

typedef struct TexSlot {
    int used;
    uint64_t key;
    C3D_Tex tex;
    uint32_t wrap_s, wrap_t;            /* GPU3D_WRAP_* currently set on the sampler */
    uint32_t w, h;
} TexSlot;

static C3D_RenderTarget *g_target;
static DVLB_s *g_dvlb;
static shaderProgram_s g_prog;
static float *g_vbuf;
static uint32_t g_vcap;                 /* vertices the buffer holds */
static TexSlot g_slots[POOL_SLOTS];
static uint32_t g_frame;
static uint32_t *g_raw;                 /* linear readback buffer, ORIENT_RAW_W * ORIENT_RAW_H words (linearAlloc) */
static uint32_t *g_ds_raw;              /* the raw tiled D24S8 buffer as GX_TextureCopy leaves it (linearAlloc), read back only on edge-marking frames */
static uint32_t g_ds[G3E_PIXELS];       /* the same, de-tiled to the logical image (gpu3d_edge.h) */
static uint32_t g_carrier;              /* G3E_CARRIER_* chosen for the frame from DISP3DCNT */
static Gpu3dRec *g_sc_rec;              /* 2.7(b) N9: the self-check's record, allocated once in pica_init (golden rule 3: no allocation after init); NULL = the self-check cannot run */
static int g_mut_nomerge;               /* hwcheck mutant 2: pica_frame_end skips the split frame's final depth merge copy */
static int g_force_ds;                  /* self-tests: read depth/stencil back on every frame */
static int g_edge_frame;                /* the frame runs the edge pass */
static int g_split;                     /* this edge frame has been split after its opaque polygons (review B3): g_carrier is TRANS from then on */
static int g_split_depthwrite;          /* a translucent polygon wrote depth after the split: the end of the frame takes a second depth copy */
static uint64_t g_split_span;           /* ticks the split took inside pica_draw (taken out of cpu_ticks) */
static G3eParams g_edge;                /* the rear-plane ID / depth and the EDGE_COLOR table of the frame */
static G3ePass g_pass_last;
static int g_pass_valid, g_pass_depthwrite;
static uint32_t g_layer[PICA_W * PICA_H];
static float g_bx, g_by;
static float g_phx = 0.5f, g_phy = 0.5f;     /* attribute phase: the DS interpolates at the integer pixel position, the PICA at the pixel centre */

/* bet 2.8: the wide frame's target and buffers (NULL until the first wide list), and the frame being drawn. The two eyes of a stereo frame are drawn one after the
   other on the SAME target with the SAME readback buffers (each eye is cleared, drawn, read back, de-tiled and edge-marked before the next one starts, and the GPU
   work is waited for inside each readback), so only the finished layer words need one buffer per eye. */
static C3D_RenderTarget *g_target_w;
static uint32_t *g_raw_w, *g_ds_raw_w;  /* linearAlloc, 240 x 400 words each */
static uint32_t *g_ds_w;                /* the de-tiled depth/stencil, 400 x 240 (malloc) */
static uint32_t *g_layer_w[2];          /* the wide layer words per eye (slot 0: mono / left, 1: right), 400 x 240 (malloc) */
static int g_wide_failed;               /* the wide buffers could not be made: wide lists are refused */
static const FrameDim *g_dim = &k_narrow;
static C3D_RenderTarget *g_cur_target;
static uint32_t *g_cur_raw, *g_cur_ds_raw, *g_cur_ds, *g_cur_layer;
static int g_cur_wide;                  /* the frame is a wide list */
static float g_e_sigma;                 /* the eye's shear (hw_wide.h): 0 mono, -sigma left, +sigma right */

/* state tracking so that a run of same-state polygons costs no extra commands (g_env: 0 none, 1 untextured, 2 textured) */
static int g_env;
static int g_tr_ids_valid;
static uint64_t g_tr_ids;

void pica_stats_reset(void)
{
    memset(&pica_unsupported, 0, sizeof pica_unsupported);
    memset(&pica_stats, 0, sizeof pica_stats);
}

void pica_set_bias(float bx, float by)
{
    g_bx = bx;
    g_by = by;
}

void pica_set_phase(float px, float py)
{
    g_phx = px;
    g_phy = py;
}

/* the full-screen quad of the frame split, at vertex index g_vcap: clip-space corners, w 1, z in the middle of the PICA range (depth test ALWAYS, so the value does not matter) */
static void split_quad_init(void)
{
    static const float cx[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, cy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    float *o = g_vbuf + (size_t)g_vcap * VTX_FLOATS;
    uint32_t i;
    for (i = 0; i < 4; i++, o += VTX_FLOATS) {
        o[0] = cy[i];           /* orient(): hardware x = logical y, hardware y = -logical x */
        o[1] = -cx[i];
        o[2] = -0.5f;
        o[3] = 1.0f;
        o[4] = o[5] = 0.0f;
        o[6] = o[7] = o[8] = 0.0f;
        o[9] = 1.0f;
    }
    GSPGPU_FlushDataCache(g_vbuf + (size_t)g_vcap * VTX_FLOATS, (u32)(4u * VTX_FLOATS * sizeof(float)));
}

int pica_init(const void *shbin, uint32_t size)
{
    if (shbin == NULL || size == 0u) return 5;
    C3D_AttrInfo *attr;
    C3D_BufInfo *buf;
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return 1;
    g_target = C3D_RenderTargetCreate((int)ORIENT_RAW_W, (int)ORIENT_RAW_H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_target) return 2;
    g_dvlb = DVLB_ParseFile((u32 *)shbin, size);
    if (!g_dvlb) return 3;
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_prog);
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 4);
    g_vcap = GPU3D_MAX_VERTS;
    g_vbuf = (float *)linearAlloc((size_t)(g_vcap + 4u) * VTX_FLOATS * sizeof(float));      /* + the 4 vertices of the stencil-rewrite quad (split_frame) */
    g_raw = (uint32_t *)linearAlloc((size_t)ORIENT_RAW_W * ORIENT_RAW_H * 4u);
    g_ds_raw = (uint32_t *)linearAlloc((size_t)ORIENT_RAW_W * ORIENT_RAW_H * 4u);
    if (!g_vbuf || !g_raw || !g_ds_raw) return 4;
    g_cur_target = g_target;
    g_cur_raw = g_raw;
    g_cur_ds_raw = g_ds_raw;
    g_cur_ds = g_ds;
    g_cur_layer = g_layer;
    split_quad_init();
    g_sc_rec = (Gpu3dRec *)calloc(1u, sizeof *g_sc_rec);
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vbuf, (ptrdiff_t)(VTX_FLOATS * sizeof(float)), 3, 0x210);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(true, 1.0f, 1.0f);
    C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
    pica_stats_reset();
    return 0;
}

void pica_exit(void)
{
    uint32_t i;
    for (i = 0; i < POOL_SLOTS; i++)
        if (g_slots[i].used) { C3D_TexDelete(&g_slots[i].tex); g_slots[i].used = 0; }
    if (g_vbuf) linearFree(g_vbuf);
    if (g_raw) linearFree(g_raw);
    if (g_ds_raw) linearFree(g_ds_raw);
    free(g_sc_rec);
    g_sc_rec = NULL;
    g_vbuf = NULL;
    g_raw = NULL;
    g_ds_raw = NULL;
    if (g_raw_w) linearFree(g_raw_w);
    if (g_ds_raw_w) linearFree(g_ds_raw_w);
    free(g_ds_w);
    free(g_layer_w[0]);
    free(g_layer_w[1]);
    g_raw_w = g_ds_raw_w = g_ds_w = g_layer_w[0] = g_layer_w[1] = NULL;
    if (g_target_w) C3D_RenderTargetDelete(g_target_w);
    g_target_w = NULL;
    if (g_target) C3D_RenderTargetDelete(g_target);
    g_target = NULL;
    if (g_dvlb) { shaderProgramFree(&g_prog); DVLB_Free(g_dvlb); }
    g_dvlb = NULL;
    C3D_Fini();
}

static uint32_t a8_to_a5(uint32_t a8);
static uint32_t c5to8_clear(uint32_t c5) { return gpu3d_c5_to_c6(c5) * 4u + 2u; }

static void pica_frame_begin(const Gpu3dList *l)
{
    uint32_t cc = l->clear_color, a5 = (cc >> 16) & 31u, i;
    uint32_t rgba = (c5to8_clear(cc & 31u) << 24) | (c5to8_clear((cc >> 5) & 31u) << 16) | (c5to8_clear((cc >> 10) & 31u) << 8) | gpu3d_a5_to_a8(a5);
    uint32_t d24, stencil;
    g_frame++;
    g_env = 0;
    g_pass_valid = 0;
    g_tr_ids_valid = 0;
    g_tr_ids = 0;
    g_split = 0;
    g_split_depthwrite = 0;
    g_split_span = 0;
    /* the stencil carrier of the frame and the edge pass inputs (gpu3d_edge.h): edge marking on = polygon IDs in the stencil, else the translucent-ID layout */
    g_carrier = g3e_carrier(l->disp3dcnt);
    g_edge_frame = g_carrier == G3E_CARRIER_EDGE && g_cur_ds_raw != NULL;
    g_edge.clear_id = (cc >> 24) & G3E_EDGE_ID_MASK;
    g_edge.clear_z = g3e_clear_depth_ds(l->clear_depth);
    for (i = 0; i < 8; i++) g_edge.edge_color[i] = l->edge_color[i];
    d24 = g3e_clear_code(l->clear_depth);
    stencil = g3e_clear_stencil(g_carrier, cc);
    pica_stats.clear_word = ((rgba >> 24) >> 2) | ((((rgba >> 16) & 255u) >> 2) << 8) | ((((rgba >> 8) & 255u) >> 2) << 16) | (a8_to_a5(rgba & 255u) << 24);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(g_cur_target, C3D_CLEAR_ALL, rgba, d24 | (stencil << 24));
    C3D_FrameDrawOn(g_cur_target);
    if ((l->disp3dcnt & GPU3D_DISP_EDGE) && !g_edge_frame) pica_unsupported.edge_frames++;
    if (l->disp3dcnt & GPU3D_DISP_AA) pica_unsupported.aa_frames++;
    if (l->disp3dcnt & GPU3D_DISP_FOG) pica_unsupported.fog_frames++;
    if (l->disp3dcnt & GPU3D_DISP_ALPHATEST) pica_unsupported.alphatest_frames++;
    if (l->disp3dcnt & GPU3D_DISP_BITMAPCLR) pica_unsupported.bitmap_clear_frames++;
    if (l->swap_bits & 2u) pica_unsupported.wbuffer_frames++;
}

/* ---- texture cache ---- */

static GPU_TEXTURE_WRAP_PARAM wrap_gpu(uint32_t m)
{
    return m == GPU3D_WRAP_REPEAT ? GPU_REPEAT : (m == GPU3D_WRAP_MIRROR ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE);
}

/* The record's new textures go into the slots the game thread chose (its mirror of this cache); a slot the record does not name keeps what it holds. */
static void upload_textures(const Gpu3dRec *r)
{
    uint32_t i;
    uint64_t t0 = svcGetSystemTick();
    for (i = 0; i < r->ntex; i++) {
        const G3rTex *t = &r->tex[i];
        TexSlot *s;
        if (!t->upload || t->slot >= POOL_SLOTS) continue;
        s = &g_slots[t->slot];
        if (s->used) {
            C3D_TexDelete(&s->tex);
            s->used = 0;
            pica_stats.tex_evictions++;
        }
        if (!C3D_TexInit(&s->tex, t->w, t->h, GPU_RGBA8)) { pica_unsupported.hw_mode_clamped++; pica_stats.tex_failed++; continue; }
        memcpy(s->tex.data, r->texels + t->texel_off, (size_t)t->w * t->h * 4u);
        C3D_TexFlush(&s->tex);
        C3D_TexSetFilter(&s->tex, GPU_NEAREST, GPU_NEAREST);
        s->used = 1;
        s->key = t->key;
        s->w = t->w;
        s->h = t->h;
        s->wrap_s = s->wrap_t = 0xFFu;
        pica_stats.tex_uploads++;
    }
    pica_stats.upload_ticks += svcGetSystemTick() - t0;
}

/* ---- state ---- */

static void set_env(int textured)
{
    C3D_TexEnv *env;
    if (g_env == (textured ? 2 : 1)) return;
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
    g_env = textured ? 2 : 1;
}

/* One stencil pass of gpu3d_edge.h as citro3d state. The relation names are the stencil's: G3E_REL_STENCIL_LT passes when (stencil & mask) < (ref & mask); citro3d's function is
   applied as (ref & mask) FUNC (stencil & mask) (measured by the replay app's carrier self-test), so "stencil less than ref" is GPU_GREATER. */
static GPU_TESTFUNC rel_gpu(uint32_t rel)
{
    switch (rel) {
    case G3E_REL_EQ: return GPU_EQUAL;
    case G3E_REL_NE: return GPU_NOTEQUAL;
    case G3E_REL_STENCIL_LT: return GPU_GREATER;
    case G3E_REL_STENCIL_GT: return GPU_LESS;
    default: return GPU_ALWAYS;
    }
}

static void apply_pass(const G3ePass *p, int depthwrite)
{
    if (g_pass_valid && g_pass_depthwrite == depthwrite && memcmp(&g_pass_last, p, sizeof *p) == 0) return;
    C3D_DepthTest(true, GPU_LESS, depthwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    C3D_StencilTest(true, rel_gpu(p->rel), p->ref, p->mask, p->replace ? p->wmask : 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, p->replace ? GPU_STENCIL_REPLACE : GPU_STENCIL_KEEP);
    if (p->blend) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_MAX, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE);   /* over a covered pixel: result alpha = max */
    else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    g_pass_last = *p;
    g_pass_depthwrite = depthwrite;
    g_pass_valid = 1;
}

/* logical clip (x right, y up) to the hardware framebuffer's clip axes: calibrated by the replay app's self-test */
static void orient(float cx, float cy, float *hx, float *hy)
{
    *hx = cy;
    *hy = -cx;
}

/* ---- the polygon: counters, texture, vertices, draw ---- */

typedef struct DrawCtx {
    const Gpu3dList *l;
    const Gpu3dRec *rec;
    uint32_t voff;                      /* vertices written so far */
    int w_mode, textures_on;
} DrawCtx;

/* one polygon's per-vertex values: pass 1 (position, depth, attributes) and pass 2 (the attribute-phase shifts) */
typedef struct PolyGeo {
    float px[GPU3D_MAX_PV], py[GPU3D_MAX_PV], iw[GPU3D_MAX_PV], dd[GPU3D_MAX_PV];
    float at[GPU3D_MAX_PV][6], sh[GPU3D_MAX_PV][6], shd[GPU3D_MAX_PV];
} PolyGeo;

static void count_modes(const Gpu3dPoly *p)
{
    uint32_t mode;
    assert(p != NULL);
    mode = (p->attr >> 4) & 3u;
    if (mode == 1u) pica_unsupported.mode_decal++;
    else if (mode == 2u) pica_unsupported.mode_toon++;
    else if (mode == 3u) pica_unsupported.mode_shadow++;
    if (p->attr & (1u << 14)) pica_unsupported.depth_equal++;
}

/* a translucent polygon: remember its ID for the frame; returns 1 when an earlier translucent polygon of the same ID was drawn (the same-ID rule has work to do) */
static int note_translucent(const Gpu3dPoly *p)
{
    uint32_t id;
    int seen;
    assert(p != NULL);
    assert(g_carrier == G3E_CARRIER_EDGE || g_carrier == G3E_CARRIER_TRANS);
    id = (p->attr >> 24) & 63u;
    seen = (g_tr_ids & (1ULL << id)) != 0u;
    if (seen) {
        if (g_carrier == G3E_CARRIER_TRANS) pica_stats.same_id_polys++;      /* the rule is applied (stencil carrier) */
        else pica_unsupported.translucent_same_id++;                          /* EDGE layout: no room for the translucent ID, not applied (not reachable on an edge frame: it is split first) */
    }
    g_tr_ids |= 1ULL << id;
    return seen;
}

/* the cache slot of polygon `pi`'s texture, or -1 (untextured: texturing off, format 0, compressed, or no resident slot). The record's table index is range-checked (review N8). */
static int lookup_texture(const DrawCtx *c, uint32_t pi, uint32_t texparam)
{
    uint32_t fmt = gpu3d_tex_format(texparam), ti;
    assert(c != NULL && c->rec != NULL);
    assert(c->rec->ntex <= G3R_TEX_MAX);
    if (!c->textures_on || fmt == 0u) return -1;
    if (fmt == 5u) { pica_unsupported.tex_compressed++; return -1; }
    ti = pi < c->rec->npolys ? c->rec->poly_tex[pi] : G3R_NO_TEX;
    pica_stats.tex_lookups++;
    if (ti != G3R_NO_TEX && ti >= c->rec->ntex) { pica_unsupported.bad_tex_index++; return -1; }
    if (ti != G3R_NO_TEX && c->rec->tex[ti].slot < POOL_SLOTS && g_slots[c->rec->tex[ti].slot].used) return (int)c->rec->tex[ti].slot;
    pica_unsupported.tex_cache_full++;
    return -1;
}

/* pass 1: the DS screen position, depth and attribute values of every vertex */
static void geo_setup(const Gpu3dList *l, const Gpu3dVert *v, uint32_t n, int w_mode, uint32_t drop, uint32_t wtag, PolyGeo *g)
{
    uint32_t i, k;
    assert(l != NULL && v != NULL && g != NULL);
    assert(n >= 3u && n <= GPU3D_MAX_PV);
    for (i = 0; i < n; i++) {
        int32_t sx, sy;
        uint32_t d24;
        float fx = 0.0f;
        int have_fx = 0;
        if (!g_cur_wide) gpu3d_viewport_xy(l->viewport, v[i].x, v[i].y, v[i].w, &sx, &sy);
        else if (wtag & GPU3D_LT_WIDE) {   /* bet 2.8: the wide map; a perspective polygon is sheared per eye (zero parallax at the camera distance; ortho stays at zero) */
            int sheared = (wtag & GPU3D_LT_PERSP) && g_e_sigma != 0.0f;
            int32_t x = sheared ? hwwide_shear(v[i].x, v[i].w, l->st_dist, g_e_sigma) : v[i].x;
            if (sheared) {
                pica_stats.wide_shear_verts++;
                if (!hwwide_shear_in_guard(v[i].w, l->st_dist, g_e_sigma, HWWIDE_GUARD_Q12)) pica_stats.wide_shear_viol++;
            }
            gpu3d_viewport_wide_xy(x, v[i].y, v[i].w, &sx, &sy);
            if (sheared) {   /* the eye's x at sub-pixel precision (no snap: the snap cracks the tilted seams, hw_wide.h hwwide_shear_sx); y keeps the DS snap */
                fx = (float)hwwide_shear_sx(v[i].x, v[i].w, l->st_dist, g_e_sigma);
                have_fx = 1;
            }
        } else {                           /* a projection the hook never saw (the 2D-in-3D sprite camera): the DS mapping, centred, at zero parallax */
            gpu3d_viewport_xy(l->viewport, v[i].x, v[i].y, v[i].w, &sx, &sy);
            sx += HWWIDE_X0;
            sy += HWWIDE_Y0;
        }
        g->px[i] = have_fx ? fx : (float)sx;
        g->py[i] = (float)sy;
        g->iw[i] = 4096.0f / (float)v[i].w;
        d24 = w_mode ? gpu3d_depth24_w(v[i].w, drop) : gpu3d_depth24_z(v[i].z, v[i].w);
        g->dd[i] = (float)d24 / 16777216.0f;
        g->at[i][0] = (float)v[i].s / 16.0f;
        g->at[i][1] = (float)v[i].t / 16.0f;
        g->at[i][2] = (float)gpu3d_c5_to_c6(v[i].r);
        g->at[i][3] = (float)gpu3d_c5_to_c6(v[i].g);
        g->at[i][4] = (float)gpu3d_c5_to_c6(v[i].b);
        g->at[i][5] = 0.0f;
        for (k = 0; k < 6; k++) g->sh[i][k] = 0.0f;
        g->shd[i] = 0.0f;
    }
}

/* the largest-area triangle of the fan: the plane the attribute gradients are taken from; -1 when the polygon has no area */
static int geo_gradient_tri(const PolyGeo *g, uint32_t n, int *ia, int *ib, int *ic)
{
    uint32_t j, k;
    float best = 0.0f;
    assert(g != NULL && ia != NULL && ib != NULL && ic != NULL);
    assert(n >= 3u && n <= GPU3D_MAX_PV);
    *ia = 0; *ib = 1; *ic = 2;
    for (j = 0; j + 2u < n; j++)
        for (k = j + 1u; k + 1u < n; k++) {
            float a2 = (g->px[k] - g->px[j]) * (g->py[k + 1] - g->py[j]) - (g->px[k + 1] - g->px[j]) * (g->py[k] - g->py[j]);
            if (a2 < 0.0f) a2 = -a2;
            if (a2 > best) { best = a2; *ia = (int)j; *ib = (int)k; *ic = (int)(k + 1u); }
        }
    return best > 0.0f ? 0 : -1;
}

/* pass 2: the DS interpolates at the integer pixel position, the PICA at the pixel centre: move every attribute back by the phase along its own screen gradient
 * (the polygon is planar, so one gradient triple serves every vertex; perspective-correct attributes are linear as attr / w) */
static void geo_phase(PolyGeo *g, uint32_t n)
{
    int ia, ib, ic;
    uint32_t i, k;
    float bax, bay, cax, cay, inv, gx_iw, gy_iw, gx_d, gy_d;
    assert(g != NULL);
    assert(n >= 3u && n <= GPU3D_MAX_PV);
    if (geo_gradient_tri(g, n, &ia, &ib, &ic) != 0 || (g_phx == 0.0f && g_phy == 0.0f)) return;
    bax = g->px[ib] - g->px[ia]; bay = g->py[ib] - g->py[ia]; cax = g->px[ic] - g->px[ia]; cay = g->py[ic] - g->py[ia];
    inv = 1.0f / (bax * cay - cax * bay);
    gx_iw = ((g->iw[ib] - g->iw[ia]) * cay - (g->iw[ic] - g->iw[ia]) * bay) * inv;
    gy_iw = ((g->iw[ic] - g->iw[ia]) * bax - (g->iw[ib] - g->iw[ia]) * cax) * inv;
    gx_d = ((g->dd[ib] - g->dd[ia]) * cay - (g->dd[ic] - g->dd[ia]) * bay) * inv;
    gy_d = ((g->dd[ic] - g->dd[ia]) * bax - (g->dd[ib] - g->dd[ia]) * cax) * inv;
    for (k = 0; k < 5; k++) {
        float qa = g->at[ia][k] * g->iw[ia], qb = g->at[ib][k] * g->iw[ib], qc = g->at[ic][k] * g->iw[ic];
        float gx_q = ((qb - qa) * cay - (qc - qa) * bay) * inv, gy_q = ((qc - qa) * bax - (qb - qa) * cax) * inv;
        for (i = 0; i < n; i++) {
            /* d(attr)/dX at the vertex = (d(attr/w)/dX - attr * d(1/w)/dX) / (1/w) */
            float gx = (gx_q - g->at[i][k] * gx_iw) / g->iw[i], gy = (gy_q - g->at[i][k] * gy_iw) / g->iw[i];
            g->sh[i][k] = -(g_phx * gx + g_phy * gy);
        }
    }
    for (i = 0; i < n; i++) g->shd[i] = -(g_phx * gx_d + g_phy * gy_d);
}

static float clamp63(float c) { return c < 0.0f ? 0.0f : (c > 63.0f ? 63.0f : c); }

/* the vertex records (VTX_FLOATS floats each) of one polygon into the vertex buffer at `o` */
static void emit_vertices(float *o, const Gpu3dVert *v, const PolyGeo *g, uint32_t n, int textured, float tw, float th, float alpha)
{
    uint32_t i;
    assert(o != NULL && v != NULL && g != NULL);
    assert(tw >= 1.0f && th >= 1.0f);
    for (i = 0; i < n; i++, o += VTX_FLOATS) {
        float nx = (g->px[i] + g_bx - g_dim->hx) / g_dim->hx, ny = (g_dim->hy - (g->py[i] + g_by)) / g_dim->hy, wf = (float)v[i].w / 4096.0f;
        float d01 = g->dd[i] + g->shd[i], hx, hy, r6, g6, b6;
        if (d01 < 1.0f / 16777216.0f) d01 = 1.0f / 16777216.0f;
        if (d01 > 1.0f) d01 = 1.0f;
        orient(nx * wf, ny * wf, &hx, &hy);
        o[0] = hx;
        o[1] = hy;
        o[2] = (d01 - 1.0f) * wf;
        o[3] = wf;
        o[4] = (g->at[i][0] + g->sh[i][0]) / tw;
        o[5] = (g->at[i][1] + g->sh[i][1]) / th;
        r6 = clamp63(g->at[i][2] + g->sh[i][2]);
        g6 = clamp63(g->at[i][3] + g->sh[i][3]);
        b6 = clamp63(g->at[i][4] + g->sh[i][4]);
        if (textured) { o[6] = (r6 + 1.0f) / 64.0f; o[7] = (g6 + 1.0f) / 64.0f; o[8] = (b6 + 1.0f) / 64.0f; }
        else { o[6] = (r6 * 4.0f + 2.0f) / 255.0f; o[7] = (g6 * 4.0f + 2.0f) / 255.0f; o[8] = (b6 * 4.0f + 2.0f) / 255.0f; }
        o[9] = alpha;
    }
}

static void bind_texture(int slot, uint32_t texparam)
{
    TexSlot *s;
    uint32_t ws_, wt_;
    assert(slot >= 0 && (uint32_t)slot < POOL_SLOTS);
    s = &g_slots[slot];
    assert(s->used);
    ws_ = gpu3d_tex_wrap_s(texparam);
    wt_ = gpu3d_tex_wrap_t(texparam);
    if (s->wrap_s != ws_ || s->wrap_t != wt_) {
        C3D_TexSetWrap(&s->tex, wrap_gpu(ws_), wrap_gpu(wt_));
        s->wrap_s = ws_;
        s->wrap_t = wt_;
    }
    C3D_TexBind(0, &s->tex);
}

/* the stencil passes and draw calls of one polygon (its vertices are at c->voff) */
static void submit_poly(const DrawCtx *c, const Gpu3dPoly *p, int tr, int seen)
{
    assert(c != NULL && p != NULL);
    assert(c->voff + p->nverts <= g_vcap);
    if (!tr) {
        G3ePass op;
        g3e_opaque_pass(g_carrier, p->attr, &op);
        apply_pass(&op, 1);
        C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)c->voff, (int)p->nverts);
        pica_stats.draw_calls++;
    } else {
        G3ePass tp[3];
        int depthwrite = (int)((p->attr >> 11) & 1u);
        uint32_t np = g3e_trans_passes(g_carrier, p->attr, seen, tp), k;
        if (depthwrite) g_split_depthwrite = 1;
        for (k = 0; k < np; k++) {
            apply_pass(&tp[k], depthwrite);
            C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)c->voff, (int)p->nverts);
        }
        pica_stats.draw_calls += np;
    }
    pica_stats.polygons_drawn++;
}

/* ---- the frame split of an edge-marking frame that has translucent polygons (review B3) ---- */

static void read_ds(int merge);

/* Called before the first translucent polygon of an EDGE-layout frame is drawn (the list order is final: opaque first). The opaque polygons are on the target in the EDGE layout:
 *   1. finish them (C3D_FrameEnd) and copy the D24S8 buffer: the opaque IDs and depth the edge pass reads;
 *   2. begin again on the same target WITHOUT a clear, and draw one full-screen quad with the depth test ALWAYS, colour and depth writes off, stencil ALWAYS with reference 0 and write
 *      mask 0x7F: bit 7 stays, the ID bits go, which is exactly the TRANS layout (covered 0x80, empty 0x00);
 *   3. the translucent polygons then use the TRANS passes, which apply the same-ID rule. */
static void split_frame(const DrawCtx *c)
{
    uint64_t ta, tb, tc, td;
    G3ePass rw;
    assert(c != NULL && g_edge_frame && !g_split);
    assert(g_carrier == G3E_CARRIER_EDGE && g_vbuf != NULL);
    ta = svcGetSystemTick();
    GSPGPU_FlushDataCache(g_vbuf, (u32)((size_t)c->voff * VTX_FLOATS * sizeof(float)));
    C3D_FrameEnd(0);
    tb = svcGetSystemTick();
    read_ds(0);
    tc = svcGetSystemTick();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameDrawOn(g_cur_target);
    set_env(0);
    g3e_rewrite_pass(&rw);
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
    C3D_StencilTest(true, rel_gpu(rw.rel), rw.ref, rw.mask, rw.wmask);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)g_vcap, 4);      /* the reserved quad at the end of the vertex buffer (pica_init) */
    g_pass_valid = 0;                                      /* apply_pass re-emits depth, stencil, blend and alpha test for the first translucent polygon */
    g_carrier = G3E_CARRIER_TRANS;
    g_split = 1;
    td = svcGetSystemTick();
    pica_stats.split_frames++;
    pica_stats.split_ticks += (tb - ta) + (td - tc);       /* the frame end and the second begin + quad; the copy itself is in ds_ticks / detile_ticks */
    pica_stats.readback_ticks += tc - tb;                  /* ... and the copy + de-tile belong to the readback bucket, as they did at the end of the frame */
    g_split_span += td - ta;
}

/* draw polygon `pi` (a skipped polygon is counted in pica_unsupported) */
static void draw_poly(DrawCtx *c, uint32_t pi)
{
    Gpu3dPoly p;
    Gpu3dVert v[GPU3D_MAX_PV];
    PolyGeo g;
    int32_t ws[GPU3D_MAX_PV];
    uint32_t drop = 0, i, tr, pa;
    int slot, seen = 0;
    assert(c != NULL && c->l != NULL && pi < c->l->npolys);
    gpu3d_list_poly(c->l, pi, &p);
    if (p.flags & GPU3D_PF_DEGENERATE) { pica_unsupported.polys_skipped++; return; }
    pa = (p.attr >> 16) & 31u;
    if (pa == 0u) { pica_unsupported.wireframe++; return; }
    if (p.nverts < 3u || p.nverts > GPU3D_MAX_PV || c->voff + p.nverts > g_vcap) { pica_unsupported.hw_mode_clamped++; return; }
    count_modes(&p);
    tr = gpu3d_is_translucent(p.attr, p.texparam);
    if (tr) {
        if (g_edge_frame && !g_split) split_frame(c);
        seen = note_translucent(&p);
    } else if (g_split) pica_stats.opaque_after_split++;
    slot = lookup_texture(c, pi, p.texparam);
    for (i = 0; i < p.nverts; i++) {
        gpu3d_list_vert(c->l, p.first + i, &v[i]);
        ws[i] = v[i].w;
    }
    if (c->w_mode) drop = gpu3d_w_drop(ws, p.nverts);
    geo_setup(c->l, v, p.nverts, c->w_mode, drop, p.wtag, &g);
    geo_phase(&g, p.nverts);
    emit_vertices(g_vbuf + (size_t)c->voff * VTX_FLOATS, v, &g, p.nverts, slot >= 0, slot >= 0 ? (float)g_slots[slot].w : 1.0f, slot >= 0 ? (float)g_slots[slot].h : 1.0f,
                  (float)(pa + 1u) / 32.0f);
    set_env(slot >= 0);
    if (slot >= 0) bind_texture(slot, p.texparam);
    submit_poly(c, &p, (int)tr, seen);
    c->voff += p.nverts;
}

static void pica_draw(const Gpu3dList *l, const Gpu3dRec *rec)
{
    uint64_t t0 = svcGetSystemTick();
    DrawCtx c;
    uint32_t pi;
    assert(l != NULL && rec != NULL);
    assert(g_vbuf != NULL && (g_vcap == GPU3D_MAX_VERTS || g_vcap == GPU3D_LIST_WIDE_VERTS));
    c.l = l;
    c.rec = rec;
    c.voff = 0;
    c.w_mode = (l->swap_bits & 2u) != 0;
    c.textures_on = (l->disp3dcnt & GPU3D_DISP_TEXTURE) != 0;
    for (pi = 0; pi < l->npolys; pi++) draw_poly(&c, pi);
    GSPGPU_FlushDataCache(g_vbuf, (u32)((size_t)c.voff * VTX_FLOATS * sizeof(float)));
    pica_stats.cpu_ticks += (svcGetSystemTick() - t0) - g_split_span;   /* the split (frame end, copy, rewrite) is its own bucket */
}

/* ---- readback ---- */

static uint32_t a8_to_a5(uint32_t a8)
{
    uint32_t v;
    if (a8 == 0u) return 0u;
    v = (a8 * 32u + 127u) / 255u;
    v = v ? v - 1u : 0u;
    return v > 31u ? 31u : v;
}

/* Depth/stencil: GX_TextureCopy of the raw tiled D24S8 buffer (no display-transfer format exists for it), de-tiled on the CPU into g_ds. merge = 0: the whole words (the only copy of an
 * unsplit frame, or the first copy of a split one: the opaque IDs and depth); merge = 1: the second copy of a split frame whose translucent polygons wrote depth, only the depth is taken.
 * A merge copy's ticks go to the split bucket (and read_ds_ticks, so the readback bucket does not count them twice). */
static uint64_t read_ds_ticks;
static void read_ds(int merge)
{
    uint64_t d0, d1, d2;
    uint32_t bytes = g_dim->raw_w * g_dim->raw_h * 4u;
    assert(g_cur_ds_raw != NULL);
    assert(merge == 0 || g_split);
    d0 = svcGetSystemTick();
    GSPGPU_InvalidateDataCache(g_cur_ds_raw, bytes);
    C3D_SyncTextureCopy((u32 *)g_cur_target->frameBuf.depthBuf, 0, (u32 *)g_cur_ds_raw, 0, bytes, 8);
    GSPGPU_InvalidateDataCache(g_cur_ds_raw, bytes);
    d1 = svcGetSystemTick();
    if (g_cur_wide) g3e_detile_ds_wh(g_cur_ds_raw, g_cur_ds, g_dim->w, g_dim->h, merge);
    else if (merge) g3e_detile_depth_merge(g_ds_raw, g_ds);
    else g3e_detile_ds(g_ds_raw, g_ds);
    d2 = svcGetSystemTick();
    if (merge) {
        pica_stats.split_ticks += d2 - d0;
        pica_stats.split_depth_copies++;
        read_ds_ticks += d2 - d0;
    } else {
        pica_stats.ds_ticks += d1 - d0;
        pica_stats.detile_ticks += d2 - d1;
    }
}

/* the colour half of the readback: display transfer into the linear buffer, then the conversion to layer words; returns the tick after the transfer */
static uint64_t read_colour(void)
{
    uint32_t x, y;
    uint64_t t2;
    const uint32_t rw = g_dim->raw_w, rh = g_dim->raw_h, w = g_dim->w, h = g_dim->h;
    uint32_t *raw = g_cur_raw, *layer = g_cur_layer;
    assert(raw != NULL && layer != NULL && g_cur_target != NULL);
    GSPGPU_InvalidateDataCache(raw, rw * rh * 4u);
    C3D_SyncDisplayTransfer((u32 *)g_cur_target->frameBuf.colorBuf, GX_BUFFER_DIM(rw, rh), raw, GX_BUFFER_DIM(rw, rh),
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_TILED(0));
    GSPGPU_InvalidateDataCache(raw, rw * rh * 4u);
    t2 = svcGetSystemTick();
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = raw[x * rw + (rw - 1u - y)];     /* calibrated: see the private design notes (the wide target is the same turn, 240 x 400) */
            uint32_t r = c >> 24, g = (c >> 16) & 255u, b = (c >> 8) & 255u, a = c & 255u;
            layer[y * w + x] = (r >> 2) | ((g >> 2) << 8) | ((b >> 2) << 16) | (a8_to_a5(a) << 24);
        }
    }
    return t2;
}

static void pica_frame_end(void)
{
    uint64_t t0 = svcGetSystemTick(), t1, t2, t2b, t3;
    assert(g_cur_target != NULL);
    C3D_FrameEnd(0);
    t1 = svcGetSystemTick();
    read_ds_ticks = 0;
    t2 = read_colour();
    t2b = svcGetSystemTick();
    if (g_edge_frame || g_force_ds) {
        uint64_t e0;
        if (!g_split) read_ds(0);                       /* an unsplit frame: its one copy (a split frame took its first at the split) */
        else if (g_split_depthwrite && !g_mut_nomerge) read_ds(1);        /* translucent polygons wrote depth: the edge pass takes the final depth */
        e0 = svcGetSystemTick();
        if (g_edge_frame) {
            pica_stats.edge_marked += g_cur_wide ? g3e_edge_pass_wh(g_cur_layer, g_cur_ds, &g_edge, g_dim->w, g_dim->h) : g3e_edge_pass(g_layer, g_ds, &g_edge);
            pica_stats.edge_frames_run++;
        }
        pica_stats.edge_ticks += svcGetSystemTick() - e0;
    }
    t3 = svcGetSystemTick();
    pica_stats.gpu_ticks += t3 - t0;
    pica_stats.readback_ticks += (t3 - t1) - read_ds_ticks;      /* a merge copy is the split bucket's */
    pica_stats.transfer_ticks += t2 - t1;
    pica_stats.convert_ticks += t2b - t2;
    pica_stats.frames++;
}

/* Bet 2.7a: see gpu3d_pica.h. The synthetic list goes through the same pica_render_record as a game frame; only the judging is new. Check frame n -> out->code[n]. */
static int selfcheck_frame(Gpu3dRec *rec, uint8_t *list, uint32_t cap, uint32_t n, int mutant, PicaSelfcheck *out)
{
    G3sVerdict v;
    size_t len = g3s_build(list, cap, n);

    rec->list_len = (uint32_t)len;
    if (len == 0u || pica_render_record(rec) != 0) return -1;
    out->code[n] = g3s_judge(mutant == 1 ? g_ds_raw : g_ds, g_layer, n, &v);
    if (n < G3S_FRAMES) {
        out->frames_run++;
        if (out->code[n] != G3S_OK && out->bad_what == NULL) {
            out->bad_frame = n;
            out->bad_x = v.x; out->bad_y = v.y; out->bad_got = v.got; out->bad_want = v.want;
            out->bad_probe = v.probe;
            out->bad_what = v.what;
        }
    } else {
        out->trans_run = 1u;
        out->t_x = v.x; out->t_y = v.y; out->t_got = v.got; out->t_want = v.want;
        out->t_probe = v.probe;
        out->t_what = v.what;
    }
    return 0;
}

int pica_selfcheck(int mutant, PicaSelfcheck *out)
{
    static uint8_t list[G3S_LIST_BYTES];
    Gpu3dRec *rec = g_sc_rec;
    PicaStats sv_stats = pica_stats;
    PicaUnsupported sv_uns = pica_unsupported;
    int sv_force = g_force_ds, rc = 0;
    uint32_t n, i;

    if (out == NULL || g_target == NULL || g_ds_raw == NULL || rec == NULL) return -1;
    memset(out, 0, sizeof *out);
    memset(rec, 0, sizeof *rec);
    rec->list = list;
    rec->list_cap = sizeof list;
    rec->npolys = 2;
    for (i = 0; i < 2u; i++) rec->poly_tex[i] = G3R_NO_TEX;
    pica_set_force_ds(1);
    for (n = 0; n < G3S_TOTAL && rc == 0; n++) {
        g_mut_nomerge = mutant == 2 && n == G3S_TRANS;
        rc = selfcheck_frame(rec, list, sizeof list, n, mutant, out);
    }
    g_mut_nomerge = 0;
    pica_stats = sv_stats;
    pica_unsupported = sv_uns;
    pica_set_force_ds(sv_force);
    return rc;
}

/* bet 2.8: the wide target and buffers, made once at the first wide list; the vertex buffer grows to a wide list's vertex count (the split quad moves to the new end) */
static int wide_start(void)
{
    float *vb;
    C3D_BufInfo *buf;
    if (g_target_w != NULL) return 0;
    if (g_wide_failed) return -1;
    g_target_w = C3D_RenderTargetCreate((int)k_wide.raw_w, (int)k_wide.raw_h, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    g_raw_w = (uint32_t *)linearAlloc((size_t)k_wide.raw_w * k_wide.raw_h * 4u);
    g_ds_raw_w = (uint32_t *)linearAlloc((size_t)k_wide.raw_w * k_wide.raw_h * 4u);
    g_ds_w = (uint32_t *)malloc((size_t)k_wide.w * k_wide.h * 4u);
    g_layer_w[0] = (uint32_t *)malloc((size_t)k_wide.w * k_wide.h * 4u);
    g_layer_w[1] = (uint32_t *)malloc((size_t)k_wide.w * k_wide.h * 4u);
    vb = (float *)linearAlloc((size_t)(GPU3D_LIST_WIDE_VERTS + 4u) * VTX_FLOATS * sizeof(float));
    if (g_target_w == NULL || g_raw_w == NULL || g_ds_raw_w == NULL || g_ds_w == NULL || g_layer_w[0] == NULL || g_layer_w[1] == NULL || vb == NULL) {
        if (vb) linearFree(vb);
        g_wide_failed = 1;
        return -1;
    }
    linearFree(g_vbuf);
    g_vbuf = vb;
    g_vcap = GPU3D_LIST_WIDE_VERTS;
    split_quad_init();
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vbuf, (ptrdiff_t)(VTX_FLOATS * sizeof(float)), 3, 0x210);
    pica_stats.wide_linear_free = (uint32_t)linearSpaceFree();
    return 0;
}

int pica_render_eye(const Gpu3dRec *rec, int eye, float slider)
{
    Gpu3dList l;
    int err;
    uint32_t slot = eye > 0 ? 1u : 0u;
    float e_sigma = 0.0f;
    if (rec == NULL || eye < -1 || eye > 1) return GPU3D_ERR_SHORT;
    assert(g_target != NULL && g_vbuf != NULL);
    err = rec->list_len != 0u ? gpu3d_list_open(&l, rec->list, rec->list_len) : GPU3D_ERR_SHORT;
    if (err != GPU3D_ERR_OK) return err;
    if ((l.wide & GPU3D_LW_WIDE) != 0u) {
        if (wide_start() != 0) return GPU3D_ERR_COUNTS;
        if (eye != 0 && (l.wide & GPU3D_LW_STEREO) != 0u) {   /* sigma from the list's own camera distance and DS _00 (hw_wide.h); 0 when no registered perspective camera drew */
            float sg = hwwide_sigma(slider, l.st_m00, l.st_dist, HWWIDE_IOD);
            e_sigma = eye < 0 ? -sg : sg;
            pica_stats.last_sigma = sg;
        }
        g_dim = &k_wide;
        g_cur_target = g_target_w;
        g_cur_raw = g_raw_w;
        g_cur_ds_raw = g_ds_raw_w;
        g_cur_ds = g_ds_w;
        g_cur_layer = g_layer_w[slot];
        g_cur_wide = 1;
        g_e_sigma = e_sigma;
    } else {
        g_dim = &k_narrow;
        g_cur_target = g_target;
        g_cur_raw = g_raw;
        g_cur_ds_raw = g_ds_raw;
        g_cur_ds = g_ds;
        g_cur_layer = g_layer;
        g_cur_wide = 0;
        g_e_sigma = 0.0f;
    }
    pica_frame_begin(&l);
    if (slot == 0u) upload_textures(rec);   /* the second eye draws the same record: its textures are in the slots already */
    pica_draw(&l, rec);
    pica_frame_end();
    if (g_cur_wide) pica_stats.wide_frames++;
    return 0;
}

int pica_render_record(const Gpu3dRec *rec)
{
    return pica_render_eye(rec, 0, 0.0f);
}

void pica_set_force_ds(int on) { g_force_ds = on; }
const uint32_t *pica_ds_words(void) { return g_ds; }
const uint32_t *pica_layer_words(void) { return g_layer; }
const uint32_t *pica_layer_wide(uint32_t slot) { return slot < 2u ? g_layer_w[slot] : NULL; }
const uint32_t *pica_raw_words(void) { return g_raw; }
