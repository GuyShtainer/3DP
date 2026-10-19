/* hw_census_test.c -- bet 2.1c: the runtime feature census of source/hw/hw_census.c alone. Native, one thread, no SDK, no libctru.
 * Every register word and G3 word is built by hand from the SDK's OWN shift values and command ids: build.sh greps them out of ioreg_GX.h / ioreg_G2.h /
 * ioreg_G3.h / ioreg_G3X.h / g2_oam.h / g3.h into a generated census_sdk.h (the same method as the VRAM test), so a decode that drifts from the header fails here.
 * Prints "hw_census_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "census_sdk.h"
#include "hw_census.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

#define IOSZ  0x1100u
static uint8_t g_io[IOSZ];
static uint8_t g_oam[0x800];

static void w16(uint32_t off, uint32_t v)
{
    g_io[off] = (uint8_t)v;
    g_io[off + 1u] = (uint8_t)(v >> 8);
}

static void w32(uint32_t off, uint32_t v)
{
    w16(off, v & 0xffffu);
    w16(off + 2u, v >> 16);
}

static void oam_set(uint32_t engine, uint32_t i, uint32_t a0, uint32_t a1)
{
    uint8_t *p = g_oam + engine * 0x400u + 8u * i;

    p[0] = (uint8_t)a0;
    p[1] = (uint8_t)(a0 >> 8);
    p[2] = (uint8_t)a1;
    p[3] = (uint8_t)(a1 >> 8);
}

static void oam_hide_all(void)
{
    uint32_t e, i;

    for (e = 0; e < 2u; e++) {
        for (i = 0; i < 128u; i++) {
            oam_set(e, i, 2u << GX_OAM_ATTR01_RSENABLE_SHIFT, 0);
        }
    }
}

static uint32_t cnt(const char *n)
{
    check(HWCEN_IsFeature(n), n, 0);
    return HWCEN_Count(n);
}

static uint32_t pack(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return a | (b << 8) | (c << 16) | (d << 24);
}

static void fresh(void)
{
    HWCEN_Reset();
    memset(g_io, 0, sizeof g_io);
    memset(g_oam, 0, sizeof g_oam);
    oam_hide_all();
}

static void fifo(uint32_t w)
{
    HWCEN_GxWord(0u, w);
}

/* ---- the G3 stream ----------------------------------------------------------------------------------------------------------- */
static uint32_t polyattr(uint32_t id, uint32_t alpha, uint32_t fe, uint32_t dt, uint32_t d1, uint32_t fc, uint32_t xl, uint32_t fr, uint32_t bk,
                         uint32_t pm, uint32_t le)
{
    return (id << REG_G3_POLYGON_ATTR_ID_SHIFT) | (alpha << REG_G3_POLYGON_ATTR_ALPHA_SHIFT) | (fe << REG_G3_POLYGON_ATTR_FE_SHIFT) |
           (dt << REG_G3_POLYGON_ATTR_DT_SHIFT) | (d1 << REG_G3_POLYGON_ATTR_D1_SHIFT) | (fc << REG_G3_POLYGON_ATTR_FC_SHIFT) |
           (xl << REG_G3_POLYGON_ATTR_XL_SHIFT) | (fr << REG_G3_POLYGON_ATTR_FR_SHIFT) | (bk << REG_G3_POLYGON_ATTR_BK_SHIFT) |
           (pm << REG_G3_POLYGON_ATTR_PM_SHIFT) | (le << REG_G3_POLYGON_ATTR_LE_SHIFT);
}

static uint32_t teximage(uint32_t fmt, uint32_t gen, uint32_t s, uint32_t t, uint32_t rs, uint32_t rt, uint32_t fs, uint32_t ft, uint32_t tr)
{
    return (fmt << REG_G3_TEXIMAGE_PARAM_TEXFMT_SHIFT) | (gen << REG_G3_TEXIMAGE_PARAM_TGEN_SHIFT) | (s << REG_G3_TEXIMAGE_PARAM_V_SIZE_SHIFT) |
           (t << REG_G3_TEXIMAGE_PARAM_T_SIZE_SHIFT) | (rs << REG_G3_TEXIMAGE_PARAM_RS_SHIFT) | (rt << REG_G3_TEXIMAGE_PARAM_RT_SHIFT) |
           (fs << REG_G3_TEXIMAGE_PARAM_FS_SHIFT) | (ft << REG_G3_TEXIMAGE_PARAM_FT_SHIFT) | (tr << REG_G3_TEXIMAGE_PARAM_TR_SHIFT) | 0x123u;
}

/* bet 2.1e (review C-B1): POLYGON_ATTR fog-enable is bit 15 (FE), far-plane clipping bit 12 (FC). One bit per word, so a swap in hw_census.c fails. */
static void test_fog_far_bits(void)
{
    fresh();
    fifo(pack(G3OP_POLYGON_ATTR, G3OP_NOP, G3OP_NOP, G3OP_NOP));
    fifo(polyattr(0u, 31u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u));   /* FE only */
    check(HWCEN_FrameEnd(NULL, NULL) == 0, "fog/far frame end A", 0);
    check(cnt("poly_fog_enable") == 1u && cnt("poly_far_plane_intersect") == 0u, "FE=1 FC=0: only poly_fog_enable", (long)cnt("poly_far_plane_intersect"));
    fresh();
    fifo(pack(G3OP_POLYGON_ATTR, G3OP_NOP, G3OP_NOP, G3OP_NOP));
    fifo(polyattr(0u, 31u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u));   /* FC only */
    check(HWCEN_FrameEnd(NULL, NULL) == 0, "fog/far frame end B", 0);
    check(cnt("poly_fog_enable") == 0u && cnt("poly_far_plane_intersect") == 1u, "FE=0 FC=1: only poly_far_plane_intersect", (long)cnt("poly_fog_enable"));
}

static void test_packed_stream(void)
{
    uint32_t i;

    fresh();
    /* one packed word: MTX_MODE, MTX_IDENTITY (no parameters), POLYGON_ATTR, NOP; then the two parameters in order */
    fifo(pack(G3OP_MTX_MODE, G3OP_MTX_IDENTITY, G3OP_POLYGON_ATTR, G3OP_NOP));
    fifo(2u);   /* MTX_MODE = position + vector */
    fifo(polyattr(5u, 20u, 1u, 1u, 1u, 0u, 1u, 1u, 0u, 1u, 0x5u));   /* FE set, FC clear: ONE of the two bits, so a bit 12 / 15 swap fails the check below */
    /* MTX_LOAD_4x4 takes 16 words, then TEXIMAGE_PARAM (fmt 5 = 4x4 compressed, repeat S, flip T, colour-0 transparent, texgen normal) must still decode */
    fifo(pack(G3OP_MTX_LOAD_4x4, G3OP_TEXIMAGE_PARAM, G3OP_SHININESS, G3OP_NOP));
    for (i = 0; i < G3OP_MTX_LOAD_4x4_NPARAMS; i++) {
        fifo(0x1000u + i);
    }
    fifo(teximage(5u, 2u, 3u, 4u, 1u, 0u, 0u, 1u, 1u));
    for (i = 0; i < G3OP_SHININESS_NPARAMS; i++) {
        fifo(0x11111111u);
    }
    fifo(pack(G3OP_BEGIN, G3OP_END, G3OP_SWAP_BUFFERS, G3OP_NOP));
    fifo(1u);   /* quads */
    fifo((1u << REG_G3_SWAP_BUFFERS_DP_SHIFT) | (1u << REG_G3_SWAP_BUFFERS_XS_SHIFT));
    fifo(pack(G3OP_NOP, G3OP_NOP, G3OP_NOP, G3OP_NOP));   /* an all-NOP word takes no parameter */
    fifo(pack(G3OP_MTX_PUSH, G3OP_NOP, G3OP_NOP, G3OP_NOP));
    check(HWCEN_FrameEnd(NULL, NULL) == 0, "frame end without registers", 0);

    check(HWCEN_CmdFrames(G3OP_MTX_MODE) == 1u && HWCEN_CmdFrames(G3OP_MTX_IDENTITY) == 1u && HWCEN_CmdFrames(G3OP_POLYGON_ATTR) == 1u, "cmd frames 1", 0);
    check(HWCEN_CmdFrames(G3OP_MTX_LOAD_4x4) == 1u && HWCEN_CmdFrames(G3OP_TEXIMAGE_PARAM) == 1u && HWCEN_CmdFrames(G3OP_SHININESS) == 1u, "cmd frames 2", 0);
    check(HWCEN_CmdFrames(G3OP_BEGIN) == 1u && HWCEN_CmdFrames(G3OP_END) == 1u && HWCEN_CmdFrames(G3OP_SWAP_BUFFERS) == 1u && HWCEN_CmdFrames(G3OP_MTX_PUSH) == 1u,
          "cmd frames 3", 0);
    check(HWCEN_CmdFrames(G3OP_VIEWPORT) == 0u && HWCEN_CmdFrames(G3OP_BOX_TEST) == 0u, "unsent commands stay 0", 0);
    check(cnt("stream_unknown_command") == 0u, "no desync over the NPARAMS table", 0);
    /* POLYGON_ATTR: id 5, alpha 20 (translucent), FE DT D1 XL set (FC clear), front only (back culled), mode 1 = decal, lights 0 and 2 */
    check(cnt("poly_alpha_translucent_1_30") == 1u && cnt("poly_alpha_opaque_31") == 0u && cnt("poly_alpha_wireframe_0") == 0u, "alpha 20", 0);
    check(cnt("poly_cull_back") == 1u && cnt("poly_cull_none") == 0u && cnt("poly_cull_front") == 0u && cnt("poly_cull_all") == 0u, "back culled", 0);
    check(cnt("poly_depth_test_equal") == 1u && cnt("poly_fog_enable") == 1u && cnt("poly_render_1dot") == 1u && cnt("poly_far_plane_intersect") == 0u &&
          cnt("poly_translucent_depth_update") == 1u && cnt("poly_id_nonzero") == 1u, "polygon flags", 0);
    check(cnt("poly_mode_decal") == 1u && cnt("poly_mode_modulate") == 0u && HWCEN_Set("poly_modes") == (1u << 1), "decal mode", (long)HWCEN_Set("poly_modes"));
    check(cnt("poly_light_0") == 1u && cnt("poly_light_1") == 0u && cnt("poly_light_2") == 1u && cnt("poly_light_3") == 0u &&
          HWCEN_Set("poly_light_masks") == (1u << 5), "light mask 0b0101", (long)HWCEN_Set("poly_light_masks"));
    check(HWCEN_Set("mtx_modes") == (1u << 2) && cnt("mtx_mode_position_vector") == 1u && cnt("mtx_mode_projection") == 0u, "matrix mode 2", 0);
    /* TEXIMAGE_PARAM: format 5, texgen 2 (normal), S size 3, T size 4 */
    check(cnt("tex_fmt_5_comp4x4") == 1u && cnt("tex_fmt_3_pltt16") == 0u && HWCEN_Set("tex_formats") == (1u << 5), "format 5", (long)HWCEN_Set("tex_formats"));
    check(HWCEN_Set("tex_sizes_s") == (1u << 3) && HWCEN_Set("tex_sizes_t") == (1u << 4), "texture sizes", 0);
    check(cnt("tex_repeat_s") == 1u && cnt("tex_repeat_t") == 0u && cnt("tex_flip_s") == 0u && cnt("tex_flip_t") == 1u && cnt("tex_color0_transparent") == 1u,
          "repeat / flip / colour 0", 0);
    check(cnt("tex_texgen_normal") == 1u && cnt("tex_texgen_texcoord") == 0u && cnt("tex_texgen_vertex") == 0u, "texgen normal", 0);
    check(cnt("shininess_used") == 1u && cnt("prim_quads") == 1u && cnt("prim_triangles") == 0u && HWCEN_Set("prim_types") == (1u << 1), "shininess / quads", 0);
    check(cnt("swap_w_buffer") == 1u && cnt("swap_z_buffer") == 0u && cnt("swap_manual_sort") == 1u && cnt("swap_auto_sort") == 0u, "swap args", 0);
    check(HWCEN_CmdTotal(G3OP_POLYGON_ATTR) == 1u, "total counted once", 0);
}

static void test_ports_and_misc(void)
{
    uint32_t i;

    fresh();
    /* the command ports: id = (addr - 0x400) / 4; a 16-word matrix is ONE command; POLYGON_ATTR and SWAP via their ports */
    for (i = 0; i < G3OP_MTX_MULT_4x4_NPARAMS; i++) {
        HWCEN_GxWord(0x400u + 4u * G3OP_MTX_MULT_4x4, i);
    }
    HWCEN_GxWord(0x400u + 4u * G3OP_MTX_MULT_4x4, 99u);   /* the next matrix: counted again */
    HWCEN_GxWord(0x400u + 4u * G3OP_POLYGON_ATTR, polyattr(0u, 31u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 0u, 0u));
    HWCEN_GxWord(0x400u + 4u * G3OP_SWAP_BUFFERS, 0u);
    HWCEN_GxWord(0x400u + 4u * G3OP_VIEWPORT, 0xbfff0000u);
    HWCEN_GxWord(0x400u + 4u * G3OP_VIEWPORT, 0xbfff0000u);   /* the same viewport twice: one entry */
    HWCEN_GxWord(0x400u + 4u * G3OP_MTX_PUSH, 0u);
    HWCEN_GxWord(0x400u + 4u * G3OP_BOX_TEST, 1u);
    HWCEN_GxWord(0x400u + 4u * G3OP_BOX_TEST, 2u);
    HWCEN_GxWord(0x400u + 4u * G3OP_BOX_TEST, 3u);
    HWCEN_GxWord(0x400u + 4u * G3OP_POS_TEST, 1u);
    HWCEN_GxWord(0x400u + 4u * G3OP_POS_TEST, 2u);
    HWCEN_GxWord(0x400u + 4u * G3OP_VEC_TEST, 1u);
    HWCEN_GxWord(0x400u + 4u * G3OP_LIGHT_COLOR, 3u << REG_G3_LIGHT_COLOR_LNUM_SHIFT);
    HWCEN_GxWord(0x400u + 4u * G3OP_LIGHT_VECTOR, 1u << REG_G3_LIGHT_VECTOR_LNUM_SHIFT);
    HWCEN_GxWord(0x400u + 4u * G3OP_SPE_EMI, 1u << REG_G3_SPE_EMI_S_SHIFT);
    HWCEN_GxWord(0x400u + 4u * G3OP_DIF_AMB, 1u << REG_G3_DIF_AMB_C_SHIFT);
    HWCEN_GxWord(0x4f0u, 1u);    /* a port with no command behind it */
    HWCEN_GxWord(0x3f0u, 1u);    /* outside the command ports: ignored */
    HWCEN_GxWord(0x610u, 1u);
    (void)HWCEN_FrameEnd(NULL, NULL);
    check(HWCEN_CmdTotal(G3OP_MTX_MULT_4x4) == 2u && HWCEN_CmdFrames(G3OP_MTX_MULT_4x4) == 1u, "17 port stores = 2 matrix commands", (long)HWCEN_CmdTotal(G3OP_MTX_MULT_4x4));
    check(cnt("poly_alpha_opaque_31") == 1u && cnt("poly_cull_none") == 1u && cnt("poly_mode_modulate") == 1u && cnt("poly_id_nonzero") == 0u, "port POLYGON_ATTR", 0);
    check(cnt("swap_z_buffer") == 1u && cnt("swap_auto_sort") == 1u, "port SWAP_BUFFERS", 0);
    check(cnt("viewport_used") == 1u && HWCEN_CmdTotal(G3OP_VIEWPORT) == 2u, "viewport", 0);
    check(cnt("box_test_used") == 1u && HWCEN_CmdTotal(G3OP_BOX_TEST) == 1u && cnt("pos_test_used") == 1u && HWCEN_CmdTotal(G3OP_POS_TEST) == 1u &&
          cnt("vec_test_used") == 1u, "tests", 0);
    check(cnt("light_color_3") == 1u && cnt("light_vector_1") == 1u && cnt("light_color_0") == 0u && cnt("spe_emi_shininess_table_bit") == 1u &&
          cnt("dif_amb_set_vertex_color") == 1u, "light numbers / flag bits", 0);
    check(cnt("stream_unknown_command") == 1u, "unknown port counted", 0);
    for (i = 0; i < 12u; i++) {   /* more distinct viewports than the table holds: bounded */
        HWCEN_GxWord(0x400u + 4u * G3OP_VIEWPORT, 0x1000u + i);
    }
    {
        char buf[HWCEN_TEXT_MAX];
        const char *vp;

        (void)HWCEN_Format(buf, sizeof buf);
        vp = strstr(buf, "viewports=");
        check(vp != NULL, "viewports line", 0);
        if (vp != NULL) {
            int commas = 0;
            const char *p = vp;

            while (*p != '\n' && *p != '\0') {
                commas += *p++ == ',';
            }
            check(commas == (int)HWCEN_VIEWPORTS - 1, "viewport list is bounded to 8 entries", commas);
        }
    }
}

static void test_stream_resync_and_unknown(void)
{
    fresh();
    /* an unknown id (0x05) is flagged and skipped without eating a parameter; the next known command still sees its argument */
    fifo(pack(0x05u, G3OP_POLYGON_ATTR, G3OP_NOP, G3OP_NOP));
    fifo(polyattr(0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 3u, 0u));
    (void)HWCEN_FrameEnd(NULL, NULL);
    check(cnt("stream_unknown_command") == 1u, "unknown id flagged", 0);
    check(cnt("poly_alpha_wireframe_0") == 1u && cnt("poly_cull_front") == 1u && cnt("poly_mode_shadow") == 1u, "decode continues after it", 0);
    /* a command word split across frames: the parameter in the next frame still belongs to it */
    fifo(pack(G3OP_TEXIMAGE_PARAM, G3OP_NOP, G3OP_NOP, G3OP_NOP));
    (void)HWCEN_FrameEnd(NULL, NULL);
    fifo(teximage(1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u));
    (void)HWCEN_FrameEnd(NULL, NULL);
    check(cnt("tex_fmt_1_a3i5") == 1u, "parameter after a frame boundary", 0);
    check(HWCEN_CmdFrames(G3OP_TEXIMAGE_PARAM) == 1u, "command counted in the frame it started", 0);
}

static void test_param_table_matches_sdk(void)
{
    /* every command with its NPARAMS from g3.h, back to back in one packed stream: any wrong count desynchronises the NEXT word's decode */
    static const struct { uint32_t op, n; } k[] = {
        { G3OP_MTX_MODE, G3OP_MTX_MODE_NPARAMS }, { G3OP_MTX_PUSH, G3OP_MTX_PUSH_NPARAMS }, { G3OP_MTX_POP, G3OP_MTX_POP_NPARAMS },
        { G3OP_MTX_STORE, G3OP_MTX_STORE_NPARAMS }, { G3OP_MTX_RESTORE, G3OP_MTX_RESTORE_NPARAMS }, { G3OP_MTX_IDENTITY, G3OP_MTX_IDENTITY_NPARAMS },
        { G3OP_MTX_LOAD_4x4, G3OP_MTX_LOAD_4x4_NPARAMS }, { G3OP_MTX_LOAD_4x3, G3OP_MTX_LOAD_4x3_NPARAMS }, { G3OP_MTX_MULT_4x4, G3OP_MTX_MULT_4x4_NPARAMS },
        { G3OP_MTX_MULT_4x3, G3OP_MTX_MULT_4x3_NPARAMS }, { G3OP_MTX_MULT_3x3, G3OP_MTX_MULT_3x3_NPARAMS }, { G3OP_MTX_SCALE, G3OP_MTX_SCALE_NPARAMS },
        { G3OP_MTX_TRANS, G3OP_MTX_TRANS_NPARAMS }, { G3OP_COLOR, G3OP_COLOR_NPARAMS }, { G3OP_NORMAL, G3OP_NORMAL_NPARAMS },
        { G3OP_TEXCOORD, G3OP_TEXCOORD_NPARAMS }, { G3OP_VTX_16, G3OP_VTX_16_NPARAMS }, { G3OP_VTX_10, G3OP_VTX_10_NPARAMS }, { G3OP_VTX_XY, G3OP_VTX_XY_NPARAMS },
        { G3OP_VTX_XZ, G3OP_VTX_XZ_NPARAMS }, { G3OP_VTX_YZ, G3OP_VTX_YZ_NPARAMS }, { G3OP_VTX_DIFF, G3OP_VTX_DIFF_NPARAMS },
        { G3OP_POLYGON_ATTR, G3OP_POLYGON_ATTR_NPARAMS }, { G3OP_TEXIMAGE_PARAM, G3OP_TEXIMAGE_PARAM_NPARAMS }, { G3OP_TEXPLTT_BASE, G3OP_TEXPLTT_BASE_NPARAMS },
        { G3OP_DIF_AMB, G3OP_DIF_AMB_NPARAMS }, { G3OP_SPE_EMI, G3OP_SPE_EMI_NPARAMS }, { G3OP_LIGHT_VECTOR, G3OP_LIGHT_VECTOR_NPARAMS },
        { G3OP_LIGHT_COLOR, G3OP_LIGHT_COLOR_NPARAMS }, { G3OP_SHININESS, G3OP_SHININESS_NPARAMS }, { G3OP_BEGIN, G3OP_BEGIN_NPARAMS },
        { G3OP_END, G3OP_END_NPARAMS }, { G3OP_SWAP_BUFFERS, G3OP_SWAP_BUFFERS_NPARAMS }, { G3OP_VIEWPORT, G3OP_VIEWPORT_NPARAMS },
        { G3OP_BOX_TEST, G3OP_BOX_TEST_NPARAMS }, { G3OP_POS_TEST, G3OP_POS_TEST_NPARAMS }, { G3OP_VEC_TEST, G3OP_VEC_TEST_NPARAMS }
    };
    size_t i;
    uint32_t j;

    fresh();
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        fifo(pack(k[i].op, G3OP_NOP, G3OP_NOP, G3OP_NOP));
        for (j = 0; j < k[i].n; j++) {
            fifo(0u);
        }
    }
    fifo(pack(G3OP_POLYGON_ATTR, G3OP_NOP, G3OP_NOP, G3OP_NOP));   /* the sentinel: a decode that is out of step would read a parameter 0 or miss it */
    fifo(polyattr(1u, 31u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 0u, 0u));
    (void)HWCEN_FrameEnd(NULL, NULL);
    check(cnt("stream_unknown_command") == 0u, "no unknown command", 0);
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        check(HWCEN_CmdFrames(k[i].op) == 1u, "every table command seen once", (long)k[i].op);
    }
    check(HWCEN_CmdTotal(G3OP_POLYGON_ATTR) == 2u && cnt("poly_id_nonzero") == 1u && cnt("poly_cull_none") == 1u, "sentinel decoded in step", 0);
}

/* ---- 2D registers ------------------------------------------------------------------------------------------------------------ */
static uint32_t dispcnt(uint32_t mode, uint32_t bgmode, uint32_t en_mask, uint32_t extra)
{
    return (mode << REG_GX_DISPCNT_MODE_SHIFT) | (bgmode << REG_GX_DISPCNT_BGMODE_SHIFT) | (en_mask << REG_GX_DISPCNT_DISPLAY_SHIFT) | extra;
}

static void test_2d_main(void)
{
    const uint32_t en_all = 0x1fu;   /* BG0-3 + OBJ */

    fresh();
    /* mode 5: BG0 = 3D layer, BG1 text 256 colours with an ext-palette slot bit and mosaic, BG2 extended 256-colour bitmap, BG3 extended direct-colour bitmap */
    w32(0, dispcnt(1u, 5u, en_all, (1u << REG_GX_DISPCNT_BG02D3D_SHIFT) | (1u << REG_GX_DISPCNT_BG_SHIFT) | (1u << REG_GX_DISPCNT_O_SHIFT) |
                                       (1u << REG_GX_DISPCNT_W0_SHIFT) | (1u << REG_GX_DISPCNT_OW_SHIFT) | (1u << REG_GX_DISPCNT_OBJMAP_SHIFT) |
                                       (2u << REG_GX_DISPCNT_EXOBJ_CH_SHIFT)));
    w16(0x08, 1u << REG_G2_BG0CNT_PRIORITY_SHIFT);
    w16(0x0a, (1u << REG_G2_BG1CNT_COLORMODE_SHIFT) | (1u << REG_G2_BG1CNT_BGPLTTSLOT_SHIFT) | (1u << REG_G2_BG1CNT_MOSAIC_SHIFT) |
              (2u << REG_G2_BG1CNT_SCREENSIZE_SHIFT));
    w16(0x0c, (1u << REG_G2_BG2CNT_COLORMODE_SHIFT) | (0u << REG_G2_BG2CNT_CHARBASE_SHIFT) | (3u << REG_G2_BG2CNT_SCREENSIZE_SHIFT));
    w16(0x0e, (1u << REG_G2_BG3CNT_COLORMODE_SHIFT) | (1u << REG_G2_BG3CNT_CHARBASE_SHIFT) | (1u << REG_G2_BG3CNT_SCREENSIZE_SHIFT));
    w16(0x4c, 0x0011u);   /* MOSAIC */
    /* BLDCNT: effect 1 (alpha), first target BG1, second target BG2; EVA 8, EVB 8 */
    w16(0x50, (1u << REG_G2_BLDCNT_EFFECT_SHIFT) | (2u << REG_G2_BLDCNT_PLANE1_SHIFT) | (4u << REG_G2_BLDCNT_PLANE2_SHIFT));
    w16(0x52, (8u << REG_G2_BLDALPHA_EVB_SHIFT) | (8u << REG_G2_BLDALPHA_EVA_SHIFT));
    w16(0x6c, (1u << REG_GX_MASTER_BRIGHT_E_MOD_SHIFT) | (7u << REG_GX_MASTER_BRIGHT_E_VALUE_SHIFT));
    w32(0x64, (1u << REG_GX_DISPCAPCNT_E_SHIFT) | (2u << REG_GX_DISPCAPCNT_MOD_SHIFT) | (1u << REG_GX_DISPCAPCNT_SRCA_SHIFT));
    {
        uint8_t io0[IOSZ], oam0[0x800];

        memcpy(io0, g_io, sizeof io0);
        memcpy(oam0, g_oam, sizeof oam0);
        check(HWCEN_FrameEnd(g_io, g_oam) == 0, "no dump due on frame 1", 0);
        check(memcmp(io0, g_io, sizeof io0) == 0 && memcmp(oam0, g_oam, sizeof oam0) == 0, "the census does not write the registers or the OAM", 0);
    }
    check(cnt("A_dispmode_graphics") == 1u && cnt("A_dispmode_off") == 0u && cnt("A_bgmode_5") == 1u && HWCEN_Set("bg_modes_A") == (1u << 5), "mode 1, bg mode 5", 0);
    check(cnt("A_bg0_3d") == 1u && cnt("A_bg0_text") == 0u, "BG0 is the 3D layer", 0);
    check(cnt("A_bg1_text") == 1u && cnt("A_bg1_256col") == 1u && cnt("A_bg1_16col") == 0u && cnt("A_bg1_extpltt_slot_bit") == 1u && cnt("A_bg1_mosaic_bit") == 1u,
          "BG1 text 256 colours + slot + mosaic", 0);
    check(HWCEN_Set("bg1_size_A") == (1u << 2), "BG1 size", 0);
    check(cnt("A_bg2_ext_bmp256") == 1u && cnt("A_bg2_ext_map") == 0u && cnt("A_bg3_ext_bmpdc") == 1u && cnt("A_bg3_ext_bmp256") == 0u, "extended kinds", 0);
    check(HWCEN_Set("bg2_size_A") == (1u << 3) && HWCEN_Set("bg3_size_A") == (1u << 1), "ext sizes", 0);
    check(cnt("A_bg_extpltt_enabled") == 1u && cnt("A_obj_extpltt_enabled") == 1u && cnt("A_win0") == 1u && cnt("A_win1") == 0u && cnt("A_objwin") == 1u, "ext palettes / windows", 0);
    check(cnt("A_obj_enabled") == 1u && cnt("A_obj_tilemap_1d") == 1u && cnt("A_obj_1d_boundary_2") == 1u && cnt("A_obj_tilemap_2d") == 0u, "OBJ mapping", 0);
    check(cnt("A_bld_alpha") == 1u && cnt("A_bld_none") == 0u && cnt("A_bld_eva_nonzero") == 1u && cnt("A_bld_evb_nonzero") == 1u && cnt("A_bld_evy_nonzero") == 0u,
          "alpha blend", 0);
    check(cnt("A_mosaic_reg_nonzero") == 1u && cnt("A_master_bright_up") == 1u && cnt("A_master_bright_down") == 0u, "mosaic / brightness", 0);
    check(cnt("A_dispcap_enable") == 1u && cnt("A_dispcap_mode_2") == 1u && cnt("A_dispcap_mode_0") == 0u && cnt("A_dispcap_src_3d_only") == 1u, "display capture", 0);
    check(cnt("B_dispmode_off") == 1u && cnt("B_dispcap_enable") == 0u, "engine B is off", 0);
    check(cnt("3d_active_on_bg0") == 1u, "3D active", 0);
}

static void test_2d_modes(void)
{
    fresh();
    w32(0, dispcnt(1u, 2u, 0xfu, 0));   /* mode 2: BG2 and BG3 affine, BG0/1 text 16 colours */
    w32(0x1000, dispcnt(1u, 6u, 0x4u, 0));   /* sub engine, mode 6: BG2 large bitmap */
    w16(0x50, (2u << REG_G2_BLDCNT_EFFECT_SHIFT) | (1u << REG_G2_BLDCNT_PLANE1_SHIFT));
    w16(0x54, 9u);   /* BLDY */
    w16(0x1050, 3u << REG_G2_BLDCNT_EFFECT_SHIFT);   /* darken with no first target: not "in use" */
    w16(0x1054, 5u);
    w16(0x106c, (2u << REG_GX_MASTER_BRIGHT_E_MOD_SHIFT) | (3u << REG_GX_MASTER_BRIGHT_E_VALUE_SHIFT));
    w32(0x1064, 0x80000000u);   /* there is no DISPCAPCNT on the sub engine */
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("A_bg0_text") == 1u && cnt("A_bg0_16col") == 1u && cnt("A_bg1_text") == 1u && cnt("A_bg2_affine") == 1u && cnt("A_bg3_affine") == 1u &&
          cnt("A_bg2_text") == 0u && cnt("A_bg0_3d") == 0u, "mode 2 BG kinds", 0);
    check(cnt("A_bld_brighten") == 1u && cnt("A_bld_evy_nonzero") == 1u && cnt("B_bld_darken") == 1u && cnt("B_bld_evy_nonzero") == 0u, "brighten / unused darken", 0);
    check(cnt("B_bg2_large_bmp") == 1u && cnt("B_bg0_text") == 0u && cnt("B_bg1_text") == 0u && cnt("B_bgmode_6") == 1u, "sub mode 6 large bitmap", 0);
    check(cnt("B_master_bright_down") == 1u && cnt("B_dispcap_enable") == 0u && cnt("3d_active_on_bg0") == 0u, "sub brightness, no capture, no 3D", 0);
    /* forced blank and the other display modes record only the mode */
    w32(0, dispcnt(1u, 0u, 0x1fu, 1u << REG_GX_DISPCNT_BLANK_SHIFT));
    w32(0x1000, dispcnt(2u, 0u, 0x1fu, 0));
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("A_forced_blank") == 1u && cnt("B_dispmode_vramdisplay") == 1u && cnt("A_bgmode_0") == 0u && cnt("B_bgmode_0") == 0u, "blank / VRAM display", 0);
    w32(0, dispcnt(3u, 0u, 0x1fu, 0));
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("A_dispmode_mainmem") == 1u, "main-memory display", 0);
}

static void test_oam(void)
{
    uint32_t a0;

    fresh();
    w32(0, dispcnt(1u, 0u, 0x10u, 0));   /* only OBJ enabled; 2D tile mapping */
    oam_set(0, 0, (0u << REG_G2_BG0CNT_PRIORITY_SHIFT) | 10u, 20u);   /* a plain 8x8, 16 colours */
    a0 = 30u | (1u << GX_OAM_ATTR01_RSENABLE_SHIFT) | (1u << GX_OAM_ATTR01_MODE_SHIFT) | (1u << GX_OAM_ATTR01_CM_SHIFT);   /* affine, semi-transparent, 256 colours */
    oam_set(0, 1, a0, 40u | (1u << 14));   /* square size 1 = 16x16 */
    a0 = 50u | (3u << GX_OAM_ATTR01_RSENABLE_SHIFT) | (2u << GX_OAM_ATTR01_MODE_SHIFT) | (1u << GX_OAM_ATTR01_MOSAIC_SHIFT) | (1u << GX_OAM_ATTR01_SHAPE_SHIFT);
    oam_set(0, 2, a0, 60u | (3u << 14));   /* double-size affine, OBJ-window mode, mosaic, wide 64x32 */
    a0 = 70u | (3u << GX_OAM_ATTR01_MODE_SHIFT) | (2u << GX_OAM_ATTR01_SHAPE_SHIFT);
    oam_set(0, 3, a0, 80u | (1u << 12) | (1u << 13) | (3u << 14));   /* bitmap mode, tall 32x64, h and v flip */
    oam_set(0, 4, 20u | (2u << GX_OAM_ATTR01_RSENABLE_SHIFT), 20u);   /* hidden (NODISPLAY): not counted */
    oam_set(0, 5, 200u, 20u);                                          /* below the screen: not counted */
    oam_set(0, 6, 20u, 300u);                                          /* x = 300 is -212 after the 9-bit wrap: 8 wide, off the left edge: not counted */
    oam_set(1, 0, 1u, 1u);                                             /* the sub engine's OBJ is not enabled: nothing counted */
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("A_obj_enabled") == 1u && cnt("A_obj_tilemap_2d") == 1u && cnt("A_obj_tilemap_1d") == 0u, "OBJ enabled, 2D map", 0);
    check(cnt("A_spr_visible") == 1u && cnt("A_spr_16col") == 1u && cnt("A_spr_256col") == 1u, "sprites seen, both colour modes", 0);
    check(cnt("A_spr_affine") == 1u && cnt("A_spr_affine_double") == 1u && cnt("A_spr_mode_normal") == 1u && cnt("A_spr_mode_alpha") == 1u &&
          cnt("A_spr_mode_window") == 1u && cnt("A_spr_mode_bitmap") == 1u, "affine / double / OBJ modes", 0);
    check(cnt("A_spr_mosaic") == 1u && cnt("A_spr_hflip") == 1u && cnt("A_spr_vflip") == 1u, "mosaic / flips", 0);
    check(cnt("A_spr_shape_square") == 1u && cnt("A_spr_shape_wide") == 1u && cnt("A_spr_shape_tall") == 1u, "shapes", 0);
    check(HWCEN_Set("obj_shapes_A") == ((1u << 0) | (1u << 1) | (1u << 7) | (1u << 11)), "shape x size set", (long)HWCEN_Set("obj_shapes_A"));
    check(cnt("B_spr_visible") == 0u, "sub OBJ disabled", 0);
    {
        char buf[HWCEN_TEXT_MAX];

        (void)HWCEN_Format(buf, sizeof buf);
        check(strstr(buf, "obj_shapes_A=8x8,16x16,64x32,32x64\n") != NULL, "shape labels in the text", 0);
        check(strstr(buf, "obj_visible_peak_A=4\n") != NULL, "peak visible sprites = 4 (hidden / off-screen skipped)", 0);
    }
}

static void test_3d_registers(void)
{
    uint32_t i;
    char buf[HWCEN_TEXT_MAX];

    fresh();
    /* 3D is not on BG0: the DISP3DCNT state is stale and is not recorded */
    w16(0x60, 0xffffu);
    w32(0, dispcnt(1u, 0u, 0x1u, 0));
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("3d_active_on_bg0") == 0u && cnt("disp3dcnt_texture_mapping") == 0u, "stale 3D state ignored", 0);
    /* BG0 on, bit 3 set: every DISP3DCNT bit, fog shift 5, rear plane bitmap, and the table registers */
    w32(0, dispcnt(1u, 0u, 0x1u, 1u << REG_GX_DISPCNT_BG02D3D_SHIFT));
    w16(0x60, (1u << REG_G3X_DISP3DCNT_TME_SHIFT) | (1u << REG_G3X_DISP3DCNT_THS_SHIFT) | (1u << REG_G3X_DISP3DCNT_ATE_SHIFT) | (1u << REG_G3X_DISP3DCNT_ABE_SHIFT) |
              (1u << REG_G3X_DISP3DCNT_AAE_SHIFT) | (1u << REG_G3X_DISP3DCNT_EME_SHIFT) | (1u << REG_G3X_DISP3DCNT_FMOD_SHIFT) | (1u << REG_G3X_DISP3DCNT_FME_SHIFT) |
              (5u << REG_G3X_DISP3DCNT_FOG_SHIFT_SHIFT) | (1u << REG_G3X_DISP3DCNT_PRI_SHIFT));
    w32(0x350, (20u << REG_G3X_CLEAR_COLOR_ALPHA_SHIFT) | (1u << REG_G3X_CLEAR_COLOR_F_SHIFT) | (9u << REG_G3X_CLEAR_COLOR_POLYGONID_SHIFT));
    g_io[0x333] = 0x7c;   /* EDGE_COLOR table */
    g_io[0x37e] = 1;      /* FOG_TABLE */
    g_io[0x3bf] = 1;      /* TOON_TABLE */
    w32(0x358, 0x1f0000u);
    w16(0x35c, 0x400u);
    w16(0x340, 12u);
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("3d_active_on_bg0") == 1u, "3D active", 0);
    check(cnt("disp3dcnt_texture_mapping") == 1u && cnt("disp3dcnt_toon_highlight") == 1u && cnt("disp3dcnt_alpha_test") == 1u && cnt("disp3dcnt_alpha_blend") == 1u &&
          cnt("disp3dcnt_antialias") == 1u && cnt("disp3dcnt_edge_marking") == 1u && cnt("disp3dcnt_fog_mode_alpha_only") == 1u && cnt("disp3dcnt_fog_enable") == 1u &&
          cnt("disp3dcnt_rear_plane_bitmap") == 1u && cnt("disp3dcnt_fog_shift_nonzero") == 1u && HWCEN_Set("disp3dcnt_fog_shift") == (1u << 5), "DISP3DCNT bits", 0);
    check(cnt("clear_alpha_lt31") == 1u && cnt("clear_fog_bit") == 1u && cnt("clear_polygon_id_nonzero") == 1u, "CLEAR_COLOR", 0);
    check(cnt("edge_color_table_written") == 1u && cnt("fog_table_written") == 1u && cnt("toon_table_written") == 1u && cnt("fog_color_nonzero") == 1u &&
          cnt("fog_offset_nonzero") == 1u && cnt("alpha_test_ref_nonzero") == 1u, "table registers", 0);
    /* a frame with a clear alpha of 31 and nothing else: the "seen in N frames" counts add up per frame, not per sample */
    w16(0x60, 0);
    w32(0x350, 31u << REG_G3X_CLEAR_COLOR_ALPHA_SHIFT);
    memset(g_io + 0x330, 0, 16);
    (void)HWCEN_FrameEnd(g_io, g_oam);
    check(cnt("3d_active_on_bg0") == 2u && cnt("clear_alpha_lt31") == 1u && cnt("disp3dcnt_texture_mapping") == 1u, "per-frame counters", 0);
    /* the text: every feature appears exactly once (names are unique), zero features are listed, the sets and the commands follow */
    (void)HWCEN_Format(buf, sizeof buf);
    check(strstr(buf, "A_bg1_text=0\n") != NULL && strstr(buf, "tex_fmt_5_comp4x4=0\n") != NULL && strstr(buf, "frames=3\n") != NULL, "zeros are listed", 0);
    check(strstr(buf, "cmd_POLYGON_ATTR=0\n") != NULL && strstr(buf, "# value sets") != NULL, "commands and sets are in the text", 0);
    for (i = 0; i < 3u; i++) {   /* unique names: the first occurrence of "name=" and a later one must not both exist */
        const char *n = i == 0u ? "\nA_dispmode_graphics=" : (i == 1u ? "\ntex_fmt_5_comp4x4=" : "\n3d_active_on_bg0=");
        const char *p = strstr(buf, n);

        check(p != NULL && strstr(p + 1, n) == NULL, "name appears once", (long)i);
    }
}

static void test_vram_and_frames(void)
{
    uint32_t f;
    char small[64];
    size_t n;

    fresh();
    HWCEN_VramCnt(7u, 0x80u | 2u);              /* bank H enabled, MST 2 */
    HWCEN_VramCnt(7u, 0x80u | 1u);              /* then MST 1 */
    HWCEN_VramCnt(7u, 0x00u);                   /* disabled: not recorded */
    HWCEN_VramCnt(4u, 0x80u | (2u << 3) | 3u);  /* bank E: MST 3, OFS 2 */
    HWCEN_VramCnt(9u, 0x81u);                   /* a bank that does not exist: ignored */
    check(HWCEN_Set("vram_mst_H") == ((1u << 2) | (1u << 1)) && HWCEN_Set("vram_mst_E") == (1u << 3) && HWCEN_Set("vram_ofs_E") == (1u << 2) &&
          HWCEN_Set("vram_mst_A") == 0u, "VRAMCNT sets", (long)HWCEN_Set("vram_mst_H"));
    check(HWCEN_IsSet("vram_mst_I") && HWCEN_IsSet("tex_formats") && HWCEN_IsSet("bg3_size_B") && !HWCEN_IsSet("bg4_size_A") && !HWCEN_IsFeature("nonsense"), "name lookup", 0);
    for (f = 1u; f <= 600u; f++) {
        int due = HWCEN_FrameEnd(g_io, g_oam);

        check(due == (f % HWCEN_DUMP_FRAMES == 0u), "periodic dump due every 300 frames", (long)f);
    }
    check(HWCEN_Frames() == 600u && cnt("B_dispmode_off") == 600u, "600 frames", (long)HWCEN_Frames());
    n = HWCEN_Format(small, sizeof small);   /* truncation: NUL-terminated, never past the buffer */
    check(n == sizeof small - 1u && small[n] == '\0' && strncmp(small, "# 3DP", 5) == 0, "bounded output", (long)n);
    HWCEN_Reset();
    check(HWCEN_Frames() == 0u && HWCEN_Set("vram_mst_H") == 0u && cnt("B_dispmode_off") == 0u && HWCEN_CmdTotal(G3OP_POLYGON_ATTR) == 0u, "reset", 0);
    check(HWCEN_Features() > 100u, "feature table size", (long)HWCEN_Features());
    (void)HWCEN_FrameEnd(NULL, NULL);
    check(HWCEN_Frames() == 1u && cnt("A_dispmode_off") == 0u, "no registers: nothing sampled", 0);
    check(HWCEN_CmdFrames(0x7fu) == 0u && HWCEN_CmdFrames(0x1000u) == 0u && HWCEN_Count("nonsense") == 0u && HWCEN_Set("nonsense") == 0u, "out-of-range lookups", 0);
}

/* Bet 2.1c2: HWCEN_GxBlock must give exactly the text of the per-word path, whatever the chunking (a command's parameters may be split across blocks). */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void test_block_equivalence(void)
{
    static const uint8_t ids[] = { 0x10, 0x11, 0x12, 0x15, 0x16, 0x1b, 0x20, 0x21, 0x22, 0x23, 0x24, 0x29, 0x2a, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41, 0x50, 0x60, 0x70, 0x71, 0x72, 0x05, 0x7f, 0x00 };
    static uint32_t words[40000];
    static char a[HWCEN_TEXT_MAX], b[HWCEN_TEXT_MAX];
    uint32_t seed = 12345u, n = 0u, i, round;

    while (n + 64u < sizeof words / sizeof words[0]) {
        uint32_t p = pack(ids[lcg(&seed) % sizeof ids], ids[lcg(&seed) % sizeof ids], ids[lcg(&seed) % sizeof ids], ids[lcg(&seed) % sizeof ids]), k;

        words[n++] = p;
        for (k = 0u; k < 70u && n < sizeof words / sizeof words[0]; k++) {   /* more than any command takes: the surplus is the next packed word, as the DS FIFO would see it */
            words[n++] = lcg(&seed) * 2654435761u;
            if ((lcg(&seed) & 7u) == 0u) {
                break;
            }
        }
    }
    fresh();
    for (i = 0; i < n; i++) {
        fifo(words[i]);
    }
    (void)HWCEN_FrameEnd(g_io, g_oam);
    HWCEN_Format(a, sizeof a);
    for (round = 0; round < 6u; round++) {
        uint32_t pos = 0u;

        fresh();
        while (pos < n) {
            uint32_t len = round == 0u ? 1u : 1u + lcg(&seed) % (round == 5u ? 4000u : 7u * round);

            if (len > n - pos) {
                len = n - pos;
            }
            HWCEN_GxBlock(words + pos, len);
            pos += len;
        }
        (void)HWCEN_FrameEnd(g_io, g_oam);
        HWCEN_Format(b, sizeof b);
        check(strcmp(a, b) == 0, "GxBlock chunking gives the per-word text", round);
    }
    check(strstr(a, "cmdtotal_VTX_16=") != NULL, "the stream hit commands", 0);
}

int main(void)
{
    HWCEN_Reset();
    test_packed_stream();
    test_fog_far_bits();
    test_ports_and_misc();
    test_stream_resync_and_unknown();
    test_param_table_matches_sdk();
    test_2d_main();
    test_2d_modes();
    test_oam();
    test_3d_registers();
    test_vram_and_frames();
    test_block_equivalence();
    printf("hw_census_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
