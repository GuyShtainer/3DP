/* gpu2d_render_test.c -- bet 2.3 slice (b): gpu2d_render() (source/hw/gpu2d.c) over hand-built frames with HAND-COMPUTED expected pixels. No VRAM model, no oracle, no build/oracle:
 * the BG / OBJ windows and the extended palettes are plain arrays pointed to by the record. Colour arithmetic in the expectations: a BGR555 channel c5 becomes the 6-bit value
 * 2 * c5 (31 -> 62), the blend / brightness maths runs on 6 bits, and the output channel is c6 << 2 | c6 >> 4 (62 -> 251 = 0xFB, 63 -> 255, 31 -> 125 = 0x7D, 15 -> 60 = 0x3C,
 * 46 -> 186 = 0xBA, 19 -> 77 = 0x4D, 43 -> 174 = 0xAE). Cases: a 16-colour tile at a BG offset with HOFS / VOFS wrap; a 256-colour tile with an extended palette on engine B
 * (and the same tile without the ext bit); BG priority order; a 16x16 sprite (1D mapping, four quadrant colours) over a BG with priority both ways; alpha blend EVA / EVB
 * (exact rounding, the EV clamp, the layer ID); master brightness up / down; display off; the 3D layer as BG0 (alpha 0, alpha 31, BG0HOFS, the 3D alpha blend); a per-line
 * frame (BG1HOFS = line on lines 10..20 only); census-excluded features counted once per frame and rendered as off. Prints "gpu2d_render_test: N checks, F failures".
 * Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu2d.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long v)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL: %s (%ld)\n", what, v);
    }
}

#define RED_62   0xFFFB0000u
#define GREEN_62 0xFF00FB00u
#define BLUE_62  0xFF0000FBu
#define BLACK    0xFF000000u
#define WHITE_62 0xFFFBFBFBu   /* BGR555 0x7FFF: 31 per channel -> 62 -> 0xFB */
#define WHITE    0xFFFFFFFFu

/* the fixture: one record, one output, the VRAM windows, ext palettes and a 3D layer, all static */
static Gpu2dFrame g_f;
static Gpu2dOut g_o;
static uint8_t g_win[4][0x80000];
static uint8_t g_bgext[2][4][0x2000];
static uint32_t g_l3d[GPU2D_PIXELS];

static void w16(uint8_t *p, uint32_t o, uint32_t v) { p[o] = (uint8_t)v; p[o + 1u] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t o, uint32_t v) { w16(p, o, v & 0xFFFFu); w16(p, o + 2u, v >> 16); }
static uint8_t *regs(uint32_t e) { return g_f.regs[e][0]; }
static uint32_t pxa(uint32_t x, uint32_t y) { return g_o.pix[0][y * GPU2D_W + x]; }
static uint32_t pxb(uint32_t x, uint32_t y) { return g_o.pix[1][y * GPU2D_W + x]; }
static uint32_t lida(uint32_t x, uint32_t y) { return g_o.lid[0][y * GPU2D_W + x]; }

/* a fresh uniform frame: both engines in display mode 1 with every layer off, black palettes, VRAM windows mapped, no 3D layer */
static void reset(void)
{
    uint32_t e;

    memset(&g_f, 0, sizeof g_f);
    memset(g_win, 0, sizeof g_win);
    memset(g_bgext, 0, sizeof g_bgext);
    memset(&g_o, 0, sizeof g_o);
    g_f.uniform = 1;
    g_f.lines = GPU2D_LINES;
    g_f.win[0] = g_win[0];
    g_f.win[1] = g_win[1];
    g_f.win[2] = g_win[2];
    g_f.win[3] = g_win[3];
    for (e = 0; e < 2u; e++) {
        uint32_t s;

        w32(regs(e), 0x00, 0x00010000u);
        for (s = 0; s < 4u; s++) {
            g_f.bg_ext[e][s] = NULL;
        }
        g_f.obj_ext[e] = NULL;
    }
}

static void render(void)
{
    gpu2d_render(&g_f, &g_o);
}

/* 16-colour tile `t` of engine A's BG VRAM (char base 0): every pixel colour index `idx` */
static void fill_tile16(uint8_t *vram, uint32_t t, uint32_t idx)
{
    memset(vram + t * 32u, (int)(idx | (idx << 4)), 32u);
}

static void test_tile_offset(void)
{
    uint32_t x, y;

    reset();
    w32(regs(0), 0x00, 0x00010100u);           /* mode 1, BG0 on */
    w16(regs(0), 0x08, 0x0100u);               /* BG0CNT: char base 0, screen base 1 (0x800), 16 colour, priority 0 */
    w16(g_f.pltt[0], 2, 0x001Fu);              /* palette 0 colour 1 = red */
    fill_tile16(g_win[0], 1, 1);
    w16(g_win[0], 0x800, 1);                   /* map (0,0) = tile 1, palette 0 */
    render();
    check(pxa(0, 0) == RED_62 && pxa(7, 7) == RED_62 && pxa(8, 0) == BLACK && pxa(0, 8) == BLACK, "tile at 0,0", (long)pxa(0, 0));
    check(lida(0, 0) == GPU2D_LID_BG0 && lida(8, 0) == GPU2D_LID_BACKDROP, "layer IDs BG0 / backdrop", (long)lida(0, 0));
    w16(regs(0), 0x10, 4);                     /* HOFS 4, VOFS 2: screen (0..3, 0..5) shows tile columns 4..7, rows 2..7 */
    w16(regs(0), 0x12, 2);
    render();
    check(pxa(0, 0) == RED_62 && pxa(3, 5) == RED_62 && pxa(4, 0) == BLACK && pxa(0, 6) == BLACK, "HOFS 4 / VOFS 2", (long)pxa(3, 5));
    w16(regs(0), 0x10, 252);                   /* HOFS 252, VOFS 252: wraps inside the 256 x 256 map: tile at screen (4..11, 4..11) */
    w16(regs(0), 0x12, 252);
    render();
    check(pxa(4, 4) == RED_62 && pxa(11, 11) == RED_62 && pxa(3, 4) == BLACK && pxa(12, 4) == BLACK && pxa(4, 3) == BLACK && pxa(4, 12) == BLACK, "HOFS / VOFS wrap", (long)pxa(11, 11));
    for (y = 0; y < GPU2D_LINES; y++) {
        for (x = 0; x < GPU2D_W; x++) {
            if (x < 4u || x > 11u || y < 4u || y > 11u) {
                if (pxa(x, y) != BLACK) {
                    check(0, "wrap: only the tile is red", (long)(y * 256u + x));
                    return;
                }
            }
        }
    }
    check(1, "wrap: only the tile is red", 0);
}

static void test_ext_palette_engine_b(void)
{
    reset();
    w32(regs(1), 0x00, 0x40010100u);           /* engine B: mode 1, BG0 on, extended BG palettes enabled (bit 30) */
    w16(regs(1), 0x08, 0x0180u);               /* BG0CNT: 256 colour, screen base 1 */
    memset(g_win[2], 3, 64);                   /* tile 0 of the sub BG: every pixel colour index 3 */
    w16(g_win[2], 0x800, 0x2000u);             /* map (0,0): tile 0, palette number 2 */
    w16(g_bgext[1][0], 2u * 512u + 3u * 2u, 0x03E0u);   /* ext slot 0, palette 2, colour 3 = green */
    g_f.bg_ext[1][0] = g_bgext[1][0];
    w16(g_f.pltt[1], 3u * 2u, 0x7C00u);        /* the normal sub palette colour 3 = blue (what a plain 256-colour BG would show) */
    render();
    check(pxb(0, 0) == GREEN_62 && pxb(7, 7) == GREEN_62, "engine B ext palette tile", (long)pxb(0, 0));
    check(pxa(0, 0) == BLACK, "engine A unaffected by engine B", (long)pxa(0, 0));
    w32(regs(1), 0x00, 0x00010100u);           /* ext bit off: the normal palette */
    render();
    check(pxb(0, 0) == BLUE_62, "256-colour tile without the ext bit uses the normal palette", (long)pxb(0, 0));
    w32(regs(1), 0x00, 0x40010100u);
    g_f.bg_ext[1][0] = NULL;                   /* nothing mapped: colour 0 = black, opaque (index 3 is not transparent) */
    render();
    check(pxb(0, 0) == BLACK && g_o.unsupported == 0u, "unmapped ext palette reads black", (long)pxb(0, 0));
}

static void test_bg_priority(void)
{
    reset();
    w32(regs(0), 0x00, 0x00010300u);           /* BG0 + BG1 */
    w16(regs(0), 0x08, 0x0100u);               /* BG0: screen base 1 (0x800) */
    w16(regs(0), 0x0A, 0x0200u | 0x0004u);     /* BG1: screen base 2 (0x1000), char base 1 (0x4000) */
    w16(g_f.pltt[0], 2, 0x001Fu);              /* colour 1 red */
    w16(g_f.pltt[0], 4, 0x03E0u);              /* colour 2 green */
    fill_tile16(g_win[0], 1, 1);               /* char base 0, tile 1: red */
    fill_tile16(g_win[0] + 0x4000, 1, 2);      /* char base 1, tile 1: green */
    w16(g_win[0], 0x800, 1);
    w16(g_win[0], 0x1000, 1);
    render();
    check(pxa(0, 0) == RED_62, "equal priority: BG0 over BG1", (long)pxa(0, 0));
    w16(regs(0), 0x0A, 0x0204u | 0x0000u);     /* BG1 priority 0 and BG0 priority 0: still BG0; now BG0 priority 1 */
    w16(regs(0), 0x08, 0x0101u);
    render();
    check(pxa(0, 0) == GREEN_62 && lida(0, 0) == GPU2D_LID_BG0 + 1u, "BG1 priority 0 over BG0 priority 1", (long)pxa(0, 0));
    w16(regs(0), 0x0A, 0x0204u | 0x0002u);     /* BG1 priority 2, BG0 priority 1 */
    render();
    check(pxa(0, 0) == RED_62, "BG0 priority 1 over BG1 priority 2", (long)pxa(0, 0));
}

static void setup_sprite(uint32_t prio_obj, uint32_t prio_bg)
{
    uint8_t *oam = g_f.oam[0];

    reset();
    w32(regs(0), 0x00, 0x00011110u);           /* mode 1, BG0 on, OBJ on, 1D mapping, boundary 0 */
    w16(regs(0), 0x08, 0x0100u | prio_bg);
    w16(g_f.pltt[0], 2, 0x001Fu);              /* BG colour 1 red */
    fill_tile16(g_win[0], 1, 1);
    w16(g_win[0], 0x800, 1);                   /* BG tile at (0..7, 0..7) ... */
    w16(g_win[0], 0x802, 1);                   /* ... and (8..15, 0..7) ... */
    w16(g_win[0], 0x840, 1);                   /* ... and the rows below up to 16 */
    w16(g_win[0], 0x842, 1);
    w16(g_win[0], 0x804, 1);                   /* columns 16..31 of the first rows, so the sprite at x 20 sits over BG pixels */
    w16(g_win[0], 0x806, 1);
    w16(g_win[0], 0x844, 1);
    w16(g_win[0], 0x846, 1);
    fill_tile16(g_win[1], 0, 1);               /* sprite tiles 0..3: colour 1..4 of OBJ palette 1 */
    fill_tile16(g_win[1], 1, 2);
    fill_tile16(g_win[1], 2, 3);
    fill_tile16(g_win[1], 3, 4);
    w16(g_f.pltt[0], 0x200u + 32u + 2u, 0x7C00u);   /* blue */
    w16(g_f.pltt[0], 0x200u + 32u + 4u, 0x03E0u);   /* green */
    w16(g_f.pltt[0], 0x200u + 32u + 6u, 0x7FFFu);   /* white */
    w16(g_f.pltt[0], 0x200u + 32u + 8u, 0x03FFu);   /* yellow-ish: r 31 g 31 b 0 */
    w16(oam, 0, 2u);                           /* y = 2, square, normal, 16 colour */
    w16(oam, 2, 4u | 0x4000u);                 /* x = 4, size 1 = 16 x 16 */
    w16(oam, 4, (1u << 12) | (prio_obj << 10) | 0u);   /* tile 0, palette 1, priority */
    {
        uint32_t n;

        for (n = 1; n < 128u; n++) {
            w16(oam, n * 8u, 0x0200u);         /* every other sprite disabled (bit 9 with bit 8 clear) */
        }
    }
}

static void test_sprite(void)
{
    setup_sprite(0, 1);                        /* sprite priority 0 over BG priority 1: the sprite shows */
    render();
    check(pxa(4, 2) == BLUE_62 && pxa(11, 9) == BLUE_62, "sprite quadrant 0 (tile 0)", (long)pxa(4, 2));
    check(pxa(12, 2) == GREEN_62 && pxa(19, 9) == GREEN_62, "sprite quadrant 1 (tile 1, 1D row)", (long)pxa(12, 2));
    check(pxa(4, 10) == WHITE_62 && pxa(11, 17) == WHITE_62, "sprite quadrant 2 (tile 2 = second row of tiles)", (long)pxa(4, 10));
    check(pxa(12, 10) == 0xFFFBFB00u && pxa(19, 17) == 0xFFFBFB00u, "sprite quadrant 3 (tile 3)", (long)pxa(12, 10));
    check(pxa(3, 2) == RED_62 && pxa(20, 2) == RED_62 && pxa(4, 1) == RED_62 && pxa(4, 18) == BLACK, "sprite extent 16 x 16 at (4,2); BG around it", (long)pxa(20, 2));
    check(lida(4, 2) == GPU2D_LID_OBJ && lida(3, 2) == GPU2D_LID_BG0, "layer ID OBJ / BG0", (long)lida(4, 2));
    setup_sprite(0, 0);                        /* equal priority: the sprite wins */
    render();
    check(pxa(4, 2) == BLUE_62, "sprite over BG, equal priority", (long)pxa(4, 2));
    setup_sprite(1, 0);                        /* sprite priority 1 behind BG priority 0 */
    render();
    check(pxa(4, 2) == RED_62 && pxa(12, 10) == RED_62 && pxa(20, 2) == RED_62, "BG priority 0 over sprite priority 1", (long)pxa(4, 2));
    setup_sprite(1, 2);
    render();
    check(pxa(4, 2) == BLUE_62, "sprite priority 1 over BG priority 2", (long)pxa(4, 2));
    setup_sprite(0, 1);                        /* two sprites on the same pixels (sprite 1: OBJ palette 2, colour 1 = green): equal priority, the LOWER OAM index wins */
    w16(g_f.pltt[0], 0x200u + 64u + 2u, 0x03E0u);
    w16(g_f.oam[0], 8, 2u);
    w16(g_f.oam[0], 10, 4u | 0x4000u);
    w16(g_f.oam[0], 12, (2u << 12) | 0u);
    render();
    check(pxa(4, 2) == BLUE_62, "two sprites, equal priority: lower OAM index on top", (long)pxa(4, 2));
    w16(g_f.oam[0], 4, (1u << 12) | (1u << 10));   /* sprite 0 priority 1, sprite 1 priority 0: the higher index now wins */
    render();
    check(pxa(4, 2) == GREEN_62, "two sprites: priority 0 beats a lower OAM index at priority 1", (long)pxa(4, 2));
}

/* two BGs, both tile 1 at map (0,0): BG0 `c0` (priority 0), BG1 `c1` (priority 1) */
static void two_bgs(uint32_t c0, uint32_t c1)
{
    reset();
    w32(regs(0), 0x00, 0x00010300u);
    w16(regs(0), 0x08, 0x0100u);
    w16(regs(0), 0x0A, 0x0201u | 0x0004u);
    w16(g_f.pltt[0], 2, c0);
    w16(g_f.pltt[0], 4, c1);
    fill_tile16(g_win[0], 1, 1);
    fill_tile16(g_win[0] + 0x4000, 1, 2);
    w16(g_win[0], 0x800, 1);
    w16(g_win[0], 0x1000, 1);
}

static void test_alpha_blend(void)
{
    two_bgs(0x001Fu, 0x7C00u);                 /* BG0 red over BG1 blue */
    w16(regs(0), 0x50, 0x0001u | 0x0040u | 0x0200u);   /* BLDCNT: first target BG0, mode alpha, second target BG1 */
    w16(regs(0), 0x52, 8u | (8u << 8));
    render();
    check(pxa(0, 0) == 0xFF7D007Du, "alpha 8/8: (62*8 + 8) >> 4 = 31 -> 0x7D", (long)pxa(0, 0));
    check(lida(0, 0) == (GPU2D_LID_BLENDED | ((GPU2D_LID_BG0 + 1u) << GPU2D_LID_SECOND_SHIFT) | GPU2D_LID_BG0), "blended layer ID (BG0 over BG1)", (long)lida(0, 0));
    check(pxa(100, 100) == BLACK && lida(100, 100) == GPU2D_LID_BACKDROP, "no blend off the tile", (long)pxa(100, 100));
    w16(regs(0), 0x52, 5u | (11u << 8));
    render();
    check(pxa(0, 0) == 0xFF4D00AEu, "alpha 5/11: r (310+8)>>4 = 19 -> 0x4D, b (682+8)>>4 = 43 -> 0xAE", (long)pxa(0, 0));
    two_bgs(0x7FFFu, 0x7FFFu);
    w16(regs(0), 0x50, 0x0001u | 0x0040u | 0x0200u);
    w16(regs(0), 0x52, 16u | (16u << 8));
    render();
    check(pxa(0, 0) == WHITE, "alpha 16/16 of white over white clamps to 63", (long)pxa(0, 0));
    w16(regs(0), 0x52, 31u | (31u << 8));      /* EV above 16 is capped at 16 */
    render();
    check(pxa(0, 0) == WHITE, "EV 31 capped at 16", (long)pxa(0, 0));
    two_bgs(0x001Fu, 0x7C00u);
    w16(regs(0), 0x50, 0x0001u | 0x0040u);     /* no second target selected: no blend */
    w16(regs(0), 0x52, 8u | (8u << 8));
    render();
    check(pxa(0, 0) == RED_62 && g_o.unsupported == 0u, "alpha mode without a second target: top layer unchanged", (long)pxa(0, 0));
}

static void test_brightness(void)
{
    reset();
    w16(regs(0), 0x6C, 0x4000u | 4u);          /* up, factor 4 on a black backdrop: (63 * 4) >> 4 = 15 -> 0x3C */
    render();
    check(pxa(0, 0) == 0xFF3C3C3Cu && pxa(255, 191) == 0xFF3C3C3Cu, "master brightness up 4", (long)pxa(0, 0));
    w16(regs(0), 0x6C, 0x4000u | 20u);         /* factor 20 is capped at 16: white */
    render();
    check(pxa(0, 0) == WHITE, "master brightness up, factor capped at 16", (long)pxa(0, 0));
    reset();
    w16(g_f.pltt[0], 0, 0x7FFFu);              /* white backdrop */
    w16(regs(0), 0x6C, 0x8000u | 4u);          /* down 4 on 0x7FFF (62): 62 - ((62 * 4 + 15) >> 4) = 46 -> 0xBA */
    render();
    check(pxa(0, 0) == 0xFFBABABAu, "master brightness down 4", (long)pxa(0, 0));
    w16(regs(1), 0x6C, 0x8000u | 16u);         /* engine B down 16: black; engine A untouched */
    w16(g_f.pltt[1], 0, 0x7FFFu);
    render();
    check(pxb(0, 0) == BLACK && pxa(0, 0) == 0xFFBABABAu, "engine B brightness is its own", (long)pxb(0, 0));
}

static void test_display_off(void)
{
    reset();
    w16(g_f.pltt[0], 0, 0x001Fu);
    w32(regs(0), 0x00, 0x00000000u);           /* display mode 0: white */
    w16(regs(0), 0x6C, 0x8000u | 16u);         /* brightness does not apply to it */
    render();
    check(pxa(0, 0) == WHITE && pxa(255, 191) == WHITE && lida(0, 0) == 0u, "display mode 0 is white", (long)pxa(0, 0));
    check(pxb(0, 0) == BLACK, "engine B (mode 1, black backdrop) still renders", (long)pxb(0, 0));
}

static void fill_3d(uint32_t x, uint32_t y, uint32_t word)
{
    g_l3d[y * GPU2D_W + x] = word;
}

static void test_3d_layer(void)
{
    reset();
    memset(g_l3d, 0, sizeof g_l3d);
    g_f.layer3d = g_l3d;
    w32(regs(0), 0x00, 0x00010108u);           /* mode 1, BG0 on, 3D as BG0 */
    fill_3d(10, 5, 0x1F000000u | 0x3Eu);       /* alpha 31, colour r = 62 */
    fill_3d(11, 5, 0x00000000u | 0x3Eu);       /* alpha 0: transparent although the colour word is set */
    render();
    check(pxa(10, 5) == RED_62 && lida(10, 5) == GPU2D_LID_3D, "3D pixel alpha 31", (long)pxa(10, 5));
    check(pxa(11, 5) == BLACK && lida(11, 5) == GPU2D_LID_BACKDROP, "3D pixel alpha 0 is transparent", (long)pxa(11, 5));
    w16(regs(0), 0x10, 3);                     /* BG0HOFS 3: screen x shows layer x + 3 */
    render();
    check(pxa(7, 5) == RED_62 && pxa(10, 5) == BLACK, "3D layer with BG0HOFS 3", (long)pxa(7, 5));
    w16(regs(0), 0x10, 0x1FDu);                /* BG0HOFS 0x1FD = -3: screen x shows layer x - 3 */
    render();
    check(pxa(13, 5) == RED_62 && pxa(10, 5) == BLACK, "3D layer with BG0HOFS -3", (long)pxa(13, 5));
    w16(regs(0), 0x10, 0);
    two_bgs(0x001Fu, 0x7C00u);                 /* BG1 blue tile at (0..7, 0..7) under a 3D pixel of alpha 15 */
    w32(regs(0), 0x00, 0x00010308u);           /* BG0 = 3D, BG1 on */
    w16(regs(0), 0x08, 0x0100u);               /* 3D priority 0 above BG1 (priority 1) */
    w16(regs(0), 0x50, 0x0200u);               /* BLDCNT second target BG1: enables the 3D alpha blend */
    memset(g_l3d, 0, sizeof g_l3d);
    g_f.layer3d = g_l3d;
    fill_3d(2, 2, (15u << 24) | 0x3Eu);        /* alpha 15 -> weights 16 / 16 */
    render();
    check(pxa(2, 2) == 0xFF7D007Du, "3D alpha 15 over blue: (62*16 + 16) >> 5 = 31 -> 0x7D", (long)pxa(2, 2));
    check(lida(2, 2) == (GPU2D_LID_BLENDED | ((GPU2D_LID_BG0 + 1u) << GPU2D_LID_SECOND_SHIFT) | GPU2D_LID_3D), "3D blended layer ID", (long)lida(2, 2));
    w16(regs(0), 0x50, 0x0000u);               /* second target off: the 3D colour is opaque */
    render();
    check(pxa(2, 2) == RED_62, "3D alpha 15 without the second-target bit: opaque", (long)pxa(2, 2));
    g_f.layer3d = NULL;                        /* no layer: BG0 draws nothing */
    render();
    check(pxa(2, 2) == BLUE_62, "no 3D layer: BG1 shows", (long)pxa(2, 2));
}

static void test_per_line(void)
{
    uint32_t n, x0, ok = 1;

    reset();
    w32(regs(0), 0x00, 0x00010200u);           /* BG1 only */
    w16(regs(0), 0x0A, 0x0100u);
    w16(g_f.pltt[0], 2, 0x001Fu);
    {
        uint32_t t;

        for (t = 0; t < 32u * 32u; t++) {      /* every map cell = tile 1; tile 1 = pixel column 0 of colour 1, the rest transparent */
            w16(g_win[0], 0x800u + 2u * t, 1);
        }
        for (t = 0; t < 8u; t++) {
            g_win[0][32u + t * 4u] = 0x01;
        }
    }
    for (n = 0; n < GPU2D_LINES; n++) {
        memcpy(g_f.regs[0][n], g_f.regs[0][0], GPU2D_REGS_LEN);
        memcpy(g_f.regs[1][n], g_f.regs[1][0], GPU2D_REGS_LEN);
        if (n >= 10u && n <= 20u) {
            w16(g_f.regs[0][n], 0x14, n);     /* BG1HOFS = line */
        }
    }
    g_f.uniform = 0;
    render();
    check(pxa(0, 9) == RED_62 && pxa(0, 21) == RED_62 && pxa(8, 9) == RED_62, "per-line: lines outside 10..20 unshifted", (long)pxa(0, 9));
    for (n = 10; n <= 20u; n++) {
        x0 = (8u - (n & 7u)) & 7u;            /* the first screen x with (x + n) % 8 == 0 */
        if (pxa(x0, n) != RED_62 || (x0 != 0u && pxa(0, n) != BLACK) || pxa(x0 + 1u, n) != BLACK) {
            ok = 0;
        }
    }
    check(ok == 1u, "per-line: BG1HOFS = line shifts exactly lines 10..20", (long)ok);
}

static void test_unsupported(void)
{
    uint8_t *oam;

    reset();
    w32(regs(0), 0x00, 0x00012100u | 0x2000u | 0x4000u);   /* BG0 on, windows 0 and 1 on: one feature, counted once, not per line */
    w16(regs(0), 0x08, 0x0100u);
    w16(g_f.pltt[0], 2, 0x001Fu);
    fill_tile16(g_win[0], 1, 1);
    w16(g_win[0], 0x800, 1);
    render();
    check(g_o.unsupported == 1u && g_o.unsupported_mask == GPU2D_UNS_WINDOW && g_f.unsupported == 1u, "window enable counted once per frame", (long)g_o.unsupported);
    check(pxa(0, 0) == RED_62 && pxa(100, 100) == BLACK, "windows rendered as if off", (long)pxa(0, 0));
    w16(regs(0), 0x50, 0x0001u | 0x00C0u);     /* BLDCNT mode 3 (darken) with first target BG0 */
    w32(regs(1), 0x00, 0x00010007u);           /* engine B: BG mode 7 */
    w32(regs(0), 0x64, 0x80000000u);           /* DISPCAPCNT enable */
    w16(regs(0), 0x4C, 0x0001u);               /* BG mosaic size, with BG0CNT bit 6 set */
    w16(regs(0), 0x08, 0x0140u);
    render();
    check(g_o.unsupported_mask == (GPU2D_UNS_WINDOW | GPU2D_UNS_BLD_BRIGHT | GPU2D_UNS_BG_MODE | GPU2D_UNS_CAPTURE | GPU2D_UNS_BG_MOSAIC) && g_o.unsupported == 5u,
          "darken, BG mode, capture, mosaic, window: one count each", (long)g_o.unsupported_mask);
    check(pxa(0, 0) == RED_62, "darken / mosaic rendered as off", (long)pxa(0, 0));
    setup_sprite(0, 1);
    oam = g_f.oam[0];
    w16(oam, 2, 4u | 0x4000u | 0x1000u);       /* horizontal flip */
    w16(oam, 8, 100u | 0x2000u);               /* sprite 1: 256 colour at y 100 */
    w16(oam, 10, 50u);
    w16(oam, 16, 120u | 0x0100u);              /* sprite 2: rotation / scaling (drawn since slice (d); zero parameters: every pixel samples the centre texel) */
    w16(oam, 18, 60u);
    w16(oam, 24, 140u | 0x0400u);              /* sprite 3: semi-transparent mode */
    w16(oam, 26, 70u);
    render();
    check(g_o.unsupported_mask == (GPU2D_UNS_SPR_FLIP | GPU2D_UNS_SPR_256 | GPU2D_UNS_SPR_MODE) && g_o.unsupported == 3u, "sprite features counted (rotation / scaling is supported now: not counted)", (long)g_o.unsupported_mask);
    check(pxa(4, 2) == BLUE_62 && pxa(50, 100) == BLACK && pxa(70, 140) == BLACK, "flipped sprite drawn unflipped, the 256-colour and the semi-transparent one not drawn", (long)pxa(4, 2));
    g_f.unsupported = 0;
    setup_sprite(0, 1);
    render();
    check(g_o.unsupported == 0u && g_f.unsupported == 0u, "a supported scene counts nothing", (long)g_o.unsupported);
}

/* ---- slice (d): rotation / scaling sprites (the X menu's cursor icon), hand-computed ---- */
static void affine_params(uint32_t pa, uint32_t pb, uint32_t pc, uint32_t pd)
{
    uint8_t *oam = g_f.oam[0];

    w16(oam, 6, pa);       /* group 0: PA, PB, PC, PD live in the attribute-3 halfwords of OAM entries 0..3 */
    w16(oam, 14, pb);
    w16(oam, 22, pc);
    w16(oam, 30, pd);
}

static void test_sprite_affine(void)
{
    uint8_t *oam;

    setup_sprite(0, 1);
    oam = g_f.oam[0];
    w16(oam, 0, 2u | 0x0100u);                 /* y 2, rotation / scaling, group 0 (attr1 bits 9..13 = 0) */
    affine_params(0x100u, 0u, 0u, 0x100u);     /* identity: the picture of the plain sprite */
    render();
    check(g_o.unsupported == 0u, "affine identity: nothing unsupported", (long)g_o.unsupported_mask);
    check(pxa(4, 2) == BLUE_62 && pxa(12, 2) == GREEN_62 && pxa(4, 10) == WHITE_62 && pxa(19, 17) == 0xFFFBFB00u && pxa(3, 2) == RED_62 && pxa(20, 2) == RED_62 && pxa(4, 18) == BLACK,
          "affine identity equals the plain 16 x 16 sprite (four quadrants, extent)", (long)pxa(4, 2));
    check(lida(4, 2) == GPU2D_LID_OBJ, "affine sprite layer ID OBJ", (long)lida(4, 2));
    /* double size, scale 0.5 (PA = PD = 0x80): a 32 x 32 box at (4, 2), the sprite shown twice as large: texel = (d / 2) + 8 with d the offset from the box centre (20, 18) */
    w16(oam, 0, 2u | 0x0300u);
    affine_params(0x80u, 0u, 0u, 0x80u);
    render();
    check(pxa(4, 2) == BLUE_62 && pxa(19, 17) == BLUE_62 && pxa(20, 2) == GREEN_62 && pxa(35, 17) == GREEN_62 && pxa(4, 18) == WHITE_62 && pxa(20, 18) == 0xFFFBFB00u && pxa(35, 33) == 0xFFFBFB00u &&
          pxa(36, 33) == BLACK && pxa(4, 34) == BLACK, "affine double size, scale 0.5: each quadrant 16 x 16 pixels, the box ends at 32", (long)pxa(20, 18));
    /* 90 degrees: PA = 0, PB = -256, PC = 256, PD = 0: texel (16 - yo, xo); row 0 of the box samples tx = 16: transparent */
    w16(oam, 0, 2u | 0x0100u);
    affine_params(0u, 0xFF00u, 0x100u, 0u);
    render();
    check(pxa(4, 2) == RED_62 && pxa(4, 3) == GREEN_62 && pxa(19, 17) == WHITE_62 && pxa(4, 17) == BLUE_62 && pxa(19, 3) == 0xFFFBFB00u, "affine 90 degrees: corners and the empty first row (hand-computed texels)", (long)pxa(4, 3));
    /* a parameter group other than 0: group 1 sits at OAM entries 4..7 (byte 32 + 6 ...) */
    w16(oam, 2, 4u | 0x4000u | (1u << 9));
    affine_params(0u, 0u, 0u, 0u);             /* group 0 now unused (all zero) */
    w16(oam, 32 + 6, 0x100u);
    w16(oam, 32 + 14, 0u);
    w16(oam, 32 + 22, 0u);
    w16(oam, 32 + 30, 0x100u);
    render();
    check(pxa(4, 2) == BLUE_62 && pxa(19, 17) == 0xFFFBFB00u, "affine group 1 read from OAM entries 4..7", (long)pxa(4, 2));
}

/* ---- review B1: the renderer reads the record's own copy of the VRAM, never the live store ---- */
static uint8_t g_live[4][0x80000];
static uint8_t g_live_pltt[2048];
static uint8_t g_live_oam[2048];
static uint8_t g_live_io[0x2000];
static Gpu2dOut g_o2;
static const uint8_t *live_window(uint32_t id) { return g_live[id]; }
static const uint8_t *live_bg_ext(uint32_t e, uint32_t slot) { (void)e; (void)slot; return NULL; }
static const uint8_t *live_obj_ext(uint32_t e) { (void)e; return NULL; }
static uint32_t live_chunks(uint32_t id) { (void)id; return 0xFFFFFFFFu; }

static void snapshot_live(void)
{
    Gpu2dSrc src;

    src.io = g_live_io;
    src.pltt = g_live_pltt;
    src.oam = g_live_oam;
    src.window = live_window;
    src.bg_ext = live_bg_ext;
    src.obj_ext = live_obj_ext;
    src.window_chunks = live_chunks;
    gpu2d_frame_begin(&g_f, &src);
    gpu2d_frame_lines(&g_f, g_live_io, 0u, GPU2D_LINES);
}

static void test_record_isolation(void)
{
    uint32_t i, same;

    memset(&g_f, 0, sizeof g_f);
    memset(&g_o, 0, sizeof g_o);
    memset(&g_o2, 0, sizeof g_o2);
    memset(g_live, 0, sizeof g_live);
    memset(g_live_pltt, 0, sizeof g_live_pltt);
    memset(g_live_oam, 0, sizeof g_live_oam);
    memset(g_live_io, 0, sizeof g_live_io);
    w32(g_live_io, 0x00, 0x00010100u);              /* engine A: mode 1, BG0 on */
    w16(g_live_io, 0x08, 0x0100u);                  /* BG0CNT: char base 0, screen base 1, 16 colour */
    w16(g_live_pltt, 2, 0x001Fu);                   /* colour 1 red */
    fill_tile16(g_live[0], 1, 1);
    w16(g_live[0], 0x800, 1);                       /* map (0,0) = tile 1 */
    snapshot_live();
    check(g_f.vram_bytes == 0x80000u + 0x40000u + 0x20000u + 0x20000u, "isolation: every chunk bound -> the four whole windows were copied (1 MB)", (long)g_f.vram_bytes);
    gpu2d_render(&g_f, &g_o);
    check(g_o.pix[0][0] == RED_62 && g_o.pix[0][7 * GPU2D_W + 7u] == RED_62 && g_o.pix[0][8u] == BLACK, "isolation: the tile renders from the snapshot", (long)g_o.pix[0][0]);
    /* the game overwrites the live VRAM / palette after the snapshot point (a main-loop Window_CopyToVRAM, a remap memcpy) */
    memset(g_live, 0xFF, sizeof g_live);
    memset(g_live_pltt, 0xFF, sizeof g_live_pltt);
    gpu2d_render(&g_f, &g_o2);
    same = memcmp(g_o.pix, g_o2.pix, sizeof g_o.pix) == 0 && memcmp(g_o.lid, g_o2.lid, sizeof g_o.lid) == 0;
    check(same == 1u, "isolation: the render of a record is identical whether the live store was overwritten after the snapshot or not", 0);
    /* control: a NEW snapshot of the overwritten store does change the picture (the test can fail) */
    snapshot_live();
    gpu2d_render(&g_f, &g_o2);
    for (i = 0; i < GPU2D_PIXELS && g_o.pix[0][i] == g_o2.pix[0][i]; i++) {
    }
    check(i < GPU2D_PIXELS, "isolation (control): a snapshot taken AFTER the overwrite renders differently", (long)i);
}

int main(void)
{
    test_record_isolation();
    test_tile_offset();
    test_ext_palette_engine_b();
    test_bg_priority();
    test_sprite();
    test_sprite_affine();
    test_alpha_blend();
    test_brightness();
    test_display_off();
    test_3d_layer();
    test_per_line();
    test_unsupported();
    printf("gpu2d_render_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
