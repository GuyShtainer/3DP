/* hw_wide.h -- bet 2.8, widescreen + stereo: the pure formulas. <stdint.h> only, so the host tests and the private wide-model harness
 * compile it on the build machine.
 *
 * Widescreen, "reveal, not stretch": the camera's projection is built with sinFovY x 240/192 (the vertical field grows by the 48 extra lines) and the aspect x 5/4
 * (400/240 for the game's 4/3), so _11 and _00 shrink by 192/240 and 256/400: one world unit covers the same number of pixels as on the DS, and the DS's 256 x 192
 * is the centre of the 400 x 240 frame at (72, 24).
 *
 * Stereo: each perspective polygon's clip-space x is sheared per eye AFTER the geometry engine: x' = x + e * sigma * (w - c), e = -1 left, +1 right, c = the camera
 * distance (zero parallax at the camera target), sigma = slider * _00 * (iod / 2) / c (a horizontal eye offset of iod / 2 world units, re-converged at depth c).
 * Orthographic projections (w constant) and the 2D layers are at zero parallax. */
#ifndef HW_WIDE_H
#define HW_WIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWWIDE_W        400
#define HWWIDE_H        240
#define HWWIDE_X0       72      /* the DS frame's place in the wide frame */
#define HWWIDE_Y0       24
#define HWWIDE_GUARD_Q12 512    /* the stereo x guard band g = 512 / 4096 = 0.125 (>= 16/256 = 0.0625, the brief's floor at full slider) */
#define HWWIDE_IOD      96.0f   /* eye separation in world units at slider 1: the bet-0 spike's iodScale (source/main.c, "2/3 of a 6.5 cm viewer") */

/* sinFovY x 5/4, rounded to nearest. A result that would reach 1.0 (FX32_ONE) cannot be passed to MTX_Perspective: then *ok = 0 and the argument comes back unchanged. */
int32_t hwwide_sin(int32_t sin_fovy, int *ok);
/* aspect x 5/4, rounded to nearest (4/3 = 5461 -> 6826: the ratio that keeps _00's per-pixel scale; 400/240 rounds to 6827) */
int32_t hwwide_aspect(int32_t aspect);
/* the per-eye shear factor at this slider (0..1): slider * (m00 / 4096) * (iod / 2) / (dist / 4096); 0 when dist <= 0 or m00 <= 0 */
float hwwide_sigma(float slider, int32_t m00, int32_t dist, float iod);
/* x + e_sigma * (w - dist), rounded to nearest, saturated to int32 */
int32_t hwwide_shear(int32_t x, int32_t w, int32_t dist, float e_sigma);
/* the sheared vertex's wide screen x WITHOUT the DS's integer snap: (x + e_sigma * (w - dist) + w) * 128 / w + 72 (w = the 24-bit w; 0 when w <= 0). An eye's perspective
   polygons are drawn at this sub-pixel x: the shear tilts the ground's tile seams on screen (it grows with depth), and snapping each sheared vertex to a pixel opens
   one-pixel T-junction cracks along them (the clear colour shows through: measured 61 / 79 uncovered pixels per eye against 18 mono on Twinleaf). floor() of it at
   e_sigma 0 is gpu3d_viewport_wide_xy's sx (the mono frame keeps the snap: gate (a)). */
double hwwide_shear_sx(int32_t x, int32_t w, int32_t dist, float e_sigma);
/* review 2.8 N2: 1 when the eye's shear of a vertex at depth w stays inside the guard band: |e_sigma * (w - dist)| <= w * (6400 / 4096) * (guard_q12 / 4096), i.e. sigma |1 - c / w| <=
   1.5625 g (the guard is g beyond the frame edge x = +-1.5625 w). The shear is applied per vertex in gpu3d_pica.c; a violation is COUNTED there (pica_stats.wide_shear_viol),
   it does not stop the frame. w <= 0 (nothing to bound) returns 1. */
int hwwide_shear_in_guard(int32_t w, int32_t dist, float e_sigma, int32_t guard_q12);

#ifdef __cplusplus
}
#endif
#endif
