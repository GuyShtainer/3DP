/* hw_screen.h -- bet 2.3 slice (c): the pure half of the screens. Pure C (<stdint.h> / <string.h> only), host-tested (tools/host-tests/hw_screen_test.c):
 *   1. hwscr_blit_bgr8   a 256 x 192 engine image onto a 3DS framebuffer (BGR8, column-major, rotated), centred, black borders;
 *   2. hwscr_a_on_top    POWCNT1 bit 15 from the I/O image: which engine is on the top screen;
 *   3. the fbdump file   one composited frame (both engines' pixels + layer IDs + the POWCNT1 mapping), pack / parse;
 *   4. HwScrPipe         the hand-off between the game thread (records a frame at every snapshot point) and the render thread (composites + presents it): one frame in
 *                        flight, an overrun is COUNTED and the new frame DROPPED, never queued;
 *   5. HwScrCon          who owns the bottom screen: the game, or the text console (ZL+ZR toggles it).
 * the private design notes. The libctru half is hw_ctr.c (framebuffers, swap, the console); the thread and the counters are hw_devs.c. */
#ifndef HW_SCREEN_H
#define HW_SCREEN_H

#include <stdint.h>

#include "gpu2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HWSCR_TOP_W     400u
#define HWSCR_BOT_W     320u
#define HWSCR_H         240u
#define HWSCR_POWCNT1   0x304u   /* io offset of POWCNT1 (0x04000304) */

/* Write the 256 x 192 image `pix` (0xFFRRGGBB words, row-major, what gpu2d_render gives) into `fb`, a libctru framebuffer of a screen `screen_w` pixels wide and
   HWSCR_H tall: BGR8, three bytes per pixel, COLUMN-major with the rows reversed (the panels are mounted rotated): screen pixel (x, y) is the three bytes at
   (x * 240 + (239 - y)) * 3, in the order B, G, R. The image is centred (x0 = (screen_w - 256) / 2, y0 = 24); every byte outside it is written 0 (black). fb must
   hold screen_w * 240 * 3 bytes. screen_w is HWSCR_TOP_W or HWSCR_BOT_W. */
void hwscr_blit_bgr8(const uint32_t *pix, uint8_t *fb, uint32_t screen_w);
/* Same, with a black screen (no image): the borders-only case, used when an engine is off. */
void hwscr_black_bgr8(uint8_t *fb, uint32_t screen_w);

/* Bet 2.8: a 400 x 240 image (the wide top screen, gpu2d_compose_wide) onto the top framebuffer, the same byte layout, edge to edge. */
void hwscr_blit_bgr8_wide(const uint32_t *pix, uint8_t *fb);

/* ---- bet 2.8: the wide fbdump file (sdmc:/3ds/3DP/fb/<vb>w.bin, written next to the FB2D file of a frame that was presented wide), little-endian:
 *   0  u32 magic 'F','B','W','1' (bytes 46 42 57 31)   4 u32 version = 1   8 u32 vb   12 u32 flags: bit 0 stereo (the right image is there; else it is zero)
 *   16 u32 width = 400   20 u32 height = 240   24 u32 the 3D slider in percent used for the frame   28 u32 the stereo shear factor sigma x 1e6 (0 mono)
 *   32 u32 engine A MASTER_BRIGHT of line 0   36 the same of line 191   40 u32 BLDCNT | BLDALPHA << 16 of line 0   44 u32 DISPCNT of line 0   48 u32 the backdrop (BGR555)
 *   52 u32 the layer word of a clear (rear-plane) pixel   56..63 reserved, zero
 *   64 the left (or mono) image, 400 * 240 * 3 bytes R, G, B   +  the right image, same   +  the left (mono) eye's wide 3D layer, 400 * 240 u32 layer words (gpu3d_pica.h)
 *   +  the right eye's, same (zero when mono).
 * Size 64 + 2 * 288000 + 2 * 384000 = 1344064 bytes. The registers and the layers let the gates infer the border's and the centre's brightness step independently (gate d)
 * and find clear-colour columns per eye (gate e). */
#define HWSCR_WDUMP_MAGIC   0x31574246u
#define HWSCR_WDUMP_VERSION 1u
#define HWSCR_WDUMP_HDR     64u
#define HWSCR_WDUMP_PIXELS  (400u * 240u)
#define HWSCR_WDUMP_SIZE    (HWSCR_WDUMP_HDR + 2u * HWSCR_WDUMP_PIXELS * 3u + 2u * HWSCR_WDUMP_PIXELS * 4u)
/* pack (right NULL = mono: the right image is written zero and flags bit 0 clear; a NULL layer = zero words; f = the frame's 2D record, for the registers) */
void hwscr_wdump_pack(uint8_t *buf, uint32_t vb, const uint32_t *left, const uint32_t *right, const uint32_t *layer_l, const uint32_t *layer_r, uint32_t clear_word,
                      const Gpu2dFrame *f, uint32_t slider_pct, float sigma);

/* POWCNT1 bit 15 (the DS display swap): 1 = engine A (main) on the top screen. io is the register image (offset 0 = 0x04000000). */
uint32_t hwscr_a_on_top(const uint8_t *io);

/* ---- the fbdump file (config.ini `fbdump = FROM:COUNT`, sdmc:/3ds/3DP/fb/<vb>.bin), all little-endian:
 *   0   u32 magic 'F','B','2','D' (bytes 46 42 32 44)    4   u32 version = 1        8   u32 vb (the DS vblank count of the snapshot point)
 *   12  u32 flags: bit 0 = engine A on the top screen (POWCNT1 bit 15)                 16  u32 width = 256        20  u32 height = 192
 *   24  u32 unsupported (GPU2D_UNSUPPORTED of this frame)                               28  u32 reserved = 0
 *   32  pix of engine A, 256 * 192 * 3 bytes R, G, B (the byte values of Gpu2dOut.pix; the alpha byte is always 0xFF and is not stored)
 *   +   pix of engine B, same;  +  lid of engine A, 256 * 192 bytes;  +  lid of engine B, same.
 * Size: 32 + 2 * 147456 + 2 * 49152 = 393248 bytes. */
#define HWSCR_DUMP_MAGIC   0x44324246u   /* "FB2D" read as a little-endian word */
#define HWSCR_DUMP_VERSION 1u
#define HWSCR_DUMP_HDR     32u
#define HWSCR_DUMP_SIZE    (HWSCR_DUMP_HDR + 2u * GPU2D_PIXELS * 3u + 2u * GPU2D_PIXELS)

typedef struct HwScrDump {
    uint32_t vb;
    uint32_t a_on_top;
    uint32_t unsupported;
} HwScrDump;

/* Pack `out` into buf (HWSCR_DUMP_SIZE bytes). */
void hwscr_dump_pack(uint8_t *buf, uint32_t vb, uint32_t a_on_top, const Gpu2dOut *out);
/* Parse a dump: 1 and *h filled (and `out` filled when not NULL, pix as 0xFF000000 | R << 16 | G << 8 | B) when the size, magic, version and geometry are right; else 0. */
int hwscr_dump_parse(const uint8_t *buf, uint32_t len, HwScrDump *h, Gpu2dOut *out);

/* ---- the frame hand-off. Two Gpu2dFrame slots; the game thread records into slot `cur` and offers it at the snapshot point; the render thread works on it until
   done. ONE frame is in flight: while it is, the next offer is an OVERRUN (counted, the offered frame dropped, `cur` stays). The record is SELF-CONTAINED (bet 2.3
   slice (d), review B1): the palettes, OAM, registers AND the VRAM the engines address were copied into the slot at the snapshot point, so the render thread reads
   nothing the game thread writes; the only shared object is the slot itself, and `busy` keeps the game thread out of it while it renders. There is deliberately NO
   detector for "the game wrote VRAM while the renderer read it": it cannot happen (the renderer never reads live VRAM), and the earlier `races` counter measured
   only "a render still running at the next DS vblank IRQ" while the game's real VRAM writers (Window_CopyToVRAM, Bg_CopyTilemapBufferToVRAM, GX_LoadOBJ, the
   VRAMCNT remap memcpys: review-2.3 B1) run from its main loop. All fields but the plain counters are touched with the __atomic builtins; the counters have one
   writer each (the game thread writes points / accepted / overruns, the render thread writes composited / presented). */
typedef struct HwScrPipe {
    uint32_t busy;         /* 1 from a successful offer until the render thread has presented */
    uint32_t cur;          /* the slot the game thread records into (0 or 1) */
    uint32_t rslot;        /* the slot the render thread works on while busy */
    uint32_t points;       /* snapshot points offered (game frames: one per snapshot point, NOT one per DS vblank) */
    uint32_t accepted;     /* offers taken */
    uint32_t overruns;     /* offers refused (frame dropped) */
    uint32_t composited;   /* gpu2d_render finished (render thread) */
    uint32_t presented;    /* frames given to the framebuffers (render thread) */
} HwScrPipe;

void hwscr_pipe_init(HwScrPipe *p);
/* The game thread, at the snapshot point after recording slot p->cur: 1 = taken (p->rslot is the slot to render, p->cur flipped), 0 = overrun (counted, dropped). */
uint32_t hwscr_pipe_offer(HwScrPipe *p);
/* The render thread: gpu2d_render has returned / the frame is presented (the job is over). */
void hwscr_pipe_rendered(HwScrPipe *p);
void hwscr_pipe_done(HwScrPipe *p);
uint32_t hwscr_pipe_busy(const HwScrPipe *p);

/* ---- who owns the bottom screen. The console is shown at power-on (the boot has no game frame yet) and hidden by the first presented game frame; ZL+ZR (a chord,
   edge-triggered) toggles it; a fatal error forces it on for good. */
typedef struct HwScrCon {
    uint32_t visible;
    uint32_t forced;
    uint32_t chord;        /* the chord was held at the previous frame */
    uint32_t user;         /* the user has toggled: the first presented frame must not hide it again */
} HwScrCon;

void hwscr_con_init(HwScrCon *c);
/* One input frame: `held` = ZL and ZR both held now. Returns 1 when the visibility changed. */
uint32_t hwscr_con_input(HwScrCon *c, uint32_t held);
/* The first (and every) presented game frame: hides the boot console unless the user already chose. Returns 1 when the visibility changed. */
uint32_t hwscr_con_presented(HwScrCon *c);
void hwscr_con_force(HwScrCon *c);
/* bet 2.6c: a notice for the player (a failed save write-back): show the console and count it as the user's choice, so the next presented frame does not hide it; ZL+ZR still toggles. */
void hwscr_con_show(HwScrCon *c);

#ifdef __cplusplus
}
#endif

#endif
