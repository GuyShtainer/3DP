/* gpu2d.h -- the 2D engines' per-frame input record (bet 2.3 slice (a), the private design notes). Pure C: <stdint.h> / <string.h> only, so the host tests and the
 * oracle tools compile it on the build machine. The renderer (gpu2d_render, slice (b)) reads this record and nothing else.
 *
 * One Gpu2dFrame is the whole input of one rendered frame of BOTH engines, taken at the game's snapshot point (after the vblank callback, bet 2.2c B3):
 *   regs   per engine, per line, the 0x70-byte register image (engine A 0x04000000..6F, engine B 0x04001000..6F), taken at the HBlank dispatch point (hw_pump.c,
 *          hwpump_hblank_frame): line n's image is read AFTER line n's HBlank callback, so a callback that wrote BG1HOFS = n is in line n. Bytes 4..7 of engine A
 *          (DISPSTAT, VCOUNT) are NOT render state and are stored as zero, so an image that does not change over the frame is byte-identical on every line.
 *          UNIFORM frames (no HBlank handler could run: the pump's truthful short-cut) hold ONE image per engine in line 0 and flag `uniform`; read lines through
 *          gpu2d_line_regs(), never index `regs` for a line > 0 directly.
 *   pltt, oam      copies taken at the snapshot point (2 x 1 KB palettes: main, sub; 2 x 1 KB OAM). The renderer runs later (core 2, bet 2.7), so they are copies.
 *   vramcnt        VRAMCNT_A..G, WRAMCNT, VRAMCNT_H, VRAMCNT_I (io 0x240..0x249) + 6 x 0: the mapping that is in force at the snapshot point.
 *   win, bg_ext, obj_ext   the VRAM VIEWS the renderer reads, resolved through the CURRENT VRAMCNT at the snapshot point and COPIED INTO THE RECORD (bet 2.3 slice (d),
 *          review B1): `vram` holds the four CPU windows (main BG 512 KB, main OBJ 256 KB, sub BG 128 KB, sub OBJ 128 KB, laid out back to back) and the ext-palette slots;
 *          win[] / bg_ext[][] / obj_ext[] point INTO THE RECORD, so gpu2d_render reads the record and never the live store (the game thread writes VRAM from its main
 *          loop, not only in the vblank callback: Window_CopyToVRAM, Bg_CopyTilemapBufferToVRAM, GX_LoadOBJ, the VRAMCNT remap memcpys). Only what the engines can
 *          address is copied: the 16 KB chunks of each window that an enabled bank covers (Gpu2dSrc.window_chunks, from VRAMCNT) and each bound 8 KB ext-palette slot;
 *          a chunk that is not bound reads as zero (a chunk that left the mask is zeroed). `vram_bytes` is what the latest frame_begin copied. A NULL bg_ext / obj_ext =
 *          nothing mapped there. LIMIT: a VRAMCNT change made INSIDE an HBlank callback is not followed (views and `vramcnt` are bound once per frame); nothing on the
 *          path does it (census, VRAMCNT hook calls only happen at the vblank callback / loads). A Gpu2dFrame must be zero-initialised once (calloc / static / memset).
 *   layer3d  the 3D layer input, an EMPTY SLOT in slice (a): NULL, or (slice (b)/(c)) 256 x 192 words in melonDS's ColorBuffer order r | g << 8 | b << 16 | a << 24 with
 *          6-bit colour and 5-bit alpha. It is the BG0 input of engine A when DISPCNT bit 3 is set.
 *   unsupported  GPU2D_UNSUPPORTED: the renderer (slice (b)) adds one per feature it meets that is outside the census scope (windows, mosaic, BLDCNT brightness, affine /
 *          extended / bitmap BGs, flipped / 256-colour sprites, sprites with a mode, DISPCAPCNT) and renders the rest as if it were off. Zero in slice (a). */
#ifndef GPU2D_H
#define GPU2D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU2D_ENGINES   2u
#define GPU2D_LINES     192u
#define GPU2D_REGS_LEN  0x70u
#define GPU2D_PLTT_LEN  1024u     /* per engine */
#define GPU2D_OAM_LEN   1024u     /* per engine */
#define GPU2D_WINDOWS   4u        /* main BG, main OBJ, sub BG, sub OBJ (hw_vram.h HWVRAM_WIN_*) */
#define GPU2D_BG_EXT_SLOTS 4u
#define GPU2D_B_BASE    0x1000u   /* engine B's registers in the I/O block */
#define GPU2D_VRAMCNT_OFF 0x240u
#define GPU2D_VRAMCNT_LEN 16u
#define GPU2D_W         256u
#define GPU2D_PIXELS    (GPU2D_W * GPU2D_LINES)
#define GPU2D_VCHUNK    0x4000u   /* the granularity of a window's binding: every bank range sits on a 16 KB boundary (hw_vram.c CHUNK) */
#define GPU2D_EXT_SLOT  0x2000u   /* one extended-palette slot (BG slot 0..3 per engine, OBJ one per engine) */
/* the record's own copy of the VRAM views: window w at GPU2D_VOFF_WIN(w), BG ext slot (e, s) at GPU2D_VOFF_BGEXT + (e * 4 + s) * 0x2000, OBJ ext e after them */
#define GPU2D_VOFF_WIN0    0x000000u
#define GPU2D_VOFF_WIN1    0x080000u
#define GPU2D_VOFF_WIN2    0x0C0000u
#define GPU2D_VOFF_WIN3    0x0E0000u
#define GPU2D_VOFF_BGEXT   0x100000u
#define GPU2D_VOFF_OBJEXT  0x110000u
#define GPU2D_VRAM_POOL    0x114000u   /* 1 130 496 bytes: 1 MB of windows + 80 KB of ext-palette slots */

/* Layer IDs of Gpu2dOut.lid (bet 2.3 slice (b)): one byte per output pixel.
 *   bits 0-2  the layer that gave the pixel its colour (the TOP layer): 0 backdrop, 1..4 BG0..BG3, 5 OBJ, 6 the 3D layer (engine A BG0 with DISPCNT bit 3)
 *   bit  7    the pixel is an alpha blend of the top layer and the one below (BLDCNT mode 1, or the 3D layer's own alpha)
 *   bits 4-6  with bit 7 set: the layer ID of the SECOND target (same numbering); 0 otherwise
 * Display off / forced blank / a frame whose engine is off write ID 0. Slice (c)'s composite and the 3D slider read this. */
#define GPU2D_LID_BACKDROP 0u
#define GPU2D_LID_BG0      1u   /* BGn = GPU2D_LID_BG0 + n */
#define GPU2D_LID_OBJ      5u
#define GPU2D_LID_3D       6u
#define GPU2D_LID_TOP_MASK 0x07u
#define GPU2D_LID_SECOND_SHIFT 4u
#define GPU2D_LID_BLENDED  0x80u

/* Feature bits of Gpu2dOut.unsupported_mask / the GPU2D_UNSUPPORTED counter: each is counted at most ONCE per feature per gpu2d_render call (frame). */
#define GPU2D_UNS_WINDOW     0x0001u   /* DISPCNT bits 13..15 (window 0/1/OBJ window) set on a rendered line */
#define GPU2D_UNS_BG_MOSAIC  0x0002u   /* an enabled BG with BGCNT bit 6 while MOSAIC's BG size is not 0 (a bit alone, with size 0, changes no pixel) */
#define GPU2D_UNS_OBJ_MOSAIC 0x0004u   /* a visible sprite with attr0 bit 12 while MOSAIC's OBJ size is not 0 */
#define GPU2D_UNS_BLD_BRIGHT 0x0008u   /* BLDCNT mode 2 / 3 (brighten / darken) with any first target selected */
#define GPU2D_UNS_BG_MODE    0x0010u   /* DISPCNT BG mode 1..7 (affine / extended / bitmap / large BGs): rendered as mode 0 text BGs */
#define GPU2D_UNS_DISP_MODE  0x0020u   /* DISPCNT display mode 2 (VRAM) / 3 (main memory FIFO): rendered as mode 1 (graphics) */
#define GPU2D_UNS_CAPTURE    0x0040u   /* DISPCAPCNT enable (engine A, io 0x64 bit 31): not performed */
#define GPU2D_UNS_SPR_AFFINE 0x0080u   /* RETIRED (bet 2.3 slice (d)): rotation / scaling 16-colour sprites are rendered (the X menu's cursor icon is one); the bit is never raised now */
#define GPU2D_UNS_SPR_FLIP   0x0100u   /* a visible sprite with H / V flip (attr1 bits 12, 13): drawn unflipped */
#define GPU2D_UNS_SPR_256    0x0200u   /* a visible 256-colour sprite (attr0 bit 13): not drawn */
#define GPU2D_UNS_SPR_MODE   0x0400u   /* a visible semi-transparent / OBJ-window / bitmap sprite (attr0 bits 10-11 != 0): not drawn */

/* Where the record is bound from (the I/O block, the palette / OAM arrays, and the VRAM model's view getters; hw_vram.h's HWVRAM_* have exactly these signatures). */
typedef struct Gpu2dSrc {
    const uint8_t *io;       /* offset 0 = 0x04000000 */
    const uint8_t *pltt;     /* 2 KB: main then sub */
    const uint8_t *oam;      /* 2 KB: main then sub */
    const uint8_t *(*window)(uint32_t id);                       /* 0 main BG, 1 main OBJ, 2 sub BG, 3 sub OBJ */
    const uint8_t *(*bg_ext)(uint32_t engine, uint32_t slot);    /* extended BG palette slot 0..3 */
    const uint8_t *(*obj_ext)(uint32_t engine);                  /* extended OBJ palette */
    uint32_t (*window_chunks)(uint32_t id);                      /* bit c set = the 16 KB chunk c of window `id` is covered by an enabled bank (hw_vram.h HWVRAM_WindowChunks) */
} Gpu2dSrc;

typedef struct Gpu2dFrame {
    uint32_t seq;            /* frames begun */
    uint32_t uniform;        /* 1: regs[e][0] stands for every line */
    uint32_t lines;          /* lines recorded this frame (192 when complete; a uniform frame counts 192) */
    uint32_t unsupported;    /* GPU2D_UNSUPPORTED (see above) */
    uint8_t vramcnt[GPU2D_VRAMCNT_LEN];
    const uint8_t *win[GPU2D_WINDOWS];   /* point into `vram` after gpu2d_frame_begin */
    const uint8_t *bg_ext[GPU2D_ENGINES][GPU2D_BG_EXT_SLOTS];
    const uint8_t *obj_ext[GPU2D_ENGINES];
    const uint32_t *layer3d;
    uint32_t vchunks[GPU2D_WINDOWS];   /* the chunk mask copied into `vram` for each window by the latest frame_begin */
    uint32_t vram_bytes;               /* bytes copied from the live VRAM by the latest frame_begin (windows + ext-palette slots) */
    uint8_t pltt[GPU2D_ENGINES][GPU2D_PLTT_LEN];
    uint8_t oam[GPU2D_ENGINES][GPU2D_OAM_LEN];
    uint8_t regs[GPU2D_ENGINES][GPU2D_LINES][GPU2D_REGS_LEN];
    uint8_t vram[GPU2D_VRAM_POOL];     /* the record's copy of the VRAM views (above) */
} Gpu2dFrame;

/* The rendered pair of screens' worth of pixels: engine A and engine B, NOT screens (POWCNT1 bit 15, the display swap, is the caller's: the harness keeps it in the
   MISC chunk word 0 (PowerControl9), the port in its io image at 0x04000304). pix: 0xFFRRGGBB, each channel the DS 6-bit value expanded as (c << 2) | (c >> 4) (what the
   harness's write_png takes, so the comparison is byte-exact), row-major 256 x 192. lid: GPU2D_LID_* above. */
typedef struct Gpu2dOut {
    uint32_t pix[GPU2D_ENGINES][GPU2D_PIXELS];
    uint8_t  lid[GPU2D_ENGINES][GPU2D_PIXELS];
    uint32_t unsupported;        /* features counted by the latest gpu2d_render (popcount of the mask) */
    uint32_t unsupported_mask;   /* GPU2D_UNS_* bits met by the latest gpu2d_render */
} Gpu2dOut;

/* Start a frame: copy palette / OAM, VRAMCNT and the bound VRAM (see above) from `src` as they are NOW; nothing of regs is touched (a frame is complete when lines == 192). */
void gpu2d_frame_begin(Gpu2dFrame *f, const Gpu2dSrc *src);
/* Record the register image of lines first .. first + count - 1 from `io` (both engines). count == GPU2D_LINES (first 0) is the uniform short-cut: one image, flag set;
   any other count writes each line and clears the flag. */
void gpu2d_frame_lines(Gpu2dFrame *f, const uint8_t *io, uint32_t first, uint32_t count);
/* The image of `engine` (0 A, 1 B) at `line` (0..191), honouring `uniform`. */
const uint8_t *gpu2d_line_regs(const Gpu2dFrame *f, uint32_t engine, uint32_t line);
/* Render both engines of `f` (needs a complete frame: lines == 192). Each line comes from gpu2d_line_regs(), so a per-line HBlank change shows; palettes / OAM from the
   record's copies, VRAM from f->win / f->bg_ext (NULL = reads zero), engine A's BG0 from f->layer3d when DISPCNT bit 3 is set (alpha 0 = transparent, BG0HOFS scrolls
   it). The record is NOT const: the features of the census-excluded list that were met are added to f->unsupported (and reported in out). Never asserts on game data. */
void gpu2d_render(Gpu2dFrame *f, Gpu2dOut *out);

/* Bet 2.8 (widescreen, the private design notes). gpu2d_render_a: engine A only, into out->pix[0] / lid[0] (the second eye of a stereo frame: its engine B and
   its feature counts are the first eye's gpu2d_render). gpu2d_compose_wide: the 400 x 240 top-screen image (0xFFRRGGBB) of engine A: the DS frame is out->pix[0] 1:1 at
   (GPU2D_WIDE_X0, GPU2D_WIDE_Y0) (2D layers and all), every border pixel is the wide 3D layer `layer_w` (400 x 240 layer words, NULL = none) over the backdrop through the
   same colour special effect and master brightness as the centre, with the registers of the nearest DS line (lines 0 / 191 above / below the frame). BG0HOFS scrolls the
   wide layer as it scrolls the centre. */
#define GPU2D_WIDE_W  400u
#define GPU2D_WIDE_H  240u
#define GPU2D_WIDE_X0 72u
#define GPU2D_WIDE_Y0 24u
void gpu2d_render_a(const Gpu2dFrame *f, Gpu2dOut *out);
void gpu2d_compose_wide(const Gpu2dFrame *f, const Gpu2dOut *out, const uint32_t *layer_w, uint32_t *dst);

#ifdef __cplusplus
}
#endif

#endif
