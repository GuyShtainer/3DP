/* gpu3d_rec_fixture.h -- plain types for gpu3d_rec_fixture.c (which sees gpu3d.h) and gpu3d_rec_test.c (which sees gpu3d_list.h / gpu3d_rec.h): the headers declare the same
 * type names, so they never meet in one translation unit. */
#ifndef GPU3D_REC_FIXTURE_H
#define GPU3D_REC_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

/* scene 0 = two textured triangles (direct-colour 8x8 texture at texture-memory offset 0x100); scene 1 = a different scene (one untextured quad). Each call runs the scene
   through the one static engine (the engine is NOT reset: the second call mutates the same engine the first one sealed from), seals it, latches it (gpu3d_vblank) and
   exports the sealed list with `disp3dcnt` into buf. Returns the byte count (0 = cap too small). */
size_t recfx_export(uint8_t *buf, size_t cap, int scene, uint32_t disp3dcnt);
#endif
