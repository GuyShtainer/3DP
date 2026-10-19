/* gpu3d_selfcheck.h -- bet 2.7a: the on-device D24S8 layout + readback-freshness check of the PICA path (config.ini `hwcheck = 1`).
 *
 * Pure C (no libctru): this file builds the synthetic PLY1 list of check frame n and judges what the PICA path read back; gpu3d_pica.c (pica_selfcheck) only runs the frames
 * through pica_render_record and hands the logical depth/stencil image and the layer words to g3s_judge. Two opaque quads overlap, the nearer one drawn second, with polygon
 * IDs and colours that differ in every frame, in a frame with edge marking on (stencil carrier = polygon IDs, gpu3d_edge.h). What a correct readback shows:
 *   probe A      only quad A:     stencil 0x80|idA, depth near zA, colour A
 *   probe AB     both:            stencil 0x80|idB (the nearer quad passed the depth test), depth near zB, colour B
 *   probe B      only quad B:     stencil 0x80|idB, depth near zB, colour B
 *   probe OUT    neither:         the rear plane: stencil = the clear ID (bit 7 clear), depth = the clear code
 * A readback that matches the PREVIOUS frame's values (and not this one's) is STALE (a cache that was not invalidated); anything else that is wrong is a LAYOUT failure (the
 * depth/stencil words are not D24S8 in the order the de-tile expects). the private design notes. */
#ifndef GPU3D_SELFCHECK_H
#define GPU3D_SELFCHECK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G3S_FRAMES      8u     /* check frames n = 0..7: the D24S8 layout + freshness chain (edge marking on, two opaque quads, unsplit) */
#define G3S_TRANS       8u     /* check frame 8 (2.7(b) N4): an opaque quad O, then a translucent quad T (alpha 15, depth write on) overlapping it: the frame SPLITS (gpu3d_pica.c split_frame),
                                  the TRANS carrier blends T over O and stores T over the empty rear plane, and the final depth is merged back (read_ds(1)). Expected, by hand, not through the
                                  port's de-tile: O alone = stencil 0x80|idO (the first copy, before the rewrite), depth zO, colour O; O+T = stencil 0x80|idO, depth zT (the merge), colour the
                                  50 % blend; T alone = stencil the clear ID, depth zT (the merge), colour T; neither = the rear plane. No stale test (nothing to compare with). */
#define G3S_TOTAL       9u     /* all check frames */
#define G3S_LIST_BYTES  4096u  /* room for the list of one check frame */

#define G3S_OK      0
#define G3S_LAYOUT  1
#define G3S_STALE   2

typedef struct G3sVerdict {
    int code;                  /* G3S_OK / G3S_LAYOUT / G3S_STALE */
    uint32_t x, y;             /* the probe pixel of the first mismatch */
    uint32_t got, want;        /* the word read there and what frame n expects (the field named by `what`) */
    const char *what;          /* "stencil" / "depth" / "colour" */
    const char *probe;         /* "A" / "AB" / "B" / "OUT" */
} G3sVerdict;

/* The PLY1 list of check frame n (0..G3S_TRANS) into buf; returns its length, 0 when cap is too small or n is out of range. */
size_t g3s_build(uint8_t *buf, size_t cap, uint32_t n);
/* Judge frame n: ds = the logical 256x192 depth/stencil image (gpu3d_edge.h: out[y * 256 + x], depth in bits 0-23, stencil in 24-31), layer = the 256x192 layer words. */
int g3s_judge(const uint32_t *ds, const uint32_t *layer, uint32_t n, G3sVerdict *v);

#ifdef __cplusplus
}
#endif

#endif
