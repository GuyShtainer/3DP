/* hw_screen.c -- see hw_screen.h. Pure C, no SDK, no libctru. */
#include <assert.h>
#include <string.h>

#include "hw_screen.h"

#define IMG_W   GPU2D_W
#define IMG_H   GPU2D_LINES
#define IMG_Y0  ((HWSCR_H - IMG_H) / 2u)   /* 24 */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One framebuffer column of an image column: the 24 black rows below, the image rows (screen row IMG_Y0 + sy lives at byte (239 - y) * 3, so the image is stored
   bottom row first), the 24 black rows above. */
static void blit_column(const uint32_t *pix, uint32_t sx, uint8_t *col)
{
    uint32_t sy;

    memset(col, 0, IMG_Y0 * 3u);                                  /* screen rows 216..239 */
    memset(col + (IMG_Y0 + IMG_H) * 3u, 0, IMG_Y0 * 3u);          /* screen rows 0..23 */
    for (sy = 0; sy < IMG_H; sy++) {
        uint32_t p = pix[sy * IMG_W + sx];
        uint8_t *d = col + (HWSCR_H - 1u - (IMG_Y0 + sy)) * 3u;

        d[0] = (uint8_t)p;          /* B */
        d[1] = (uint8_t)(p >> 8);   /* G */
        d[2] = (uint8_t)(p >> 16);  /* R */
    }
}

void hwscr_blit_bgr8(const uint32_t *pix, uint8_t *fb, uint32_t screen_w)
{
    uint32_t x0, sx;

    assert(pix != NULL && fb != NULL);
    assert(screen_w == HWSCR_TOP_W || screen_w == HWSCR_BOT_W);
    x0 = (screen_w - IMG_W) / 2u;
    memset(fb, 0, (size_t)x0 * HWSCR_H * 3u);
    for (sx = 0; sx < IMG_W; sx++) {
        blit_column(pix, sx, fb + (size_t)(x0 + sx) * HWSCR_H * 3u);
    }
    memset(fb + (size_t)(x0 + IMG_W) * HWSCR_H * 3u, 0, (size_t)(screen_w - x0 - IMG_W) * HWSCR_H * 3u);
}

void hwscr_blit_bgr8_wide(const uint32_t *pix, uint8_t *fb)
{
    uint32_t x, y;

    assert(pix != NULL && fb != NULL);
    for (x = 0; x < HWSCR_TOP_W; x++) {
        uint8_t *col = fb + (size_t)x * HWSCR_H * 3u;

        for (y = 0; y < HWSCR_H; y++) {
            uint32_t p = pix[y * HWSCR_TOP_W + x];
            uint8_t *d = col + (HWSCR_H - 1u - y) * 3u;

            d[0] = (uint8_t)p;          /* B */
            d[1] = (uint8_t)(p >> 8);   /* G */
            d[2] = (uint8_t)(p >> 16);  /* R */
        }
    }
}

static void wdump_image(uint8_t *o, const uint32_t *pix)
{
    uint32_t i;

    for (i = 0; i < HWSCR_WDUMP_PIXELS; i++, o += 3) {
        uint32_t p = pix != NULL ? pix[i] : 0u;

        o[0] = (uint8_t)(p >> 16);
        o[1] = (uint8_t)(p >> 8);
        o[2] = (uint8_t)p;
    }
}

void hwscr_wdump_pack(uint8_t *buf, uint32_t vb, const uint32_t *left, const uint32_t *right, const uint32_t *layer_l, const uint32_t *layer_r, uint32_t clear_word,
                      const Gpu2dFrame *f, uint32_t slider_pct, float sigma)
{
    const uint8_t *r0, *r191;
    uint32_t i;

    assert(buf != NULL && left != NULL && f != NULL);
    memset(buf, 0, HWSCR_WDUMP_HDR);
    r0 = gpu2d_line_regs(f, 0u, 0u);
    r191 = gpu2d_line_regs(f, 0u, GPU2D_LINES - 1u);
    put_le32(buf + 0, HWSCR_WDUMP_MAGIC);
    put_le32(buf + 4, HWSCR_WDUMP_VERSION);
    put_le32(buf + 8, vb);
    put_le32(buf + 12, right != NULL ? 1u : 0u);
    put_le32(buf + 16, HWSCR_TOP_W);
    put_le32(buf + 20, HWSCR_H);
    put_le32(buf + 24, slider_pct);
    put_le32(buf + 28, sigma > 0.0f ? (uint32_t)(sigma * 1e6f + 0.5f) : 0u);
    put_le32(buf + 32, (uint32_t)r0[0x6C] | ((uint32_t)r0[0x6D] << 8));
    put_le32(buf + 36, (uint32_t)r191[0x6C] | ((uint32_t)r191[0x6D] << 8));
    put_le32(buf + 40, get_le32(r0 + 0x50));
    put_le32(buf + 44, get_le32(r0));
    put_le32(buf + 48, (uint32_t)f->pltt[0][0] | ((uint32_t)f->pltt[0][1] << 8));
    put_le32(buf + 52, clear_word);
    wdump_image(buf + HWSCR_WDUMP_HDR, left);
    wdump_image(buf + HWSCR_WDUMP_HDR + HWSCR_WDUMP_PIXELS * 3u, right);
    for (i = 0; i < HWSCR_WDUMP_PIXELS; i++) {
        put_le32(buf + HWSCR_WDUMP_HDR + 2u * HWSCR_WDUMP_PIXELS * 3u + 4u * i, layer_l != NULL ? layer_l[i] : 0u);
        put_le32(buf + HWSCR_WDUMP_HDR + 2u * HWSCR_WDUMP_PIXELS * 3u + HWSCR_WDUMP_PIXELS * 4u + 4u * i, layer_r != NULL ? layer_r[i] : 0u);
    }
}

void hwscr_black_bgr8(uint8_t *fb, uint32_t screen_w)
{
    assert(fb != NULL);
    assert(screen_w == HWSCR_TOP_W || screen_w == HWSCR_BOT_W);
    memset(fb, 0, (size_t)screen_w * HWSCR_H * 3u);
}

uint32_t hwscr_a_on_top(const uint8_t *io)
{
    assert(io != NULL);
    return (uint32_t)((io[HWSCR_POWCNT1 + 1u] >> 7) & 1u);
}

/* ---- the fbdump file ---------------------------------------------------------------------------------------------------- */
static uint8_t *pack_pix(uint8_t *d, const uint32_t *pix)
{
    uint32_t i;

    for (i = 0; i < GPU2D_PIXELS; i++) {
        d[0] = (uint8_t)(pix[i] >> 16);
        d[1] = (uint8_t)(pix[i] >> 8);
        d[2] = (uint8_t)pix[i];
        d += 3;
    }
    return d;
}

void hwscr_dump_pack(uint8_t *buf, uint32_t vb, uint32_t a_on_top, const Gpu2dOut *out)
{
    uint8_t *d;

    assert(buf != NULL && out != NULL);
    put_le32(buf + 0, HWSCR_DUMP_MAGIC);
    put_le32(buf + 4, HWSCR_DUMP_VERSION);
    put_le32(buf + 8, vb);
    put_le32(buf + 12, a_on_top != 0u ? 1u : 0u);
    put_le32(buf + 16, IMG_W);
    put_le32(buf + 20, IMG_H);
    put_le32(buf + 24, out->unsupported);
    put_le32(buf + 28, 0u);
    d = pack_pix(buf + HWSCR_DUMP_HDR, out->pix[0]);
    d = pack_pix(d, out->pix[1]);
    memcpy(d, out->lid[0], GPU2D_PIXELS);
    memcpy(d + GPU2D_PIXELS, out->lid[1], GPU2D_PIXELS);
}

int hwscr_dump_parse(const uint8_t *buf, uint32_t len, HwScrDump *h, Gpu2dOut *out)
{
    const uint8_t *s;
    uint32_t e, i;

    assert(buf != NULL && h != NULL);
    if (len != HWSCR_DUMP_SIZE || get_le32(buf) != HWSCR_DUMP_MAGIC || get_le32(buf + 4) != HWSCR_DUMP_VERSION || get_le32(buf + 16) != IMG_W ||
        get_le32(buf + 20) != IMG_H) {
        return 0;
    }
    h->vb = get_le32(buf + 8);
    h->a_on_top = get_le32(buf + 12) & 1u;
    h->unsupported = get_le32(buf + 24);
    if (out == NULL) {
        return 1;
    }
    out->unsupported = h->unsupported;
    out->unsupported_mask = 0u;
    s = buf + HWSCR_DUMP_HDR;
    for (e = 0; e < GPU2D_ENGINES; e++) {
        for (i = 0; i < GPU2D_PIXELS; i++) {
            out->pix[e][i] = 0xFF000000u | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2];
            s += 3;
        }
    }
    memcpy(out->lid[0], s, GPU2D_PIXELS);
    memcpy(out->lid[1], s + GPU2D_PIXELS, GPU2D_PIXELS);
    return 1;
}

/* ---- the frame hand-off ------------------------------------------------------------------------------------------------- */
void hwscr_pipe_init(HwScrPipe *p)
{
    assert(p != NULL);
    memset(p, 0, sizeof *p);
}

uint32_t hwscr_pipe_offer(HwScrPipe *p)
{
    assert(p != NULL && p->cur < 2u);
    p->points++;
    if (__atomic_load_n(&p->busy, __ATOMIC_ACQUIRE) != 0u) {
        p->overruns++;
        return 0u;
    }
    p->accepted++;
    p->rslot = p->cur;
    p->cur ^= 1u;
    __atomic_store_n(&p->busy, 1u, __ATOMIC_RELEASE);   /* last: the render thread is woken after this, and reads rslot */
    return 1u;
}

void hwscr_pipe_rendered(HwScrPipe *p)
{
    assert(p != NULL);
    p->composited++;
}

void hwscr_pipe_done(HwScrPipe *p)
{
    assert(p != NULL);
    p->presented++;
    __atomic_store_n(&p->busy, 0u, __ATOMIC_RELEASE);
}

uint32_t hwscr_pipe_busy(const HwScrPipe *p)
{
    assert(p != NULL);
    return __atomic_load_n(&p->busy, __ATOMIC_ACQUIRE);
}

/* ---- the bottom screen's owner ------------------------------------------------------------------------------------------- */
void hwscr_con_init(HwScrCon *c)
{
    assert(c != NULL);
    memset(c, 0, sizeof *c);
    c->visible = 1u;
}

uint32_t hwscr_con_input(HwScrCon *c, uint32_t held)
{
    uint32_t edge;

    assert(c != NULL);
    edge = held != 0u && c->chord == 0u;
    c->chord = held != 0u ? 1u : 0u;
    if (!edge || c->forced != 0u) {
        return 0u;
    }
    c->visible ^= 1u;
    c->user = 1u;
    return 1u;
}

uint32_t hwscr_con_presented(HwScrCon *c)
{
    assert(c != NULL);
    if (c->visible != 0u && c->user == 0u && c->forced == 0u) {
        c->visible = 0u;
        return 1u;
    }
    return 0u;
}

void hwscr_con_show(HwScrCon *c)
{
    assert(c != NULL);
    c->visible = 1u;
    c->user = 1u;
}

void hwscr_con_force(HwScrCon *c)
{
    assert(c != NULL);
    c->forced = 1u;
    c->visible = 1u;
}
