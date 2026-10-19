/* gpu3d_rec.h -- the 3D half of the frame record (bet 2.5 slice (b); design: the private design notes).
 *
 * The record is what the game thread hands to the render thread at the snapshot point, next to the 2D record (gpu2d.h Gpu2dFrame). It OWNS everything the PICA rasteriser
 * reads, so the render thread never touches the live engine state or the live texture VRAM (the rule 2.3(d) set for the 2D side):
 *   - `list`    the engine's sealed polygon list as PLY1 bytes (gpu3d_export_list), written by the game thread into a buffer the record owns;
 *   - `tex`     one entry per distinct texture of the frame: its content key, the render-thread cache slot it lives in, and, when the slot does not hold it yet, the
 *               DECODED texel block (tiled RGBA8, gpu3d_tex_decode) in the record's `texels` pool;
 *   - `poly_tex` per polygon, the index into `tex` (G3R_NO_TEX = untextured).
 * The texture cache is DECIDED on the game thread and OBEYED on the render thread: the game thread keeps a mirror of which key sits in which of the render thread's G3R_SLOTS
 * slots (G3rMirror); gpu3d_rec_textures plans the frame against the mirror COMMITTED so far and leaves the mirror as it will be in `rec->mirror`; the caller copies
 * it into the committed mirror only when the render thread TOOK the record (an overrun drops the record, and then nothing it planned happened). The render thread
 * uploads the entries marked `upload` and binds the others by slot.
 *
 * Pure C: <stdint.h> / <stddef.h> only. gpu3d_rec.c includes the PLY1 reader (gpu3d_list.h) and the texture module (gpu3d_tex.h); this header includes neither, so the
 * engine file that includes it (gpu3d.h declares the same type names as gpu3d_list.h) can too. */
#ifndef GPU3D_REC_H
#define GPU3D_REC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G3R_SLOTS        48u       /* texture slots of the render thread's cache */
#define G3R_TEX_MAX      128u      /* distinct textures (texparam, palette base) one frame can name */
#define G3R_POLYS        4096u     /* poly_tex entries: a wide list's polygon limit (bet 2.8, gpu3d_list.h GPU3D_LIST_WIDE_POLYS; the DS polygon RAM is 2048) */
#define G3R_NO_TEX       0xFFFFu
#define G3R_TEXMEM_BYTES 0x80000u   /* the flat texture view (4 slots of 128 KB) */
#define G3R_PALMEM_BYTES 0x18000u   /* the flat texture-palette view (6 slots of 16 KB) */
#define G3R_SCRATCH_BYTES (G3R_TEXMEM_BYTES + G3R_PALMEM_BYTES)
#define G3R_POOL_WORDS   (512u * 1024u)   /* decoded texels one record can carry (2 MB): a first frame decodes every texture it names */
#define G3R_LIST_MAX     (256u + 32u * 2048u + 32u * 20480u)   /* the largest PLY1 image: header, 2048 polygons, 20480 expanded vertices (gpu3d_list.h GPU3D_MAX_VERTS) */
#define G3R_WIDE_LIST_MAX (256u + 32u * 4096u + 32u * 40960u)  /* bet 2.8: the largest wide PLY1 image (gpu3d_list.h GPU3D_LIST_WIDE_POLYS / _VERTS) */

/* what the game thread believes one render-thread cache slot holds */
typedef struct G3rResident {
    uint64_t key;                  /* gpu3d_tex_key of the decoded content */
    uint32_t last_use;             /* the mirror frame of the latest record that used it */
    uint16_t w, h;
    uint8_t used;
} G3rResident;

typedef struct G3rMirror {
    G3rResident slot[G3R_SLOTS];
    uint32_t frame;                /* records planned so far (the LRU clock) */
} G3rMirror;

typedef struct G3rTex {
    uint64_t key;
    uint32_t texparam;             /* of the first polygon that named it (the sampler's wrap bits are read from each polygon's own texparam) */
    uint32_t texpalette;
    uint16_t w, h;
    uint16_t slot;                 /* the render thread's cache slot */
    uint16_t upload;               /* 1: the slot does not hold this key yet; the decoded block is at texels[texel_off ..) */
    uint32_t texel_off;            /* in 32-bit words */
} G3rTex;

/* counts of one record (the game thread's hash / decode work), summed by the caller into the run totals */
typedef struct G3rStats {
    uint32_t lookups;              /* textured polygons */
    uint32_t hits;                 /* of them: resolved without decoding (same texture seen earlier in the frame, or already in a slot) */
    uint32_t keys;                 /* content keys computed (distinct texture / palette pairs of the frame) */
    uint32_t decodes;              /* misses decoded into the pool */
    uint32_t unsupported;          /* compressed (format 5) polygons: drawn untextured */
    uint32_t tex_full;             /* more than G3R_TEX_MAX distinct textures: untextured */
    uint32_t cache_full;           /* no free or evictable slot: untextured */
    uint32_t pool_full;            /* the texel pool had no room for the decode: untextured, not resident */
    uint64_t hashed_bytes;         /* texture + palette bytes run through the content key */
    uint64_t texel_bytes;          /* decoded bytes written into the pool */
    uint64_t hash_ticks;           /* clock ticks in gather + key (0 without a clock) */
    uint64_t decode_ticks;         /* clock ticks in the decodes */
} G3rStats;

typedef struct Gpu3dRec {
    uint8_t *list;                 /* G3R_LIST_MAX bytes, owned by the record's creator */
    uint32_t list_cap;
    uint32_t list_len;             /* bytes of PLY1 in `list`; 0 = no 3D layer this frame */
    uint32_t key;                  /* the frame key the list was exported with */
    uint32_t npolys;               /* polygons of the list as gpu3d_rec_textures saw them */
    uint16_t poly_tex[G3R_POLYS];
    G3rTex tex[G3R_TEX_MAX];
    uint32_t ntex;
    uint32_t *texels;              /* texel_cap words, owned by the record's creator */
    uint32_t texel_cap;
    uint32_t texel_used;
    G3rMirror mirror;              /* the render-thread cache as it will be once this record has been rendered */
    G3rStats st;
} Gpu3dRec;

/* Where the texture bytes come from: either the VRAM model's slot views (tex_slot 0..3 of 128 KB, tex_pltt 0..5 of 16 KB; NULL = nothing mapped, reads zero), gathered
   into `scratch` (GPU3D_TEXMEM_BYTES + GPU3D_TEXPAL_BYTES = 0x98000 bytes) for the bytes a texture references only, or two flat views (flat_tex 512 KB, flat_pal 96 KB)
   used as they are. clock (optional) is a tick counter for the hash / decode time split. */
typedef struct G3rSrc {
    const uint8_t *(*tex_slot)(uint32_t slot);
    const uint8_t *(*tex_pltt)(uint32_t slot);
    uint8_t *scratch;
    const uint8_t *flat_tex;
    const uint8_t *flat_pal;
    uint64_t (*clock)(void);
} G3rSrc;

void gpu3d_rec_reset(Gpu3dRec *r);                       /* no list, no textures (keeps the buffers and the mirror) */
void gpu3d_rec_mirror_init(G3rMirror *m);

/* Plan the textures of the list in r->list (list_len bytes of PLY1): fills poly_tex, tex, texels, st and r->mirror (= *committed advanced by this record). A list that
   does not open as PLY1 leaves the record without textures and returns the reader's GPU3D_ERR_* code; 0 otherwise. Honours DISP3DCNT bit 0 (texture mapping). */
int gpu3d_rec_textures(Gpu3dRec *r, const G3rMirror *committed, const G3rSrc *src);

/* The bytes one record carries: list + texel pool actually used (+ the per-polygon index and table), for the bytes-per-frame line. */
uint64_t gpu3d_rec_bytes(const Gpu3dRec *r);

#ifdef __cplusplus
}
#endif
#endif
