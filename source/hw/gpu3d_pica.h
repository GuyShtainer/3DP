/* gpu3d_pica.h -- the PICA200 rasteriser of a DS polygon list (bet 2.5 slice (a); bet doc: the private design notes).
 *
 * pica_render_record draws one sealed DS frame, as the frame record (gpu3d_rec.h) carries it (a PLY1 list, the private design notes, plus the decoded textures
 * the game thread planned), into a 256x192 RGBA8 + D24S8 render target with citro3d and gives the result back as DS-style 3D-layer words. The record is the ONLY input:
 * the texture cache (G3R_SLOTS slots) is driven by the slot numbers and upload flags the game thread wrote into it (bet 2.5 slice (b)). libctru + citro3d file: the only part of the 3D path that touches the GPU; list parsing, texture decoding
 * and the arithmetic live in the pure files gpu3d_list.c / gpu3d_tex.c. One caller, one thread: every call happens on the render thread (Hard convention 2).
 * What the PICA path does not reproduce is counted in pica_unsupported and the frame is drawn as well as it can be; a NULL argument is refused with an error code. */
#ifndef GPU3D_PICA_H
#define GPU3D_PICA_H

#include <stddef.h>
#include <stdint.h>

#include "gpu3d_rec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PICA_W 256
#define PICA_H 192

/* Counters of DS features the PICA path does not reproduce (reset by pica_stats_reset). Polygon counts unless a name ends in _frames (header-level state, counted once
 * per frame). Drawn anyway with the closest equivalent where one exists; the gate report quotes these next to the pixel numbers. */
typedef struct PicaUnsupported {
    uint32_t wireframe;          /* alpha 0 polygons (outline only on the DS): skipped */
    uint32_t mode_decal;         /* polygon mode 1: drawn as modulation */
    uint32_t mode_toon;          /* polygon mode 2 (toon / highlight): drawn as modulation */
    uint32_t mode_shadow;        /* polygon mode 3: drawn as modulation (shadow polygons are not stencil-masked) */
    uint32_t tex_compressed;     /* texture format 5: drawn untextured */
    uint32_t tex_cache_full;     /* texture needed but the cache has no free or evictable slot: drawn untextured */
    uint32_t depth_equal;        /* depth test "equal" (attr bit 14): drawn with the ordinary less test */
    uint32_t translucent_same_id;/* translucent polygons whose ID was already drawn translucent this frame (the DS skips pixels whose latest blend had that ID) drawn in the EDGE stencil layout,
                                    where the rule cannot be applied. Since bet 2.5(d) an edge-marking frame is split before its first translucent polygon (PicaStats.split_frames), so the rule
                                    is applied there too (PicaStats.same_id_polys) and this stays 0 */
    uint32_t bad_tex_index;      /* polygons whose record texture index was past the record's table (review N8): drawn untextured */
    uint32_t wbuffer_frames;     /* frames in W-buffer mode: depth is interpolated linearly in screen space, the DS interpolates w perspective-correctly */
    uint32_t edge_frames;        /* frames with edge marking on (DISP3DCNT bit 5) that did NOT get an edge pass (0 = every edge-marking frame was marked) */
    uint32_t aa_frames;          /* frames with anti-aliasing on (bit 4): none */
    uint32_t fog_frames;         /* frames with fog on (bit 7): none */
    uint32_t alphatest_frames;   /* frames with the alpha test on (bit 2): the PICA test is always "alpha > 0" */
    uint32_t bitmap_clear_frames;/* frames with a rear-plane bitmap (bit 14): flat clear */
    uint32_t polys_skipped;      /* degenerate polygons the producer's renderer skips (flag bit 5) */
    uint32_t hw_mode_clamped;    /* texture or vertex data that had to be clamped to fit the PICA (size, vertex budget) */
} PicaUnsupported;

typedef struct PicaStats {
    uint32_t frames;
    uint32_t polygons_drawn;
    uint32_t draw_calls;
    uint32_t tex_lookups;        /* polygons that bound a texture */
    uint32_t tex_uploads;        /* texture blocks uploaded into a slot (the record's `upload` entries) */
    uint32_t tex_evictions;      /* of them: slots that held another texture */
    uint32_t tex_failed;         /* uploads that failed (no linear memory): the slot stays empty, its polygons are drawn untextured */
    uint64_t cpu_ticks;          /* ARM11 system ticks (268.1 MHz) spent drawing on the CPU (vertex build, texture binds, command building) */
    uint64_t gpu_ticks;          /* ticks from the end of the command building to the finished readback (GPU draw + display transfer + conversion) */
    uint64_t readback_ticks;     /* the display transfer + word conversion share of gpu_ticks */
    uint64_t upload_ticks;       /* texture uploads (TexInit + copy + flush), part of the raster */
    uint64_t transfer_ticks;     /* the display transfer into the linear buffer (GX_DisplayTransfer, waited for), part of readback_ticks */
    uint64_t convert_ticks;      /* the conversion of the linear buffer to the layer words, the rest of readback_ticks */
    uint64_t ds_ticks;           /* the depth/stencil readback (GX_TextureCopy, waited for), edge-marking frames only; NOT part of readback_ticks */
    uint64_t detile_ticks;       /* the Morton de-tile of the depth/stencil readback on the CPU */
    uint64_t edge_ticks;         /* the edge pass on the CPU, edge-marking frames only */
    uint32_t edge_frames_run;    /* frames that got the edge pass */
    uint64_t edge_marked;        /* pixels marked by the edge pass, summed over frames */
    uint32_t same_id_polys;      /* translucent polygons that met an earlier translucent polygon of their ID in a frame whose stencil carries the translucent ID (the rule applied) */
    uint32_t split_frames;       /* edge-marking frames split after their opaque polygons (bet 2.5(d), review B3: the stencil rewritten from the EDGE to the TRANS layout) */
    uint32_t split_depth_copies; /* of them: frames that took a second depth copy because a translucent polygon wrote depth (POLYGON_ATTR bit 11) */
    uint32_t opaque_after_split; /* opaque polygons that came after the first translucent one (the list order is final, so 0 on real lists): drawn with the TRANS opaque pass */
    uint64_t split_ticks;        /* the EXTRA work of the split: the first frame end, the second begin + the rewrite quad, and the second depth copy + merge de-tile; the first copy and its
                                    de-tile are ds_ticks / detile_ticks (they replace the end-of-frame ones), and are part of readback_ticks */
    uint32_t wide_frames;        /* bet 2.8: frames (eyes) drawn on the 400 x 240 target */
    uint32_t wide_linear_free;   /* bet 2.8: linearSpaceFree() right after the wide target and buffers were made (0: never made) */
    uint32_t wide_shear_viol;    /* review 2.8 N2: sheared vertices whose shear left the guard band (hw_wide.h hwwide_shear_in_guard); 0 on the field camera */
    uint32_t wide_shear_verts;   /* ... of this many sheared vertices */
    float last_sigma;            /* bet 2.8: the stereo shear factor of the latest eye drawn (hw_wide.h hwwide_sigma) */
    uint32_t clear_word;         /* bet 2.8: the layer word a clear (rear-plane) pixel of the latest frame reads back as (the read_colour conversion of the clear RGBA) */
} PicaStats;

extern PicaUnsupported pica_unsupported;
extern PicaStats pica_stats;
void pica_stats_reset(void);

/* shbin = the assembled vertex shader (source/hw/gpu3d_pica.v.pica); initialises citro3d, the render target, the shader and the texture cache. 0 on success, else a fixed code. */
int  pica_init(const void *shbin, uint32_t shbin_size);
void pica_exit(void);

/* Tunables read by pica_init from the caller (the replay app reads sdmc:/3ds/3DP/pica/params.txt): the screen-position bias in pixels, added to the integer DS
 * screen position before the PICA samples it. */
void pica_set_bias(float bx, float by);
/* the attribute phase in pixels (default 0.5 0.5): attributes (texture coordinate, colour, depth) are moved back by this much along their screen gradient, because the DS
 * evaluates them at the integer pixel position and the PICA at the pixel centre. 0 0 switches the correction off. */
void pica_set_phase(float px, float py);

/* One frame: clears the target to the list's rear-plane colour / depth, uploads the record's new textures, draws every polygon in list order, finishes the GPU work and
 * does the readback and the conversion (pica_stats has the split). 0 on success; else the PLY1 reader's GPU3D_ERR_* code (nothing was drawn and pica_layer_words()
 * is the previous frame's). */
int pica_render_record(const Gpu3dRec *r);
/* Bet 2.8: the same for one eye. A narrow list draws as pica_render_record. A wide list (PLY1 0x9C bit 0) draws on the 400 x 240 target (made at the first one; a failure to
 * make it refuses the frame with GPU3D_ERR_COUNTS): eye 0 mono, -1 left, +1 right; a stereo list's GPU3D_LT_PERSP polygons are sheared by -/+ hwwide_sigma(slider, the list's
 * DS _00, its camera distance, HWWIDE_IOD). Eye 0 and -1 land in pica_layer_wide(0), +1 in pica_layer_wide(1); only eye 0 / -1 uploads the record's textures, so a stereo
 * frame draws -1 then +1 (pica_render_record = eye 0). */
int pica_render_eye(const Gpu3dRec *r, int eye, float slider);

/* self-test aids: read depth/stencil back on every frame (not only on edge-marking frames); the last frame's depth/stencil as the logical 256x192 image of gpu3d_edge.h
   (D24S8 words, valid for frames that read it back) */
void pica_set_force_ds(int on);
const uint32_t *pica_ds_words(void);

/* The last frame as the harness's 3D layer: out[256*192] words, bits 0-5 R, 8-13 G, 16-21 B (6-bit DS colour), 24-28 alpha (5 bit), y from the top (L3D1 layer, colour half). */
const uint32_t *pica_layer_words(void);
/* Bet 2.8: the latest wide layer of slot 0 (mono / left eye) or 1 (right eye): 400 x 240 words in the same format, y from the top; NULL before the first wide list */
const uint32_t *pica_layer_wide(uint32_t slot);

/* calibration aid for the replay app's self-test: the raw hardware-order readback (PICA_W * PICA_H RGBA8 words, the display transfer's output layout, before any reorientation) */
const uint32_t *pica_raw_words(void);

/* Bet 2.7a (config.ini `hwcheck`): the on-device D24S8 layout + readback-freshness self-check. Renders the G3S_FRAMES synthetic frames of gpu3d_selfcheck.h through
 * pica_render_record (edge marking on, depth/stencil read back every frame) and judges each readback. mutant = 1 judges the RAW tiled buffer as if it were the logical image (a
 * planted de-tile fault: the check must FAIL); mutant = 2 skips the split frame's final depth merge copy (only the translucent frame G3S_TRANS must FAIL). pica_stats / pica_unsupported / the force flag are restored afterwards, so the game's counters do not move. Render thread, after
 * pica_init succeeded. Returns 0 when the frames ran (the verdicts are in *out), -1 when they could not (no target, no memory, a frame refused). */
typedef struct PicaSelfcheck {
    uint32_t frames_run;
    int code[9];                 /* G3S_OK / G3S_LAYOUT / G3S_STALE per frame; [8] = the translucent check frame (G3S_TRANS) */
    uint32_t bad_x, bad_y, bad_got, bad_want;   /* the first mismatch of the first failing frame */
    const char *bad_probe, *bad_what;
    uint32_t bad_frame;
    uint32_t trans_run;          /* 2.7(b) N4: the translucent frame (G3S_TRANS) ran; its verdict is code[G3S_TRANS] and the first mismatch below */
    uint32_t t_x, t_y, t_got, t_want;
    const char *t_probe, *t_what;
} PicaSelfcheck;
int pica_selfcheck(int mutant, PicaSelfcheck *out);

#ifdef __cplusplus
}
#endif
#endif
