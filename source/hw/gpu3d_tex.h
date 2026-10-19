/* gpu3d_tex.h -- DS texture memory to PICA texture data (bet 2.5 slice (a); pure C, host-tested).
 *
 * The DS keeps textures in VRAM banks viewed as one flat 512 KB texture memory (a texture starts at byte (TEXIMAGE_PARAM offset field) << 3) and their palettes in a flat
 * 96 KB texture-palette memory (byte (PLTT_BASE) << 4, or << 3 for the 4-colour format). This module decodes ONE texture to the layout the PICA wants: RGBA8, 8x8-tiled
 * (z-order inside a tile, tiles row by row), one 32-bit word per texel written as 0xRRGGBBAA. ROWS ARE STORED BOTTOM-UP: DS texel row y lives in memory row h-1-y, because the PICA sampler reads memory row h-1-floor(v*h) (measured in Azahar by the replay app's self-test), so a plain v = t/h samples the right DS row. Formats 1 (A3I5), 2 (4 colours), 3 (16 colours), 4 (256 colours),
 * 6 (A5I3) and 7 (direct colour) are decoded; format 5 (4x4 compressed) and format 0 (no texture) are refused with GPU3D_TEX_UNSUPPORTED (the caller counts them).
 *
 * Colour encoding: the DS expands a 5-bit channel c to 6 bits as 2c + 1 (0 stays 0) and modulates with the vertex colour as
 * ((t + 1) * (v + 1) - 1) >> 6 on the 6-bit values. A texel channel is stored as the PICA fraction (t6 + 1) / 64, rounded DOWN to 8 bits, so that a PICA modulate with a
 * vertex colour stored as (v6 + 1) / 64 gives that product. Alpha: 5-bit alpha 0 stays 0 (the texel is not drawn); otherwise (a5 + 1) / 32 as 8 bits, so 31 is 255. */
#ifndef GPU3D_TEX_H
#define GPU3D_TEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU3D_TEX_OK           0
#define GPU3D_TEX_UNSUPPORTED  1    /* format 0 (none) or 5 (compressed) */
#define GPU3D_TEX_BAD          2    /* size field out of the PICA's range (cannot happen for a 3-bit size field: 8..1024) */

#define GPU3D_TEXMEM_BYTES  0x80000u
#define GPU3D_TEXPAL_BYTES  0x18000u
#define GPU3D_TEX_MAXTEXELS (1024u * 1024u)

/* wrap modes of one axis */
#define GPU3D_WRAP_CLAMP   0
#define GPU3D_WRAP_REPEAT  1
#define GPU3D_WRAP_MIRROR  2

uint32_t gpu3d_tex_format(uint32_t texparam);                       /* bits 26-28 */
uint32_t gpu3d_tex_width(uint32_t texparam);                        /* 8 << bits 20-22 */
uint32_t gpu3d_tex_height(uint32_t texparam);                       /* 8 << bits 23-25 */
uint32_t gpu3d_tex_wrap_s(uint32_t texparam);                       /* GPU3D_WRAP_*: repeat bit 16, flip bit 18 (flip without repeat is a clamp) */
uint32_t gpu3d_tex_wrap_t(uint32_t texparam);                       /* repeat bit 17, flip bit 19 */
int      gpu3d_tex_color0_transparent(uint32_t texparam);           /* bit 29 (formats 2, 3, 4 only; the other formats ignore it) */

/* the bytes one polygon's texture can touch: [*toff, *toff + *tlen) of texture memory and [*poff, *poff + *plen) of palette memory (plen 0 for formats 1.. without a palette
 * (7) ), clipped to the memories' sizes. Returns GPU3D_TEX_OK / UNSUPPORTED. */
int gpu3d_tex_ranges(uint32_t texparam, uint32_t texpalette, uint32_t *toff, uint32_t *tlen, uint32_t *poff, uint32_t *plen);

/* reference sampler of one axis: the texel index (0..size-1) that a texel coordinate c (any integer, in texels) addresses under a wrap mode. Used to document and test the
 * flip / repeat / clamp semantics that the PICA sampler is set to reproduce. */
uint32_t gpu3d_tex_wrap_index(int32_t c, uint32_t size, uint32_t mode);

/* PICA tiled index of texel (x, y) in a texture `w` texels wide (w, h multiples of 8) */
uint32_t gpu3d_tex_tiled_index(uint32_t x, uint32_t y, uint32_t w);

/* Decodes the texture into `out` (width * height words, tiled, rows bottom-up as above). Memory outside the 512 KB / 96 KB views reads as zero. */
int gpu3d_tex_decode(uint32_t texparam, uint32_t texpalette, const uint8_t *texmem, size_t texlen, const uint8_t *palmem, size_t pallen, uint32_t *out);

/* content hash (64-bit FNV-1a) of the bytes a texture references: texparam, texpalette and the referenced texture and palette bytes. Equal hash = equal decoded texture. */
uint64_t gpu3d_tex_key(uint32_t texparam, uint32_t texpalette, const uint8_t *texmem, size_t texlen, const uint8_t *palmem, size_t pallen);

/* DS 5-bit colour channel to the stored texel channel / vertex fraction (see the header comment) */
uint32_t gpu3d_c5_to_c6(uint32_t c5);
uint32_t gpu3d_c6_mod8(uint32_t c6);       /* floor((c6 + 1) * 255 / 64): floor, so that (c6_mod8 >> 2) == c6 */
uint32_t gpu3d_a5_to_a8(uint32_t a5);

#ifdef __cplusplus
}
#endif
#endif
