/* gpu3d_tex_test.c -- bet 2.5a: DS texture formats 1/2/3 (and 4/6/7) to the PICA tiled RGBA8 layout, hand-computed texels, colour-0 transparency, wrap/flip semantics,
 * tiling order, the referenced byte ranges and the content key. Build: tools/host-tests/build.sh. */
#include "gpu3d_tex.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* texel (x, y) of a square texture w wide: DS row y is stored in memory row w-1-y */
static uint32_t px(const uint32_t *t, uint32_t x, uint32_t y, uint32_t w) { return t[gpu3d_tex_tiled_index(x, w - 1u - y, w)]; }
static uint32_t TP(uint32_t fmt, uint32_t sz, uint32_t extra) { return (fmt << 26) | (sz << 20) | (sz << 23) | extra; }   /* square, 8 << sz, offset 0 */

int main(void)
{
    static uint8_t tm[GPU3D_TEXMEM_BYTES], pm[GPU3D_TEXPAL_BYTES];
    static uint32_t out[64 * 64];
    uint32_t toff, tlen, poff, plen, x, y;

    /* conversions: 5-bit to 6-bit (2c+1, 0 stays 0), to the stored fraction (c6+1)/64 as 8 bits, alpha */
    CHECK(gpu3d_c5_to_c6(0) == 0 && gpu3d_c5_to_c6(1) == 3 && gpu3d_c5_to_c6(31) == 63, "c5 to c6");
    CHECK(gpu3d_c6_mod8(63) == 255 && gpu3d_c6_mod8(0) == 3 && gpu3d_c6_mod8(31) == 127, "c6 to 8 bit %u %u", gpu3d_c6_mod8(0), gpu3d_c6_mod8(31));
    { uint32_t c6, bad = 0; for (c6 = 0; c6 < 64; c6++) if ((gpu3d_c6_mod8(c6) >> 2) != c6) bad++; CHECK(bad == 0, "c6_mod8 >> 2 == c6 for all 64 values (%u wrong)", bad); }
    CHECK(gpu3d_a5_to_a8(0) == 0 && gpu3d_a5_to_a8(31) == 255 && gpu3d_a5_to_a8(15) == 128 && gpu3d_a5_to_a8(1) == 16, "a5 to a8 %u %u", gpu3d_a5_to_a8(15), gpu3d_a5_to_a8(1));

    /* geometry of the texparam fields */
    CHECK(gpu3d_tex_width(TP(3, 2, 0)) == 32 && gpu3d_tex_height((3u << 26) | (1u << 20) | (4u << 23)) == 128, "size fields");
    CHECK(gpu3d_tex_wrap_s(1u << 16) == GPU3D_WRAP_REPEAT && gpu3d_tex_wrap_s((1u << 16) | (1u << 18)) == GPU3D_WRAP_MIRROR && gpu3d_tex_wrap_s(1u << 18) == GPU3D_WRAP_CLAMP
          && gpu3d_tex_wrap_s(0) == GPU3D_WRAP_CLAMP, "wrap s");
    CHECK(gpu3d_tex_wrap_t(1u << 17) == GPU3D_WRAP_REPEAT && gpu3d_tex_wrap_t((1u << 17) | (1u << 19)) == GPU3D_WRAP_MIRROR && gpu3d_tex_wrap_t(1u << 19) == GPU3D_WRAP_CLAMP, "wrap t");

    /* wrap / flip reference sampler: size 8 */
    CHECK(gpu3d_tex_wrap_index(-1, 8, GPU3D_WRAP_CLAMP) == 0 && gpu3d_tex_wrap_index(9, 8, GPU3D_WRAP_CLAMP) == 7, "clamp");
    CHECK(gpu3d_tex_wrap_index(8, 8, GPU3D_WRAP_REPEAT) == 0 && gpu3d_tex_wrap_index(-1, 8, GPU3D_WRAP_REPEAT) == 7 && gpu3d_tex_wrap_index(13, 8, GPU3D_WRAP_REPEAT) == 5, "repeat");
    CHECK(gpu3d_tex_wrap_index(7, 8, GPU3D_WRAP_MIRROR) == 7 && gpu3d_tex_wrap_index(8, 8, GPU3D_WRAP_MIRROR) == 7 && gpu3d_tex_wrap_index(9, 8, GPU3D_WRAP_MIRROR) == 6
          && gpu3d_tex_wrap_index(15, 8, GPU3D_WRAP_MIRROR) == 0 && gpu3d_tex_wrap_index(16, 8, GPU3D_WRAP_MIRROR) == 0 && gpu3d_tex_wrap_index(17, 8, GPU3D_WRAP_MIRROR) == 1
          && gpu3d_tex_wrap_index(-1, 8, GPU3D_WRAP_MIRROR) == 0 && gpu3d_tex_wrap_index(-2, 8, GPU3D_WRAP_MIRROR) == 1, "mirror (flip)");

    /* tiling: 8x8 tiles, z-order, tiles row by row (a 16-wide texture: tile 1 is x 8..15) */
    CHECK(gpu3d_tex_tiled_index(0, 0, 16) == 0 && gpu3d_tex_tiled_index(1, 0, 16) == 1 && gpu3d_tex_tiled_index(0, 1, 16) == 2 && gpu3d_tex_tiled_index(1, 1, 16) == 3
          && gpu3d_tex_tiled_index(2, 0, 16) == 4 && gpu3d_tex_tiled_index(0, 2, 16) == 8 && gpu3d_tex_tiled_index(4, 0, 16) == 16 && gpu3d_tex_tiled_index(0, 4, 16) == 32
          && gpu3d_tex_tiled_index(7, 7, 16) == 63 && gpu3d_tex_tiled_index(8, 0, 16) == 64 && gpu3d_tex_tiled_index(0, 8, 16) == 128 && gpu3d_tex_tiled_index(8, 8, 16) == 192, "tiled");

    /* format 3, 16 colours, 8x8 with colour-0 transparency. palette at PLTT_BASE 2 (byte 0x20): colour i = (i, 2i, 31-i) as r,g,b 5-bit */
    memset(tm, 0, sizeof tm); memset(pm, 0, sizeof pm);
    for (x = 0; x < 16; x++) { uint32_t c = x | ((2 * x & 31u) << 5) | ((31 - x) << 10); pm[0x20 + 2 * x] = (uint8_t)c; pm[0x21 + 2 * x] = (uint8_t)(c >> 8); }
    for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) { uint32_t i = y * 8 + x; uint32_t v = (x + y) & 15u; tm[i / 2] = (uint8_t)(i & 1 ? (tm[i / 2] | (v << 4)) : (tm[i / 2] | v)); }
    CHECK(gpu3d_tex_decode(TP(3, 0, 1u << 29), 2, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK, "decode f3");
    CHECK((px(out, 0, 0, 8) & 255) == 0, "colour 0 transparent (index 0)");
    CHECK((px(out, 1, 0, 8) & 255) == 255 && (px(out, 1, 0, 8) >> 24) == gpu3d_c6_mod8(gpu3d_c5_to_c6(1)) && ((px(out, 1, 0, 8) >> 16) & 255) == gpu3d_c6_mod8(gpu3d_c5_to_c6(2))
          && ((px(out, 1, 0, 8) >> 8) & 255) == gpu3d_c6_mod8(gpu3d_c5_to_c6(30)), "texel (1,0) = palette 1: %08x", px(out, 1, 0, 8));
    CHECK((px(out, 5, 3, 8) & 255) == 255 && (px(out, 5, 3, 8) >> 24) == gpu3d_c6_mod8(gpu3d_c5_to_c6(8)), "texel (5,3) = palette 8: %08x", px(out, 5, 3, 8));
    CHECK(gpu3d_tex_decode(TP(3, 0, 0), 2, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK && (px(out, 0, 0, 8) & 255) == 255, "without the colour-0 bit index 0 is opaque");

    /* format 2, 4 colours: PLTT_BASE counts 8-byte units; 8x8; texel (x, y) index = (x + 2y) & 3, packed 2 bits per texel little end first */
    memset(tm, 0, sizeof tm);
    for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) { uint32_t i = y * 8 + x; tm[i / 4] = (uint8_t)(tm[i / 4] | (((x + 2 * y) & 3u) << (2 * (i & 3)))); }
    pm[0x18] = 0x1F; pm[0x19] = 0x00;       /* base 3 << 3 = 0x18: colour 0 = red 31 */
    pm[0x1A] = 0xE0; pm[0x1B] = 0x03;       /* colour 1 = green 31 */
    pm[0x1C] = 0x00; pm[0x1D] = 0x7C;       /* colour 2 = blue 31 */
    pm[0x1E] = 0xFF; pm[0x1F] = 0x7F;       /* colour 3 = white */
    CHECK(gpu3d_tex_decode(TP(2, 0, 1u << 29), 3, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK, "decode f2");
    CHECK((px(out, 0, 0, 8) & 255) == 0 && (px(out, 1, 0, 8) == 0x03FF03FFu) && (px(out, 2, 0, 8) == 0x0303FFFFu) && (px(out, 3, 0, 8) == 0xFFFFFFFFu) && (px(out, 0, 1, 8) == 0x0303FFFFu),
          "f2 texels %08x %08x %08x %08x", px(out, 1, 0, 8), px(out, 2, 0, 8), px(out, 3, 0, 8), px(out, 0, 1, 8));
    CHECK(gpu3d_tex_color0_transparent(TP(2, 0, 1u << 29)) == 1 && gpu3d_tex_color0_transparent(TP(2, 0, 0)) == 0, "c0 bit");

    /* format 1, A3I5: byte = alpha3 << 5 | index5; palette base 1 (byte 0x10). alpha3 7 -> 31, 0 -> 0, 4 -> 4*4+2 = 18 */
    memset(tm, 0, sizeof tm);
    pm[0x10 + 2 * 3] = 0xFF; pm[0x10 + 2 * 3 + 1] = 0x7F;    /* index 3 white */
    tm[0] = (uint8_t)((7u << 5) | 3u); tm[1] = (uint8_t)((0u << 5) | 3u); tm[2] = (uint8_t)((4u << 5) | 3u); tm[3] = (uint8_t)((1u << 5) | 3u);
    CHECK(gpu3d_tex_decode(TP(1, 0, 0), 1, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK, "decode f1");
    CHECK((px(out, 0, 0, 8) & 255) == 255 && (px(out, 1, 0, 8) & 255) == 0 && (px(out, 2, 0, 8) & 255) == gpu3d_a5_to_a8(18) && (px(out, 3, 0, 8) & 255) == gpu3d_a5_to_a8(4),
          "f1 alpha %u %u %u %u", px(out, 0, 0, 8) & 255, px(out, 1, 0, 8) & 255, px(out, 2, 0, 8) & 255, px(out, 3, 0, 8) & 255);

    /* format 6, A5I3 */
    memset(tm, 0, sizeof tm);
    tm[0] = (uint8_t)((31u << 3) | 3u); tm[1] = (uint8_t)((0u << 3) | 3u); tm[2] = (uint8_t)((16u << 3) | 3u);
    pm[0x10 + 6] = 0xFF; pm[0x10 + 7] = 0x7F;
    CHECK(gpu3d_tex_decode(TP(6, 0, 0), 1, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK && (px(out, 0, 0, 8) & 255) == 255 && (px(out, 1, 0, 8) & 255) == 0
          && (px(out, 2, 0, 8) & 255) == gpu3d_a5_to_a8(16), "f6");

    /* format 7 direct colour: bit 15 opaque */
    memset(tm, 0, sizeof tm);
    tm[0] = 0x1F; tm[1] = 0x80; tm[2] = 0x1F; tm[3] = 0x00;
    CHECK(gpu3d_tex_decode(TP(7, 0, 0), 0, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_OK && px(out, 0, 0, 8) == 0xFF0303FFu, "f7 opaque red: %08x", px(out, 0, 0, 8));
    CHECK((px(out, 1, 0, 8) & 255) == 0, "f7 transparent");

    /* unsupported: format 0 (none) and 5 (compressed) */
    CHECK(gpu3d_tex_decode(TP(0, 0, 0), 0, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_UNSUPPORTED && gpu3d_tex_decode(TP(5, 0, 0), 0, tm, sizeof tm, pm, sizeof pm, out) == GPU3D_TEX_UNSUPPORTED,
          "unsupported formats");

    /* byte ranges and the content key */
    CHECK(gpu3d_tex_ranges(TP(3, 2, 0x100), 5, &toff, &tlen, &poff, &plen) == GPU3D_TEX_OK && toff == 0x800 && tlen == 32 * 32 / 2 && poff == 0x50 && plen == 32, "ranges f3 %u %u %u %u", toff, tlen, poff, plen);
    CHECK(gpu3d_tex_ranges(TP(2, 1, 0x10), 5, &toff, &tlen, &poff, &plen) == GPU3D_TEX_OK && toff == 0x80 && tlen == 16 * 16 / 4 && poff == 0x28 && plen == 8, "ranges f2");
    CHECK(gpu3d_tex_ranges(TP(7, 0, 0xFFFF), 0, &toff, &tlen, &poff, &plen) == GPU3D_TEX_OK && toff == 0x7FFF8 && tlen == 8 && plen == 0, "range clipped to the memory");
    {
        uint64_t k1, k2, k3;
        memset(tm, 0, sizeof tm); memset(pm, 0, sizeof pm);
        k1 = gpu3d_tex_key(TP(3, 0, 0), 0, tm, sizeof tm, pm, sizeof pm);
        k2 = gpu3d_tex_key(TP(3, 0, (1u << 16) | (1u << 18)), 0, tm, sizeof tm, pm, sizeof pm);   /* sampler bits do not change the content */
        tm[3] = 1;
        k3 = gpu3d_tex_key(TP(3, 0, 0), 0, tm, sizeof tm, pm, sizeof pm);
        CHECK(k1 == k2 && k1 != k3, "key: same content same key, changed referenced byte changes it");
        tm[3] = 0; tm[64] = 9;                                                                     /* outside the 8x8 f3 texture (32 bytes) */
        CHECK(gpu3d_tex_key(TP(3, 0, 0), 0, tm, sizeof tm, pm, sizeof pm) == k1, "key ignores unreferenced bytes");
    }
    printf("gpu3d_tex_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
