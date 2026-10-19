/* hw_screen_test.c -- bet 2.3 slice (c): the pure half of the screens (source/hw/hw_screen.c).
 *   1. the present / centre mapping: a synthetic 256 x 192 pattern through hwscr_blit_bgr8 into a host buffer of each screen size; the four corners and a mid pixel land at the
 *      byte offsets computed by hand from the libctru layout ((x * 240 + (239 - y)) * 3, B G R), the rotation is right (a column is contiguous and runs bottom-up), every byte
 *      outside the image is black, the byte count is exact;
 *   2. POWCNT1 bit 15 -> the engine on the top screen;
 *   3. the fbdump file: header bytes by hand, size, a pack -> parse round trip on both engines' pixels and layer IDs, bad magic / size / version refused;
 *   4. the hand-off pipe: accepted, overrun counted and the frame dropped (not queued), done frees it, the vblank-overlap race counter;
 *   5. the console owner: shown at power-on, hidden by the first presented frame, the ZL+ZR chord toggles on its press edge only, a user choice survives later frames, a
 *      forced console cannot be toggled off;
 *   6. config.ini `fbdump = FROM:COUNT` (bounds).
 * Prints "hw_screen_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_cfg.h"
#include "hw_screen.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long v)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL: %s (%ld)\n", what, v);
    }
}

static uint32_t pat(uint32_t x, uint32_t y)   /* a distinct non-black colour per pixel */
{
    return 0xFF000000u | ((x & 255u) << 16) | ((y & 255u) << 8) | (((x + y) & 127u) + 1u);
}

static void fill(uint32_t *pix)
{
    uint32_t x, y;

    for (y = 0; y < 192u; y++) {
        for (x = 0; x < 256u; x++) {
            pix[y * 256u + x] = pat(x, y);
        }
    }
}

/* the bytes of image pixel (sx, sy) on a screen of width sw, computed from the layout rule, not from the implementation */
static size_t off_of(uint32_t sw, uint32_t sx, uint32_t sy)
{
    uint32_t x = (sw - 256u) / 2u + sx;
    uint32_t y = 24u + sy;

    return ((size_t)x * 240u + (239u - y)) * 3u;
}

static void test_blit_one(uint32_t sw)
{
    static uint32_t pix[256 * 192];
    uint8_t *fb = (uint8_t *)malloc((size_t)sw * 240u * 3u);
    size_t i, nonzero = 0, expect_nonzero = 0;
    uint32_t sx, sy;
    const uint32_t px[5][2] = { { 0, 0 }, { 255, 0 }, { 0, 191 }, { 255, 191 }, { 100, 50 } };
    int k, all_ok = 1;

    assert(fb != NULL);
    fill(pix);
    memset(fb, 0xA5, (size_t)sw * 240u * 3u);   /* stale content: the borders must be rewritten to black */
    hwscr_blit_bgr8(pix, fb, sw);
    for (k = 0; k < 5; k++) {
        size_t o = off_of(sw, px[k][0], px[k][1]);
        uint32_t c = pat(px[k][0], px[k][1]);

        if (fb[o] != (uint8_t)c || fb[o + 1] != (uint8_t)(c >> 8) || fb[o + 2] != (uint8_t)(c >> 16)) {
            all_ok = 0;
            printf("  pixel (%u,%u) at byte %zu: got %02x %02x %02x want B %02x G %02x R %02x\n", px[k][0], px[k][1], o, fb[o], fb[o + 1], fb[o + 2], (uint8_t)c,
                   (uint8_t)(c >> 8), (uint8_t)(c >> 16));
        }
    }
    check(all_ok, sw == 400u ? "top: corners and a mid pixel at the hand-computed offsets (B G R)" : "bottom: corners and a mid pixel at the hand-computed offsets (B G R)", sw);
    /* rotation: down one image row = 3 bytes LOWER in the same column; right one image column = 240 * 3 bytes HIGHER */
    check(off_of(sw, 5, 11) == off_of(sw, 5, 10) - 3u && off_of(sw, 6, 10) == off_of(sw, 5, 10) + 720u, "rotation: a row step is -3 bytes, a column step is +720 bytes", 0);
    check(fb[off_of(sw, 5, 11)] == (uint8_t)pat(5, 11) && fb[off_of(sw, 6, 10)] == (uint8_t)pat(6, 10), "rotation: the neighbours land one step away", 0);
    for (i = 0; i < (size_t)sw * 240u * 3u; i++) {
        nonzero += fb[i] != 0u;
    }
    for (sy = 0; sy < 192u; sy++) {
        for (sx = 0; sx < 256u; sx++) {
            uint32_t c = pat(sx, sy);

            expect_nonzero += (uint8_t)c != 0u;
            expect_nonzero += (uint8_t)(c >> 8) != 0u;
            expect_nonzero += (uint8_t)(c >> 16) != 0u;
        }
    }
    check(nonzero == expect_nonzero, "borders black: every nonzero byte of the framebuffer belongs to the image", (long)nonzero);
    /* the four border strips, byte by byte */
    {
        uint32_t x, y;
        int black = 1;

        for (x = 0; x < sw; x++) {
            for (y = 0; y < 240u; y++) {
                int inside = x >= (sw - 256u) / 2u && x < (sw - 256u) / 2u + 256u && y >= 24u && y < 24u + 192u;
                size_t o = ((size_t)x * 240u + (239u - y)) * 3u;

                if (!inside && (fb[o] | fb[o + 1] | fb[o + 2]) != 0u) {
                    black = 0;
                }
            }
        }
        check(black, "borders black: left, right, 24 rows above and 24 below", sw);
    }
    hwscr_black_bgr8(fb, sw);
    for (i = 0; i < (size_t)sw * 240u * 3u && fb[i] == 0u; i++) {
    }
    check(i == (size_t)sw * 240u * 3u, "hwscr_black_bgr8 clears the whole screen", sw);
    free(fb);
}

static void test_powcnt(void)
{
    uint8_t io[0x400];

    memset(io, 0, sizeof io);
    check(hwscr_a_on_top(io) == 0u, "POWCNT1 = 0: engine A on the bottom", 0);
    io[0x304] = 0x01;
    io[0x305] = 0x00;
    check(hwscr_a_on_top(io) == 0u, "POWCNT1 bit 0 alone: still bottom", 0);
    io[0x305] = 0x80;
    check(hwscr_a_on_top(io) == 1u, "POWCNT1 bit 15: engine A on top", 0);
    io[0x305] = 0x7F;
    check(hwscr_a_on_top(io) == 0u, "POWCNT1 bits 8..14 are not the swap", 0);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_dump(void)
{
    static Gpu2dOut a, b;
    uint8_t *buf = (uint8_t *)malloc(HWSCR_DUMP_SIZE);
    HwScrDump h;
    uint32_t e, i;
    int same = 1;

    assert(buf != NULL);
    check(HWSCR_DUMP_SIZE == 393248u, "dump size is 32 + 2 * 147456 + 2 * 49152", (long)HWSCR_DUMP_SIZE);
    for (e = 0; e < 2u; e++) {
        for (i = 0; i < GPU2D_PIXELS; i++) {
            a.pix[e][i] = 0xFF000000u | ((i * 7u + e * 3u) & 0xFFFFFFu);
            a.lid[e][i] = (uint8_t)((i + e * 5u) & 0xFFu);
        }
    }
    a.unsupported = 3u;
    hwscr_dump_pack(buf, 1234u, 1u, &a);
    check(buf[0] == 'F' && buf[1] == 'B' && buf[2] == '2' && buf[3] == 'D', "header magic bytes F B 2 D", 0);
    check(rd32(buf + 4) == 1u && rd32(buf + 8) == 1234u && rd32(buf + 12) == 1u && rd32(buf + 16) == 256u && rd32(buf + 20) == 192u && rd32(buf + 24) == 3u && rd32(buf + 28) == 0u,
          "header fields: version, vb, flags, 256, 192, unsupported, reserved", 0);
    check(buf[32] == (uint8_t)(a.pix[0][0] >> 16) && buf[33] == (uint8_t)(a.pix[0][0] >> 8) && buf[34] == (uint8_t)a.pix[0][0], "first pixel is stored R G B", 0);
    check(buf[32 + 147456] == (uint8_t)(a.pix[1][0] >> 16), "engine B's pixels follow engine A's", 0);
    check(buf[32 + 2 * 147456] == a.lid[0][0] && buf[32 + 2 * 147456 + 49152 + 5] == a.lid[1][5], "layer IDs follow the pixels", 0);
    memset(&b, 0, sizeof b);
    check(hwscr_dump_parse(buf, HWSCR_DUMP_SIZE, &h, &b) == 1 && h.vb == 1234u && h.a_on_top == 1u && h.unsupported == 3u, "parse: header", 0);
    for (e = 0; e < 2u; e++) {
        for (i = 0; i < GPU2D_PIXELS; i++) {
            same = same && b.pix[e][i] == a.pix[e][i] && b.lid[e][i] == a.lid[e][i];
        }
    }
    check(same, "pack -> parse round trip: both engines' pixels and layer IDs identical", 0);
    check(hwscr_dump_parse(buf, HWSCR_DUMP_SIZE - 1u, &h, NULL) == 0, "short file refused", 0);
    buf[0] = 'X';
    check(hwscr_dump_parse(buf, HWSCR_DUMP_SIZE, &h, NULL) == 0, "bad magic refused", 0);
    buf[0] = 'F';
    buf[4] = 2;
    check(hwscr_dump_parse(buf, HWSCR_DUMP_SIZE, &h, NULL) == 0, "bad version refused", 0);
    buf[4] = 1;
    buf[16] = 255;
    check(hwscr_dump_parse(buf, HWSCR_DUMP_SIZE, &h, NULL) == 0, "bad width refused", 0);
    free(buf);
}

static void test_pipe(void)
{
    HwScrPipe p;

    hwscr_pipe_init(&p);
    check(p.cur == 0u && hwscr_pipe_busy(&p) == 0u, "pipe starts idle on slot 0", 0);
    check(hwscr_pipe_offer(&p) == 1u && p.rslot == 0u && p.cur == 1u && hwscr_pipe_busy(&p) == 1u, "offer 1: taken, render slot 0, recording slot flips to 1", 0);
    check(hwscr_pipe_offer(&p) == 0u && p.overruns == 1u && p.cur == 1u && p.rslot == 0u && p.accepted == 1u, "offer 2 while busy: OVERRUN counted, frame dropped, nothing queued (cur unchanged)", 0);
    check(hwscr_pipe_offer(&p) == 0u && p.overruns == 2u && p.points == 3u, "offer 3: overrun again; points counts every offer", 0);
    hwscr_pipe_rendered(&p);
    check(p.composited == 1u && hwscr_pipe_busy(&p) == 1u, "after gpu2d_render: composited counted, still busy until presented", 0);
    check(hwscr_pipe_offer(&p) == 0u && p.overruns == 3u, "still an overrun until the frame is presented", 0);
    hwscr_pipe_done(&p);
    check(hwscr_pipe_busy(&p) == 0u && p.presented == 1u, "done: idle, presented 1", 0);
    check(hwscr_pipe_offer(&p) == 1u && p.rslot == 1u && p.cur == 0u, "offer 4: taken, render slot 1 (the one recorded), recording back to slot 0", 0);
    hwscr_pipe_rendered(&p);
    hwscr_pipe_done(&p);
    check(p.points == 5u && p.accepted == 2u && p.overruns == 3u && p.composited == 2u && p.presented == 2u && p.points == p.accepted + p.overruns,
          "totals: points == accepted + overruns, composited == presented == accepted", 0);
}

static void test_console(void)
{
    HwScrCon c;

    hwscr_con_init(&c);
    check(c.visible == 1u, "console shown at power-on", 0);
    check(hwscr_con_input(&c, 0u) == 0u && c.visible == 1u, "no chord: unchanged", 0);
    check(hwscr_con_presented(&c) == 1u && c.visible == 0u, "the first presented frame hides the boot console", 0);
    check(hwscr_con_presented(&c) == 0u && c.visible == 0u, "later frames change nothing", 0);
    check(hwscr_con_input(&c, 1u) == 1u && c.visible == 1u, "ZL+ZR pressed: console on", 0);
    check(hwscr_con_input(&c, 1u) == 0u && c.visible == 1u, "held: no repeat", 0);
    check(hwscr_con_presented(&c) == 0u && c.visible == 1u, "a presented frame does not hide a console the user asked for", 0);
    check(hwscr_con_input(&c, 0u) == 0u && c.visible == 1u, "released: unchanged", 0);
    check(hwscr_con_input(&c, 1u) == 1u && c.visible == 0u, "pressed again: console off", 0);
    hwscr_con_force(&c);
    check(c.visible == 1u, "fatal error: console forced on", 0);
    check(hwscr_con_input(&c, 0u) == 0u && hwscr_con_input(&c, 1u) == 0u && c.visible == 1u && hwscr_con_presented(&c) == 0u, "a forced console cannot be toggled or hidden", 0);
    hwscr_con_init(&c);
    check(hwscr_con_input(&c, 1u) == 1u && c.visible == 0u, "toggled off during boot: stays off", 0);
    hwscr_con_init(&c);
    (void)hwscr_con_presented(&c);
    hwscr_con_show(&c);   /* bet 2.6c: a save-failure notice */
    check(c.visible == 1u && hwscr_con_presented(&c) == 0u && c.visible == 1u, "a notice brings the console up and a presented frame does not hide it", 0);
    check(hwscr_con_input(&c, 1u) == 1u && c.visible == 0u, "and ZL+ZR still hides it", 0);
}

static void test_cfg(void)
{
    HwCfg c;
    uint32_t skipped = 0;
    const char *ok = "fbdump = 1005:60\n";
    const char *bad[] = { "fbdump = 1005\n", "fbdump = 1005:0\n", "fbdump = 1005:201\n", "fbdump = :5\n", "fbdump = a:5\n" };
    unsigned i;

    hwcfg_defaults(&c);
    check(c.fbdump_count == 0u, "default: fbdump off", 0);
    check(hwcfg_parse(&c, ok, strlen(ok), &skipped) == 1u && c.fbdump_from == 1005u && c.fbdump_count == 60u && skipped == 0u, "fbdump = FROM:COUNT parsed", 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        hwcfg_defaults(&c);
        check(hwcfg_parse(&c, bad[i], strlen(bad[i]), &skipped) == 0u && c.fbdump_count == 0u, "bad fbdump value rejected", (long)i);
    }
    hwcfg_defaults(&c);
    {
        const char *g = "gxtrace = 100:600\n";

        check(hwcfg_parse(&c, g, strlen(g), &skipped) == 1u && c.gx_trace_count == 600u && c.fbdump_count == 0u, "gxtrace keeps its own bound (600) and does not touch fbdump", 0);
    }
}

int main(void)
{
    test_blit_one(HWSCR_TOP_W);
    test_blit_one(HWSCR_BOT_W);
    test_powcnt();
    test_dump();
    test_pipe();
    test_console();
    test_cfg();
    printf("hw_screen_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
