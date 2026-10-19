/* hw_census.c -- the runtime feature census (bet 2.1c). Design, feeds and output: hw_census.h. Pure C, fixed tables, no allocation.
 *
 * SDK header citations (all under port/subprojects/NitroSDK-4.2.30001/include/nitro/):
 *   DISPCNT            hw/ARM9/ioreg_GX.h:167-243   (BG mode 0-2, BG0 2D/3D bit 3, OBJ tile map bit 4, display enables 8-12, blank 7, windows 13-15,
 *                                                    mode 16-17, OBJ 1D boundary 20-21, BG ext palette 30, OBJ ext palette 31); the sub engine's twin: ioreg_GXS.h:41-97
 *   DISPCAPCNT         hw/ARM9/ioreg_GX.h:331-359   (enable 31, mode 29-30, source A = 3D only when bit 24)
 *   MASTER_BRIGHT      hw/ARM9/ioreg_GX.h:453-457   (mode 14-15, value 0-4); sub: ioreg_GXS.h:124-128
 *   BGxCNT             hw/ARM9/ioreg_G2.h:257-410   (screen size 14-15, ext palette slot 13 (BG0/1), colour mode 7, mosaic 6, char base 2-5)
 *   WINxH/V, WININ/OUT hw/ARM9/ioreg_G2.h:881-975 (only the DISPCNT enable bits are read)
 *   MOSAIC, BLDCNT, BLDALPHA, BLDY   hw/ARM9/ioreg_G2.h:989-1058 (effect 6-7, EVA 0-4, EVB 8-12, EVY 0-4)
 *   extended BG kinds  gx/gx_bgcnt.h:288-290        (colour-mode bit 7 and char-base bit 2: (0,*) 256x16 palette map, (0,1)->bit7=1 bit2=0 256 bitmap,
 *                                                    bit7=1 bit2=1 direct-colour bitmap)
 *   OAM                gx/g2_oam.h:80-131           (attr0: Y 0-7, RSENABLE 8-9 (1 affine, 2 hidden, 3 affine double), MODE 10-11 (0 normal, 1 alpha,
 *                                                    2 window, 3 bitmap), mosaic 12, 256 colours 13, shape 14-15; attr1: X 0-8, flips 12-13, size 14-15)
 *   BG mode table      NitroSDK gx/gx_bgcnt.h GX_BGMODE_* warnings + GBATEK (modes 0-6: which of BG0-3 are text / affine / extended / large)
 *   DISP3DCNT          hw/ARM9/ioreg_G3X.h:803-847  (TME 0, THS 1, ATE 2, ABE 3, AAE 4, EME 5, FMOD 6, FME 7, fog shift 8-11, rear-plane bitmap bit 14 (named
 *                                                    PRI in the header; GBATEK "rear-plane mode"))
 *   CLEAR_COLOR 0x350  hw/ARM9/ioreg_G3X.h:1233-1253 (colour 0-14, fog 15, alpha 16-20, polygon id 24-29); edge colours 0x330, ALPHA_TEST_REF 0x340,
 *                      FOG_COLOR 0x358, FOG_OFFSET 0x35C, FOG_TABLE 0x360-0x37F, TOON_TABLE 0x380-0x3BF: ioreg_G3X.h:35-569 (plain registers: sampled)
 *   G3 commands        gx/g3.h:11-70 (ids and *_NPARAMS), command port = 0x400 + 4 * id (ioreg_G3.h:23-240)
 *   POLYGON_ATTR       hw/ARM9/ioreg_G3.h:815-855   (light mask 0-3, mode 4-5, back 6, front 7, XL 11, FC 12, D1 13, DT 14, FE 15, alpha 16-20, id 24-29)
 *   TEXIMAGE_PARAM     hw/ARM9/ioreg_G3.h:878-910   (RS 16, RT 17, FS 18, FT 19, S size 20-22 (header name V_SIZE), T size 23-25, format 26-28, TR 29, TGEN 30-31)
 *   MTX_MODE 251, BEGIN_VTXS type 1134, SWAP_BUFFERS XS bit 0 / DP bit 1 (ioreg_G3.h:1149-1153), VIEWPORT, DIF_AMB C bit 15 (:961), SPE_EMI S bit 15 (:1004),
 *   LIGHT_VECTOR / LIGHT_COLOR light number 30-31 (:1035, :1078).
 * The host test builds every word by hand from these shifts and checks the decode. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hw_census.h"

/* ---- feature tables ---------------------------------------------------------------------------------------------------------- */
enum {   /* 2D features, per engine; the printed name is "A_" / "B_" + the string below */
    F2_DISP_OFF, F2_DISP_GRAPHICS, F2_DISP_VRAM, F2_DISP_MAINMEM, F2_FORCED_BLANK,
    F2_BGMODE0, F2_BGMODE1, F2_BGMODE2, F2_BGMODE3, F2_BGMODE4, F2_BGMODE5, F2_BGMODE6, F2_BGMODE_INVALID,
    F2_BG0_3D, F2_BG_EXTPLTT, F2_OBJ_EXTPLTT,
    F2_OBJ_ENABLED, F2_OBJ_TILE_1D, F2_OBJ_TILE_2D, F2_OBJ_1D_B0, F2_OBJ_1D_B1, F2_OBJ_1D_B2, F2_OBJ_1D_B3,
    F2_WIN0, F2_WIN1, F2_OBJWIN,
    F2_BLD_NONE, F2_BLD_ALPHA, F2_BLD_BRIGHTEN, F2_BLD_DARKEN, F2_BLD_EVA, F2_BLD_EVB, F2_BLD_EVY,
    F2_MOSAIC_REG, F2_BRIGHT_UP, F2_BRIGHT_DOWN,
    F2_CAP_ENABLE, F2_CAP_MODE0, F2_CAP_MODE1, F2_CAP_MODE2, F2_CAP_MODE3, F2_CAP_SRC_3D,
    F2_SPR_VISIBLE, F2_SPR_AFFINE, F2_SPR_AFFINE_DOUBLE, F2_SPR_MODE_NORMAL, F2_SPR_MODE_ALPHA, F2_SPR_MODE_WINDOW, F2_SPR_MODE_BITMAP,
    F2_SPR_16COL, F2_SPR_256COL, F2_SPR_MOSAIC, F2_SPR_HFLIP, F2_SPR_VFLIP, F2_SPR_SQUARE, F2_SPR_WIDE, F2_SPR_TALL,
    F2_NSIMPLE
};
static const char *const k_name2[F2_NSIMPLE] = {
    "dispmode_off", "dispmode_graphics", "dispmode_vramdisplay", "dispmode_mainmem", "forced_blank",
    "bgmode_0", "bgmode_1", "bgmode_2", "bgmode_3", "bgmode_4", "bgmode_5", "bgmode_6", "bgmode_invalid",
    "bg0_3d", "bg_extpltt_enabled", "obj_extpltt_enabled",
    "obj_enabled", "obj_tilemap_1d", "obj_tilemap_2d", "obj_1d_boundary_0", "obj_1d_boundary_1", "obj_1d_boundary_2", "obj_1d_boundary_3",
    "win0", "win1", "objwin",
    "bld_none", "bld_alpha", "bld_brighten", "bld_darken", "bld_eva_nonzero", "bld_evb_nonzero", "bld_evy_nonzero",
    "mosaic_reg_nonzero", "master_bright_up", "master_bright_down",
    "dispcap_enable", "dispcap_mode_0", "dispcap_mode_1", "dispcap_mode_2", "dispcap_mode_3", "dispcap_src_3d_only",
    "spr_visible", "spr_affine", "spr_affine_double", "spr_mode_normal", "spr_mode_alpha", "spr_mode_window", "spr_mode_bitmap",
    "spr_16col", "spr_256col", "spr_mosaic", "spr_hflip", "spr_vflip", "spr_shape_square", "spr_shape_wide", "spr_shape_tall"
};
#define BG_KINDS 6u   /* per BG: text, affine, ext tile map, ext 256-colour bitmap, ext direct-colour bitmap, large bitmap */
#define BG_ATTRS 4u   /* per BG: 16 colours, 256 colours, ext-palette slot bit (BG0/1 text), mosaic bit */
#define F2_KIND0 F2_NSIMPLE
#define F2_ATTR0 (F2_KIND0 + 4u * BG_KINDS)
#define N2D      (F2_ATTR0 + 4u * BG_ATTRS)
static const char *const k_kind[BG_KINDS] = { "text", "affine", "ext_map", "ext_bmp256", "ext_bmpdc", "large_bmp" };
static const char *const k_attr[BG_ATTRS] = { "16col", "256col", "extpltt_slot_bit", "mosaic_bit" };
enum { K_TEXT, K_AFFINE, K_EXTMAP, K_EXTBMP256, K_EXTBMPDC, K_LARGE, K_NONE };

enum {   /* 3D features (one set for the single 3D engine) */
    F3_ACTIVE, F3_CNT_TME, F3_CNT_THS, F3_CNT_ATE, F3_CNT_ABE, F3_CNT_AAE, F3_CNT_EME, F3_CNT_FMOD, F3_CNT_FME, F3_CNT_REAR_BITMAP, F3_CNT_FOGSHIFT_NZ,
    F3_CLEAR_ALPHA_LT31, F3_CLEAR_FOG, F3_CLEAR_ID_NZ, F3_EDGE_COLOR_W, F3_FOG_TABLE_W, F3_TOON_TABLE_W, F3_FOG_COLOR_NZ, F3_FOG_OFFSET_NZ, F3_ALPHA_REF_NZ,
    F3_LIGHT0, F3_LIGHT1, F3_LIGHT2, F3_LIGHT3,
    F3_PM_MODULATE, F3_PM_DECAL, F3_PM_TOON_HIGHLIGHT, F3_PM_SHADOW,
    F3_ALPHA_OPAQUE, F3_ALPHA_TRANSLUCENT, F3_ALPHA_WIREFRAME,
    F3_CULL_NONE, F3_CULL_BACK, F3_CULL_FRONT, F3_CULL_ALL,
    F3_DEPTH_EQUAL, F3_POLY_FOG, F3_ONE_DOT, F3_FAR_INTERSECT, F3_XL_DEPTH, F3_POLYID_NZ,
    F3_TEXFMT0, F3_TEXFMT1, F3_TEXFMT2, F3_TEXFMT3, F3_TEXFMT4, F3_TEXFMT5, F3_TEXFMT6, F3_TEXFMT7,
    F3_REPEAT_S, F3_REPEAT_T, F3_FLIP_S, F3_FLIP_T, F3_COLOR0_TRANSP, F3_TEXGEN1, F3_TEXGEN2, F3_TEXGEN3,
    F3_SWAP_W, F3_SWAP_Z, F3_SWAP_MANUAL, F3_SWAP_AUTO,
    F3_VIEWPORT, F3_BOX_TEST, F3_POS_TEST, F3_VEC_TEST, F3_SHININESS_CMD, F3_SPE_EMI_TABLE, F3_DIF_AMB_VCOLOR,
    F3_PRIM_TRI, F3_PRIM_QUAD, F3_PRIM_TRISTRIP, F3_PRIM_QUADSTRIP,
    F3_MTX_PROJ, F3_MTX_POS, F3_MTX_POSVEC, F3_MTX_TEX,
    F3_LVEC0, F3_LVEC1, F3_LVEC2, F3_LVEC3, F3_LCOL0, F3_LCOL1, F3_LCOL2, F3_LCOL3,
    F3_UNKNOWN_CMD,
    N3D
};
static const char *const k_name3[N3D] = {
    "3d_active_on_bg0", "disp3dcnt_texture_mapping", "disp3dcnt_toon_highlight", "disp3dcnt_alpha_test", "disp3dcnt_alpha_blend", "disp3dcnt_antialias",
    "disp3dcnt_edge_marking", "disp3dcnt_fog_mode_alpha_only", "disp3dcnt_fog_enable", "disp3dcnt_rear_plane_bitmap", "disp3dcnt_fog_shift_nonzero",
    "clear_alpha_lt31", "clear_fog_bit", "clear_polygon_id_nonzero", "edge_color_table_written", "fog_table_written", "toon_table_written",
    "fog_color_nonzero", "fog_offset_nonzero", "alpha_test_ref_nonzero",
    "poly_light_0", "poly_light_1", "poly_light_2", "poly_light_3",
    "poly_mode_modulate", "poly_mode_decal", "poly_mode_toon_highlight", "poly_mode_shadow",
    "poly_alpha_opaque_31", "poly_alpha_translucent_1_30", "poly_alpha_wireframe_0",
    "poly_cull_none", "poly_cull_back", "poly_cull_front", "poly_cull_all",
    "poly_depth_test_equal", "poly_fog_enable", "poly_render_1dot", "poly_far_plane_intersect", "poly_translucent_depth_update", "poly_id_nonzero",
    "tex_fmt_0_none", "tex_fmt_1_a3i5", "tex_fmt_2_pltt4", "tex_fmt_3_pltt16", "tex_fmt_4_pltt256", "tex_fmt_5_comp4x4", "tex_fmt_6_a5i3", "tex_fmt_7_direct",
    "tex_repeat_s", "tex_repeat_t", "tex_flip_s", "tex_flip_t", "tex_color0_transparent", "tex_texgen_texcoord", "tex_texgen_normal", "tex_texgen_vertex",
    "swap_w_buffer", "swap_z_buffer", "swap_manual_sort", "swap_auto_sort",
    "viewport_used", "box_test_used", "pos_test_used", "vec_test_used", "shininess_used", "spe_emi_shininess_table_bit", "dif_amb_set_vertex_color",
    "prim_triangles", "prim_quads", "prim_triangle_strip", "prim_quad_strip",
    "mtx_mode_projection", "mtx_mode_position", "mtx_mode_position_vector", "mtx_mode_texture",
    "light_vector_0", "light_vector_1", "light_vector_2", "light_vector_3", "light_color_0", "light_color_1", "light_color_2", "light_color_3",
    "stream_unknown_command"
};

enum {   /* value sets: a bitmask of the values seen */
    S_BGMODES = 0,                 /* + engine */
    S_OBJ_SHAPES = 2,              /* + engine; value = shape * 4 + size */
    S_BGSIZE = 4,                  /* + engine * 4 + bg: BGxCNT screen size 0-3 */
    S_VRAM_MST = 12,               /* + bank 0-8: the MST values the bank was enabled with */
    S_VRAM_OFS = 21,               /* + bank 0-8: the OFS values */
    S_TEX_FORMATS = 30, S_TEX_S, S_TEX_T, S_TEXGEN, S_MTX_MODES, S_POLY_MODES, S_POLY_LIGHTS, S_PRIM_TYPES, S_FOG_SHIFT, S_LIGHT_VEC_N, S_LIGHT_COL_N,
    NSETS
};
#define VBANKS 9u
static const char k_bank[VBANKS] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I' };
static const char *const k_shape_label[12] = { "8x8", "16x16", "32x32", "64x64", "16x8", "32x8", "32x16", "64x32", "8x16", "8x32", "16x32", "32x64" };

/* ---- state ------------------------------------------------------------------------------------------------------------------- */
#define CMDS 128u
typedef struct Census {
    uint32_t frames;
    uint32_t n2[2][N2D];
    uint8_t s2[2][N2D];
    uint32_t n3[N3D];
    uint8_t s3[N3D];
    uint32_t set[NSETS];
    uint32_t cmd_frames[CMDS], cmd_total[CMDS];
    uint8_t cmd_seen[CMDS], pidx[CMDS];
    uint8_t q[4], qn, qi, left, argn, cur;   /* the packed-command state shared by every input path */
    uint32_t obj_peak[2];
    uint64_t id_mask;
    uint32_t alpha_min;
    uint32_t viewport[HWCEN_VIEWPORTS], nview;
} Census;

static Census s_c;
static int8_t s_argc[CMDS];
static int s_argc_ready;
static uint8_t s_slow[256];   /* 1 = HWCEN_GxBlock must take the per-word path for a packed word holding this id (decoded parameter, special on_cmd, unknown) */
static uint8_t s_nargs[256];  /* parameter words of a fast id */
static uint8_t s_arg0[CMDS];   /* 1 = on_arg looks at this command's first parameter word (the others are only counted) */

static const struct { uint8_t id; int8_t argc; const char *name; } k_cmds[] = {   /* gx/g3.h:11-70 and the *_NPARAMS that follow */
    { 0x10, 1, "MTX_MODE" }, { 0x11, 0, "MTX_PUSH" }, { 0x12, 1, "MTX_POP" }, { 0x13, 1, "MTX_STORE" }, { 0x14, 1, "MTX_RESTORE" },
    { 0x15, 0, "MTX_IDENTITY" }, { 0x16, 16, "MTX_LOAD_4x4" }, { 0x17, 12, "MTX_LOAD_4x3" }, { 0x18, 16, "MTX_MULT_4x4" }, { 0x19, 12, "MTX_MULT_4x3" },
    { 0x1a, 9, "MTX_MULT_3x3" }, { 0x1b, 3, "MTX_SCALE" }, { 0x1c, 3, "MTX_TRANS" },
    { 0x20, 1, "COLOR" }, { 0x21, 1, "NORMAL" }, { 0x22, 1, "TEXCOORD" }, { 0x23, 2, "VTX_16" }, { 0x24, 1, "VTX_10" }, { 0x25, 1, "VTX_XY" },
    { 0x26, 1, "VTX_XZ" }, { 0x27, 1, "VTX_YZ" }, { 0x28, 1, "VTX_DIFF" }, { 0x29, 1, "POLYGON_ATTR" }, { 0x2a, 1, "TEXIMAGE_PARAM" }, { 0x2b, 1, "TEXPLTT_BASE" },
    { 0x30, 1, "DIF_AMB" }, { 0x31, 1, "SPE_EMI" }, { 0x32, 1, "LIGHT_VECTOR" }, { 0x33, 1, "LIGHT_COLOR" }, { 0x34, 32, "SHININESS" },
    { 0x40, 1, "BEGIN_VTXS" }, { 0x41, 0, "END_VTXS" }, { 0x50, 1, "SWAP_BUFFERS" }, { 0x60, 1, "VIEWPORT" },
    { 0x70, 3, "BOX_TEST" }, { 0x71, 2, "POS_TEST" }, { 0x72, 1, "VEC_TEST" }
};
#define NCMDS (sizeof k_cmds / sizeof k_cmds[0])

static void argc_init(void)
{
    size_t i;

    memset(s_argc, -1, sizeof s_argc);
    for (i = 0u; i < NCMDS; i++) {
        s_argc[k_cmds[i].id] = k_cmds[i].argc;
    }
    {
        static const uint8_t k_arg0[] = { 0x10, 0x29, 0x2a, 0x30, 0x31, 0x32, 0x33, 0x40, 0x50, 0x60 };   /* the cases of on_arg */

        memset(s_arg0, 0, sizeof s_arg0);
        for (i = 0u; i < sizeof k_arg0; i++) {
            s_arg0[k_arg0[i]] = 1u;
        }
        memset(s_slow, 1, sizeof s_slow);
        memset(s_nargs, 0, sizeof s_nargs);
        for (i = 0u; i < NCMDS; i++) {
            uint8_t id = k_cmds[i].id;

            if (s_arg0[id] == 0u && id != 0x34 && id != 0x70 && id != 0x71 && id != 0x72) {   /* the on_cmd switch marks these */
                s_slow[id] = 0u;
                s_nargs[id] = (uint8_t)k_cmds[i].argc;
            }
        }
        s_slow[0] = 0u;   /* a NOP byte inside a packed word */
    }
    s_argc_ready = 1;
}

void HWCEN_Reset(void)
{
    memset(&s_c, 0, sizeof s_c);
    s_c.alpha_min = 32u;
    if (!s_argc_ready) {
        argc_init();
    }
}

static void mark2(uint32_t e, uint32_t f)
{
    assert(e < 2u && f < N2D);
    s_c.s2[e][f] = 1u;
}

static void mark3(uint32_t f)
{
    assert(f < N3D);
    s_c.s3[f] = 1u;
}

static void set_add(uint32_t id, uint32_t value)
{
    assert(id < NSETS && value < 32u);
    s_c.set[id] |= 1u << value;
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(p + 2) << 16);
}

/* ---- the G3 command stream ---------------------------------------------------------------------------------------------------- */
static void dec_polygon_attr(uint32_t v)
{
    uint32_t le = v & 0xfu, pm = (v >> 4) & 3u, bk = (v >> 6) & 1u, fr = (v >> 7) & 1u, alpha = (v >> 16) & 0x1fu, id = (v >> 24) & 0x3fu;
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        if ((le >> i) & 1u) {
            mark3(F3_LIGHT0 + i);
        }
    }
    set_add(S_POLY_LIGHTS, le);
    set_add(S_POLY_MODES, pm);
    mark3(F3_PM_MODULATE + pm);
    mark3(alpha == 31u ? F3_ALPHA_OPAQUE : (alpha == 0u ? F3_ALPHA_WIREFRAME : F3_ALPHA_TRANSLUCENT));
    if (alpha != 0u && alpha < s_c.alpha_min) {
        s_c.alpha_min = alpha;
    }
    mark3(bk ? (fr ? F3_CULL_NONE : F3_CULL_FRONT) : (fr ? F3_CULL_BACK : F3_CULL_ALL));
    if ((v >> 14) & 1u) { mark3(F3_DEPTH_EQUAL); }
    if ((v >> 15) & 1u) { mark3(F3_POLY_FOG); }       /* REG_G3_POLYGON_ATTR_FE_SHIFT 15 (ioreg_G3.h:823); bits 12 and 15 were swapped before bet 2.1e */
    if ((v >> 13) & 1u) { mark3(F3_ONE_DOT); }
    if ((v >> 12) & 1u) { mark3(F3_FAR_INTERSECT); }  /* REG_G3_POLYGON_ATTR_FC_SHIFT 12 (ioreg_G3.h:835): far-plane clipping */
    if ((v >> 11) & 1u) { mark3(F3_XL_DEPTH); }
    if (id != 0u) { mark3(F3_POLYID_NZ); }
    s_c.id_mask |= (uint64_t)1u << id;
}

static void dec_teximage(uint32_t v)
{
    uint32_t fmt = (v >> 26) & 7u, tgen = (v >> 30) & 3u;

    mark3(F3_TEXFMT0 + fmt);
    set_add(S_TEX_FORMATS, fmt);
    set_add(S_TEXGEN, tgen);
    if (tgen != 0u) { mark3(F3_TEXGEN1 + tgen - 1u); }
    if ((v >> 16) & 1u) { mark3(F3_REPEAT_S); }
    if ((v >> 17) & 1u) { mark3(F3_REPEAT_T); }
    if ((v >> 18) & 1u) { mark3(F3_FLIP_S); }
    if ((v >> 19) & 1u) { mark3(F3_FLIP_T); }
    if ((v >> 29) & 1u) { mark3(F3_COLOR0_TRANSP); }
    if (fmt != 0u) {   /* a texture is bound: its sizes count */
        set_add(S_TEX_S, (v >> 20) & 7u);
        set_add(S_TEX_T, (v >> 23) & 7u);
    }
}

static void dec_viewport(uint32_t v)
{
    uint32_t i;

    for (i = 0u; i < s_c.nview && s_c.viewport[i] != v; i++) {
    }
    if (i == s_c.nview && i < HWCEN_VIEWPORTS) {
        s_c.viewport[i] = v;
        s_c.nview++;
    }
}

static void on_arg(uint32_t cmd, uint32_t idx, uint32_t v)
{
    if (idx != 0u) {   /* the multi-word commands (matrices, SHININESS, BOX_TEST, POS_TEST) are only counted */
        return;
    }
    switch (cmd) {
    case 0x10: set_add(S_MTX_MODES, v & 3u); mark3(F3_MTX_PROJ + (v & 3u)); break;
    case 0x29: dec_polygon_attr(v); break;
    case 0x2a: dec_teximage(v); break;
    case 0x30: if ((v >> 15) & 1u) { mark3(F3_DIF_AMB_VCOLOR); } break;
    case 0x31: if ((v >> 15) & 1u) { mark3(F3_SPE_EMI_TABLE); } break;
    case 0x32: set_add(S_LIGHT_VEC_N, v >> 30); mark3(F3_LVEC0 + (v >> 30)); break;
    case 0x33: set_add(S_LIGHT_COL_N, v >> 30); mark3(F3_LCOL0 + (v >> 30)); break;
    case 0x40: set_add(S_PRIM_TYPES, v & 3u); mark3(F3_PRIM_TRI + (v & 3u)); break;
    case 0x50: mark3(((v >> 1) & 1u) ? F3_SWAP_W : F3_SWAP_Z); mark3((v & 1u) ? F3_SWAP_MANUAL : F3_SWAP_AUTO); break;
    case 0x60: mark3(F3_VIEWPORT); dec_viewport(v); break;
    default: break;
    }
}

static void on_cmd(uint32_t cmd)
{
    assert(cmd < CMDS);
    s_c.cmd_seen[cmd] = 1u;
    s_c.cmd_total[cmd]++;
    switch (cmd) {
    case 0x34: mark3(F3_SHININESS_CMD); break;
    case 0x70: mark3(F3_BOX_TEST); break;
    case 0x71: mark3(F3_POS_TEST); break;
    case 0x72: mark3(F3_VEC_TEST); break;
    default: break;
    }
}

/* Start the next queued command; stops at the first one that takes parameters. */
static void fifo_advance(void)
{
    while (s_c.qi < s_c.qn && s_c.left == 0u) {
        uint32_t c = s_c.q[s_c.qi++];

        if (c >= CMDS || s_argc[c] < 0) {
            mark3(F3_UNKNOWN_CMD);
            continue;
        }
        on_cmd(c);
        s_c.cur = (uint8_t)c;
        s_c.argn = 0u;
        s_c.left = (uint8_t)s_argc[c];
    }
}

static void fifo_word(uint32_t w)
{
    if (s_c.left == 0u) {   /* a packed command word: up to four ids, low byte first; zero bytes are NOPs */
        uint32_t i;

        s_c.qn = 0u;
        s_c.qi = 0u;
        for (i = 0u; i < 4u; i++) {
            uint32_t b = (w >> (8u * i)) & 0xffu;

            if (b != 0u) {
                s_c.q[s_c.qn++] = (uint8_t)b;
            }
        }
        fifo_advance();
        return;
    }
    on_arg(s_c.cur, s_c.argn++, w);
    if (--s_c.left == 0u) {
        fifo_advance();
    }
}

static void port_store(uint32_t cmd, uint32_t v)
{
    int argc = s_argc[cmd];

    if (argc < 0) {
        mark3(F3_UNKNOWN_CMD);
        return;
    }
    if (argc == 0) {
        on_cmd(cmd);
        return;
    }
    if (s_c.pidx[cmd] == 0u) {
        on_cmd(cmd);
    }
    on_arg(cmd, s_c.pidx[cmd], v);
    s_c.pidx[cmd] = (uint8_t)((s_c.pidx[cmd] + 1u) % (uint32_t)argc);
}

/* A run of FIFO words, in program order; the same result as HWCEN_GxWord(0, w) for each (host-tested), but the parameter words of a command nobody
   decodes are skipped in one step: the cost is per command, not per word. */
void HWCEN_GxBlock(const uint32_t *w, uint32_t n)
{
    uint32_t i = 0u;

    if (!s_argc_ready) {
        argc_init();
    }
    while (i < n) {
        uint32_t m;

        if (s_c.left == 0u) {
            uint32_t p = w[i], b0 = p & 0xffu, b1 = (p >> 8) & 0xffu, b2 = (p >> 16) & 0xffu, b3 = p >> 24;

            if ((s_slow[b0] | s_slow[b1] | s_slow[b2] | s_slow[b3]) == 0u) {   /* fast: nothing in this word is decoded; count the ids and skip their parameters */
                uint32_t total = (uint32_t)s_nargs[b0] + s_nargs[b1] + s_nargs[b2] + s_nargs[b3];

                if (i + 1u + total <= n) {
                    s_c.cmd_total[b0]++;
                    s_c.cmd_total[b1]++;
                    s_c.cmd_total[b2]++;
                    s_c.cmd_total[b3]++;
                    s_c.cmd_seen[b0] = 1u;
                    s_c.cmd_seen[b1] = 1u;
                    s_c.cmd_seen[b2] = 1u;
                    s_c.cmd_seen[b3] = 1u;
                    s_c.cmd_seen[0] = 0u;   /* NOP bytes were counted as id 0: not a command */
                    s_c.cmd_total[0] = 0u;
                    i += 1u + total;
                    continue;
                }
            }
            fifo_word(w[i++]);
            continue;
        }
        m = n - i;
        if (m > s_c.left) {
            m = s_c.left;
        }
        if (s_arg0[s_c.cur] != 0u && s_c.argn == 0u) {
            on_arg(s_c.cur, 0u, w[i]);
        }
        i += m;
        s_c.argn = (uint8_t)(s_c.argn + m);
        s_c.left = (uint8_t)(s_c.left - m);
        if (s_c.left == 0u) {
            fifo_advance();
        }
    }
}

void HWCEN_GxWord(uint32_t addr, uint32_t word)
{
    if (!s_argc_ready) {
        argc_init();
    }
    if (addr == 0u) {
        fifo_word(word);
    } else if (addr >= 0x440u && addr < 0x600u) {
        port_store((addr - 0x400u) >> 2, word);
    }
}

void HWCEN_VramCnt(uint32_t bank, uint32_t value)
{
    if (bank >= VBANKS || (value & 0x80u) == 0u) {   /* bit 7 = enable (ioreg_GX.h VRAMCNT_x_E) */
        return;
    }
    set_add(S_VRAM_MST + bank, value & 7u);
    set_add(S_VRAM_OFS + bank, (value >> 3) & 3u);
}

/* ---- 2D sampling ------------------------------------------------------------------------------------------------------------- */
static uint32_t bg_kind(uint32_t mode, uint32_t bg)
{
    static const uint8_t tab[7][4] = {   /* BG mode table (GBATEK; gx_bgcnt.h BG mode warnings) */
        { K_TEXT, K_TEXT, K_TEXT, K_TEXT },
        { K_TEXT, K_TEXT, K_TEXT, K_AFFINE },
        { K_TEXT, K_TEXT, K_AFFINE, K_AFFINE },
        { K_TEXT, K_TEXT, K_TEXT, K_EXTMAP },
        { K_TEXT, K_TEXT, K_AFFINE, K_EXTMAP },
        { K_TEXT, K_TEXT, K_EXTMAP, K_EXTMAP },
        { K_TEXT, K_NONE, K_LARGE, K_NONE }
    };

    return mode < 7u ? tab[mode][bg] : K_NONE;
}

/* an extended BG (kind K_EXTMAP stands for "extended"): the BGxCNT bits pick the sub-kind (gx_bgcnt.h:288-290) */
static uint32_t ext_kind(uint32_t cnt)
{
    if (((cnt >> 7) & 1u) == 0u) {
        return K_EXTMAP;
    }
    return ((cnt >> 2) & 1u) ? K_EXTBMPDC : K_EXTBMP256;
}

static void sample_bg(uint32_t e, const uint8_t *io, uint32_t dispcnt, uint32_t mode, uint32_t bg)
{
    uint32_t cnt = rd16(io + 8u + 2u * bg), kind = bg_kind(mode, bg), size = (cnt >> 14) & 3u;

    if (kind == K_NONE || ((dispcnt >> (8u + bg)) & 1u) == 0u) {
        return;
    }
    if (bg == 0u && e == 0u && ((dispcnt >> 3) & 1u)) {   /* BG0 shows the 3D layer instead of tiles */
        mark2(e, F2_BG0_3D);
        return;
    }
    if (kind == K_EXTMAP) {
        kind = ext_kind(cnt);
    }
    mark2(e, F2_KIND0 + bg * BG_KINDS + kind);
    set_add(S_BGSIZE + e * 4u + bg, size);
    if (kind == K_TEXT) {
        uint32_t col256 = (cnt >> 7) & 1u;

        mark2(e, F2_ATTR0 + bg * BG_ATTRS + col256);
        if (bg < 2u && col256 && ((dispcnt >> 30) & 1u) && ((cnt >> 13) & 1u)) {
            mark2(e, F2_ATTR0 + bg * BG_ATTRS + 2u);
        }
    }
    if ((cnt >> 6) & 1u) {
        mark2(e, F2_ATTR0 + bg * BG_ATTRS + 3u);
    }
}

static void sample_blend(uint32_t e, const uint8_t *io)
{
    uint32_t bld = rd16(io + 0x50u), eff = (bld >> 6) & 3u, p1 = bld & 0x3fu, p2 = (bld >> 8) & 0x3fu, alpha = rd16(io + 0x52u), y = rd16(io + 0x54u);

    mark2(e, F2_BLD_NONE + eff);
    if (eff == 1u && p1 != 0u && p2 != 0u) {
        if ((alpha & 0x1fu) != 0u) { mark2(e, F2_BLD_EVA); }
        if (((alpha >> 8) & 0x1fu) != 0u) { mark2(e, F2_BLD_EVB); }
    }
    if ((eff == 2u || eff == 3u) && p1 != 0u && (y & 0x1fu) != 0u) {
        mark2(e, F2_BLD_EVY);
    }
}

static void sample_misc2d(uint32_t e, const uint8_t *io, uint32_t dispcnt)
{
    uint32_t bright = rd16(io + 0x6cu), bm = (bright >> 14) & 3u;

    if ((dispcnt >> 13) & 1u) { mark2(e, F2_WIN0); }
    if ((dispcnt >> 14) & 1u) { mark2(e, F2_WIN1); }
    if ((dispcnt >> 15) & 1u) { mark2(e, F2_OBJWIN); }
    sample_blend(e, io);
    if (rd16(io + 0x4cu) != 0u) { mark2(e, F2_MOSAIC_REG); }
    if ((bright & 0x1fu) != 0u && bm == 1u) { mark2(e, F2_BRIGHT_UP); }
    if ((bright & 0x1fu) != 0u && bm == 2u) { mark2(e, F2_BRIGHT_DOWN); }
    if (e == 0u) {   /* DISPCAPCNT exists on the main engine only (0x064) */
        uint32_t cap = rd32(io + 0x64u);

        if (cap >> 31) {
            mark2(e, F2_CAP_ENABLE);
            mark2(e, F2_CAP_MODE0 + ((cap >> 29) & 3u));
            if ((cap >> 24) & 1u) { mark2(e, F2_CAP_SRC_3D); }
        }
    }
}

static void sample_obj_regs(uint32_t e, uint32_t dispcnt)
{
    mark2(e, F2_OBJ_ENABLED);
    if ((dispcnt >> 4) & 1u) {
        mark2(e, F2_OBJ_TILE_1D);
        mark2(e, F2_OBJ_1D_B0 + ((dispcnt >> 20) & 3u));
    } else {
        mark2(e, F2_OBJ_TILE_2D);
    }
    if ((dispcnt >> 31) & 1u) { mark2(e, F2_OBJ_EXTPLTT); }
}

/* a sprite's pixel size from attr0 shape (14-15) and attr1 size (14-15): g2_oam.h:143-154 */
static void obj_dims(uint32_t shape, uint32_t size, uint32_t *w, uint32_t *h)
{
    static const uint8_t ww[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
    static const uint8_t hh[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

    *w = ww[shape][size];
    *h = hh[shape][size];
}

static int obj_onscreen(uint32_t a0, uint32_t a1, uint32_t w, uint32_t h)
{
    int y = (int)(a0 & 0xffu), x = (int)(a1 & 0x1ffu);

    if (x >= 256) { x -= 512; }
    if (y + (int)h > 256) { y -= 256; }   /* Y wraps at 256 */
    return x < 256 && x + (int)w > 0 && y < 192 && y + (int)h > 0;
}

static void sample_sprite(uint32_t e, uint32_t a0, uint32_t a1)
{
    uint32_t rs = (a0 >> 8) & 3u, mode = (a0 >> 10) & 3u, shape = (a0 >> 14) & 3u, size = (a1 >> 14) & 3u, w, h;

    if (rs == 2u || shape == 3u) {   /* hidden / the invalid shape */
        return;
    }
    obj_dims(shape, size, &w, &h);
    if (rs == 3u) { w *= 2u; h *= 2u; }
    if (!obj_onscreen(a0, a1, w, h)) {
        return;
    }
    mark2(e, F2_SPR_VISIBLE);
    set_add(S_OBJ_SHAPES + e, shape * 4u + size);
    mark2(e, F2_SPR_SQUARE + shape);
    mark2(e, F2_SPR_MODE_NORMAL + mode);
    mark2(e, ((a0 >> 13) & 1u) ? F2_SPR_256COL : F2_SPR_16COL);
    if ((a0 >> 12) & 1u) { mark2(e, F2_SPR_MOSAIC); }
    if (rs == 1u) { mark2(e, F2_SPR_AFFINE); }
    if (rs == 3u) { mark2(e, F2_SPR_AFFINE); mark2(e, F2_SPR_AFFINE_DOUBLE); }
    if (rs == 0u && ((a1 >> 12) & 1u)) { mark2(e, F2_SPR_HFLIP); }
    if (rs == 0u && ((a1 >> 13) & 1u)) { mark2(e, F2_SPR_VFLIP); }
}

static void sample_oam(uint32_t e, const uint8_t *oam)
{
    uint32_t i;

    for (i = 0u; i < 128u; i++) {   /* 128 entries of 8 bytes (attr0, attr1, attr2, affine parameter) per engine */
        sample_sprite(e, rd16(oam + 8u * i), rd16(oam + 8u * i + 2u));
    }
}

static uint32_t count_visible(const uint8_t *oam)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < 128u; i++) {
        uint32_t a0 = rd16(oam + 8u * i), a1 = rd16(oam + 8u * i + 2u), shape = (a0 >> 14) & 3u, rs = (a0 >> 8) & 3u, w, h;

        if (rs == 2u || shape == 3u) {
            continue;
        }
        obj_dims(shape, (a1 >> 14) & 3u, &w, &h);
        n += obj_onscreen(a0, a1, rs == 3u ? 2u * w : w, rs == 3u ? 2u * h : h) ? 1u : 0u;
    }
    return n;
}

static int sample_engine(uint32_t e, const uint8_t *io, const uint8_t *oam)
{
    uint32_t dispcnt = rd32(io), mode = (dispcnt >> 16) & 3u, bgmode = dispcnt & 7u, bg;

    mark2(e, F2_DISP_OFF + mode);
    if ((dispcnt >> 7) & 1u) {
        mark2(e, F2_FORCED_BLANK);
        return 0;
    }
    if (mode != 1u) {
        return 0;
    }
    mark2(e, F2_BGMODE0 + bgmode);
    set_add(S_BGMODES + e, bgmode);
    if ((dispcnt >> 30) & 1u) { mark2(e, F2_BG_EXTPLTT); }
    for (bg = 0u; bg < 4u; bg++) {
        sample_bg(e, io, dispcnt, bgmode, bg);
    }
    sample_misc2d(e, io, dispcnt);
    if ((dispcnt >> 12) & 1u) {
        sample_obj_regs(e, dispcnt);
        if (oam != NULL) {
            uint32_t n = count_visible(oam);

            sample_oam(e, oam);
            if (n > s_c.obj_peak[e]) { s_c.obj_peak[e] = n; }
        }
    }
    return e == 0u && ((dispcnt >> 3) & 1u) && ((dispcnt >> 8) & 1u);
}

/* ---- 3D register sampling ---------------------------------------------------------------------------------------------------- */
static int any_nonzero(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        if (p[i] != 0u) {
            return 1;
        }
    }
    return 0;
}

static void sample_3d(const uint8_t *io)
{
    uint32_t cnt = rd16(io + 0x60u), clr = rd32(io + 0x350u), i;

    mark3(F3_ACTIVE);
    for (i = 0u; i < 8u; i++) {   /* TME THS ATE ABE AAE EME FMOD FME = bits 0-7, then the rear-plane bit 14 */
        if ((cnt >> i) & 1u) { mark3(F3_CNT_TME + i); }
    }
    if ((cnt >> 14) & 1u) { mark3(F3_CNT_REAR_BITMAP); }
    if ((cnt >> 8) & 0xfu) { mark3(F3_CNT_FOGSHIFT_NZ); }
    set_add(S_FOG_SHIFT, (cnt >> 8) & 0xfu);
    if (((clr >> 16) & 0x1fu) < 31u) { mark3(F3_CLEAR_ALPHA_LT31); }
    if ((clr >> 15) & 1u) { mark3(F3_CLEAR_FOG); }
    if ((clr >> 24) & 0x3fu) { mark3(F3_CLEAR_ID_NZ); }
    if (any_nonzero(io + 0x330u, 16u)) { mark3(F3_EDGE_COLOR_W); }
    if (any_nonzero(io + 0x360u, 32u)) { mark3(F3_FOG_TABLE_W); }
    if (any_nonzero(io + 0x380u, 64u)) { mark3(F3_TOON_TABLE_W); }
    if (rd32(io + 0x358u) != 0u) { mark3(F3_FOG_COLOR_NZ); }
    if (rd16(io + 0x35cu) != 0u) { mark3(F3_FOG_OFFSET_NZ); }
    if ((rd16(io + 0x340u) & 0x1fu) != 0u) { mark3(F3_ALPHA_REF_NZ); }
}

/* ---- frame end --------------------------------------------------------------------------------------------------------------- */
static void close_frame(void)
{
    uint32_t e, f;

    for (e = 0u; e < 2u; e++) {
        for (f = 0u; f < N2D; f++) {
            s_c.n2[e][f] += s_c.s2[e][f];
            s_c.s2[e][f] = 0u;
        }
    }
    for (f = 0u; f < N3D; f++) {
        s_c.n3[f] += s_c.s3[f];
        s_c.s3[f] = 0u;
    }
    for (f = 0u; f < CMDS; f++) {
        s_c.cmd_frames[f] += s_c.cmd_seen[f];
        s_c.cmd_seen[f] = 0u;
    }
}

int HWCEN_FrameEnd(const uint8_t *io, const uint8_t *oam)
{
    int three_d = 0;

    if (io != NULL) {
        three_d = sample_engine(0u, io, oam);
        (void)sample_engine(1u, io + 0x1000u, oam != NULL ? oam + 0x400u : NULL);
        if (three_d) {
            sample_3d(io);
        }
    }
    close_frame();
    s_c.frames++;
    return s_c.frames % HWCEN_DUMP_FRAMES == 0u;
}

/* ---- names, lookups, text ----------------------------------------------------------------------------------------------------- */
static void name2(char *out, size_t cap, uint32_t e, uint32_t f)
{
    char eng = e == 0u ? 'A' : 'B';

    assert(f < N2D);
    if (f < F2_NSIMPLE) {
        snprintf(out, cap, "%c_%s", eng, k_name2[f]);
    } else if (f < F2_ATTR0) {
        snprintf(out, cap, "%c_bg%u_%s", eng, (unsigned)((f - F2_KIND0) / BG_KINDS), k_kind[(f - F2_KIND0) % BG_KINDS]);
    } else {
        snprintf(out, cap, "%c_bg%u_%s", eng, (unsigned)((f - F2_ATTR0) / BG_ATTRS), k_attr[(f - F2_ATTR0) % BG_ATTRS]);
    }
}

static void set_name(char *out, size_t cap, uint32_t id)
{
    if (id < S_OBJ_SHAPES) {
        snprintf(out, cap, "bg_modes_%c", id == 0u ? 'A' : 'B');
    } else if (id < S_BGSIZE) {
        snprintf(out, cap, "obj_shapes_%c", id == S_OBJ_SHAPES ? 'A' : 'B');
    } else if (id < S_VRAM_MST) {
        snprintf(out, cap, "bg%u_size_%c", (unsigned)((id - S_BGSIZE) % 4u), (id - S_BGSIZE) < 4u ? 'A' : 'B');
    } else if (id < S_VRAM_OFS) {
        snprintf(out, cap, "vram_mst_%c", k_bank[id - S_VRAM_MST]);
    } else if (id < S_TEX_FORMATS) {
        snprintf(out, cap, "vram_ofs_%c", k_bank[id - S_VRAM_OFS]);
    } else {
        static const char *const n[] = { "tex_formats", "tex_sizes_s", "tex_sizes_t", "tex_texgen", "mtx_modes", "poly_modes", "poly_light_masks",
                                         "prim_types", "disp3dcnt_fog_shift", "light_vector_nums", "light_color_nums" };
        snprintf(out, cap, "%s", n[id - S_TEX_FORMATS]);
    }
}

uint32_t HWCEN_Features(void)
{
    return 2u * N2D + N3D;
}

int HWCEN_IsFeature(const char *name)
{
    char buf[48];
    uint32_t e, f;

    assert(name != NULL);
    for (e = 0u; e < 2u; e++) {
        for (f = 0u; f < N2D; f++) {
            name2(buf, sizeof buf, e, f);
            if (strcmp(buf, name) == 0) {
                return 1;
            }
        }
    }
    for (f = 0u; f < N3D; f++) {
        if (strcmp(k_name3[f], name) == 0) {
            return 1;
        }
    }
    return 0;
}

uint32_t HWCEN_Count(const char *name)
{
    char buf[48];
    uint32_t e, f;

    assert(name != NULL);
    for (e = 0u; e < 2u; e++) {
        for (f = 0u; f < N2D; f++) {
            name2(buf, sizeof buf, e, f);
            if (strcmp(buf, name) == 0) {
                return s_c.n2[e][f];
            }
        }
    }
    for (f = 0u; f < N3D; f++) {
        if (strcmp(k_name3[f], name) == 0) {
            return s_c.n3[f];
        }
    }
    return 0u;
}

int HWCEN_IsSet(const char *name)
{
    char buf[48];
    uint32_t id;

    assert(name != NULL);
    for (id = 0u; id < NSETS; id++) {
        set_name(buf, sizeof buf, id);
        if (strcmp(buf, name) == 0) {
            return 1;
        }
    }
    return 0;
}

uint32_t HWCEN_Set(const char *name)
{
    char buf[48];
    uint32_t id;

    assert(name != NULL);
    for (id = 0u; id < NSETS; id++) {
        set_name(buf, sizeof buf, id);
        if (strcmp(buf, name) == 0) {
            return s_c.set[id];
        }
    }
    return 0u;
}

uint32_t HWCEN_CmdFrames(uint32_t cmd)
{
    return cmd < CMDS ? s_c.cmd_frames[cmd] : 0u;
}

uint32_t HWCEN_CmdTotal(uint32_t cmd)
{
    return cmd < CMDS ? s_c.cmd_total[cmd] : 0u;
}

uint32_t HWCEN_Frames(void)
{
    return s_c.frames;
}

typedef struct Out {
    char *buf;
    size_t cap, len;
} Out;

static void put(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->len + 1u >= o->cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n >= o->cap - o->len ? o->cap - o->len - 1u : (size_t)n;
    }
}

static void put_set(Out *o, uint32_t id)
{
    char nm[48];
    uint32_t v, first = 1u;
    int shapes = id == S_OBJ_SHAPES || id == S_OBJ_SHAPES + 1u;

    set_name(nm, sizeof nm, id);
    put(o, "%s=", nm);
    for (v = 0u; v < 32u; v++) {
        if ((s_c.set[id] >> v) & 1u) {
            if (shapes && v < 12u) {
                put(o, "%s%s", first ? "" : ",", k_shape_label[v]);
            } else {
                put(o, "%s%u", first ? "" : ",", (unsigned)v);
            }
            first = 0u;
        }
    }
    put(o, "\n");
}

static void put_cmds(Out *o)
{
    size_t i;
    uint32_t c;

    put(o, "# G3 commands: cmd_<name>=<frames seen in>  (cmdtotal_<name> = occurrences, drifts between runs)\n");
    for (i = 0u; i < NCMDS; i++) {
        put(o, "cmd_%s=%u\n", k_cmds[i].name, (unsigned)s_c.cmd_frames[k_cmds[i].id]);
    }
    for (c = 0u; c < CMDS; c++) {   /* an id outside the table can only come from a desynchronised stream */
        if (s_argc[c] < 0 && s_c.cmd_frames[c] != 0u) {
            put(o, "cmd_unknown_0x%02x=%u\n", (unsigned)c, (unsigned)s_c.cmd_frames[c]);
        }
    }
    for (i = 0u; i < NCMDS; i++) {
        put(o, "cmdtotal_%s=%u\n", k_cmds[i].name, (unsigned)s_c.cmd_total[k_cmds[i].id]);
    }
}

static void put_numbers(Out *o)
{
    uint32_t i, ids = 0u;

    for (i = 0u; i < 64u; i++) {
        ids += (uint32_t)((s_c.id_mask >> i) & 1u);
    }
    put(o, "# numbers\n");
    put(o, "obj_visible_peak_A=%u\nobj_visible_peak_B=%u\n", (unsigned)s_c.obj_peak[0], (unsigned)s_c.obj_peak[1]);
    put(o, "polygon_ids_distinct=%u\npolygon_alpha_min_nonzero=%u\n", (unsigned)ids, (unsigned)(s_c.alpha_min == 32u ? 0u : s_c.alpha_min));
    put(o, "viewports=");
    for (i = 0u; i < s_c.nview; i++) {
        put(o, "%s0x%08x", i == 0u ? "" : ",", (unsigned)s_c.viewport[i]);
    }
    put(o, "\n");
}

size_t HWCEN_Format(char *buf, size_t cap)
{
    Out o;
    char nm[48];
    uint32_t e, f, id;

    assert(buf != NULL && cap > 1u);
    o.buf = buf;
    o.cap = cap;
    o.len = 0u;
    buf[0] = '\0';
    put(&o, "# 3DP runtime feature census v1 (bet 2.1c)\n");
    put(&o, "# feature=<number of frames it was seen in>; exact counts drift between binaries (virtual time): compare seen / not seen\n");
    put(&o, "frames=%u\n", (unsigned)s_c.frames);
    for (e = 0u; e < 2u; e++) {
        for (f = 0u; f < N2D; f++) {
            name2(nm, sizeof nm, e, f);
            put(&o, "%s=%u\n", nm, (unsigned)s_c.n2[e][f]);
        }
    }
    for (f = 0u; f < N3D; f++) {
        put(&o, "%s=%u\n", k_name3[f], (unsigned)s_c.n3[f]);
    }
    put(&o, "# value sets (values seen, any frame)\n");
    for (id = 0u; id < NSETS; id++) {
        put_set(&o, id);
    }
    put_numbers(&o);
    put_cmds(&o);
    return o.len;
}
