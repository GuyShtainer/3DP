/* gpu3d.c -- the DS geometry engine on the CPU, slice (a): command decoder, matrix stacks, computed readbacks. See gpu3d.h and the private design notes.
 * Pure C. Fixed point throughout: 20.12 matrices, products accumulated in 64 bits and shifted right by 12 (floor), stored back in 32 bits. */
#include <assert.h>
#include <string.h>

#include "gpu3d.h"

/* ---- the command table (GBATEK "DS 3D Video", "3D Commands": opcode, parameter count) ------------------------------------------------------------------ */
typedef struct CmdInfo {
    uint8_t op, nparams;
    const char *name;
} CmdInfo;

static const CmdInfo k_cmds[] = {
    { 0x00, 0, "NOP" },
    { 0x10, 1, "MTX_MODE" }, { 0x11, 0, "MTX_PUSH" }, { 0x12, 1, "MTX_POP" }, { 0x13, 1, "MTX_STORE" }, { 0x14, 1, "MTX_RESTORE" }, { 0x15, 0, "MTX_IDENTITY" },
    { 0x16, 16, "MTX_LOAD_4x4" }, { 0x17, 12, "MTX_LOAD_4x3" }, { 0x18, 16, "MTX_MULT_4x4" }, { 0x19, 12, "MTX_MULT_4x3" }, { 0x1a, 9, "MTX_MULT_3x3" },
    { 0x1b, 3, "MTX_SCALE" }, { 0x1c, 3, "MTX_TRANS" },
    { 0x20, 1, "COLOR" }, { 0x21, 1, "NORMAL" }, { 0x22, 1, "TEXCOORD" }, { 0x23, 2, "VTX_16" }, { 0x24, 1, "VTX_10" }, { 0x25, 1, "VTX_XY" }, { 0x26, 1, "VTX_XZ" },
    { 0x27, 1, "VTX_YZ" }, { 0x28, 1, "VTX_DIFF" }, { 0x29, 1, "POLYGON_ATTR" }, { 0x2a, 1, "TEXIMAGE_PARAM" }, { 0x2b, 1, "PLTT_BASE" },
    { 0x30, 1, "DIF_AMB" }, { 0x31, 1, "SPE_EMI" }, { 0x32, 1, "LIGHT_VECTOR" }, { 0x33, 1, "LIGHT_COLOR" }, { 0x34, 32, "SHININESS" },
    { 0x40, 1, "BEGIN_VTXS" }, { 0x41, 0, "END_VTXS" }, { 0x50, 1, "SWAP_BUFFERS" }, { 0x60, 1, "VIEWPORT" },
    { 0x70, 3, "BOX_TEST" }, { 0x71, 2, "POS_TEST" }, { 0x72, 1, "VEC_TEST" },
};
#define NCMDS ((uint32_t)(sizeof k_cmds / sizeof k_cmds[0]))

static const CmdInfo *cmd_info(uint32_t op)
{
    uint32_t i;

    for (i = 0u; i < NCMDS; i++) {
        if (k_cmds[i].op == op) {
            return &k_cmds[i];
        }
    }
    return NULL;
}

/* one byte per opcode: the parameter count, or 0xff for an opcode GBATEK does not list (the executor asks this twice per command: a table, not a search); s_npx the same with
   the unlisted opcodes at 0. gpu3d_reset builds them, so the executor reads them without a check. */
static uint8_t s_np[256], s_npx[256];
static uint32_t s_np_ready;

static void np_build(void)
{
    uint32_t i;

    memset(s_np, 0xff, sizeof s_np);
    for (i = 0u; i < NCMDS; i++) {
        s_np[k_cmds[i].op] = (uint8_t)k_cmds[i].nparams;
    }
    for (i = 0u; i < 256u; i++) {
        s_npx[i] = s_np[i] == 0xffu ? 0u : s_np[i];
    }
    s_np_ready = 1u;
}

uint32_t gpu3d_nparams(uint32_t cmd)
{
    if (s_np_ready == 0u) {
        np_build();
    }
    return s_npx[cmd & 0xffu];
}

int gpu3d_known(uint32_t cmd)
{
    if (s_np_ready == 0u) {
        np_build();
    }
    return s_np[cmd & 0xffu] != 0xffu;
}

const char *gpu3d_name(uint32_t cmd)
{
    const CmdInfo *c = cmd_info(cmd & 0xffu);

    return c != NULL ? c->name : "UNKNOWN";
}

/* ---- matrices ------------------------------------------------------------------------------------------------------------------------------------------ */
static void mtx_identity(int32_t *m)
{
    uint32_t i;

    for (i = 0u; i < 16u; i++) {
        m[i] = (i % 5u) == 0u ? 0x1000 : 0;
    }
}

static int32_t wrap32(int64_t v)
{
    return (int32_t)(uint32_t)(uint64_t)v;   /* the low 32 bits: what the engine stores */
}

/* m = s * m for the operand s given as a full 4x4 (row-major); only the first `rows` rows of m are replaced (3 for the 3x3 multiply, 4 otherwise). The operand's
   missing elements are filled in by the callers (zeros, and 1.0 at [3][3] for the 4x3 form), so one routine serves all three multiplies. */
static void mtx_mult(int32_t *m, const int32_t *s, uint32_t rows)
{
    int32_t old[16];
    uint32_t r, c;

    assert(rows == 3u || rows == 4u);
    memcpy(old, m, sizeof old);
    for (r = 0u; r < rows; r++) {
        for (c = 0u; c < 4u; c++) {
            int64_t acc = (int64_t)s[r * 4u + 0u] * old[0u + c] + (int64_t)s[r * 4u + 1u] * old[4u + c] + (int64_t)s[r * 4u + 2u] * old[8u + c] +
                          (int64_t)s[r * 4u + 3u] * old[12u + c];

            m[r * 4u + c] = wrap32(acc >> 12);
        }
    }
}

/* the operand of MTX_LOAD_4x3 / MTX_MULT_4x3: three rows of three, a translation row, and the implied last column (0, 0, 0, 1.0) */
static void operand_4x3(int32_t *s, const uint32_t *p)
{
    uint32_t r, c;

    for (r = 0u; r < 4u; r++) {
        for (c = 0u; c < 3u; c++) {
            s[r * 4u + c] = (int32_t)p[r * 3u + c];
        }
        s[r * 4u + 3u] = r == 3u ? 0x1000 : 0;
    }
}

static void operand_3x3(int32_t *s, const uint32_t *p)
{
    uint32_t r, c;

    memset(s, 0, 16u * sizeof s[0]);
    for (r = 0u; r < 3u; r++) {
        for (c = 0u; c < 3u; c++) {
            s[r * 4u + c] = (int32_t)p[r * 3u + c];
        }
    }
}

static void mtx_scale(int32_t *m, const uint32_t *p)
{
    uint32_t r, c;

    for (r = 0u; r < 3u; r++) {
        for (c = 0u; c < 4u; c++) {
            m[r * 4u + c] = wrap32(((int64_t)(int32_t)p[r] * m[r * 4u + c]) >> 12);
        }
    }
}

static void mtx_trans(int32_t *m, const uint32_t *p)
{
    uint32_t c;

    for (c = 0u; c < 4u; c++) {
        int64_t d = ((int64_t)(int32_t)p[0] * m[c] + (int64_t)(int32_t)p[1] * m[4u + c] + (int64_t)(int32_t)p[2] * m[8u + c]) >> 12;

        m[12u + c] = wrap32((int64_t)m[12u + c] + d);
    }
}

static void clip_update(Gpu3d *g)
{
    if (g->clip_dirty) {
        memcpy(g->clip, g->proj, sizeof g->clip);
        mtx_mult(g->clip, g->pos, 4u);   /* clip = position x projection (row-vector convention) */
        g->clip_dirty = 0u;
    }
}

/* ---- the matrix commands ------------------------------------------------------------------------------------------------------------------------------- */
static void raise_error(Gpu3d *g)
{
    g->gxstat_sticky |= GPU3D_GXSTAT_ERROR;
    g->overflows++;
}

static void note_high(Gpu3d *g)
{
    if ((g->pos_sp & 0x1fu) > g->pos_high) {
        g->pos_high = g->pos_sp & 0x1fu;
    }
    if (g->proj_sp > g->proj_high) {
        g->proj_high = g->proj_sp;
    }
}

typedef enum MtxOp { OP_IDENTITY, OP_LOAD, OP_MULT4, OP_MULT3, OP_SCALE, OP_TRANS } MtxOp;

/* ---- bet 2.8: the projection's wide tag follows it through the one-slot projection stack ---- */
static void proj_tag_save(Gpu3d *g)
{
    g->proj_stack_tag = g->proj_tag;
    g->proj_stack_dist = g->proj_dist;
    g->proj_stack_m00 = g->proj_m00;
}

static void proj_tag_load(Gpu3d *g)
{
    g->proj_tag = g->proj_stack_tag;
    g->proj_dist = g->proj_stack_dist;
    g->proj_m00 = g->proj_stack_m00;
}

/* A projection LOAD on a wide engine: tagged when it matches a registered widened projection (only the cells the widening changes and the w column: a game that edits
   another cell of the loaded matrix, like fieldmap.c's depth bias in _32, keeps the tag). A match LOADs the DS matrix instead: _00 and _11 become the registered
   narrow values (what the game's camera builds with wide = 0), every other cell stays as sent. The wideness then lives in the clip volume (wide_planes) and the screen
   map (screen_wide), so the DS's 256 x 192 is the centre of the wide frame bit for bit. IDENTITY clears the tag; MULT / SCALE / TRANS keep it (the camera matrix the
   NitroSystem flush multiplies in does not change what the projection maps). Returns the matrix to apply (s, or sub with the substitution). */
static const int32_t *proj_tag_matrix(Gpu3d *g, MtxOp op, const int32_t *s, int32_t *sub)
{
    uint32_t i;

    if (g->wide_on == 0u || (op != OP_LOAD && op != OP_IDENTITY)) {
        return s;
    }
    g->proj_tag = 0u;
    g->proj_dist = 0;
    g->proj_m00 = 0;
    if (op == OP_IDENTITY) {
        return s;
    }
    assert(s != NULL && sub != NULL);
    g->st_reg_loads++;
    for (i = 0u; i < GPU3D_WIDE_REG; i++) {
        if (g->wide_reg[i].used != 0u && g->wide_reg[i].m00 == s[0] && g->wide_reg[i].m11 == s[5] && g->wide_reg[i].m23 == s[11] && g->wide_reg[i].m33 == s[15]) {
            g->proj_tag = GPU3D_WT_WIDE | ((s[11] != 0 && s[15] == 0) ? GPU3D_WT_PERSP : 0u);
            g->proj_dist = g->wide_reg[i].dist;
            g->proj_m00 = g->wide_reg[i].n00;
            g->st_reg_hits++;
            memcpy(sub, s, 16u * sizeof *sub);
            sub[0] = g->wide_reg[i].n00;
            sub[5] = g->wide_reg[i].n11;
            return sub;
        }
    }
    return s;
}

static void cmd_push(Gpu3d *g)
{
    if (g->mode == 0u) {
        if (g->proj_sp > 0u) {
            raise_error(g);
        }
        memcpy(g->proj_stack, g->proj, sizeof g->proj);
        proj_tag_save(g);
        g->proj_sp = (g->proj_sp + 1u) & 1u;
    } else if (g->mode == 3u) {
        if (g->tex_sp > 0u) {
            raise_error(g);
        }
        memcpy(g->tex_stack, g->tex, sizeof g->tex);
        g->tex_sp = (g->tex_sp + 1u) & 1u;
    } else {
        if (g->pos_sp > 30u) {
            raise_error(g);
        }
        memcpy(g->pos_stack[g->pos_sp & 0x1fu], g->pos, sizeof g->pos);
        memcpy(g->vec_stack[g->pos_sp & 0x1fu], g->vec, sizeof g->vec);
        g->pos_sp = (g->pos_sp + 1u) & 0x3fu;
    }
    note_high(g);
}

static void cmd_pop(Gpu3d *g, uint32_t param)
{
    if (g->mode == 0u) {
        if (g->proj_sp == 0u) {
            raise_error(g);
        }
        g->proj_sp = (g->proj_sp - 1u) & 1u;
        memcpy(g->proj, g->proj_stack, sizeof g->proj);
        proj_tag_load(g);
        g->clip_dirty = 1u;
    } else if (g->mode == 3u) {
        if (g->tex_sp == 0u) {
            raise_error(g);
        }
        g->tex_sp = (g->tex_sp - 1u) & 1u;
        memcpy(g->tex, g->tex_stack, sizeof g->tex);
    } else {
        int32_t n = (int32_t)(param << 26) >> 26;   /* a signed 6-bit count */

        g->pos_sp = (uint32_t)((int32_t)g->pos_sp - n) & 0x3fu;
        if (g->pos_sp > 30u) {
            raise_error(g);
        }
        memcpy(g->pos, g->pos_stack[g->pos_sp & 0x1fu], sizeof g->pos);
        memcpy(g->vec, g->vec_stack[g->pos_sp & 0x1fu], sizeof g->vec);
        g->clip_dirty = 1u;
    }
    note_high(g);
}

static void cmd_store(Gpu3d *g, uint32_t param)
{
    uint32_t slot = param & 0x1fu;

    if (g->mode == 0u) {
        memcpy(g->proj_stack, g->proj, sizeof g->proj);
        proj_tag_save(g);
    } else if (g->mode == 3u) {
        memcpy(g->tex_stack, g->tex, sizeof g->tex);
    } else {
        if (slot > 30u) {
            raise_error(g);
        }
        memcpy(g->pos_stack[slot], g->pos, sizeof g->pos);
        memcpy(g->vec_stack[slot], g->vec, sizeof g->vec);
    }
}

static void cmd_restore(Gpu3d *g, uint32_t param)
{
    uint32_t slot = param & 0x1fu;

    if (g->mode == 0u) {
        memcpy(g->proj, g->proj_stack, sizeof g->proj);
        proj_tag_load(g);
        g->clip_dirty = 1u;
    } else if (g->mode == 3u) {
        memcpy(g->tex, g->tex_stack, sizeof g->tex);
    } else {
        if (slot > 30u) {
            raise_error(g);
        }
        memcpy(g->pos, g->pos_stack[slot], sizeof g->pos);
        memcpy(g->vec, g->vec_stack[slot], sizeof g->vec);
        g->clip_dirty = 1u;
    }
}

/* apply one operation to one matrix; `s` is the full 4x4 operand for LOAD / MULT, `raw` the raw parameters for SCALE / TRANS */
static void mtx_apply(int32_t *m, MtxOp op, const int32_t *s, const uint32_t *raw)
{
    switch (op) {
    case OP_IDENTITY:
        mtx_identity(m);
        break;
    case OP_LOAD:
        memcpy(m, s, 16u * sizeof m[0]);
        break;
    case OP_MULT4:
        mtx_mult(m, s, 4u);
        break;
    case OP_MULT3:
        mtx_mult(m, s, 3u);
        break;
    case OP_SCALE:
        mtx_scale(m, raw);
        break;
    default:
        mtx_trans(m, raw);
        break;
    }
}

/* the mode selects the target: 0 projection, 3 texture, 1 position, 2 position AND vector (SCALE leaves the vector matrix alone) */
static void mtx_target(Gpu3d *g, MtxOp op, const int32_t *s, const uint32_t *raw)
{
    if (g->mode == 0u) {
        int32_t sub[16];

        int32_t nar[16];

        if (op == OP_LOAD && g->proj_load_hook != NULL) {   /* bet 2.8 host model only (the private wide-model harness): NULL in the port */
            memcpy(sub, s, sizeof sub);
            g->proj_load_hook(g, sub);
            s = sub;
        }
        s = proj_tag_matrix(g, op, s, nar);
        mtx_apply(g->proj, op, s, raw);
        g->clip_dirty = 1u;
    } else if (g->mode == 3u) {
        mtx_apply(g->tex, op, s, raw);
    } else {
        mtx_apply(g->pos, op, s, raw);
        if (g->mode == 2u && op != OP_SCALE) {
            mtx_apply(g->vec, op, s, raw);
        }
        g->clip_dirty = 1u;
    }
}

/* ---- clipping (GBATEK "DS 3D Polygon Definitions by Vertices", Clipping): the polygon is cut against the six sides of the view volume, Z first, then Y, then X ----------
   A vertex outside a plane is replaced by up to two vertices where its edges to the neighbours that are inside cross the plane. The interpolation is the DS integer one:
   t = (w_in -+ c_in) / ((w_in -+ c_in) - (w_out -+ c_out)), every attribute is in + (out - in) * numerator / denominator with a 64-bit product and truncation towards zero. */
/* The polygon arrays (Gpu3dPoly.vtx / zdepth / wnorm) and every clipper buffer hold GPU3D_MAX_PV = 10 vertices. A convex planar quad needs at most 10 (six planes), but a skew or
   self-crossing quad can gain two vertices per plane and would reach 11-13 (bet 2.4 review B1, 3 of 100 000 random quads). melonDS has the same 10-entry buffers (GPU3D.cpp:
   `Vertex temp[10]` in ClipAgainstPlane, `Vertex clippedvertices[10]` in SubmitPolygon, `Polygon::Vertices[10]`) and no bound check, so what it does past 10 is a stack overflow: undefined,
   it hangs the harness (slice 2.4d). The engine stops adding vertices at CLIP_MAX, per plane, and counts the polygon (st_clip_over); the polygon itself is kept. */
#define CLIP_MAX GPU3D_MAX_PV

/* a / b truncated towards zero for b != 0, through the FPU's double divide instead of the library's shift-subtract 64-bit one (~550 cycles on the ARM11, which has no
   integer divide). Exact: a and b are exact doubles, and a non-integer quotient is at least 1/|b| from the next integer, a relative gap of 1/(|a|) >= 2^-52 of the quotient's size, wider
   than the 2^-53 rounding error of the division, so the rounded quotient never crosses an integer the true one has not reached. */
static int64_t div_trunc_exact(int64_t a, int32_t b)
{
    if (a > -((int64_t)1 << 52) && a < ((int64_t)1 << 52)) {
        return (int64_t)((double)a / (double)b);
    }
    return a / b;
}

static int32_t lerp32(int32_t a, int32_t b, int64_t num, int32_t den)
{
    int64_t d = (int64_t)wrap32((int64_t)b - (int64_t)a);

    return wrap32((int64_t)a + (den != 0 ? div_trunc_exact(d * num, den) : 0));
}

static int16_t lerp16(int16_t a, int16_t b, int64_t num, int32_t den)
{
    int64_t d = (int64_t)b - (int64_t)a;

    return (int16_t)(uint16_t)(uint64_t)((int64_t)a + (den != 0 ? div_trunc_exact(d * num, den) : 0));
}

/* Bet 2.8, the wide view volume of the polygon (or BOX_TEST) being clipped: the x and y planes as multiples of w in 1/4096; 0 = the DS planes (+-w). Set only around one
   clip / outcode by build_polygon_run, cmd_box_test and submit_vertex (the engine is single-threaded) and always put back to 0, so a narrow engine never sees anything
   but 0. Wide: x = +-w * 400/256 (6400), y = +-w * 240/192 (5120); stereo adds the guard band g to x: +-w * 400/256 * (1 + g / 4096). Linear in w, so the DS
   interpolation along an edge is unchanged in form, and a polygon inside the DS planes is inside these: the DS's polygons reach the rasteriser untouched. */
static int32_t s_px, s_py;

/* Rounded UP for w > 0 (review of the first Azahar frames, 2.8): a cut vertex lands ON the plane, and floor(w * 6400 / 4096) put the right / bottom cut at 399.x / 239.x,
   which the floored screen map turns into an edge at 399 / 239: the last column and row of the wide frame were never covered (the clear colour). Rounded up, the cut maps
   to 400 / 240 (the left / top cut to -1, as before). The volume grows by less than one unit of w; a polygon inside the DS planes is untouched. */
static int64_t plane_w(int64_t w, uint32_t axis)
{
    int64_t s = axis == 0u ? s_px : (axis == 1u ? s_py : 0);

    if (s == 0) {
        return w;
    }
    return w > 0 ? (w * s + 4095) >> 12 : (w * s) >> 12;
}

static int outside_plane(const Gpu3dVtx *v, uint32_t axis, int32_t sign)
{
    int64_t w = plane_w(v->pos[3], axis);

    return sign > 0 ? (int64_t)v->pos[axis] > w : (int64_t)v->pos[axis] < -w;
}

/* bit i set = vertex i is outside the plane (n <= CLIP_MAX = 10, so a word holds them all): one pass of cheap tests instead of three per vertex inside the plane loop */
static uint32_t outside_bits(const Gpu3dVtx *v, uint32_t n, uint32_t axis, int32_t sign)
{
    uint32_t i, bits = 0u;

    for (i = 0u; i < n; i++) {
        bits |= (uint32_t)outside_plane(&v[i], axis, sign) << i;
    }
    return bits;
}

/* the vertex where the edge from `in` (outside the plane) to `other` (inside) crosses the plane  coordinate[axis] == sign * w */
static Gpu3dVtx cut_vertex(const Gpu3dVtx *in, const Gpu3dVtx *other, uint32_t axis, int32_t sign, int attribs)
{
    Gpu3dVtx o;
    int64_t num = wrap32(plane_w(in->pos[3], axis) - (int64_t)sign * in->pos[axis]);
    int32_t den = wrap32(num - (int64_t)wrap32(plane_w(other->pos[3], axis) - (int64_t)sign * other->pos[axis]));
    uint32_t k;

    memset(&o, 0, sizeof o);
    for (k = 0u; k < 3u; k++) {
        if (k != axis) {
            o.pos[k] = lerp32(in->pos[k], other->pos[k], num, den);
        }
    }
    o.pos[3] = lerp32(in->pos[3], other->pos[3], num, den);
    o.pos[axis] = wrap32((int64_t)sign * plane_w(o.pos[3], axis));
    if (attribs) {
        for (k = 0u; k < 3u; k++) {
            o.col[k] = lerp32(in->col[k], other->col[k], num, den);
        }
        o.tex[0] = lerp16(in->tex[0], other->tex[0], num, den);
        o.tex[1] = lerp16(in->tex[1], other->tex[1], num, den);
    }
    o.clipped = 1u;
    return o;
}

/* one plane: src (n vertices; the first `start` are already inside and are carried over) -> dst; returns the new count. `out` = outside_bits of src, with at least one vertex at
   or after `start` outside (the callers skip the plane otherwise). *over: set when a vertex was dropped for want of room (CLIP_MAX). */
static uint32_t clip_plane(Gpu3dVtx *dst, const Gpu3dVtx *src, uint32_t n, uint32_t start, uint32_t axis, int32_t sign, int attribs, int far_ok, uint32_t out, uint32_t *over)
{
    uint32_t i, m = start;

    for (i = 0u; i < start; i++) {
        dst[i] = src[i];
    }
    for (i = start; i < n; i++) {
        uint32_t prev = i == 0u ? n - 1u : i - 1u, next = i + 1u == n ? 0u : i + 1u;

        if ((out & (1u << i)) == 0u) {
            if (m < CLIP_MAX) {
                dst[m++] = src[i];
            } else {
                *over = 1u;
            }
            continue;
        }
        if (axis == 2u && sign > 0 && !far_ok) {
            return 0u;
        }
        if ((out & (1u << prev)) == 0u) {
            if (m < CLIP_MAX) {
                dst[m++] = cut_vertex(&src[i], &src[prev], axis, sign, attribs);
            } else {
                *over = 1u;
            }
        }
        if ((out & (1u << next)) == 0u) {
            if (m < CLIP_MAX) {
                dst[m++] = cut_vertex(&src[i], &src[next], axis, sign, attribs);
            } else {
                *over = 1u;
            }
        }
    }
    return m;
}

/* both planes of one axis; afterwards the colours are snapped to 5-bit steps again (the low 12 bits become 0xFFF). A plane no vertex (at or after `start`) is outside of leaves
   the polygon as it is (the plane loop would copy every vertex, and the snap is a no-op on a vertex nothing interpolated), so it costs one pass of tests and no copy. */
static uint32_t clip_axis(Gpu3dVtx *v, uint32_t n, uint32_t start, uint32_t axis, int attribs, int far_ok, uint32_t *over)
{
    Gpu3dVtx tmp[CLIP_MAX];
    uint32_t m, i, k, o1, o2;

    if (n <= start) {
        return start;   /* only the carried vertices are left (or none, after a far-plane rejection: the oracle's quirk, see clip_polygon) */
    }
    o1 = outside_bits(v, n, axis, 1);
    if ((o1 >> start) != 0u) {
        if (axis == 2u && !far_ok) {
            return 0u;   /* the far plane may not cut this polygon */
        }
        m = clip_plane(tmp, v, n, start, axis, 1, attribs, far_ok, o1, over);
        o2 = outside_bits(tmp, m, axis, -1);
        if (m > start && (o2 >> start) != 0u) {
            m = clip_plane(v, tmp, m, start, axis, -1, attribs, far_ok, o2, over);
        } else {
            memcpy(v, tmp, (size_t)m * sizeof v[0]);
        }
    } else {
        o2 = outside_bits(v, n, axis, -1);
        if ((o2 >> start) == 0u) {
            return n;
        }
        m = clip_plane(tmp, v, n, start, axis, -1, attribs, far_ok, o2, over);
        memcpy(v, tmp, (size_t)m * sizeof v[0]);
    }
    for (i = 0u; i < m; i++) {
        for (k = 0u; k < 3u; k++) {
            v[i].col[k] = (int32_t)(((uint32_t)v[i].col[k] & ~0xfffu) + 0xfffu);
        }
    }
    return m;
}

/* The six sides of the view volume as one outcode per vertex: bit 2a = outside the +side of axis a (pos[a] > w), bit 2a+1 = outside the -side (pos[a] < -w). */
static uint8_t outcode(const int32_t *pos)
{
    int64_t w = pos[3];
    uint32_t c = 0u, a;

    for (a = 0u; a < 3u; a++) {
        int64_t wa = plane_w(w, a);

        c |= ((int64_t)pos[a] > wa ? 1u : 0u) << (2u * a);
        c |= ((int64_t)pos[a] + wa < 0 ? 2u : 0u) << (2u * a);
    }
    return (uint8_t)c;
}

#define CLIP_REJECT 0u   /* clip_polygon would return 0 */
#define CLIP_INSIDE 1u   /* clip_polygon would return n and leave the vertices as they are */
#define CLIP_NEEDED 2u

/* Decide, from the outcodes alone, what clip_polygon (Z, then Y, then X) would do, in the cases where that is exact. Vertices before `start` are carried and already inside.
   Nothing outside any plane: the polygon passes every pass untouched. With start == 0, a pass whose plane has every vertex outside leaves nothing (the far plane rejects even
   earlier, with the same result) and a pass whose planes have no vertex outside changes nothing, so the next axis sees the original vertices: the first axis with a vertex
   outside either plane (and not all outside the + plane, or all outside the - plane with none outside the + one) is where the exact decision needs the real clipper. */
static uint32_t clip_verdict(const Gpu3dVtx *v, uint32_t n, uint32_t start)
{
    uint32_t any = 0u, all = 0x3fu, i, a;

    for (i = start; i < n; i++) {
        uint32_t c = v[i].oc;

        any |= c;
        all &= c;
    }
    if (any == 0u) {
        return CLIP_INSIDE;
    }
    if (start != 0u) {
        return CLIP_NEEDED;
    }
    for (a = 3u; a-- > 0u;) {
        uint32_t plus = 1u << (2u * a), minus = plus << 1;

        if ((all & plus) != 0u || ((any & plus) == 0u && (all & minus) != 0u)) {
            return CLIP_REJECT;
        }
        if ((any & (plus | minus)) != 0u) {
            return CLIP_NEEDED;
        }
    }
    return CLIP_INSIDE;
}

/* The whole polygon. A strip polygon that reuses two vertices of its predecessor keeps them as vertices 0 and 1 (start == 2). A far-plane rejection of such a polygon leaves
   the two carried vertices behind: the later axes still start their count at `start`, so the result is 2, not 0 (the oracle does this; no hardware evidence either way). */
static uint32_t clip_polygon(Gpu3dVtx *v, uint32_t n, uint32_t start, int attribs, int far_ok, uint32_t *over)
{
    n = clip_axis(v, n, start, 2u, attribs, far_ok, over);
    n = clip_axis(v, n, start, 1u, attribs, far_ok, over);
    n = clip_axis(v, n, start, 0u, attribs, far_ok, over);
    return n;
}

/* bet 2.8: the tag geometry under the current projection gets: a registered projection under the full-screen viewport is wide-mapped (a partial viewport is the game's
   own window: centred, the DS planes) */
static uint32_t eff_tag(const Gpu3d *g)
{
    uint32_t tag = g->proj_tag & (GPU3D_WT_WIDE | GPU3D_WT_PERSP);

    if ((tag & GPU3D_WT_WIDE) != 0u && !(g->viewport[0] == 0u && g->viewport[3] == 0u && g->viewport[4] == 256u && g->viewport[5] == 192u)) {
        return 0u;
    }
    return tag;
}

/* bet 2.8: set the clip planes of geometry under the current projection (s_px / s_py); the callers put them back to 0. Returns 1 when the guard band is in them. */
static int wide_planes(const Gpu3d *g)
{
    uint32_t tag = eff_tag(g);

    if ((tag & GPU3D_WT_WIDE) == 0u) {
        s_px = 0;
        s_py = 0;
        return 0;
    }
    s_py = 5120;
    if (g->stereo_on != 0u && (tag & GPU3D_WT_PERSP) != 0u) {
        s_px = (int32_t)(((int64_t)6400 * (4096 + g->xguard)) >> 12);
        return g->xguard != 0;
    }
    s_px = 6400;
    return 0;
}

/* ---- BOX_TEST (GBATEK "DS 3D Tests": the cuboid's eight corners through the clip matrix; visible when any face is at least partly inside the view volume) ------------ */
static void box_corners(Gpu3dVtx *corner, const uint32_t *p, const int32_t *clip)
{
    int16_t x0 = (int16_t)(p[0] & 0xffffu), y0 = (int16_t)(p[0] >> 16), z0 = (int16_t)(p[1] & 0xffffu);
    int16_t x1 = (int16_t)(p[1] >> 16), y1 = (int16_t)(p[2] & 0xffffu), z1 = (int16_t)(p[2] >> 16);
    int32_t xs[2], ys[2], zs[2], k;
    static const uint8_t pick[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 } };

    x1 = (int16_t)(x1 + x0);   /* the second corner is stored as an extent: the sum wraps at 16 bits */
    y1 = (int16_t)(y1 + y0);
    z1 = (int16_t)(z1 + z0);
    xs[0] = x0; xs[1] = x1;
    ys[0] = y0; ys[1] = y1;
    zs[0] = z0; zs[1] = z1;
    for (k = 0; k < 8; k++) {
        int64_t x = xs[pick[k][0]], y = ys[pick[k][1]], z = zs[pick[k][2]];
        uint32_t c;

        memset(&corner[k], 0, sizeof corner[k]);
        for (c = 0u; c < 4u; c++) {
            corner[k].pos[c] = wrap32((x * clip[c] + y * clip[4u + c] + z * clip[8u + c] + (int64_t)0x1000 * clip[12u + c]) >> 12);
        }
    }
}

static void cmd_box_test(Gpu3d *g, const uint32_t *p)
{
    static const uint8_t faces[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 3, 4, 5 }, { 1, 2, 7, 6 }, { 0, 1, 6, 5 }, { 2, 3, 4, 7 } };
    Gpu3dVtx corner[8], face[CLIP_MAX];
    uint32_t f, k, over = 0u;

    g->gxstat_sticky &= ~GPU3D_GXSTAT_BOXRES;
    g->box_tests++;
    clip_update(g);
    box_corners(corner, p, g->clip);
    (void)wide_planes(g);   /* bet 2.8: the wide view volume (and the stereo guard band: a prop inside it is drawn, so it cannot pop in one eye) */
    for (f = 0u; f < 6u; f++) {
        for (k = 0u; k < 4u; k++) {
            face[k] = corner[faces[f][k]];
        }
        if (clip_polygon(face, 4u, 0u, 0, (g->cur_polygon_attr & (1u << 12)) != 0u, &over) > 0u) {
            g->gxstat_sticky |= GPU3D_GXSTAT_BOXRES;
            g->box_in_view++;
            s_px = s_py = 0;
            return;
        }
    }
    s_px = s_py = 0;
}

static void cmd_pos_test(Gpu3d *g, const uint32_t *p)
{
    int64_t x = (int16_t)(p[0] & 0xffffu), y = (int16_t)(p[0] >> 16), z = (int16_t)(p[1] & 0xffffu);
    uint32_t c;

    clip_update(g);
    for (c = 0u; c < 4u; c++) {
        g->pos_res[c] = wrap32((x * g->clip[c] + y * g->clip[4u + c] + z * g->clip[8u + c] + (int64_t)0x1000 * g->clip[12u + c]) >> 12);
    }
    g->cur_vtx[0] = (int16_t)(p[0] & 0xffffu);   /* POS_TEST also loads the stored vertex */
    g->cur_vtx[1] = (int16_t)(p[0] >> 16);
    g->cur_vtx[2] = (int16_t)(p[1] & 0xffffu);
    g->pos_tests++;
}

/* VEC_TEST: a 10-bit-per-axis (1.9) vector through the 3x3 vector matrix; the result is a 13-bit value sign-extended to 16 */
static void cmd_vec_test(Gpu3d *g, uint32_t param)
{
    int32_t n[3], c;

    n[0] = (int16_t)((param & 0x3ffu) << 6) >> 6;
    n[1] = (int16_t)(((param >> 10) & 0x3ffu) << 6) >> 6;
    n[2] = (int16_t)(((param >> 20) & 0x3ffu) << 6) >> 6;
    for (c = 0; c < 3; c++) {
        uint32_t acc = (uint32_t)n[0] * (uint32_t)g->vec[c] + (uint32_t)n[1] * (uint32_t)g->vec[4 + c] + (uint32_t)n[2] * (uint32_t)g->vec[8 + c];   /* 32-bit wrap */
        int32_t r = (int32_t)acc >> 9;

        if ((r & 0x1000) != 0) {
            r |= 0xf000;
        }
        g->vec_res[c] = (int16_t)r;
    }
    g->vec_tests++;
}

/* ---- the vertex pipeline (GBATEK "DS 3D Geometry Commands", "DS 3D Polygon Definitions by Vertices", "DS 3D Polygon Light Parameters", "DS 3D Texture Coordinates") ----------
   All arithmetic is the DS's: products of 16-bit values and 20.12 matrix cells wrap at 32 bits where the hardware multiplies in 32 bits, and are 64-bit where it does not. */
static int16_t sx10(uint32_t v, uint32_t shift_down)   /* a signed 10-bit field at bit `shift_down`, sign-extended */
{
    return (int16_t)((int16_t)(((v >> shift_down) & 0x3ffu) << 6) >> 6);
}

static int32_t mul32(int32_t a, int32_t b)   /* a 32-bit multiply: the low 32 bits */
{
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

static int32_t sext(int32_t v, uint32_t bits)   /* keep the low `bits` bits, sign-extended */
{
    return (int32_t)((uint32_t)v << (32u - bits)) >> (32u - bits);
}

_Static_assert(GPU3D_MEMO_N == 128u, "light_vertex hashes into 1 << 7 slots");

/* the lighting unit: runs at NORMAL */
static void tex_from_normal(Gpu3d *g)
{
    int64_t n0 = g->normal[0], n1 = g->normal[1], n2 = g->normal[2];

    g->texc[0] = (int16_t)(uint16_t)(uint64_t)(g->rawtex[0] + ((n0 * g->tex[0] + n1 * g->tex[4] + n2 * g->tex[8]) >> 21));
    g->texc[1] = (int16_t)(uint16_t)(uint64_t)(g->rawtex[1] + ((n0 * g->tex[1] + n1 * g->tex[5] + n2 * g->tex[9]) >> 21));
}

static void light_vertex(Gpu3d *g)
{
    int32_t nt[3], i;
    uint32_t acc[3], c, key = 0u, slot = 0u;
    int memo = (g->texparam >> 30) != 2u;   /* the texture-from-normal mode also writes the texture coordinate, from state the memo does not key on */

    g->st_lit += (g->cur_polygon_attr & 0xfu) != 0u ? 1u : 0u;
    if (memo) {   /* the colour is a function of the normal and of state that has not changed while light_epoch has not */
        key = ((uint32_t)g->normal[0] & 0x3ffu) | (((uint32_t)g->normal[1] & 0x3ffu) << 10) | (((uint32_t)g->normal[2] & 0x3ffu) << 20) | (1u << 31);
        slot = ((key ^ (g->light_epoch * 0x9e3779b1u)) * 0x85ebca6bu) >> (32u - 7u);   /* GPU3D_MEMO_N = 1 << 7 */
        if (g->light_memo[slot].key == key && g->light_memo[slot].epoch == g->light_epoch) {
            g->vcolor[0] = g->light_memo[slot].col[0];
            g->vcolor[1] = g->light_memo[slot].col[1];
            g->vcolor[2] = g->light_memo[slot].col[2];
            g->st_lit_memo += (g->cur_polygon_attr & 0xfu) != 0u ? 1u : 0u;
            return;
        }
    } else {
        tex_from_normal(g);
    }
    for (c = 0u; c < 3u; c++) {   /* the normal through the vector matrix: 1 sign bit, 10 fractional bits */
        int32_t s = mul32(g->normal[0], g->vec[c]) + mul32(g->normal[1], g->vec[4u + c]) + mul32(g->normal[2], g->vec[8u + c]);

        nt[c] = (int32_t)((uint32_t)s << 9) >> 21;
        acc[c] = (uint32_t)g->mat_emi[c] << 14;
    }
    for (i = 0; i < 4; i++) {
        int32_t dot, shine;

        if ((g->cur_polygon_attr & (1u << i)) == 0u) {
            continue;
        }
        dot = ((g->light_dir[i][0] * nt[0]) >> 9) + ((g->light_dir[i][1] * nt[1]) >> 9) + ((g->light_dir[i][2] * nt[2]) >> 9);
        if (dot > 0) {
            int32_t dd = sext(dot, 11);

            for (c = 0u; c < 3u; c++) {   /* diffuse: each product is cut to 20 bits before it is added */
                acc[c] += (uint32_t)(g->mat_dif[c] * g->light_col[i][c] * dd) & 0xfffffu;
            }
            dot = sext(dot + nt[2], 11);   /* the half-vector term: (L + (0,0,-1)) . N */
            dot = ((dot * dot) >> 10) & 0x3ff;
            shine = ((dot * g->spec_recip[i]) >> 8) - (1 << 9);
            if (shine < 0) {
                shine = 0;
            } else {
                shine = sext(shine, 14);
                shine = shine < 0 ? 0 : (shine > 0x1ff ? 0x1ff : shine);
            }
        } else {
            shine = 0;
        }
        if (g->use_shin != 0u) {
            shine = (int32_t)g->shin[shine >> 2] << 1;
        }
        for (c = 0u; c < 3u; c++) {   /* specular and ambient, both scaled by the light colour */
            acc[c] += (uint32_t)(((int32_t)g->mat_spe[c] * shine + ((int32_t)g->mat_amb[c] << 9)) * (int32_t)g->light_col[i][c]);
        }
    }
    for (c = 0u; c < 3u; c++) {
        g->vcolor[c] = (uint8_t)((acc[c] >> 14) > 31u ? 31u : (acc[c] >> 14));
    }
    if (memo) {
        g->light_memo[slot].key = key;
        g->light_memo[slot].epoch = g->light_epoch;
        g->light_memo[slot].col[0] = g->vcolor[0];
        g->light_memo[slot].col[1] = g->vcolor[1];
        g->light_memo[slot].col[2] = g->vcolor[2];
    }
}

static void do_texcoord(Gpu3d *g, uint32_t p)
{
    int32_t s = (int16_t)(p & 0xffffu), t = (int16_t)(p >> 16);

    g->rawtex[0] = (int16_t)s;
    g->rawtex[1] = (int16_t)t;
    if ((g->texparam >> 30) == 1u) {   /* (S, T, 1/16, 1/16) through the texture matrix; the two 1/16 terms are the matrix rows 2 and 3 taken as they are */
        g->texc[0] = (int16_t)(uint16_t)(uint32_t)((mul32(s, g->tex[0]) + mul32(t, g->tex[4]) + g->tex[8] + g->tex[12]) >> 12);
        g->texc[1] = (int16_t)(uint16_t)(uint32_t)((mul32(s, g->tex[1]) + mul32(t, g->tex[5]) + g->tex[9] + g->tex[13]) >> 12);
    } else {
        g->texc[0] = g->rawtex[0];
        g->texc[1] = g->rawtex[1];
    }
}

/* Every input of the lighting memo changed: the memo's entries are stale. The epoch is 32 bits; on the wrap (about 2^32 bumps, ~10 h at ~2000 per frame; review N2) the memo is
   emptied so an entry written 2^32 bumps ago can never match again. */
static void bump_epoch(Gpu3d *g)
{
    if (++g->light_epoch == 0u) {
        memset(g->light_memo, 0, sizeof g->light_memo);
        g->light_epoch = 1u;
    }
}

static Gpu3dPoly *cur_poly(Gpu3d *g, uint32_t i)
{
    return &g->pram[g->cur_bank][i];
}

static void screen_ds(const Gpu3d *g, Gpu3dVtx *cv, uint32_t start, uint32_t nverts);
static void screen_wide(Gpu3d *g, Gpu3dVtx *cv, uint32_t start, uint32_t nverts);
static int zero_dot(Gpu3d *g, const Gpu3dVtx *cv, uint32_t nverts, uint32_t attr);
static void poly_store(Gpu3d *g, const Gpu3dVtx *cv, uint32_t start, uint32_t nverts, uint32_t lastn, int32_t reuse0, int32_t reuse1, uint32_t attr, int facing, int line);

static void build_polygon_run(Gpu3d *g, uint32_t nverts)
{
    Gpu3dVtx cv[CLIP_MAX];
    Gpu3dVtx *tv = g->tmpv;
    uint32_t start = 0u, lastn = 0u, i;
    int32_t reuse0 = 0, reuse1 = 0;
    int64_t nx, ny, nz, dot;
    int facing, line = 0;
    uint32_t attr = g->cur_polygon_attr;
    uint32_t verdict;

    /* a strip polygon shares the two vertices its predecessor ended with, if that polygon had the full vertex count and neither was made by clipping */
    if (g->prim >= 2u && g->last_strip >= 0) {
        const Gpu3dPoly *lp = cur_poly(g, (uint32_t)g->last_strip);
        uint32_t id0, id1;

        if (g->prim == 2u) {
            id0 = (g->consec & 1u) != 0u ? 2u : 0u;
            id1 = (g->consec & 1u) != 0u ? 1u : 2u;
            lastn = 3u;
        } else {
            id0 = 3u;
            id1 = 2u;
            lastn = 4u;
        }
        if (lp->nverts == lastn && !g->vram[g->cur_bank][lp->vtx[id0]].clipped && !g->vram[g->cur_bank][lp->vtx[id1]].clipped) {
            reuse0 = (int32_t)lp->vtx[id0];
            reuse1 = (int32_t)lp->vtx[id1];
            start = 2u;
        }
    }
    /* Outside the view volume before anything else: a polygon every vertex of which is outside one plane leaves nothing whichever way it faces, so the facing test is spared
       (the counters: such a polygon is counted as clipped away, where it used to count as culled when it also faced away). */
    verdict = clip_verdict(tv, nverts, start);   /* (the outcodes were taken with the guard band of the vertex's projection: submit_vertex) */
    if (verdict == CLIP_REJECT) {   /* clipped away whole: what the clipper's 0 would have counted */
        g->st_clip_calls++;
        g->st_clip_cut++;
        g->last_strip = -1;
        g->st_clipped_out++;
        return;
    }
    /* facing: the sign of the dot product of the first vertex with the polygon normal, taken in (x, y, w) space */
    {
        int64_t d0x = wrap32((int64_t)tv[0].pos[0] - tv[1].pos[0]), d0y = wrap32((int64_t)tv[0].pos[1] - tv[1].pos[1]), d0w = wrap32((int64_t)tv[0].pos[3] - tv[1].pos[3]);
        int64_t d2x = wrap32((int64_t)tv[2].pos[0] - tv[1].pos[0]), d2y = wrap32((int64_t)tv[2].pos[1] - tv[1].pos[1]), d2w = wrap32((int64_t)tv[2].pos[3] - tv[1].pos[3]);

        nx = d0y * d2w - d0w * d2y;
        ny = d0w * d2x - d0x * d2w;
        nz = d0x * d2y - d0y * d2x;
        while (((nx >> 31) ^ (nx >> 63)) != 0 || ((ny >> 31) ^ (ny >> 63)) != 0 || ((nz >> 31) ^ (nz >> 63)) != 0) {   /* scaled until each fits 32 bits */
            nx >>= 4;
            ny >>= 4;
            nz >>= 4;
        }
        dot = (int64_t)tv[1].pos[0] * nx + (int64_t)tv[1].pos[1] * ny + (int64_t)tv[1].pos[3] * nz;
    }
    facing = dot <= 0;
    if ((dot < 0 && (attr & (1u << 7)) == 0u) || (dot > 0 && (attr & (1u << 6)) == 0u)) {   /* bit 7: render the front, bit 6: render the back */
        g->last_strip = -1;
        g->st_culled++;
        return;
    }

    if (start == 2u) {
        cv[0] = g->vram[g->cur_bank][reuse0];
        cv[1] = g->vram[g->cur_bank][reuse1];
    }
    for (i = start; i < nverts; i++) {
        cv[i] = tv[i];
    }
    if (nverts == 3u) {
        line = (cv[0].pos[0] == cv[1].pos[0] && cv[0].pos[1] == cv[1].pos[1] && cv[0].pos[2] == cv[1].pos[2] && cv[0].pos[3] == cv[1].pos[3]) ||
               (cv[0].pos[0] == cv[2].pos[0] && cv[0].pos[1] == cv[2].pos[1] && cv[0].pos[2] == cv[2].pos[2] && cv[0].pos[3] == cv[2].pos[3]) ||
               (cv[1].pos[0] == cv[2].pos[0] && cv[1].pos[1] == cv[2].pos[1] && cv[1].pos[2] == cv[2].pos[2] && cv[1].pos[3] == cv[2].pos[3]);
    }
    {
        uint32_t before = nverts, any = 0u;
#ifdef HW_G3_PROF
        uint64_t tc = g->clock != NULL ? g->clock() : 0u;
#endif

        uint32_t over = 0u;

        if (verdict != CLIP_INSIDE) {
            g->st_guard_polys += (uint32_t)wide_planes(g);
            nverts = clip_polygon(cv, nverts, start, 1, (attr & (1u << 12)) != 0u, &over);
            s_px = s_py = 0;
        }
        g->st_clip_over += over;
        for (i = 0u; i < nverts; i++) {
            any |= cv[i].clipped;
            g->st_cutv += cv[i].clipped;
        }
        g->st_clip_calls++;
        g->st_clip_cut += (nverts != before || any != 0u) ? 1u : 0u;
#ifdef HW_G3_PROF
        if (g->clock != NULL) {
            g->prof_ticks[GPU3D_PROF_CLIP] += g->clock() - tc;
            g->prof_n[GPU3D_PROF_CLIP]++;
        }
#endif
    }
    if (nverts == 0u) {
        g->last_strip = -1;
        g->st_clipped_out++;
        return;
    }
    if (g->npoly >= g->lim_polys || g->nvtx + nverts > g->lim_verts) {   /* does not fit: dropped, and the RAM-overflow flag is raised (lim_* = the DS limits unless wide) */
        g->last_strip = -1;
        g->ram_overflow = 1u;
        g->st_overflow++;
        return;
    }
    if (g->npoly >= GPU3D_MAX_POLYS || g->nvtx + nverts > GPU3D_MAX_VERTS) {   /* bet 2.8: a wide engine keeps a polygon the DS would have dropped */
        g->st_wide_overflow++;
    }
    if (g->wide_on != 0u) {
        screen_wide(g, cv, start, nverts);
    } else {
        screen_ds(g, cv, start, nverts);
    }
    if (zero_dot(g, cv, nverts, attr)) {
        return;
    }
    poly_store(g, cv, start, nverts, lastn, reuse0, reuse1, attr, facing, line);
}

/* the screen transform: w is cut to 24 bits first; x and y by the viewport in 32-bit unsigned arithmetic (w above 16 bits loses a bit before the divide) */
static void screen_ds(const Gpu3d *g, Gpu3dVtx *cv, uint32_t start, uint32_t nverts)
{
    uint32_t i;

    assert(nverts <= CLIP_MAX && start <= nverts);
    for (i = start; i < nverts; i++) {
        uint32_t w, px, py;

        cv[i].pos[3] = (int32_t)((uint32_t)cv[i].pos[3] & 0x00ffffffu);
        w = (uint32_t)cv[i].pos[3];
        if (w == 0u) {
            px = 0u;
            py = 0u;
        } else {
            uint32_t den = w;

            px = (uint32_t)cv[i].pos[0] + w;
            py = w - (uint32_t)cv[i].pos[1];
            if (w > 0xffffu) {
                px >>= 1;
                py >>= 1;
                den >>= 1;
            }
            den <<= 1;
            px = (px * g->viewport[4]) / den + g->viewport[0];
            py = (py * g->viewport[5]) / den + g->viewport[3];
        }
        cv[i].sx = (int16_t)(px & 0x1ffu);
        cv[i].sy = (int16_t)(py & 0xffu);
    }
}

/* floor(a / b), b > 0 (a wide-frame vertex left of or above the DS frame has a negative numerator) */
static int64_t floordiv64(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* Bet 2.8, a wide engine: the polygon's tag decides the mapping. GPU3D_WT_WIDE (a registered projection under the full-screen viewport): the DS's own full-screen
   transform, floored, plus (72, 24): x = (x + w) * 256 / 2w + 72, y = (w - y) * 192 / 2w + 24, on the DS matrix (proj_tag_matrix) over the wide volume. Inside the DS
   planes this is the DS position + (72, 24) exactly; outside it continues the same scale to the 400 x 240 edges (64-bit signed, so a guard-band vertex left of the screen
   keeps its sign). Otherwise the DS transform of the game's viewport, offset by (72, 24): narrow-centred, 1:1. */
static void screen_wide(Gpu3d *g, Gpu3dVtx *cv, uint32_t start, uint32_t nverts)
{
    uint32_t i, tag = eff_tag(g);

    assert(g->wide_on != 0u && nverts <= CLIP_MAX);
    if ((g->proj_tag & GPU3D_WT_WIDE) != 0u && (tag & GPU3D_WT_WIDE) == 0u) {
        g->st_partial_vp++;
    }
    g->cur_wtag = tag;
    if ((tag & GPU3D_WT_WIDE) == 0u) {
        screen_ds(g, cv, start, nverts);
        for (i = start; i < nverts; i++) {
            cv[i].sx = (int16_t)(cv[i].sx + 72);
            cv[i].sy = (int16_t)(cv[i].sy + 24);
        }
        return;
    }
    for (i = start; i < nverts; i++) {
        int64_t w;

        cv[i].pos[3] = (int32_t)((uint32_t)cv[i].pos[3] & 0x00ffffffu);
        w = cv[i].pos[3];
        if (w == 0) {
            cv[i].sx = 0;
            cv[i].sy = 0;
            continue;
        }
        cv[i].sx = (int16_t)(floordiv64(((int64_t)cv[i].pos[0] + w) * 256, 2 * w) + 72);
        cv[i].sy = (int16_t)(floordiv64((w - (int64_t)cv[i].pos[1]) * 192, 2 * w) + 24);
    }
}

/* a polygon whose vertices all land on one pixel and are all farther than DISP_1DOT_DEPTH is a one-dot polygon: dropped unless attribute bit 13 asks to keep it */
static int zero_dot(Gpu3d *g, const Gpu3dVtx *cv, uint32_t nverts, uint32_t attr)
{
    uint32_t i;

    assert(nverts >= 1u && nverts <= CLIP_MAX);
    if ((attr & (1u << 13)) == 0u) {
        int zerodot = 1, behind = 1;

        for (i = 0u; i < nverts; i++) {
            if (cv[i].sx != cv[0].sx || cv[i].sy != cv[0].sy) {
                zerodot = 0;
                break;
            }
            if ((uint32_t)cv[i].pos[3] <= g->zero_dot_w) {
                behind = 0;
                break;
            }
        }
        if (zerodot && behind) {
            g->last_strip = -1;
            g->st_dot++;
            return 1;
        }
    }
    return 0;
}

/* the polygon into the building buffer: its record, its vertices (sharing the strip's two), bounds, sort key, depths */
static void poly_store(Gpu3d *g, const Gpu3dVtx *cv, uint32_t start, uint32_t nverts, uint32_t lastn, int32_t reuse0, int32_t reuse1, uint32_t attr, int facing, int line)
{
    uint32_t i, wsize = 0u, texfmt, alpha;
    Gpu3dPoly *poly;
    int32_t ytop = g->wide_on != 0u ? 240 : 192, ybot = 0;

    assert(nverts >= 1u && nverts <= CLIP_MAX);
    poly = cur_poly(g, g->npoly);
    memset(poly, 0, sizeof *poly);
    g->npoly++;
    poly->attr = attr;
    poly->texparam = g->texparam;
    poly->texpalette = (uint16_t)g->texpal;
    texfmt = (g->texparam >> 26) & 7u;
    alpha = (attr >> 16) & 0x1fu;
    poly->flags = (uint8_t)((((texfmt == 1u || texfmt == 6u) || (alpha > 0u && alpha < 31u)) ? GPU3D_PF_TRANSLUCENT : 0u) | (facing ? GPU3D_PF_FACING : 0u) |
                            ((attr & 0x3f000030u) == 0x30u ? GPU3D_PF_SHADOWMASK : 0u) |
                            (((attr & 0x30u) == 0x30u && (attr & 0x3f000030u) != 0x30u) ? GPU3D_PF_SHADOW : 0u) | (line ? GPU3D_PF_LINE : 0u));
    if ((poly->flags & GPU3D_PF_TRANSLUCENT) == 0u) {
        g->nopaque++;
    }
    if (g->last_strip >= 0 && start > 0u) {
        if (nverts == lastn) {
            poly->vtx[0] = (uint16_t)reuse0;
            poly->vtx[1] = (uint16_t)reuse1;
        } else {
            g->vram[g->cur_bank][g->nvtx] = g->vram[g->cur_bank][reuse0];
            poly->vtx[0] = (uint16_t)g->nvtx;
            g->vram[g->cur_bank][g->nvtx + 1u] = g->vram[g->cur_bank][reuse1];
            poly->vtx[1] = (uint16_t)(g->nvtx + 1u);
            g->nvtx += 2u;
        }
        poly->nverts = 2u;
    }
    for (i = start; i < nverts; i++) {
        g->vram[g->cur_bank][g->nvtx] = cv[i];
        poly->vtx[i] = (uint16_t)g->nvtx;
        g->nvtx++;
        poly->nverts++;
    }

    /* bounds, the w normalisation shift, the degenerate flag */
    {
        int32_t xbot = 0;

        for (i = 0u; i < nverts; i++) {
            const Gpu3dVtx *v = &g->vram[g->cur_bank][poly->vtx[i]];
            uint32_t w = (uint32_t)v->pos[3];

            if (v->sy < ytop) {
                ytop = v->sy;
            }
            if (v->sy > ybot || (v->sy == ybot && v->sx > xbot)) {
                xbot = v->sx;
                ybot = v->sy;
            }
            if (w == 0u) {
                poly->flags |= GPU3D_PF_DEGENERATE;
            }
            while ((w >> wsize) != 0u && wsize < 32u) {
                wsize += 4u;
            }
        }
    }
    if (ybot > (g->wide_on != 0u ? 240 : 192)) {
        poly->flags |= GPU3D_PF_DEGENERATE;
    }
    if (g->wide_on != 0u) {   /* bet 2.8: the tag, and the stereo parameters of the buffer */
        poly->wtag = (uint8_t)g->cur_wtag;
        if ((g->cur_wtag & GPU3D_WT_WIDE) != 0u) {
            g->st_wide_polys++;
        } else {
            g->st_centred_polys++;
        }
        if ((g->cur_wtag & GPU3D_WT_PERSP) != 0u) {
            g->bank_dist[g->cur_bank] = g->proj_dist;
            g->bank_m00[g->cur_bank] = g->proj_m00;
        }
    }
    poly->ytop = (int16_t)ytop;
    poly->ybottom = (int16_t)ybot;
    poly->sortkey = ((uint32_t)ybot << 8) | (uint32_t)ytop | ((poly->flags & GPU3D_PF_TRANSLUCENT) != 0u ? 0x10000u : 0u);
    if ((g->flush_attr & 2u) != 0u) {
        poly->flags |= GPU3D_PF_WBUFFER;
    }
    for (i = 0u; i < nverts; i++) {
        const Gpu3dVtx *v = &g->vram[g->cur_bank][poly->vtx[i]];
        int32_t w, ws, z;

        if (wsize < 16u) {
            w = (int32_t)((uint32_t)v->pos[3] << (16u - wsize));
            ws = w >> (16u - wsize);
        } else {
            w = v->pos[3] >> (wsize - 16u);
            ws = (int32_t)((uint32_t)w << (wsize - 16u));
        }
        if ((g->flush_attr & 2u) != 0u) {
            z = ws;
        } else if (v->pos[3] != 0) {
            z = (int32_t)(uint32_t)(uint64_t)((div_trunc_exact((int64_t)v->pos[2] * 0x4000, v->pos[3]) + 0x3fff) * 0x200);
        } else {
            z = 0x7ffe00;
        }
        z = z < 0 ? 0 : (z > 0xffffff ? 0xffffff : z);
        poly->zdepth[i] = z;
        poly->wnorm[i] = w;
    }
    g->last_strip = g->prim >= 2u ? (int32_t)(g->npoly - 1u) : -1;
    g->st_polys++;
}

static void build_polygon(Gpu3d *g, uint32_t nverts)
{
#ifdef HW_G3_PROF
    if (g->clock != NULL) {
        uint64_t t0 = g->clock();

        build_polygon_run(g, nverts);
        g->prof_ticks[GPU3D_PROF_POLY] += g->clock() - t0;
        g->prof_n[GPU3D_PROF_POLY]++;
        return;
    }
#endif
    build_polygon_run(g, nverts);
}

static void submit_vertex(Gpu3d *g)
{
    Gpu3dVtx *t = &g->tmpv[g->vnum];
    int64_t x = g->cur_vtx[0], y = g->cur_vtx[1], z = g->cur_vtx[2];
    uint32_t c;

    g->st_vtxs++;
    clip_update(g);
    for (c = 0u; c < 4u; c++) {
        t->pos[c] = wrap32((x * g->clip[c] + y * g->clip[4u + c] + z * g->clip[8u + c] + (int64_t)0x1000 * g->clip[12u + c]) >> 12);
    }
    for (c = 0u; c < 3u; c++) {
        t->col[c] = (int32_t)(((uint32_t)g->vcolor[c] << 12) + 0xfffu);
    }
    if ((g->texparam >> 30) == 3u) {   /* the vertex through the texture matrix, 12 + 12 fractional bits down to 12.4 */
        t->tex[0] = (int16_t)(uint16_t)(uint64_t)(((x * g->tex[0] + y * g->tex[4] + z * g->tex[8]) >> 24) + g->rawtex[0]);
        t->tex[1] = (int16_t)(uint16_t)(uint64_t)(((x * g->tex[1] + y * g->tex[5] + z * g->tex[9]) >> 24) + g->rawtex[1]);
    } else {
        t->tex[0] = g->texc[0];
        t->tex[1] = g->texc[1];
    }
    (void)wide_planes(g);
    t->oc = outcode(t->pos);
    s_px = s_py = 0;
    t->clipped = 0u;
    g->vnum++;

    switch (g->prim) {
    case 0:
        if (g->vnum == 3u) {
            g->vnum = 0u;
            build_polygon(g, 3u);
            g->consec++;
        }
        break;
    case 1:
        if (g->vnum == 4u) {
            g->vnum = 0u;
            build_polygon(g, 4u);
            g->consec++;
        }
        break;
    case 2:
        if ((g->consec & 1u) != 0u) {   /* odd triangles of a strip are wound the other way: swap the first two */
            Gpu3dVtx s = g->tmpv[1];

            g->tmpv[1] = g->tmpv[0];
            g->tmpv[0] = s;
            g->vnum = 2u;
            build_polygon(g, 3u);
            g->consec++;
            g->tmpv[1] = g->tmpv[2];
        } else if (g->vnum == 3u) {
            g->vnum = 2u;
            build_polygon(g, 3u);
            g->consec++;
            g->tmpv[0] = g->tmpv[1];
            g->tmpv[1] = g->tmpv[2];
        }
        break;
    default:
        if (g->vnum == 4u) {
            Gpu3dVtx s = g->tmpv[3];

            g->tmpv[3] = g->tmpv[2];
            g->tmpv[2] = s;
            g->vnum = 2u;
            build_polygon(g, 4u);
            g->consec++;
            g->tmpv[0] = g->tmpv[3];
            g->tmpv[1] = g->tmpv[2];
        }
        break;
    }
}

/* ---- the sealed list: SWAP_BUFFERS (sorting as GBATEK "DS 3D Polygon Definitions by Vertices" / SWAP_BUFFERS describe it) ------------------------------------------- */
static void sort_range(uint16_t *a, uint32_t n, const Gpu3dPoly *ram)
{
    uint16_t tmp[GPU3D_WIDE_POLYS];
    uint32_t width, i;

    for (width = 1u; width < n; width *= 2u) {   /* a stable bottom-up merge sort on the sort key */
        for (i = 0u; i < n; i += 2u * width) {
            uint32_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2u * width < n ? i + 2u * width : n, l = lo, r = mid, o = lo;

            while (l < mid && r < hi) {
                tmp[o++] = ram[a[r]].sortkey < ram[a[l]].sortkey ? a[r++] : a[l++];
            }
            while (l < mid) {
                tmp[o++] = a[l++];
            }
            while (r < hi) {
                tmp[o++] = a[r++];
            }
        }
        memcpy(a, tmp, n * sizeof a[0]);
    }
}

static void do_swap(Gpu3d *g, uint32_t param)
{
    g->flush_attr = param & 3u;
    g->st_swaps++;
    if (g->pend_valid) {   /* the engine would have been halted here until the vertical blank; the port never waits, so the earlier list is shown now */
        g->rend_bank = g->pend_bank;
        g->rend_n = g->pend_n;
        memcpy(g->rend_order, g->pend_order, sizeof g->rend_order);
        g->st_swaps_dropped++;
    }
    if (g->npoly != 0u) {
        uint32_t io = 0u, it = g->nopaque, i;

        for (i = 0u; i < g->npoly; i++) {   /* opaque polygons first, then translucent ones, each group in submission order */
            if ((cur_poly(g, i)->flags & GPU3D_PF_TRANSLUCENT) != 0u) {
                g->pend_order[it++] = (uint16_t)i;
            } else {
                g->pend_order[io++] = (uint16_t)i;
            }
        }
        sort_range(g->pend_order, (g->flush_attr & 1u) != 0u ? g->nopaque : g->npoly, g->pram[g->cur_bank]);   /* auto-sort by bottom Y, then top Y; bit 0 set = manual: translucent keep their order */
    }
    g->pend_bank = g->cur_bank;
    g->pend_n = g->npoly;
    g->pend_nvtx = g->nvtx;
    g->pend_valid = 1u;
    g->cur_bank ^= 1u;
    g->npoly = 0u;
    g->nvtx = 0u;
    g->nopaque = 0u;
    g->swap_pending = 1u;
    g->bank_dist[g->cur_bank] = 0;   /* bet 2.8: the new building buffer has no perspective camera yet */
    g->bank_m00[g->cur_bank] = 0;
}

uint32_t gpu3d_render_count(const Gpu3d *g)
{
    return g->rend_n;
}

const Gpu3dPoly *gpu3d_render_poly(const Gpu3d *g, uint32_t i)
{
    return i < g->rend_n ? &g->pram[g->rend_bank][g->rend_order[i]] : NULL;
}

const Gpu3dVtx *gpu3d_render_vtx(const Gpu3d *g, const Gpu3dPoly *p, uint32_t k)
{
    return k < p->nverts ? &g->vram[g->rend_bank][p->vtx[k]] : NULL;
}

void gpu3d_write_disp3dcnt(Gpu3d *g, uint32_t value)
{
    if ((value & (1u << 13)) != 0u) {
        g->ram_overflow = 0u;
    }
}

uint32_t gpu3d_disp3dcnt_flags(const Gpu3d *g)
{
    return g->ram_overflow != 0u ? (1u << 13) : 0u;
}

void gpu3d_write_1dot(Gpu3d *g, uint32_t value)
{
    g->zero_dot_w = (value & 0x7fffu) * 0x200u + 0x1ffu;
}

/* the commands of slice (b) */
static void exec_geometry(Gpu3d *g, uint32_t op, const uint32_t *p)
{
    uint32_t i, l;

    switch (op) {
    case 0x20:
        g->vcolor[0] = (uint8_t)(p[0] & 0x1fu);
        g->vcolor[1] = (uint8_t)((p[0] >> 5) & 0x1fu);
        g->vcolor[2] = (uint8_t)((p[0] >> 10) & 0x1fu);
        break;
    case 0x21:
        g->normal[0] = sx10(p[0], 0);
        g->normal[1] = sx10(p[0], 10);
        g->normal[2] = sx10(p[0], 20);
        light_vertex(g);
        break;
    case 0x22: do_texcoord(g, p[0]); break;
    case 0x23:
        g->cur_vtx[0] = (int16_t)(p[0] & 0xffffu);
        g->cur_vtx[1] = (int16_t)(p[0] >> 16);
        g->cur_vtx[2] = (int16_t)(p[1] & 0xffffu);
        submit_vertex(g);
        break;
    case 0x24:
        g->cur_vtx[0] = (int16_t)((p[0] & 0x3ffu) << 6);
        g->cur_vtx[1] = (int16_t)((p[0] & 0xffc00u) >> 4);
        g->cur_vtx[2] = (int16_t)((p[0] & 0x3ff00000u) >> 14);
        submit_vertex(g);
        break;
    case 0x25:
        g->cur_vtx[0] = (int16_t)(p[0] & 0xffffu);
        g->cur_vtx[1] = (int16_t)(p[0] >> 16);
        submit_vertex(g);
        break;
    case 0x26:
        g->cur_vtx[0] = (int16_t)(p[0] & 0xffffu);
        g->cur_vtx[2] = (int16_t)(p[0] >> 16);
        submit_vertex(g);
        break;
    case 0x27:
        g->cur_vtx[1] = (int16_t)(p[0] & 0xffffu);
        g->cur_vtx[2] = (int16_t)(p[0] >> 16);
        submit_vertex(g);
        break;
    case 0x28:   /* VTX_DIFF: signed 10-bit deltas in 1/64 steps */
        g->cur_vtx[0] = (int16_t)(g->cur_vtx[0] + sx10(p[0], 0));
        g->cur_vtx[1] = (int16_t)(g->cur_vtx[1] + sx10(p[0], 10));
        g->cur_vtx[2] = (int16_t)(g->cur_vtx[2] + sx10(p[0], 20));
        submit_vertex(g);
        break;
    case 0x2a: g->texparam = p[0]; break;
    case 0x2b: g->texpal = p[0] & 0x1fffu; break;
    case 0x30:
        bump_epoch(g);
        for (i = 0u; i < 3u; i++) {
            g->mat_dif[i] = (uint8_t)((p[0] >> (5u * i)) & 0x1fu);
            g->mat_amb[i] = (uint8_t)((p[0] >> (16u + 5u * i)) & 0x1fu);
        }
        if ((p[0] & 0x8000u) != 0u) {   /* bit 15: the diffuse colour is also the current vertex colour */
            for (i = 0u; i < 3u; i++) {
                g->vcolor[i] = g->mat_dif[i];
            }
        }
        break;
    case 0x31:
        bump_epoch(g);
        for (i = 0u; i < 3u; i++) {
            g->mat_spe[i] = (uint8_t)((p[0] >> (5u * i)) & 0x1fu);
            g->mat_emi[i] = (uint8_t)((p[0] >> (16u + 5u * i)) & 0x1fu);
        }
        g->use_shin = (p[0] & 0x8000u) != 0u;
        break;
    case 0x32: {   /* the light direction through the vector matrix, 11 bits, negated; plus the reciprocal the specular term needs */
        int32_t d[3], den, s;

        bump_epoch(g);
        l = p[0] >> 30;
        d[0] = sx10(p[0], 0);
        d[1] = sx10(p[0], 10);
        d[2] = sx10(p[0], 20);
        for (i = 0u; i < 3u; i++) {
            s = mul32(d[0], g->vec[i]) + mul32(d[1], g->vec[4u + i]) + mul32(d[2], g->vec[8u + i]);
            g->light_dir[l][i] = (int16_t)sext(-(s >> 12), 11);
        }
        s = mul32(d[0], g->vec[2]) + mul32(d[1], g->vec[6]) + mul32(d[2], g->vec[10]);
        den = -((int32_t)((uint32_t)s << 9) >> 21) + (1 << 9);
        g->spec_recip[l] = den == 0 ? 0 : (1 << 18) / den;
        break;
    }
    case 0x33:
        bump_epoch(g);
        l = p[0] >> 30;
        g->light_col[l][0] = (uint8_t)(p[0] & 0x1fu);
        g->light_col[l][1] = (uint8_t)((p[0] >> 5) & 0x1fu);
        g->light_col[l][2] = (uint8_t)((p[0] >> 10) & 0x1fu);
        break;
    case 0x34:
        bump_epoch(g);
        for (i = 0u; i < 32u; i++) {
            g->shin[4u * i] = (uint8_t)p[i];
            g->shin[4u * i + 1u] = (uint8_t)(p[i] >> 8);
            g->shin[4u * i + 2u] = (uint8_t)(p[i] >> 16);
            g->shin[4u * i + 3u] = (uint8_t)(p[i] >> 24);
        }
        break;
    case 0x40:
        bump_epoch(g);   /* BEGIN_VTXS latches the polygon attributes (the light bits) */
        g->prim = p[0] & 3u;
        g->vnum = 0u;
        g->consec = 0u;
        g->last_strip = -1;
        g->cur_polygon_attr = g->polygon_attr;   /* BEGIN_VTXS latches the attributes */
        break;
    case 0x50: do_swap(g, p[0]); break;
    default: {   /* 0x60 VIEWPORT */
        uint32_t x0 = p[0] & 0xffu, y0 = (191u - ((p[0] >> 8) & 0xffu)) & 0xffu, x1 = (p[0] >> 16) & 0xffu, y1 = (191u - (p[0] >> 24)) & 0xffu;

        g->viewport_cmd = p[0];
        g->viewport[0] = x0;
        g->viewport[1] = y0;
        g->viewport[2] = x1;
        g->viewport[3] = y1;
        g->viewport[4] = (x1 - x0 + 1u) & 0x1ffu;
        g->viewport[5] = (y0 - y1 + 1u) & 0xffu;
        break;
    }
    }
}

/* ---- the executor --------------------------------------------------------------------------------------------------------------------------------------- */
static void exec_matrix(Gpu3d *g, uint32_t op, const uint32_t *p)
{
    int32_t s[16];

    switch (op) {
    case 0x10: g->mode = p[0] & 3u; break;
    case 0x11: cmd_push(g); break;
    case 0x12: cmd_pop(g, p[0]); break;
    case 0x13: cmd_store(g, p[0]); break;
    case 0x14: cmd_restore(g, p[0]); break;
    case 0x15: mtx_target(g, OP_IDENTITY, NULL, NULL); break;
    case 0x16:
        memcpy(s, p, sizeof s);
        mtx_target(g, OP_LOAD, s, NULL);
        break;
    case 0x17:
        operand_4x3(s, p);
        mtx_target(g, OP_LOAD, s, NULL);
        break;
    case 0x18:
        memcpy(s, p, sizeof s);
        mtx_target(g, OP_MULT4, s, NULL);
        break;
    case 0x19:
        operand_4x3(s, p);
        mtx_target(g, OP_MULT4, s, NULL);
        break;
    case 0x1a:
        operand_3x3(s, p);
        mtx_target(g, OP_MULT3, s, NULL);
        break;
    case 0x1b: mtx_target(g, OP_SCALE, NULL, p); break;
    default: mtx_target(g, OP_TRANS, NULL, p); break;
    }
}

#ifdef HW_G3_PROF
static uint32_t prof_class(uint32_t op)
{
    if (op >= 0x10u && op <= 0x1cu) {
        return GPU3D_PROF_MATRIX;
    }
    if (op >= 0x23u && op <= 0x28u) {
        return GPU3D_PROF_VERTEX;
    }
    if (op == 0x21u) {
        return GPU3D_PROF_NORMAL;
    }
    if (op == 0x50u) {
        return GPU3D_PROF_SWAP;
    }
    return op >= 0x70u ? GPU3D_PROF_TEST : GPU3D_PROF_STATE;
}
#endif

/* the executor's dispatch, one byte per opcode: which handler runs it (CL_UNKNOWN: GBATEK does not list it; CL_NONE: listed, nothing to do in slice (b)) */
enum { CL_NONE, CL_MATRIX, CL_ATTR, CL_GEOM, CL_BOX, CL_POS, CL_VEC, CL_UNKNOWN };
static uint8_t s_cls[256];

static void cls_build(void)
{
    uint32_t op;

    for (op = 0u; op < 256u; op++) {
        uint32_t k = CL_NONE;

        if (!gpu3d_known(op)) {
            k = CL_UNKNOWN;
        } else if (op >= 0x10u && op <= 0x1cu) {
            k = CL_MATRIX;
        } else if (op == 0x29u) {
            k = CL_ATTR;
        } else if ((op >= 0x20u && op <= 0x28u) || op == 0x2au || op == 0x2bu || (op >= 0x30u && op <= 0x34u) || op == 0x40u || op == 0x50u || op == 0x60u) {
            k = CL_GEOM;
        } else if (op == 0x70u) {
            k = CL_BOX;
        } else if (op == 0x71u) {
            k = CL_POS;
        } else if (op == 0x72u) {
            k = CL_VEC;
        }
        s_cls[op] = (uint8_t)k;
    }
}

static void execute_run(Gpu3d *g, uint32_t op, const uint32_t *p)
{
    assert(op != 0u && op <= 0xffu);
    g->done++;
    g->last_cmd = op;
    g->count[op]++;
    switch (s_cls[op]) {
    case CL_GEOM: exec_geometry(g, op, p); break;
    case CL_MATRIX:
        bump_epoch(g);   /* the vector matrix may change */
        exec_matrix(g, op, p);
        break;
    case CL_ATTR: g->polygon_attr = p[0]; break;
    case CL_BOX: cmd_box_test(g, p); break;
    case CL_POS: cmd_pos_test(g, p); break;
    case CL_VEC: cmd_vec_test(g, p[0]); break;
    case CL_UNKNOWN:
        g->unknown[op]++;
        g->unknown_total++;
        break;
    default: break;   /* every other known opcode: parameters consumed, no state in slice (a) */
    }
}

static void execute(Gpu3d *g, uint32_t op, const uint32_t *p)
{
#ifdef HW_G3_PROF
    if (g->clock != NULL) {
        uint64_t t0 = g->clock();
        uint32_t k = prof_class(op);

        execute_run(g, op, p);
        g->prof_ticks[k] += g->clock() - t0;
        g->prof_n[k]++;
        return;
    }
#endif
    execute_run(g, op, p);
}

/* ---- the input: FIFO packing and command ports ------------------------------------------------------------------------------------------------------- */
static void pk_next(Gpu3d *g)
{
    g->pk_cmds >>= 8;
    g->pk_left--;
    g->cur_need = 0u;
}

/* run the opcodes of the packed word from the current one on, stopping at the first that still wants parameters */
static void pk_run(Gpu3d *g, uint32_t word)
{
    while (g->pk_left > 0u) {
        uint32_t op = g->pk_cmds & 0xffu, need = s_npx[op];

        if (op == 0u) {
            pk_next(g);
            continue;
        }
        if (need > 0u) {
            g->cur_cmd = op;
            g->cur_need = need;
            return;
        }
        execute(g, op, &word);
        pk_next(g);
    }
}

uint32_t gpu3d_fifo(Gpu3d *g, uint32_t word)
{
    uint32_t before;

    assert(g != NULL);
    before = g->done;
    g->words++;
    if (g->pk_left == 0u) {
        g->pk_cmds = word;
        g->pk_left = 4u;
        pk_run(g, word);
        return g->done - before;
    }
    assert(g->cur_need > 0u);
    if (g->cur_need == 1u) {
        execute(g, g->cur_cmd, &word);
        pk_next(g);
        pk_run(g, word);
        return g->done - before;
    }
    assert(g->nparams < GPU3D_PARAMS_MAX);
    g->params[g->nparams++] = word;
    if (g->nparams >= g->cur_need) {
        g->nparams = 0u;
        execute(g, g->cur_cmd, g->params);
        pk_next(g);
        pk_run(g, word);
    }
    return g->done - before;
}

uint32_t gpu3d_port(Gpu3d *g, uint32_t off, uint32_t word)
{
    uint32_t op, need, before;

    assert(g != NULL && off >= 0x440u && off < 0x600u);
    before = g->done;
    g->words++;
    if (off >= 0x5ccu) {
        return 0u;   /* opcodes 0x73..0x7F have no command port on the DS: the store is dropped */
    }
    op = (off - 0x400u) >> 2;
    need = s_npx[op];
    if (need <= 1u) {
        execute(g, op, &word);   /* a one-parameter command (or none: the stored value is ignored) runs at once and leaves a half-gathered command alone */
        return g->done - before;
    }
    assert(g->nparams < GPU3D_PARAMS_MAX);
    g->params[g->nparams++] = word;
    if (g->nparams >= need) {
        g->nparams = 0u;
        execute(g, op, g->params);
    }
    return g->done - before;
}

uint32_t gpu3d_entry(Gpu3d *g, uint32_t addr, uint32_t word)
{
    assert(g != NULL);
    if (addr >= 0x440u && addr < 0x600u) {
        return gpu3d_port(g, addr, word);
    }
    return gpu3d_fifo(g, word);   /* addr 0 or 0x400..0x43F: the FIFO */
}

/* ---- readbacks ------------------------------------------------------------------------------------------------------------------------------------------- */
void gpu3d_reset(Gpu3d *g)
{
    assert(g != NULL);
    if (s_np_ready == 0u) {
        np_build();
    }
    cls_build();
    memset(g, 0, sizeof *g);
    mtx_identity(g->proj);
    mtx_identity(g->pos);
    mtx_identity(g->vec);
    mtx_identity(g->tex);
    g->clip_dirty = 1u;
    g->last_strip = -1;
    g->zero_dot_w = 0xffffffu;
    g->lim_polys = GPU3D_MAX_POLYS;
    g->lim_verts = GPU3D_MAX_VERTS;
}

/* Bet 2.8. Wide lifts the polygon/vertex limits to the port's arrays (the DS limits are the narrow path's; past them the wide
   engine counts st_wide_overflow and keeps going). Stereo needs wide; guard_q12 is the x guard band g of a stereo perspective polygon (|x| <= w(1+g)), q12. */
void gpu3d_wide_config(Gpu3d *g, uint32_t wide, uint32_t stereo, int32_t guard_q12)
{
    assert(g != NULL && guard_q12 >= 0 && guard_q12 <= 4096);
    g->wide_on = wide != 0u ? 1u : 0u;
    g->stereo_on = (g->wide_on != 0u && stereo != 0u) ? 1u : 0u;
    g->xguard = g->stereo_on != 0u ? guard_q12 : 0;
    g->lim_polys = g->wide_on != 0u ? GPU3D_WIDE_POLYS : GPU3D_MAX_POLYS;
    g->lim_verts = g->wide_on != 0u ? GPU3D_WIDE_VERTS : GPU3D_MAX_VERTS;
}

/* a widened projection the camera hook just built (m = 16 words, row-major 20.12), the DS matrix's _00 / _11 (what the camera builds with wide = 0), and its camera
   distance: the geometry engine tags what is drawn under it and draws it with the DS matrix */
void gpu3d_wide_register(Gpu3d *g, const int32_t *m, int32_t n00, int32_t n11, int32_t dist)
{
    uint32_t i, slot;

    assert(g != NULL && m != NULL);
    if (g->wide_on == 0u) {
        return;
    }
    for (i = 0u; i < GPU3D_WIDE_REG; i++) {
        if (g->wide_reg[i].used != 0u && g->wide_reg[i].m00 == m[0] && g->wide_reg[i].m11 == m[5] && g->wide_reg[i].m23 == m[11] && g->wide_reg[i].m33 == m[15]) {
            g->wide_reg[i].dist = dist;
            g->wide_reg[i].n00 = n00;
            g->wide_reg[i].n11 = n11;
            return;
        }
    }
    slot = g->wide_reg_next % GPU3D_WIDE_REG;
    g->wide_reg_next = (slot + 1u) % GPU3D_WIDE_REG;
    g->wide_reg[slot].m00 = m[0];
    g->wide_reg[slot].m11 = m[5];
    g->wide_reg[slot].m23 = m[11];
    g->wide_reg[slot].m33 = m[15];
    g->wide_reg[slot].dist = dist;
    g->wide_reg[slot].n00 = n00;
    g->wide_reg[slot].n11 = n11;
    g->wide_reg[slot].used = 1u;
}

/* the stereo parameters of the list on show: the camera distance and the DS _00 of its (last) perspective camera; 0, 0 = no perspective polygon (mono) */
void gpu3d_render_stereo(const Gpu3d *g, int32_t *dist, int32_t *m00)
{
    assert(g != NULL && dist != NULL && m00 != NULL);
    *dist = g->bank_dist[g->rend_bank & 1u];
    *m00 = g->bank_m00[g->rend_bank & 1u];
}

static uint32_t gxstat(const Gpu3d *g)
{
    return g->gxstat_sticky | (g->swap_pending != 0u ? GPU3D_GXSTAT_BUSY : 0u) | ((g->pos_sp & 0x1fu) << GPU3D_GXSTAT_PVSHIFT) | ((g->proj_sp & 1u) << GPU3D_GXSTAT_PJSHIFT) | GPU3D_GXSTAT_HALF | GPU3D_GXSTAT_EMPTY;
}

uint32_t gpu3d_read(Gpu3d *g, uint32_t off)
{
    static const uint8_t vecidx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };

    assert(g != NULL);
    off &= ~3u;
    if (off == GPU3D_REG_GXSTAT) {
        return gxstat(g);
    }
    if (off >= GPU3D_REG_POS_RESULT && off < GPU3D_REG_POS_RESULT + 16u) {
        return (uint32_t)g->pos_res[(off - GPU3D_REG_POS_RESULT) >> 2];
    }
    if (off == GPU3D_REG_VEC_RESULT) {
        return (uint32_t)(uint16_t)g->vec_res[0] | ((uint32_t)(uint16_t)g->vec_res[1] << 16);
    }
    if (off == GPU3D_REG_VEC_RESULT + 4u) {
        return (uint32_t)(uint16_t)g->vec_res[2];
    }
    if (off >= GPU3D_REG_CLIPMTX && off < GPU3D_REG_CLIPMTX + 64u) {
        clip_update(g);
        return (uint32_t)g->clip[(off - GPU3D_REG_CLIPMTX) >> 2];
    }
    if (off >= GPU3D_REG_VECMTX && off < GPU3D_REG_VECMTX + 36u) {
        return (uint32_t)g->vec[vecidx[(off - GPU3D_REG_VECMTX) >> 2]];
    }
    return 0u;
}

void gpu3d_vblank(Gpu3d *g)
{
    uint32_t db, dv, sealed;

    assert(g != NULL);
    /* the frame this vertical blank closes (slice 2.4d, review N7: "the count of props drawn is logged per frame"): BOX_TESTs run, of them passed, polygons sealed by its swap */
    db = g->box_tests - g->fr_last_box;
    dv = g->box_in_view - g->fr_last_view;
    sealed = g->pend_valid ? g->pend_n : 0u;
    g->fr_last_box = g->box_tests;
    g->fr_last_view = g->box_in_view;
    g->last_frame_box = db;
    g->last_frame_view = dv;
    g->last_frame_sealed = sealed;
    g->fr_vblanks++;
    g->fr_box += db;
    g->fr_view += dv;
    g->fr_box_max = db > g->fr_box_max ? db : g->fr_box_max;
    g->fr_view_max = dv > g->fr_view_max ? dv : g->fr_view_max;
    if (g->pend_valid) {
        g->fr_sealed++;
        g->fr_sealed_polys += sealed;
        g->fr_sealed_polys_max = sealed > g->fr_sealed_polys_max ? sealed : g->fr_sealed_polys_max;
    }
    if (g->pend_valid || db != 0u) {
        g->fr_3d++;
    }
    g->swap_pending = 0u;
    if (g->pend_valid) {   /* the list the latest swap sealed is the one on show from this vertical blank */
        g->rend_bank = g->pend_bank;
        g->rend_n = g->pend_n;
        memcpy(g->rend_order, g->pend_order, sizeof g->rend_order);
        g->pend_valid = 0u;
    }
}

void gpu3d_write_gxstat(Gpu3d *g, uint32_t value)
{
    assert(g != NULL);
    if ((value & GPU3D_GXSTAT_ERROR) != 0u) {
        g->gxstat_sticky &= ~GPU3D_GXSTAT_ERROR;
        g->proj_sp = 0u;
        g->tex_sp = 0u;
    }
    g->gxstat_sticky = (g->gxstat_sticky & 0x3fffffffu) | (value & 0xc0000000u);
}
