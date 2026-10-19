/* hw_input.c -- the 3DS controls as DS input, and the scripted-input file (bet 1.6b). Pure C, host-tested.
 *
 * Script format (sdmc:/3ds/3DP/input.txt, optional; when present it REPLACES the real controls, see port_main.c):
 *   # comment
 *   <frame> <token> [<token> ...]
 * <frame> is a decimal frame number (frames counted by the pump's vblank hook, 60 per second); the line applies from that frame on and stays in
 * force until the next line. Lines must be in ascending frame order. Tokens, separated by spaces or '+':
 *   A B SELECT START RIGHT LEFT UP DOWN R L X Y   a held key
 *   NONE                                          nothing held (the default before the first line)
 *   0x...                                         a held-key mask in the DS bit layout (A=1 B=2 SELECT=4 START=8 RIGHT=0x10 LEFT=0x20 UP=0x40 DOWN=0x80 R=0x100 L=0x200 X=0x400 Y=0x800)
 *   T<x>,<y>                                      the pen down at DS touch-screen pixel (x 0-255, y 0-191)
 * Example:   120 A      200 NONE      260 START+T128,96      300 NONE
 */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

#define CTR_CPAD_RIGHT (1u << 28)
#define CTR_CPAD_LEFT  (1u << 29)
#define CTR_CPAD_UP    (1u << 30)
#define CTR_CPAD_DOWN  (1u << 31)
#define TOUCH_OFF_X 32
#define TOUCH_OFF_Y 24
#define XY_RELEASED 0x2c00u   /* X (bit 10), Y (bit 11) and DEBUG (bit 13) released: active low */

uint16_t hwin_keys_from_ctr(uint32_t ctr_held)
{
    uint16_t k = (uint16_t)(ctr_held & HWIN_ALL);   /* libctru's KEY_A..KEY_Y are bits 0-11 in the same order as the DS register */

    if (ctr_held & CTR_CPAD_RIGHT) {
        k |= HWIN_RIGHT;
    }
    if (ctr_held & CTR_CPAD_LEFT) {
        k |= HWIN_LEFT;
    }
    if (ctr_held & CTR_CPAD_UP) {
        k |= HWIN_UP;
    }
    if (ctr_held & CTR_CPAD_DOWN) {
        k |= HWIN_DOWN;
    }
    assert((k & ~HWIN_ALL) == 0u);
    return k;
}

void hwin_touch_from_ctr(int cx, int cy, uint16_t *x, uint16_t *y)
{
    int dx = cx - TOUCH_OFF_X, dy = cy - TOUCH_OFF_Y;

    assert(x != NULL && y != NULL);
    dx = dx < 0 ? 0 : (dx >= (int)HWIN_TOUCH_W ? (int)HWIN_TOUCH_W - 1 : dx);
    dy = dy < 0 ? 0 : (dy >= (int)HWIN_TOUCH_H ? (int)HWIN_TOUCH_H - 1 : dy);
    *x = (uint16_t)dx;
    *y = (uint16_t)dy;
}

uint16_t hwin_keyinput(uint16_t keys)
{
    return (uint16_t)(~keys & 0x03ffu);
}

uint16_t hwin_xybuf(uint16_t keys)
{
    uint16_t v = XY_RELEASED;

    if (keys & HWIN_X) {
        v = (uint16_t)(v & ~0x0400u);
    }
    if (keys & HWIN_Y) {
        v = (uint16_t)(v & ~0x0800u);
    }
    return v;
}

uint16_t hwin_raw_from_px(uint16_t px)
{
    assert(px < 256u);
    return (uint16_t)(px * 16u + 8u);
}

/* Two display points well inside the screen; the raw values are the 12-bit readings of those pixels' centres, so that the SDK's own
   TP_CalcCalibrateParam / TP_GetCalibratedPoint map hwin_raw_from_px() back to the pixel exactly (host-tested against the real tp.c). */
HwTpCal hwin_tp_cal(void)
{
    HwTpCal c;

    c.dx1 = 32;
    c.dy1 = 24;
    c.dx2 = 224;
    c.dy2 = 168;
    c.raw_x1 = hwin_raw_from_px(c.dx1);
    c.raw_y1 = hwin_raw_from_px(c.dy1);
    c.raw_x2 = hwin_raw_from_px(c.dx2);
    c.raw_y2 = hwin_raw_from_px(c.dy2);
    return c;
}

typedef struct KeyName {
    const char *name;
    uint16_t bit;
} KeyName;

static const KeyName s_names[] = {
    { "A", HWIN_A }, { "B", HWIN_B }, { "SELECT", HWIN_SELECT }, { "START", HWIN_START }, { "RIGHT", HWIN_RIGHT }, { "LEFT", HWIN_LEFT },
    { "UP", HWIN_UP }, { "DOWN", HWIN_DOWN }, { "R", HWIN_R }, { "L", HWIN_L }, { "X", HWIN_X }, { "Y", HWIN_Y }, { "NONE", 0u },
};

static int is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '+' || c == '\r';
}

static int parse_uint(const char *s, uint32_t len, uint32_t *used, uint32_t *out)
{
    uint32_t i = 0, v = 0;

    assert(s != NULL && used != NULL && out != NULL);
    while (i < len && s[i] >= '0' && s[i] <= '9' && i < 9u) {
        v = v * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    *used = i;
    *out = v;
    return i > 0u ? 0 : -1;
}

static int parse_hex(const char *s, uint32_t len, uint32_t *out)
{
    uint32_t i, v = 0;

    assert(s != NULL && out != NULL);
    if (len < 3u || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || len > 10u) {
        return -1;
    }
    for (i = 2; i < len; i++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* one token (no separators) into `st` */
static int parse_token(const char *t, uint32_t len, HwInState *st)
{
    uint32_t i, used, a, b;

    assert(t != NULL && st != NULL && len > 0u);
    if ((t[0] == 'T' || t[0] == 't') && len > 1u) {
        if (parse_uint(t + 1, len - 1u, &used, &a) != 0 || 1u + used >= len || t[1u + used] != ',') {
            return -1;
        }
        if (parse_uint(t + 2u + used, len - 2u - used, &i, &b) != 0 || 2u + used + i != len || a >= HWIN_TOUCH_W || b >= HWIN_TOUCH_H) {
            return -1;
        }
        st->touch = 1;
        st->x = (uint16_t)a;
        st->y = (uint16_t)b;
        return 0;
    }
    if (parse_hex(t, len, &a) == 0) {
        st->keys = (uint16_t)(st->keys | (a & HWIN_ALL));
        return 0;
    }
    for (i = 0; i < sizeof s_names / sizeof s_names[0]; i++) {
        if (strlen(s_names[i].name) == len && strncmp(s_names[i].name, t, len) == 0) {
            st->keys = (uint16_t)(st->keys | s_names[i].bit);
            return 0;
        }
    }
    return -1;
}

static int parse_line(const char *l, uint32_t len, uint32_t *frame, HwInState *st)
{
    uint32_t i = 0, used, ntok = 0;

    assert(l != NULL && frame != NULL && st != NULL);
    if (parse_uint(l, len, &used, frame) != 0) {
        return -1;
    }
    i = used;
    memset(st, 0, sizeof *st);
    while (i < len) {
        uint32_t start;

        while (i < len && is_sep(l[i])) {
            i++;
        }
        start = i;
        while (i < len && !is_sep(l[i])) {
            i++;
        }
        if (i > start) {
            if (parse_token(l + start, i - start, st) != 0) {
                return -1;
            }
            ntok++;
        }
    }
    return ntok > 0u ? 0 : -1;
}

int hwin_script_parse(HwInScript *s, const char *text, uint32_t len, uint32_t *bad_line)
{
    uint32_t pos = 0, line_no = 0;

    assert(s != NULL && text != NULL);
    memset(s, 0, sizeof *s);
    if (bad_line != NULL) {
        *bad_line = 0;
    }
    while (pos < len) {
        uint32_t end = pos, lo, hi;

        while (end < len && text[end] != '\n') {
            end++;
        }
        line_no++;
        lo = pos;
        hi = end;
        while (lo < hi && (text[lo] == ' ' || text[lo] == '\t')) {
            lo++;
        }
        if (lo < hi && text[lo] != '#' && text[lo] != '\r') {
            uint32_t fr = 0;
            HwInState st;

            if (s->n >= HWIN_SCRIPT_MAX || parse_line(text + lo, hi - lo, &fr, &st) != 0 || (s->n > 0u && fr < s->frame[s->n - 1u])) {
                if (bad_line != NULL) {
                    *bad_line = line_no;
                }
                return -1;
            }
            s->frame[s->n] = fr;
            s->st[s->n] = st;
            s->n++;
        }
        pos = end + 1u;
    }
    return (int)s->n;
}

HwInState hwin_script_at(const HwInScript *s, uint32_t frame)
{
    HwInState none;
    uint32_t i;

    assert(s != NULL && s->n <= HWIN_SCRIPT_MAX);
    memset(&none, 0, sizeof none);
    for (i = s->n; i > 0u; i--) {
        if (s->frame[i - 1u] <= frame) {
            return s->st[i - 1u];
        }
    }
    return none;
}

uint32_t hwin_script_split(const char *text, uint32_t len)
{
    static const char marker[] = "#game";
    uint32_t pos = 0;

    assert(text != NULL || len == 0u);
    while (pos < len) {
        uint32_t end = pos, hi;

        while (end < len && text[end] != '\n') {
            end++;
        }
        hi = end;
        if (hi > pos && text[hi - 1u] == '\r') {
            hi--;
        }
        if (hi - pos == sizeof marker - 1u && memcmp(text + pos, marker, sizeof marker - 1u) == 0) {
            return end < len ? end + 1u : len;
        }
        pos = end + 1u;
    }
    return 0;
}
