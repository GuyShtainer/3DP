/* gpu3d.h -- the DS geometry engine on the CPU (bet 2.4, slice (a): command decoder, matrix stacks, computed readbacks). Pure C: only <stdint.h> / <string.h>, so it
 * compiles for the ARM11 and, unchanged, on the host (the private stream-replay gate).
 *
 * Written from GBATEK "DS 3D Video" (the section names are listed in the private design notes); where GBATEK and the melonDS oracle disagree the oracle's
 * observable behaviour is followed and the doc says where. No melonDS source is used here.
 *
 * The input is the ordered stream the 2.1 paths record (hw_gx.h): FIFO words (`gpu3d_fifo`: packed opcodes, then their parameters) and command-port stores
 * (`gpu3d_port`: offset 0x440..0x5FF, one parameter per store, the opcode is (offset - 0x400) / 4). `gpu3d_entry` takes a trace entry as it stands (addr 0 or 0x400..0x43F
 * = FIFO, 0x440..0x5FF = port). A command executes the moment its last parameter arrives: there is no FIFO model and no cycle count, so the engine is never busy.
 *
 * Slice (a) executes the matrix commands (MTX_*), POLYGON_ATTR / BEGIN_VTXS (their value is read by the box test), BOX_TEST, POS_TEST and VEC_TEST. Slice (b) adds the
 * vertex pipeline: vertices and the clip transform, COLOR / NORMAL / TEXCOORD with the four texture-coordinate modes, the lighting unit (materials, four lights, the
 * shininess table), polygon assembly for the four primitive types, clipping against the six planes, culling, the screen transform, the polygon / vertex RAM limits, and
 * SWAP_BUFFERS, which seals the frame's polygon list (sorted as the DS sorts it); `gpu3d_vblank` makes the sealed list the one `gpu3d_render_*` returns.
 * An opcode GBATEK does not list consumes no parameters and is counted in `unknown[]` (never asserted).
 */
#ifndef GPU3D_H
#define GPU3D_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU3D_POS_DEPTH   32u   /* physical slots of the position / vector stacks (31 usable: slot 31 is the overflow slot) */
#define GPU3D_PARAMS_MAX  32u   /* the longest command (SHININESS) */

#define GPU3D_GXSTAT_BOXRES   (1u << 1)
#define GPU3D_GXSTAT_PVSHIFT  8u          /* bits 8..12: position/vector stack level */
#define GPU3D_GXSTAT_PJSHIFT  13u         /* bit 13: projection stack level */
#define GPU3D_GXSTAT_ERROR    (1u << 15)  /* stack overflow / underflow, sticky until a 1 is written to it */
#define GPU3D_GXSTAT_BUSY     (1u << 27)  /* geometry engine busy: only while SWAP_BUFFERS waits for the vertical blank (no cycle model otherwise) */
#define GPU3D_GXSTAT_HALF     (1u << 25)  /* FIFO less than half full (always, the FIFO is empty) */
#define GPU3D_GXSTAT_EMPTY    (1u << 26)  /* FIFO empty (always) */

#define GPU3D_REG_GXSTAT       0x600u
#define GPU3D_REG_POS_RESULT   0x620u     /* 4 words */
#define GPU3D_REG_VEC_RESULT   0x630u     /* 3 halfwords at 0x630, 0x632, 0x634 */
#define GPU3D_REG_CLIPMTX      0x640u     /* 16 words, row-major */
#define GPU3D_REG_VECMTX       0x680u     /* 9 words, row-major 3x3 */
#define GPU3D_REG_END          0x6a8u

#define GPU3D_MAX_POLYS  2048u   /* polygon RAM (per buffer): the DS limit */
#define GPU3D_MAX_VERTS  6144u   /* vertex RAM (per buffer): the DS limit */
#define GPU3D_WIDE_POLYS 4096u   /* bet 2.8: the limits of a wide engine (gpu3d_wide_config): the wider view holds more of the scene. The buffers are this big; a narrow engine stops at the DS limits */
#define GPU3D_WIDE_VERTS 12288u
#define GPU3D_WIDE_REG   16u     /* bet 2.8: registered widened projections (gpu3d_wide_register), a ring */
#define GPU3D_MEMO_N     128u    /* entries of the lighting memo (a power of two) */
#define GPU3D_MAX_PV     10u     /* vertices of one polygon: a quad clipped by the six planes */

#define GPU3D_PF_TRANSLUCENT 0x01u
#define GPU3D_PF_FACING      0x02u   /* the polygon faces the viewer (dot <= 0) */
#define GPU3D_PF_SHADOWMASK  0x04u
#define GPU3D_PF_SHADOW      0x08u
#define GPU3D_PF_WBUFFER     0x10u
#define GPU3D_PF_DEGENERATE  0x20u
#define GPU3D_PF_LINE        0x40u   /* a triangle with two coincident clip-space vertices */

/* One vertex of the polygon RAM. pos = x, y, z, w in 20.12 clip space after clipping (w truncated to 24 bits once the screen transform ran); col = the lit colour
   with 12 fractional bits kept for the clipper (5-bit colour << 12, plus 0xFFF); tex = 12.4 texel coordinates; sx / sy = screen position (9 / 8 bits). */
typedef struct Gpu3dVtx {
    int32_t pos[4];
    int32_t col[3];
    int16_t tex[2];
    int16_t sx, sy;
    uint8_t clipped;   /* created by clipping: cannot be shared with the next polygon of a strip */
    uint8_t oc;        /* the view-volume outcode of a freshly transformed vertex (bit 2a = beyond +w on axis a, bit 2a+1 = beyond -w); meaningful only while the vertex is in the building buffer */
} Gpu3dVtx;

typedef struct Gpu3dPoly {
    uint32_t attr, texparam;
    uint16_t texpalette;
    uint8_t nverts, flags;
    uint8_t wtag;                   /* bet 2.8: GPU3D_WT_* (0 on a narrow engine) */
    uint16_t vtx[GPU3D_MAX_PV];     /* indices into the same buffer's vertex RAM (strip neighbours share vertices) */
    int32_t zdepth[GPU3D_MAX_PV];   /* 24-bit depth per vertex (Z-buffer: from z / w; W-buffer: the normalised w) */
    int32_t wnorm[GPU3D_MAX_PV];    /* the polygon's w values normalised to 16 bits */
    int16_t ytop, ybottom;
    uint32_t sortkey;
} Gpu3dPoly;

/* the command classes of the Rule 7 instrument. GPU3D_PROF_VERTEX is the whole vertex command (transform + polygon assembly), GPU3D_PROF_POLY the part inside build_polygon,
   GPU3D_PROF_CLIP the part of that inside clip_polygon: the vertex transform alone is VERTEX - POLY, the assembly without clipping is POLY - CLIP. */
enum { GPU3D_PROF_MATRIX, GPU3D_PROF_VERTEX, GPU3D_PROF_POLY, GPU3D_PROF_CLIP, GPU3D_PROF_NORMAL, GPU3D_PROF_STATE, GPU3D_PROF_SWAP, GPU3D_PROF_TEST, GPU3D_PROF_N };

typedef struct Gpu3d {
    int32_t proj[16], pos[16], vec[16], tex[16];   /* current matrices, 20.12 fixed point, row-major (m[row * 4 + col]) */
    int32_t clip[16];                              /* position x projection, valid when clip_dirty == 0 */
    int32_t proj_stack[16], tex_stack[16];
    int32_t pos_stack[GPU3D_POS_DEPTH][16], vec_stack[GPU3D_POS_DEPTH][16];
    uint32_t mode;                                 /* MTX_MODE: 0 projection, 1 position, 2 position + vector, 3 texture */
    uint32_t proj_sp, tex_sp;                      /* 0 or 1 */
    uint32_t pos_sp;                               /* 6 bit; the level bits are its low 5 */
    uint32_t clip_dirty;
    uint32_t swap_pending;                         /* SWAP_BUFFERS ran: the engine stays busy until gpu3d_vblank */
    uint32_t gxstat_sticky;                        /* box-test result, error flag, the FIFO-IRQ bits 30..31 */
    int32_t pos_res[4];
    int16_t vec_res[3];
    uint32_t polygon_attr, cur_polygon_attr;       /* POLYGON_ATTR as written; the value latched by BEGIN_VTXS */

    uint32_t pk_cmds, pk_left;                     /* FIFO packing: the opcodes still to run in the current packed word, how many of the 4 bytes remain */
    uint32_t params[GPU3D_PARAMS_MAX], nparams;    /* parameters gathered for the running command (shared by both input paths, as on the DS) */
    uint32_t cur_cmd, cur_need;

    uint32_t done;                                 /* commands executed so far (opcode 0 is not a command) */
    uint32_t last_cmd;                             /* opcode of the latest executed command */
    uint32_t count[256];                           /* executed commands per opcode (known and unknown) */
    uint32_t unknown[256];                         /* of count[]: opcodes GBATEK does not list */
    uint32_t unknown_total;
    uint32_t words;                                /* stream entries fed */
    uint32_t pos_high, proj_high;                  /* stack level high-water marks (the pointer's low 5 bits / 0..1) */
    uint32_t overflows;                            /* commands that raised the error flag */
    uint32_t box_tests, box_in_view, pos_tests, vec_tests;

    /* ---- slice (b): the vertex pipeline ---- */
    int16_t cur_vtx[3];                            /* the stored last vertex (x, y, z) */
    uint8_t vcolor[3];                             /* current vertex colour, 5 bits per channel */
    int16_t texc[2], rawtex[2];                    /* texture coordinate after the (mode 1 / 2) transform, and as sent */
    int16_t normal[3];
    int16_t light_dir[4][3];                       /* per light, already through the vector matrix, 11 bits */
    int32_t spec_recip[4];
    uint8_t light_col[4][3], mat_dif[3], mat_amb[3], mat_spe[3], mat_emi[3];
    uint32_t use_shin;
    uint8_t shin[128];
    uint32_t texparam, texpal;
    uint32_t prim, vnum, consec;                   /* BEGIN_VTXS type, vertices gathered for the next polygon, polygons built since BEGIN */
    int32_t last_strip;                            /* index of the strip's previous polygon in the building buffer, -1 = none */
    uint32_t viewport[6];
    uint32_t viewport_cmd;                         /* the VIEWPORT command word as sent (x1 | y1 << 8 | x2 << 16 | y2 << 24, y from the top): the PLY1 header carries it */
    uint32_t zero_dot_w;                           /* DISP_1DOT_DEPTH as a 24-bit w limit */
    uint32_t flush_attr;                           /* SWAP_BUFFERS parameter of the latest swap */
    uint32_t ram_overflow;                         /* DISP3DCNT bit 13: a polygon did not fit; sticky until acknowledged */
    Gpu3dVtx tmpv[4];

    uint32_t cur_bank, nvtx, npoly, nopaque;       /* the building buffer */
    uint32_t pend_bank, pend_n, pend_nvtx, pend_valid;   /* sealed by SWAP_BUFFERS, waiting for the vertical blank */
    uint32_t rend_bank, rend_n;                    /* the sealed list: valid after gpu3d_vblank */
    uint16_t pend_order[GPU3D_WIDE_POLYS], rend_order[GPU3D_WIDE_POLYS];
    Gpu3dVtx vram[2][GPU3D_WIDE_VERTS];
    Gpu3dPoly pram[2][GPU3D_WIDE_POLYS];
    uint32_t st_polys, st_vtxs, st_culled, st_clipped_out, st_dot, st_overflow, st_swaps, st_swaps_dropped;   /* counters */
    uint32_t st_lit_memo;                          /* of st_lit: NORMALs answered from the lighting memo */
    uint32_t light_epoch;                          /* bumped by every command that changes an input of light_vertex except the normal itself */
    struct { uint32_t key, epoch; uint8_t col[3]; } light_memo[GPU3D_MEMO_N];   /* (normal, epoch) -> colour, direct-mapped; key 0 = empty */
    uint32_t st_clip_calls, st_clip_cut, st_lit, st_cutv;   /* polygons that reached the clipper / that it actually cut or rejected; NORMALs with at least one light on */
    uint32_t st_clip_over;                         /* polygons the clipper had to cut short at GPU3D_MAX_PV vertices (skew quads; slice 2.4d, review B1): vertices dropped, polygon kept */
    /* per DS vertical blank (slice 2.4d, review N7): what the frame just ended did. fr_* are run totals over the vblanks seen; a "3D frame" is a vblank that sealed a list or ran a BOX_TEST. */
    uint32_t fr_vblanks, fr_3d, fr_sealed, fr_sealed_polys, fr_sealed_polys_max;   /* vblanks; 3D frames; lists sealed (a swap) and the polygons in them (sum, max) */
    uint32_t fr_box, fr_box_max, fr_view, fr_view_max;   /* BOX_TESTs run in the frame and those that passed (in view): sums and per-frame maxima */
    uint32_t fr_last_box, fr_last_view, fr_last_polys;   /* the previous vblank's totals, for the deltas; the last frame's own figures follow */
    uint32_t last_frame_box, last_frame_view, last_frame_sealed;   /* the frame the latest gpu3d_vblank closed: BOX_TESTs, of them in view, polygons in the list sealed in it (0 = none) */

    /* Rule 7 instrument (slice (c)): when `clock` is set the engine adds the ticks of each command class to prof_ticks[], a tick pair per command / polygon, never per word. */
    uint64_t (*clock)(void);
    uint64_t prof_ticks[GPU3D_PROF_N], prof_n[GPU3D_PROF_N];

    /* ---- bet 2.8: the wide / stereo path. gpu3d_reset leaves all of it zero / the DS limits: the narrow engine is the DS. ---- */
    uint32_t lim_polys, lim_verts;                 /* the RAM limits in force: GPU3D_MAX_* (DS), GPU3D_WIDE_* on a wide engine */
    uint32_t wide_on, stereo_on;                   /* gpu3d_wide_config */
    int32_t xguard;                                /* stereo: the x guard band g in 1/4096 (the clipper and BOX_TEST keep |x| <= w * 400/256 * (1 + g / 4096) for a GPU3D_WT_PERSP polygon) */
    struct { int32_t m00, m11, m23, m33, n00, n11, dist; uint32_t used; } wide_reg[GPU3D_WIDE_REG];   /* widened projections (gpu3d_wide_register): matched on m*, drawn with n00 / n11 */
    uint32_t wide_reg_next;
    uint32_t proj_tag, proj_stack_tag, cur_wtag;             /* GPU3D_WT_* of the projection matrix: set by a LOAD (registry match), cleared by IDENTITY, kept by MULT / SCALE / TRANS, saved by the stack */
    int32_t proj_dist, proj_stack_dist;            /* the matched entry's camera distance (fx32, the stereo zero-parallax depth) */
    int32_t proj_m00, proj_stack_m00;              /* the matched entry's DS _00 (the projection's x scale, stereo) */
    int32_t bank_dist[2], bank_m00[2];             /* per polygon buffer: the stereo parameters of the latest GPU3D_WT_PERSP polygon built into it (0 = none) */
    uint32_t st_wide_polys, st_centred_polys, st_partial_vp, st_guard_polys, st_wide_overflow;   /* polygons tagged wide / centred, wide-tagged polygons under a partial viewport (centred instead), polygons clipped with the guard band, polygons past the DS limits that the wide limits kept */
    uint32_t st_reg_loads, st_reg_hits;            /* projection LOADs seen on a wide engine / of them matched the registry */
    void (*proj_load_hook)(struct Gpu3d *g, int32_t *m);   /* bet 2.8: a host model's projection LOAD substitution (the private wide-model harness); NULL in the port */
} Gpu3d;

/* Bet 2.8 polygon tags (Gpu3dPoly.wtag, PLY1 polygon byte +24): */
#define GPU3D_WT_WIDE  0x01u   /* mapped on the wide viewport (400 x 240); clear on a wide engine = narrow-centred: the game's own viewport, offset by (72, 24) */
#define GPU3D_WT_PERSP 0x02u   /* a registered perspective projection: the stereo shear applies (ortho and unregistered projections are at zero parallax) */

void gpu3d_reset(Gpu3d *g);

/* One FIFO word / one command-port store / one trace entry. Return the number of commands that finished with this word (0..4). */
uint32_t gpu3d_fifo(Gpu3d *g, uint32_t word);
uint32_t gpu3d_port(Gpu3d *g, uint32_t off, uint32_t word);
uint32_t gpu3d_entry(Gpu3d *g, uint32_t addr, uint32_t word);
#define gpu3d_word(g, w) gpu3d_fifo((g), (w))

/* The vertical blank: the engine resumes after a SWAP_BUFFERS (GXSTAT busy clears) and the polygon list that swap sealed becomes the render list. */
void gpu3d_vblank(Gpu3d *g);

/* A 32-bit read of a geometry-engine status / result register at I/O offset 0x600..0x6A4 (0 elsewhere). 0x630 packs VEC_RESULT X (low) and Y (high), 0x634 holds Z. */
uint32_t gpu3d_read(Gpu3d *g, uint32_t off);
/* A 32-bit write to GXSTAT: bit 15 acknowledges the error flag and resets the projection / texture stack pointers; bits 30..31 are the FIFO-IRQ condition. */
void gpu3d_write_gxstat(Gpu3d *g, uint32_t value);

/* The sealed render list (the list of the latest SWAP_BUFFERS that a vertical blank has passed): polygons in the DS draw order (opaque, then translucent, Y-sorted as the
   swap bits ask). gpu3d_render_vtx(g, p, k) is the k-th vertex of polygon p. */
uint32_t gpu3d_render_count(const Gpu3d *g);
const Gpu3dPoly *gpu3d_render_poly(const Gpu3d *g, uint32_t i);
const Gpu3dVtx *gpu3d_render_vtx(const Gpu3d *g, const Gpu3dPoly *p, uint32_t k);
/* The sealed render list as the PLY1 byte image bet 2.5 reads (the private design notes, source/hw/gpu3d_list.h of the 2.5 lane): header 256 bytes, then one
   32-byte record per polygon in draw order, then 32 bytes per vertex (each polygon owns its vertices: a vertex shared by two strip polygons is written twice). `regs`
   supplies what lives outside the engine (the registers the 2D/3D block holds); NULL = all zero. Returns the byte count, or 0 when `cap` is too small
   (gpu3d_export_size gives the needed size). Written byte by byte, little-endian: the buffer needs no alignment. Header bit 0x1C.0 (manual translucent sort) is written 0 as
   the format asks (the rasteriser never sorts); the W-buffer bit is the OR of the polygons' flags. Lives in gpu3d_export.c (its own file: the 2.5 lane's header names the
   same types as this one, so the two are never included together). */
typedef struct Gpu3dExportRegs {
    uint32_t key, disp3dcnt, clear_color, clear_depth, alpha_ref, fog_color, fog_offset;
    uint32_t tex_hash_lo, tex_hash_hi;
    uint16_t edge_color[8], toon[32];
} Gpu3dExportRegs;
size_t gpu3d_export_size(const Gpu3d *g);
size_t gpu3d_export_list(const Gpu3d *g, const Gpu3dExportRegs *regs, uint8_t *buf, size_t cap);

/* Bet 2.8: the wide / stereo engine. wide != 0: a projection LOAD that matches a registered widened projection (_00, _11, _23, _33) is applied as the DS matrix (the
   registered n00 / n11 in place of _00 / _11) and what is drawn under it is mapped on the 400 x 240 frame: the clip volume grows to x = +-w * 400/256, y = +-w * 240/192 and
   the screen map is the DS's plus (72, 24), so the DS frame is the wide frame's centre bit for bit. Everything else is the game's viewport offset by (72, 24); the RAM
   limits become GPU3D_WIDE_*. stereo != 0 (needs wide): GPU3D_WT_PERSP polygons and BOX_TESTs clip x with the guard band `guard_q12` (g in 1/4096, of the wide half-width).
   wide == 0 leaves the engine the DS. gpu3d_wide_register: the projection matrix (16 fx32, row-major MtxFx44) the game's camera just built with the widened arguments, the
   _00 / _11 the same camera builds with the DS arguments, and the camera distance (fx32). */
void gpu3d_wide_config(Gpu3d *g, uint32_t wide, uint32_t stereo, int32_t guard_q12);
void gpu3d_wide_register(Gpu3d *g, const int32_t *m, int32_t n00, int32_t n11, int32_t dist);
/* the stereo parameters of the sealed render list (0, 0 = no perspective polygon of a registered projection in it) */
void gpu3d_render_stereo(const Gpu3d *g, int32_t *dist, int32_t *m00);

/* DISP3DCNT write: bit 12 / 13 written as 1 clear the line-buffer / RAM overflow flags. gpu3d_disp3dcnt_flags returns the engine-owned bit 13. */
void gpu3d_write_disp3dcnt(Gpu3d *g, uint32_t value);
uint32_t gpu3d_disp3dcnt_flags(const Gpu3d *g);
/* DISP_1DOT_DEPTH (0x4000610, 15 bits) */
void gpu3d_write_1dot(Gpu3d *g, uint32_t value);

/* Parameters the DS takes after opcode `cmd` (GBATEK "3D Commands"); 0 for opcodes it does not list. gpu3d_known: 1 for a listed opcode. */
uint32_t gpu3d_nparams(uint32_t cmd);
int gpu3d_known(uint32_t cmd);
const char *gpu3d_name(uint32_t cmd);

#ifdef __cplusplus
}
#endif

#endif
