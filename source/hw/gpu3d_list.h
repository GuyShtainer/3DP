/* gpu3d_list.h -- the DS polygon list that the PICA rasteriser draws (bet 2.5 slice (a); format doc: the private design notes).
 *
 * One Gpu3dList is one sealed frame of the DS geometry engine: a header, the polygons and their vertices, as bytes in a little-endian "PLY1" file or buffer.
 * Today the producer is the oracle harness (the private melonDS harness's --poly-dump); bet 2.4(b) makes the port's own geometry engine write the same bytes, so
 * nothing in the rasteriser changes when the port's list replaces the dumped one. Pure C: <stdint.h> / <stddef.h> only (the host tests and the oracle tools compile it
 * on the build machine). Nothing here asserts on the contents of a list: a malformed buffer is refused with a fixed error code, an unsupported feature is counted by the
 * caller. Files are read through accessors, never cast to structs (byte order and packing are explicit).
 *
 * Header (256 bytes, all fields u32 LE unless noted):
 *   0x00 magic "PLY1"        0x04 version (1)           0x08 frame key           0x0C header size (256)
 *   0x10 DISP3DCNT           0x14 CLEAR_COLOR           0x18 CLEAR_DEPTH         0x1C swap bits: bit 0 manual translucent sort (0 = unknown, see doc), bit 1 W-buffer
 *   0x20 VIEWPORT command word (x1 | y1 << 8 | x2 << 16 | y2 << 24, y counted from the top as the game sent it)
 *   0x24 polygon count       0x28 vertex count          0x2C/0x30 texture dump hash (u64: low, high) of the matching tex_<hash16>.bin
 *   0x34 alpha-test reference   0x38 edge colours 8 x u16 (EDGE_COLOR)   0x48 fog colour   0x4C fog offset   0x50 toon table 32 x u16
 *   0x90 polygon count as the producer's renderer counted it (the count proof)   0x94 polygons the producer's renderer skips (flag bit 5)   0x98 flags (bit 0 stale)
 *   0x9C bet 2.8 wide word: bit 0 wide list (the frame is 400 x 240; polygons tagged GPU3D_LT_WIDE map to it, the others are the DS mapping at +72, +24), bit 1 stereo
 *   0xA0 stereo camera distance (s32 20.12, the convergence w)   0xA4 the camera projection's DS _00 (s32 20.12)   0xA8 the stereo x guard band g (q12)
 *   0xAC .. 0xFF reserved, zero.  0x9C..0xAB are zero on a narrow list (the bytes are the same as before bet 2.8).
 * Polygon record (32 bytes): +0 POLYGON_ATTR as sent (u32)  +4 TEXIMAGE_PARAM (u32)  +8 PLTT_BASE (u16, palette base >> 4 as the DS holds it)  +10 vertex count (u8, 3..10)
 *   +11 flags (u8: bit0 translucent, bit1 facing the viewer, bit2 shadow mask, bit3 shadow, bit4 W-buffer, bit5 degenerate, bit6 line)  +12 first vertex index (u32)
 *   +16 top Y (s16)  +18 bottom Y (s16)  +20 sort key (u32)  +24 bet 2.8 tag (u8: bit 0 wide-mapped, bit 1 perspective; 0 on a narrow list)  +25 reserved
 * Vertex record (32 bytes): +0 x +4 y +8 z +12 w (s32, 20.12 clip space after the DS frustum clipping, W truncated to 24 bits)  +16 r +17 g +18 b (u8, 5-bit lit colour,
 *   0..31)  +20 s +22 t (s16, 4.4 texel coordinates after texgen)  +24 screen x (s16) +26 screen y (s16) +28 depth (u32, the producer's 24-bit depth for this vertex). */
#ifndef GPU3D_LIST_H
#define GPU3D_LIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU3D_HDR_BYTES   256u
#define GPU3D_POLY_BYTES  32u
#define GPU3D_VERT_BYTES  32u
#define GPU3D_MAX_POLYS   2048u    /* the DS polygon RAM */
#define GPU3D_MAX_VERTS   20480u   /* vertex records of one list: EXPANDED (each polygon owns its vertices, a strip neighbour's shared vertices are written again), so up to GPU3D_MAX_POLYS x GPU3D_MAX_PV.
                                      The DS vertex RAM holds 6144 SHARED vertices; the expanded count of a full DS frame can exceed that (a strip of quads shares two vertices per quad, and the
                                      clipper adds up to six per polygon), bet 2.5 slice (b), review 2.4 N6 */
#define GPU3D_MAX_PV      10u      /* vertices of one polygon: a quad clipped by six planes */
#define GPU3D_LIST_WIDE_POLYS 4096u    /* bet 2.8: a wide list's limits (the port's wide polygon RAM, its vertices expanded) */
#define GPU3D_LIST_WIDE_VERTS 40960u
#define GPU3D_LW_WIDE    0x01u      /* header 0x9C bits */
#define GPU3D_LW_STEREO  0x02u
#define GPU3D_LT_WIDE    0x01u      /* polygon +24 bits (the same values as the engine's GPU3D_WT_*) */
#define GPU3D_LT_PERSP   0x02u

#define GPU3D_ERR_OK       0
#define GPU3D_ERR_SHORT    1       /* buffer shorter than the header, or than header + polygons + vertices */
#define GPU3D_ERR_MAGIC    2
#define GPU3D_ERR_VERSION  3
#define GPU3D_ERR_COUNTS   4       /* more polygons / vertices than the DS RAM holds */
#define GPU3D_ERR_RANGE    5       /* a polygon's vertex range leaves the vertex array, or its vertex count is not 3..10 */

/* DISP3DCNT bits (GBATEK "DS 3D Video", 4000060h) */
#define GPU3D_DISP_TEXTURE   (1u << 0)
#define GPU3D_DISP_HIGHLIGHT (1u << 1)
#define GPU3D_DISP_ALPHATEST (1u << 2)
#define GPU3D_DISP_BLEND     (1u << 3)
#define GPU3D_DISP_AA        (1u << 4)
#define GPU3D_DISP_EDGE      (1u << 5)
#define GPU3D_DISP_FOGALPHA  (1u << 6)
#define GPU3D_DISP_FOG       (1u << 7)
#define GPU3D_DISP_BITMAPCLR (1u << 14)

/* polygon flag bits */
#define GPU3D_PF_TRANSLUCENT 0x01u
#define GPU3D_PF_FACING      0x02u
#define GPU3D_PF_SHADOWMASK  0x04u
#define GPU3D_PF_SHADOW      0x08u
#define GPU3D_PF_WBUFFER     0x10u
#define GPU3D_PF_DEGENERATE  0x20u
#define GPU3D_PF_LINE        0x40u

typedef struct Gpu3dPoly {
    uint32_t attr;          /* POLYGON_ATTR as sent */
    uint32_t texparam;      /* TEXIMAGE_PARAM */
    uint16_t texpalette;    /* PLTT_BASE */
    uint8_t  nverts;
    uint8_t  flags;
    uint32_t first;         /* index of the first vertex */
    int16_t  ytop, ybottom;
    uint32_t sortkey;
    uint8_t  wtag;          /* bet 2.8: GPU3D_LT_* */
} Gpu3dPoly;

typedef struct Gpu3dVert {
    int32_t x, y, z, w;     /* 20.12 clip space */
    uint8_t r, g, b;        /* 5-bit lit colour */
    int16_t s, t;           /* 4.4 texel coordinates */
    int16_t sx, sy;         /* the producer's screen position (a cross-check, the rasteriser recomputes it) */
    uint32_t depth24;       /* the producer's 24-bit depth */
} Gpu3dVert;

typedef struct Gpu3dList {
    const uint8_t *buf;     /* the whole file / buffer (not owned) */
    size_t len;
    uint32_t key, disp3dcnt, clear_color, clear_depth, swap_bits, viewport;
    uint32_t npolys, nverts;
    uint64_t tex_hash;
    uint32_t alpha_ref, fog_color, fog_offset, flags;
    uint32_t render_count, degenerate_count;
    uint16_t edge_color[8];
    uint16_t toon[32];
    uint32_t wide;          /* bet 2.8: GPU3D_LW_* */
    int32_t st_dist, st_m00, st_guard;
} Gpu3dList;

/* Validates and binds a buffer. Returns GPU3D_ERR_OK or a GPU3D_ERR_* code; *l is only meaningful on OK. */
int gpu3d_list_open(Gpu3dList *l, const uint8_t *buf, size_t len);
const char *gpu3d_list_strerror(int err);
void gpu3d_list_poly(const Gpu3dList *l, uint32_t i, Gpu3dPoly *out);     /* i < npolys (a larger i yields a zeroed record) */
void gpu3d_list_vert(const Gpu3dList *l, uint32_t i, Gpu3dVert *out);     /* i < nverts */

/* Writer (host tests, the future port engine): the buffer size of a list, and the byte image of a header / polygon / vertex. */
size_t gpu3d_list_size(uint32_t npolys, uint32_t nverts);
void gpu3d_list_put_header(uint8_t *buf, const Gpu3dList *l);             /* writes the 256 header bytes from the fields of *l */
void gpu3d_list_put_poly(uint8_t *buf, uint32_t i, const Gpu3dPoly *p, uint32_t npolys);
void gpu3d_list_put_vert(uint8_t *buf, uint32_t i, const Gpu3dVert *v, uint32_t npolys);

/* The DS rule for "translucent" (GBATEK "DS 3D Video", POLYGON_ATTR alpha and TEXIMAGE_PARAM format): alpha 1..30, or a texture format that carries partial alpha
 * (1 = A3I5, 6 = A5I3). Alpha 0 is wireframe and alpha 31 opaque; format 0 is no texture. */
int gpu3d_is_translucent(uint32_t attr, uint32_t texparam);

/* The viewport transform in the DS integer arithmetic (GBATEK "DS 3D Video", viewport): clip x, y, w (20.12) and the VIEWPORT word to screen pixels, y counted from the top.
 * Writes *sx in 0..256 and *sy in 0..192. w <= 0 yields the viewport origin. */
void gpu3d_viewport_xy(uint32_t viewport, int32_t x, int32_t y, int32_t w, int32_t *sx, int32_t *sy);
/* Bet 2.8: the wide viewport (400 x 240, a GPU3D_LT_WIDE polygon of a wide list): the DS full-screen transform, floored, plus (72, 24) -- x = (x + w) * 256 / 2w + 72,
 * y = (w - y) * 192 / 2w + 24 -- so a vertex inside the DS planes lands on its DS pixel + (72, 24) exactly (the engine draws these polygons with the DS matrix over a wider
 * volume). 64-bit signed, NOT clamped (a guard-band vertex lies left of 0 or right of 400; the PICA clips it). The geometry engine's wide transform. w <= 0: (0, 0). */
void gpu3d_viewport_wide_xy(int32_t x, int32_t y, int32_t w, int32_t *sx, int32_t *sy);

/* The DS 24-bit depth of a vertex: Z-buffer mode maps clip z / w from [-1, 1] to [0, 0xFFFFFF] (kept in 15 significant bits as the DS stores it), W-buffer mode is w
 * itself with the polygon's low bits dropped (gpu3d_w_drop). The rasteriser's depth-test OUTCOME is what has to agree with the oracle, not this value. */
uint32_t gpu3d_depth24_z(int32_t z, int32_t w);
uint32_t gpu3d_depth24_w(int32_t w, uint32_t drop);
/* The number of low bits W-buffer mode drops for one polygon: the DS keeps 16 significant bits of w, sized by the polygon's LARGEST w (the bit length rounded up to a
 * multiple of 4; 0 when it fits in 16 bits). */
uint32_t gpu3d_w_drop(const int32_t *w, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif
