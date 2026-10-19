/* hw_cfg.c -- config.ini parse and format. Pure C99. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hw_cfg.h"

#define LINE_MAX_BYTES 120u

void hwcfg_defaults(HwCfg *c)
{
    static const char name[] = "PLAYER";
    static const uint8_t mac[6] = { 0x00, 0x09, 0xbf, 0x3d, 0x50, 0x01 };   /* Nintendo's OUI + a fixed tail */
    size_t i;

    assert(c != NULL);
    assert(sizeof name - 1u <= HWCFG_NAME_MAX);
    memset(c, 0, sizeof *c);
    for (i = 0; i < sizeof name - 1u; i++) {
        c->name[i] = (uint16_t)(unsigned char)name[i];
    }
    c->name_len = (uint8_t)(sizeof name - 1u);
    c->color = 11;
    c->birth_month = 1;
    c->birth_day = 1;
    c->language = 1;
    memcpy(c->mac, mac, sizeof mac);
    c->allowed_channels = HWCFG_ALLOWED_CHANNELS_DEFAULT;
    c->rtc_offset = 0;
    c->id_rtc_offset = 0;
    c->id_rtc_set = 0;
    c->rtc_fixed = 0;
    c->rtc_fixed_set = 0;
    c->census = 0;
    c->clock_frame = 0;
    c->gx_seal_frame_end = 0;
    c->gx_trace_from = 0;
    c->gx_trace_count = 0;
    c->fbdump_from = 0;
    c->fbdump_count = 0;
    c->polydump_from = 0;
    c->polydump_count = 0;
    c->render_sync = 0;
    c->render_core = 2;
    c->hwcheck = 0;
}

static int parse_uint(const char *s, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    uint32_t n = 0;

    for (; *s >= '0' && *s <= '9' && n < 10u; s++, n++) {
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > max) {
            return 0;
        }
    }
    *out = v;
    return n > 0u && (*s == '\0');
}

/* a frame window "FROM:COUNT": FROM 0..10^7, COUNT 1..HWCFG_GXTRACE_MAX (a trace is never unbounded: a field run is ~100 MB) */
static int parse_window_max(const char *s, uint32_t max, uint32_t *from, uint32_t *count)
{
    char head[12];
    const char *colon = strchr(s, ':');
    size_t n;
    uint32_t f = 0, c = 0;

    if (colon == NULL) {
        return 0;
    }
    n = (size_t)(colon - s);
    if (n == 0u || n >= sizeof head) {
        return 0;
    }
    memcpy(head, s, n);
    head[n] = '\0';
    if (!parse_uint(head, 10000000u, &f) || !parse_uint(colon + 1, max, &c) || c == 0u) {
        return 0;
    }
    *from = f;
    *count = c;
    return 1;
}

static int parse_window(const char *s, uint32_t *from, uint32_t *count)
{
    return parse_window_max(s, HWCFG_GXTRACE_MAX, from, count);
}

/* a channel mask: decimal or 0x-prefixed hex, 1..max */
static int parse_mask(const char *s, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    uint32_t n = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; n < 8u; s++, n++) {
            uint32_t d;

            if (*s >= '0' && *s <= '9') {
                d = (uint32_t)(*s - '0');
            } else if (*s >= 'a' && *s <= 'f') {
                d = (uint32_t)(*s - 'a' + 10);
            } else if (*s >= 'A' && *s <= 'F') {
                d = (uint32_t)(*s - 'A' + 10);
            } else {
                break;
            }
            v = v * 16u + d;
            if (v > max) {
                return 0;
            }
        }
        *out = v;
        return n > 0u && *s == '\0';
    }
    return parse_uint(s, max, out);
}

static int parse_s64(const char *s, int64_t *out)
{
    int neg = 0;
    int64_t v = 0;
    uint32_t n = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    for (; *s >= '0' && *s <= '9' && n < 15u; s++, n++) {
        v = v * 10 + (*s - '0');
    }
    if (n == 0u || *s != '\0') {
        return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int parse_mac(const char *s, uint8_t *mac)
{
    uint8_t tmp[6];
    uint32_t i;

    for (i = 0; i < 6u; i++) {
        int hi = hexval(s[0]);
        int lo = hi < 0 ? -1 : hexval(s[1]);

        if (lo < 0) {
            return 0;
        }
        tmp[i] = (uint8_t)(hi * 16 + lo);
        s += 2;
        if (i < 5u) {
            if (*s != ':') {
                return 0;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return 0;
    }
    memcpy(mac, tmp, sizeof tmp);
    return 1;
}

static int parse_name(const char *s, HwCfg *c)
{
    uint32_t n = 0;

    for (; *s && n < HWCFG_NAME_MAX; s++, n++) {
        unsigned char ch = (unsigned char)*s;
        c->name[n] = (uint16_t)(ch >= 0x20 && ch < 0x7f ? ch : '?');
    }
    if (*s || n == 0u) {
        return 0;
    }
    c->name_len = (uint8_t)n;
    memset(&c->name[n], 0, (HWCFG_NAME_MAX - n) * sizeof c->name[0]);
    return 1;
}

static int apply(HwCfg *c, const char *key, const char *val)
{
    uint32_t v = 0;

    if (strcmp(key, "owner_name") == 0) {
        return parse_name(val, c);
    }
    if (strcmp(key, "owner_color") == 0 && parse_uint(val, 15u, &v)) {
        c->color = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "birth_month") == 0 && parse_uint(val, 12u, &v) && v >= 1u) {
        c->birth_month = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "birth_day") == 0 && parse_uint(val, 31u, &v) && v >= 1u) {
        c->birth_day = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "language") == 0 && parse_uint(val, 5u, &v)) {
        c->language = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "mac") == 0) {
        return parse_mac(val, c->mac);
    }
    if (strcmp(key, "allowed_channels") == 0 && parse_mask(val, 0x3fffu, &v) && v != 0u) {
        c->allowed_channels = (uint16_t)v;
        return 1;
    }
    if (strcmp(key, "rom") == 0) {   /* release P4b: 1..HWCFG_ROM_MAX bytes; RF2: UTF-8 bytes (0x80..0xFF) are kept as they are (the sdmc devoptab takes UTF-8 paths), control bytes and 0x7F are not */
        size_t n = strlen(val), i;

        if (n == 0u || n > HWCFG_ROM_MAX) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)val[i] < 0x20u || (unsigned char)val[i] == 0x7fu) {
                return 0;
            }
        }
        memcpy(c->rom, val, n + 1u);
        c->rom_set = 1;
        return 1;
    }
    if (strcmp(key, "setup") == 0 && parse_uint(val, 1u, &v)) {
        c->setup_off = (uint8_t)(v == 0u ? 1u : 0u);
        return 1;
    }
    if (strcmp(key, "rendersync") == 0 && parse_uint(val, 1u, &v)) {
        c->render_sync = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "render_core") == 0 && parse_uint(val, 2u, &v)) {
        c->render_core = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "hwcheck") == 0 && parse_uint(val, 3u, &v)) {
        c->hwcheck = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "wide") == 0 && parse_uint(val, 1u, &v)) {   /* bet 2.8 */
        c->wide = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "stereo_slider") == 0 && parse_uint(val, 100u, &v)) {
        c->stereo_slider = (uint8_t)(v + 1u);
        return 1;
    }
    if (strcmp(key, "stereo") == 0 && parse_uint(val, 1u, &v)) {
        c->stereo = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "census") == 0 && parse_uint(val, 1u, &v)) {
        c->census = (uint8_t)v;
        return 1;
    }
    if (strcmp(key, "clock") == 0) {
        if (strcmp(val, "frame") == 0 || strcmp(val, "wall") == 0) {
            c->clock_frame = val[0] == 'f' ? 1u : 0u;
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "gxseal") == 0) {
        if (strcmp(val, "snapshot") == 0 || strcmp(val, "frame-end") == 0) {
            c->gx_seal_frame_end = val[0] == 'f' ? 1u : 0u;
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "gxtrace") == 0) {
        return parse_window(val, &c->gx_trace_from, &c->gx_trace_count);
    }
    if (strcmp(key, "fbdump") == 0) {
        return parse_window_max(val, HWCFG_FBDUMP_MAX, &c->fbdump_from, &c->fbdump_count);
    }
    if (strcmp(key, "polydump") == 0) {
        return parse_window_max(val, HWCFG_POLYDUMP_MAX, &c->polydump_from, &c->polydump_count);
    }
    if (strcmp(key, "rtc_fixed") == 0) {   /* 2000-01-01 .. 2099-12-31 UTC: the DS RTC has a two-digit year */
        int64_t e;

        if (parse_s64(val, &e) && e >= HWCFG_RTC_FIXED_MIN && e <= HWCFG_RTC_FIXED_MAX) {
            c->rtc_fixed = e;
            c->rtc_fixed_set = 1;
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "rtc_offset") == 0) {
        return parse_s64(val, &c->rtc_offset);
    }
    return 0;
}

/* trim leading and trailing blanks in place; returns the start */
static char *trim(char *s)
{
    size_t n;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = strlen(s);
    while (n > 0u && (s[n - 1u] == ' ' || s[n - 1u] == '\t' || s[n - 1u] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

uint32_t hwcfg_parse(HwCfg *c, const char *text, size_t len, uint32_t *skipped)
{
    uint32_t applied = 0;
    uint32_t bad = 0;
    size_t pos = 0;

    assert(c != NULL);
    assert(text != NULL || len == 0u);
    while (pos < len) {
        char line[LINE_MAX_BYTES + 1u];
        size_t n = 0;
        char *eq, *s;

        while (pos < len && text[pos] != '\n') {
            if (n < LINE_MAX_BYTES) {
                line[n++] = text[pos];
            }
            pos++;
        }
        pos++;
        line[n] = '\0';
        s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {   /* bet 2.6a: the first section header ends the flat keys; sections are hwcfg_save_lookup's */
            break;
        }
        eq = strchr(s, '=');
        if (!eq) {
            bad++;
            continue;
        }
        *eq = '\0';
        if (apply(c, trim(s), trim(eq + 1))) {
            applied++;
        } else {
            bad++;
        }
    }
    if (skipped) {
        *skipped = bad;
    }
    return applied;
}

size_t hwcfg_format(const HwCfg *c, char *buf, size_t cap)
{
    char name[HWCFG_NAME_MAX + 1u];
    uint32_t i;
    int n;

    if (!c || !buf || cap < 2u) {
        return 0;
    }
    assert(c->name_len <= HWCFG_NAME_MAX);
    for (i = 0; i < c->name_len && i < HWCFG_NAME_MAX; i++) {
        name[i] = (char)(c->name[i] >= 0x20u && c->name[i] < 0x7fu ? c->name[i] : '?');
    }
    name[i] = '\0';
    n = snprintf(buf, cap,
                 "# 3DP: the identity of the emulated Nintendo DS. The game compares these with its save file; change them and the\n"
                 "# game treats the save as coming from another console (it restarts the play-time penalty clock).\n"
                 "owner_name = %s\n"
                 "owner_color = %u\n"
                 "birth_month = %u\n"
                 "birth_day = %u\n"
                 "language = %u\n"
                 "mac = %02x:%02x:%02x:%02x:%02x:%02x\n"
                 "allowed_channels = 0x%04x\n"
                 "rtc_offset = %lld\n",
                 name, (unsigned)c->color, (unsigned)c->birth_month, (unsigned)c->birth_day, (unsigned)c->language, c->mac[0], c->mac[1],
                 c->mac[2], c->mac[3], c->mac[4], c->mac[5], (unsigned)c->allowed_channels, (long long)c->rtc_offset);
    if (n < 0) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1u;
}

/* ---- bet 2.6a: per-save identity sections ---------------------------------------------------------------------------------------------------------------------- */

/* "[save T-S]" -> 1 and the ids when the (trimmed) line is a well-formed save header */
static int parse_save_header(const char *s, uint32_t *tid, uint32_t *sid)
{
    char buf[LINE_MAX_BYTES + 1u];
    char *dash, *end;
    size_t n;

    if (strncmp(s, "[save ", 6) != 0) {
        return 0;
    }
    n = strlen(s);
    if (n < 10u || s[n - 1u] != ']') {
        return 0;
    }
    memcpy(buf, s + 6, n - 7u);
    buf[n - 7u] = '\0';
    dash = strchr(buf, '-');
    if (dash == NULL) {
        return 0;
    }
    *dash = '\0';
    end = trim(buf);
    if (!parse_uint(end, 0xFFFFu, tid)) {
        return 0;
    }
    end = trim(dash + 1);
    return parse_uint(end, 0xFFFFu, sid);
}

static void section_key(HwCfgSave *o, const char *key, const char *val)
{
    uint32_t v = 0;
    int64_t r = 0;

    if (strcmp(key, "mac") == 0 && parse_mac(val, o->mac)) {
        o->mac_set = 1;
    } else if (strcmp(key, "rtc_offset") == 0 && parse_s64(val, &r)) {
        o->rtc_offset = r;
        o->rtc_set = 1;
    } else if (strcmp(key, "adopted_from_save") == 0 && parse_uint(val, 1u, &v)) {
        o->adopted = (uint8_t)v;
    }
}

int hwcfg_save_lookup(const char *text, size_t len, uint32_t tid, uint32_t sid, HwCfgSave *out)
{
    size_t pos = 0;
    int in = 0;

    assert(out != NULL);
    assert(text != NULL || len == 0u);
    memset(out, 0, sizeof *out);
    out->adopted = 1;
    while (pos < len) {
        char line[LINE_MAX_BYTES + 1u];
        size_t n = 0;
        char *s, *eq;

        while (pos < len && text[pos] != '\n') {
            if (n < LINE_MAX_BYTES) {
                line[n++] = text[pos];
            }
            pos++;
        }
        pos++;
        line[n] = '\0';
        s = trim(line);
        if (*s == '[') {
            uint32_t t = 0, d = 0;

            if (out->found && in) {
                break;   /* the first matching section is complete */
            }
            in = !out->found && parse_save_header(s, &t, &d) && t == tid && d == sid;
            if (in) {
                out->found = 1;
            }
            continue;
        }
        if (!in || *s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        eq = strchr(s, '=');
        if (eq != NULL) {
            *eq = '\0';
            section_key(out, trim(s), trim(eq + 1));
        }
    }
    return out->found;
}

size_t hwcfg_format_save(uint32_t tid, uint32_t sid, const uint8_t mac[6], int64_t rtc_offset, char *buf, size_t cap)
{
    int n;

    if (mac == NULL || buf == NULL || cap < 2u || tid > 0xFFFFu || sid > 0xFFFFu) {
        return 0;
    }
    n = snprintf(buf, cap,
                 "\n# The identity of the console this save was last played on, read from the save itself on first sight (bet 2.6a). The game compares it with\n"
                 "# the emulated DS on Continue; a mismatch means \"played on another DS\" (time-based events stall). Edit the values to override them,\n"
                 "# or set adopted_from_save = 0 to ignore this section and use the mac / rtc_offset keys at the top of the file.\n"
                 "[save %u-%u]\n"
                 "mac = %02x:%02x:%02x:%02x:%02x:%02x\n"
                 "rtc_offset = %lld\n"
                 "adopted_from_save = 1\n",
                 (unsigned)tid, (unsigned)sid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (long long)rtc_offset);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}

int hwcfg_adopt_identity(HwCfg *c, const char *text, size_t len, uint32_t tid, uint32_t sid, const uint8_t mac[6], int64_t rtc_offset)
{
    HwCfgSave s;
    int found;

    assert(c != NULL && mac != NULL);
    found = hwcfg_save_lookup(text, len, tid, sid, &s);
    if (found && !s.adopted) {
        return HWCFG_ID_OPTOUT;
    }
    memcpy(c->mac, found && s.mac_set ? s.mac : mac, 6);
    c->id_rtc_offset = found && s.rtc_set ? s.rtc_offset : rtc_offset;
    c->id_rtc_set = 1;
    return found ? HWCFG_ID_SECTION : HWCFG_ID_NEW;
}
