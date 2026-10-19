/* gpu3d_edge.h -- edge marking, the stencil carrier and the depth reconstruction of the PICA 3D path (bet 2.5 slice (c); write-up: the private design notes).
 *
 * Pure C: <stdint.h> only, so the host tests (tools/host-tests/gpu3d_edge_test.c) drive every function with synthetic buffers. gpu3d_pica.c (the citro3d file) only does the
 * GPU work: it reads the raw tiled D24S8 buffer back and hands it to these functions.
 *
 * Written from GBATEK ("DS 3D Video": Edge Marking, Depth Buffering, Alpha Blending) and the PICA200 framebuffer layout as the libctru / citro3d headers document it.
 *
 * The stencil carrier. The PICA has one 8-bit stencil per pixel, and the DS rules need three per-pixel facts: "this pixel holds a polygon" (a translucent polygon over an empty
 * pixel is a plain store, over a covered one a blend), the polygon ID of the pixel's top OPAQUE polygon (edge marking), and the ID of the latest TRANSLUCENT polygon blended
 * there (a translucent polygon skips a pixel whose latest blend had its own ID). One frame chooses between two layouts from DISP3DCNT:
 *   G3E_CARRIER_EDGE   (edge marking on)   bit 7 = covered, bits 0-5 = the opaque polygon ID. Cleared to the rear-plane polygon ID (bit 7 clear).
 *   G3E_CARRIER_TRANS  (edge marking off)  0x00 = empty, 0x80 = covered by an opaque polygon, 0xC0 | id = latest blend came from translucent polygon `id`.
 * With edge marking AND translucent polygons on in one frame (every overworld frame) the frame is SPLIT after the opaques (bet 2.5(d), review B3): the opaque polygons are drawn in the
 * EDGE layout, the D24S8 buffer is copied (the opaque IDs and depth the edge pass needs), one full-screen quad rewrites the stencil to the TRANS layout (g3e_rewrite_pass: bit 7 kept,
 * the ID bits cleared: covered 0x80, empty 0x00), and the translucent polygons are drawn with the TRANS passes, which apply the same-ID rule.
 * Every stencil test and write of a polygon is described by a G3ePass (one compare ref serves the test and the REPLACE value, as on the PICA), built here and proved by the host
 * test with a model of the stencil unit; gpu3d_pica.c only maps a pass to citro3d calls. */
#ifndef GPU3D_EDGE_H
#define GPU3D_EDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G3E_W      256u                /* the logical image: x right, y down */
#define G3E_H      192u
#define G3E_RAW_W  192u                /* the hardware framebuffer: the logical image turned (192 wide, 256 high), 8x8 tiles in z-order */
#define G3E_RAW_H  256u
#define G3E_PIXELS (G3E_W * G3E_H)
#define G3E_WMAX   400u                /* bet 2.8: the widest logical image (the wide 400 x 240 frame; its raw buffer is 240 x 400) */

enum { G3E_CARRIER_EDGE = 0, G3E_CARRIER_TRANS = 1 };

/* stencil values */
#define G3E_EDGE_COVERED    0x80u      /* edge layout: bit 7 */
#define G3E_EDGE_ID_MASK    0x3Fu
#define G3E_TRANS_EMPTY     0x00u      /* trans layout */
#define G3E_TRANS_OPAQUE    0x80u
#define G3E_TRANS_BLEND     0xC0u      /* | translucent polygon ID (0..63) */

/* The layout a frame uses: edge marking on (DISP3DCNT bit 5) decides for EDGE, everything else uses TRANS. */
uint32_t g3e_carrier(uint32_t disp3dcnt);

/* the value the stencil buffer is cleared to (clear_color = CLEAR_COLOR: rear-plane polygon ID in bits 24-29) */
uint32_t g3e_clear_stencil(uint32_t carrier, uint32_t clear_color);

/* One stencil test + write, as the PICA applies it to a pixel that reaches the depth test: the pixel passes the stencil when `rel` holds between (stencil & mask) and (ref & mask);
   REL_STENCIL_LT means (stencil & mask) < (ref & mask). If it then also passes the depth test the new stencil is (stencil & ~wmask) | (ref & wmask) when `replace` is set, else
   unchanged. A pixel that fails either test keeps its stencil. `blend` says what the colour unit does: 0 = store (opaque polygon, or a translucent one over an empty pixel),
   1 = alpha-blend over the pixel. */
enum { G3E_REL_ALWAYS, G3E_REL_EQ, G3E_REL_NE, G3E_REL_STENCIL_LT, G3E_REL_STENCIL_GT };
typedef struct G3ePass {
    uint8_t blend, rel, ref, mask, wmask, replace;
} G3ePass;

/* the pass of an opaque polygon (POLYGON_ATTR `attr`) */
void g3e_opaque_pass(uint32_t carrier, uint32_t attr, G3ePass *out);
/* The pass of the stencil-rewriting full-screen quad of a split frame (EDGE layout -> TRANS layout): every pixel passes (the caller sets the depth test to ALWAYS and the colour and
   depth writes off), the write mask 0x7F with reference 0 keeps bit 7 and clears the ID bits. */
void g3e_rewrite_pass(G3ePass *out);
/* the passes, in draw order, of a translucent polygon; `seen` = a translucent polygon with the same ID was already drawn this frame. Returns 2 or 3. */
uint32_t g3e_trans_passes(uint32_t carrier, uint32_t attr, int seen, G3ePass out[3]);
int      g3e_pass_stencil_ok(const G3ePass *p, uint32_t stencil);
uint32_t g3e_pass_stencil_new(const G3ePass *p, uint32_t stencil);

/* index of pixel (x, y) in the 8x8-tile z-order layout of a buffer `w` pixels wide (w a multiple of 8): tiles row by row, inside a tile x bit 0, y bit 1, x bit 1, ... */
uint32_t g3e_tiled_index(uint32_t x, uint32_t y, uint32_t w);

/* The raw tiled D24S8 buffer (G3E_RAW_W x G3E_RAW_H words, as GX_TextureCopy leaves it) to the logical image: out[y * 256 + x] is the word of logical pixel (x, y); the hardware
   pixel of logical (x, y) is (191 - y, x), the same turn the colour readback uses (gpu3d_pica.c). A word is depth in bits 0-23, stencil in bits 24-31. */
void g3e_detile_ds(const uint32_t *tiled, uint32_t *out);
/* The same walk for a SECOND copy of the buffer taken at the end of a split frame: only the depth (bits 0-23) of `io` is replaced, the stencil byte (the opaque IDs of the first copy) stays. */
void g3e_detile_depth_merge(const uint32_t *tiled, uint32_t *io);
/* bet 2.8: the same for a logical w x h image (raw buffer h wide, w high; w, h multiples of 8, <= G3E_WMAX): merge = 0 the whole words (g3e_detile_ds), 1 the depth only
   (g3e_detile_depth_merge). out[y * w + x]. */
void g3e_detile_ds_wh(const uint32_t *tiled, uint32_t *out, uint32_t w, uint32_t h, int merge);

/* The depth reconstruction. gpu3d_pica.c feeds the PICA the vertex depth d24 / 2^24 (d24 = gpu3d_depth24_z or gpu3d_depth24_w: the DS quantity, per depth mode), which the PICA
   stores as a 24-bit code D = d01 * 0xFFFFFF (scale 2^24 - 1, not 2^24) and interpolates linearly in screen space. g3e_depth_to_ds undoes the scale, ds = D * 2^24 / 0xFFFFFF, which
   for a 24-bit code is D + (D >> 23) (integer only, no division: the edge pass calls it five times per pixel). It is the SAME formula in Z-buffer and W-buffer mode: the mode
   is applied to the vertex (Z: z/w quantised to 15 significant bits; W: w with the polygon's low bits dropped) before the PICA sees it, so the buffer already holds the right
   quantity; what the PICA cannot do is interpolate w perspective-correctly (counted as wbuffer_frames). g3e_depth_from_ds is the model of the hardware conversion the tests use
   (round to nearest); the round trip is within one code. */
uint32_t g3e_depth_to_ds(uint32_t code);
uint32_t g3e_depth_from_ds(uint32_t d24);

/* The rear plane as a depth value of the DS buffer: (CLEAR_DEPTH & 0x7FFF) * 0x200 + 0x1FF. */
uint32_t g3e_clear_depth_ds(uint32_t clear_depth);
/* the 24-bit code to clear the PICA depth buffer to so that it reads back as g3e_clear_depth_ds (the inverse of g3e_depth_to_ds) */
uint32_t g3e_clear_code(uint32_t clear_depth);

typedef struct G3eParams {
    uint32_t clear_id;          /* polygon ID of the rear plane (CLEAR_COLOR bits 24-29), 0..63 */
    uint32_t clear_z;           /* g3e_clear_depth_ds of CLEAR_DEPTH */
    uint16_t edge_color[8];     /* EDGE_COLOR table, 5-5-5, indexed by polygon ID >> 3 */
} G3eParams;

/* The edge colour as the DS stores it in the 3D layer: 6 bits per channel (c << 1, plus one when non-zero), packed as a layer word's colour bits (R 0-5, G 8-13, B 16-21). */
uint32_t g3e_edge_rgb(uint16_t c555);

/* Edge marking over a logical image of D24S8 words (g3e_detile_ds, EDGE layout) and the layer words (bits 0-5 R, 8-13 G, 16-21 B, 24-28 alpha, same indexing).
   A pixel is marked when it holds a polygon (bit 7) and at least one of its four neighbours has a different polygon ID AND a depth greater than its own (so only the nearer
   side of a boundary is drawn); a neighbour outside the screen is the rear plane (clear_id, clear_z). A marked pixel gets the edge colour of its OWN ID (EDGE_COLOR[id >> 3]) at
   coverage 16 of 31, weighed 17/15 over the colour of the DEEPEST differing-ID neighbour (the pixel behind: the PICA path has no second layer, the neighbour is the proxy) with
   the pixel's own alpha kept; with nothing behind (a rear-plane neighbour or the screen border, alpha 0) the edge colour is written as it is at alpha 16.
   Returns the number of marked pixels. */
uint32_t g3e_edge_pass(uint32_t *layer, const uint32_t *ds, const G3eParams *p);
/* bet 2.8: the same over a logical w x h image (w <= G3E_WMAX; the wide frame is 400 x 240) */
uint32_t g3e_edge_pass_wh(uint32_t *layer, const uint32_t *ds, const G3eParams *p, uint32_t w, uint32_t h);

/* The same rule on one pixel: 1 when (x, y) is marked. Exposed for the tests (the reference for the optimised pass). */
int g3e_edge_pixel(const uint32_t *ds, const G3eParams *p, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif
#endif
