/* hw_setup_core.c: the pure-C half of the first-launch setup screen (release P4a). Only <stdint.h>, <stddef.h>, <string.h>.
 * Every input (the FNT/FAT, the config, the save tail) comes from the user's files, so every read is length-checked and every
 * loop has a fixed bound. Our own code. */
#include "hw_setup_core.h"
#include <string.h>

/* ---------- SHA-1 (FIPS 180-4) ---------- */

static uint32_t rol32(uint32_t v, unsigned n) { return (v << n) | (v >> (32u - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void setup_sha1_init(setup_sha1 *c)
{
    if (c == NULL) return;
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->used = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void setup_sha1_update(setup_sha1 *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    if (c == NULL || (p == NULL && n != 0) || c->used >= 64) return;
    c->len += (uint64_t)n;
    if (c->used != 0) {
        size_t room = 64u - c->used;
        size_t take = n < room ? n : room;
        memcpy(c->buf + c->used, p, take);
        c->used += (uint32_t)take; p += take; n -= take;
        if (c->used < 64) return;
        sha1_block(c->h, c->buf);
        c->used = 0;
    }
    while (n >= 64) {          /* bounded by n / 64 */
        sha1_block(c->h, p);
        p += 64; n -= 64;
    }
    if (n != 0) {
        memcpy(c->buf, p, n);
        c->used = (uint32_t)n;
    }
}

void setup_sha1_final(setup_sha1 *c, uint8_t out[20])
{
    uint64_t bits;
    uint8_t pad[72];
    size_t padlen;
    unsigned i;
    if (c == NULL || out == NULL) return;
    bits = c->len * 8u;
    padlen = (c->used < 56u) ? (56u - c->used) : (120u - c->used);
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    setup_sha1_update(c, pad, padlen + 8);
    for (i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* ---------- ROM header ---------- */

static uint16_t crc16_modbus(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    unsigned b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

int setup_rom_header_ok(const uint8_t hdr[0x200])
{
    uint16_t want;
    if (hdr == NULL) return 0;
    if (hdr[0x0C] != 'C' || hdr[0x0D] != 'P' || hdr[0x0E] != 'U' || hdr[0x0F] != 'E') return 0;
    want = (uint16_t)(hdr[0x15E] | (hdr[0x15F] << 8));
    return crc16_modbus(hdr, 0x15E) == want ? 1 : 0;
}

/* ---------- NitroFS ---------- */

static int rd16(const uint8_t *t, uint32_t len, uint32_t off, uint32_t *v)
{
    if (off > len || len - off < 2u) return 0;
    *v = (uint32_t)t[off] | ((uint32_t)t[off + 1] << 8);
    return 1;
}

static int rd32(const uint8_t *t, uint32_t len, uint32_t off, uint32_t *v)
{
    if (off > len || len - off < 4u) return 0;
    *v = (uint32_t)t[off] | ((uint32_t)t[off + 1] << 8) | ((uint32_t)t[off + 2] << 16) | ((uint32_t)t[off + 3] << 24);
    return 1;
}

/* Scan one directory's sub-table for `name` (nlen bytes). Result: 1 file found (*id), 2 dir found (*id = dir id), 0 absent, -1 malformed. */
static int nfs_scan_dir(const uint8_t *fnt, uint32_t fnt_len, uint32_t dir_idx, uint32_t ndirs,
                        const char *name, uint32_t nlen, uint32_t *id)
{
    uint32_t sub_off, file_id, pos, steps;
    if (dir_idx >= ndirs || !rd32(fnt, fnt_len, dir_idx * 8u, &sub_off) || !rd16(fnt, fnt_len, dir_idx * 8u + 4u, &file_id)) return -1;
    pos = sub_off;
    for (steps = 0; steps <= fnt_len; steps++) {     /* each step consumes >= 1 byte of the table */
        uint32_t tag, l;
        if (pos >= fnt_len) return -1;               /* no terminator before the table ends */
        tag = fnt[pos++];
        if (tag == 0) return 0;
        l = tag & 0x7Fu;
        if (l == 0 || fnt_len - pos < l) return -1;
        {
            int same = (l == nlen) && memcmp(fnt + pos, name, l) == 0;
            pos += l;
            if (tag & 0x80u) {
                uint32_t sub;
                if (!rd16(fnt, fnt_len, pos, &sub)) return -1;
                pos += 2;
                if (same) { *id = sub; return 2; }
            } else {
                if (same) { *id = file_id; return 1; }
                file_id++;
            }
        }
    }
    return -1;
}

int setup_nitrofs_find(const uint8_t *fnt, uint32_t fnt_len, const uint8_t *fat, uint32_t fat_len,
                       const char *path, uint32_t *start, uint32_t *end)
{
    uint32_t ndirs, dir_idx = 0, p = 0, depth, s, e;
    if (fnt == NULL || fat == NULL || path == NULL || start == NULL || end == NULL) return -1;
    if (path[0] != '/') return 0;
    if (!rd16(fnt, fnt_len, 6, &ndirs) || ndirs == 0 || ndirs > 4096u || (uint64_t)ndirs * 8u > fnt_len) return -1;
    p = 1;
    for (depth = 0; depth < 32; depth++) {           /* path depth is capped at 32 components */
        uint32_t q = p, id = 0;
        int r;
        while (path[q] != '\0' && path[q] != '/') { if (q - p > 127u) return 0; q++; }
        if (q == p) return 0;                        /* empty component */
        r = nfs_scan_dir(fnt, fnt_len, dir_idx, ndirs, path + p, q - p, &id);
        if (r < 0) return -1;
        if (r == 0) return 0;
        if (path[q] == '\0') {
            if (r != 1) return 0;                    /* the path names a directory */
            if (id >= fat_len / 8u) return -1;
            if (!rd32(fat, fat_len, id * 8u, &s) || !rd32(fat, fat_len, id * 8u + 4u, &e) || e < s) return -1;
            *start = s; *end = e;
            return 1;
        }
        if (r != 2) return 0;                        /* a file used as a directory */
        if ((id & 0xF000u) != 0xF000u) return -1;
        dir_idx = id & 0xFFFu;
        p = q + 1;
    }
    return 0;
}

/* ---------- save files ---------- */

#define SAVE_RAW_BYTES 524288u
#define DSV_FOOTER_BYTES 122u
/* DeSmuME's footer (src/mc.cpp, BackupDevice::ensure / readFooter, https://github.com/TASEmulators/desmume): the 82-char snip text,
 * then six little-endian u32 (data written, padded size, memory type, address size, memory size, version = 0), then the 16-byte cookie. */
static const char dsv_snip[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
static const char dsv_cookie[] = "|-DESMUME SAVE-|";

int setup_save_kind(uint64_t file_size, const uint8_t *tail, size_t tail_len, uint64_t *raw_len)
{
    const uint8_t *f;
    uint32_t pad, version, written;
    uint64_t raw;
    if (raw_len != NULL) *raw_len = 0;
    if (file_size == SAVE_RAW_BYTES) { if (raw_len != NULL) *raw_len = SAVE_RAW_BYTES; return SETUP_SAVE_RAW; }
    if (tail == NULL || file_size < (uint64_t)SAVE_RAW_BYTES + DSV_FOOTER_BYTES) return SETUP_SAVE_BAD;
    if (tail_len < DSV_FOOTER_BYTES || tail_len > file_size) return SETUP_SAVE_BAD;
    f = tail + (tail_len - DSV_FOOTER_BYTES);
    if (memcmp(f, dsv_snip, 82) != 0 || memcmp(f + 106, dsv_cookie, 16) != 0) return SETUP_SAVE_BAD;
    written = (uint32_t)f[82] | ((uint32_t)f[83] << 8) | ((uint32_t)f[84] << 16) | ((uint32_t)f[85] << 24);
    pad = (uint32_t)f[86] | ((uint32_t)f[87] << 8) | ((uint32_t)f[88] << 16) | ((uint32_t)f[89] << 24);
    version = (uint32_t)f[102] | ((uint32_t)f[103] << 8) | ((uint32_t)f[104] << 16) | ((uint32_t)f[105] << 24);
    raw = file_size - DSV_FOOTER_BYTES;
    if (version != 0 || (uint64_t)pad != raw || written > pad || raw < SAVE_RAW_BYTES) return SETUP_SAVE_BAD;
    if (raw_len != NULL) *raw_len = SAVE_RAW_BYTES;
    return SETUP_SAVE_DSV;
}

/* ---------- config.ini ---------- */

#define CFG_MAX 4096u
#define ROM_PATH_MAX 80u

typedef struct { char *out; size_t cap; size_t len; int bad; } cfg_out;

static void cfg_put(cfg_out *o, const char *s, size_t n)
{
    if (o->bad) return;
    if (n > o->cap - o->len || n > CFG_MAX - o->len) { o->bad = 1; return; }
    if (n != 0) memcpy(o->out + o->len, s, n);
    o->len += n;
}

static int is_ws(char c) { return c == ' ' || c == '\t'; }

/* 0 normal line, 1 section header, 2 a `rom` key line */
static int cfg_classify(const char *l, size_t n)
{
    size_t i = 0;
    while (i < n && is_ws(l[i])) i++;
    if (i < n && l[i] == '[') return 1;
    if (n - i >= 3 && l[i] == 'r' && l[i + 1] == 'o' && l[i + 2] == 'm') {
        i += 3;
        while (i < n && is_ws(l[i])) i++;
        if (i < n && l[i] == '=') return 2;
    }
    return 0;
}

int setup_rom_path_ok(const char *path)
{
    size_t n = 0;
    if (path == NULL) return 0;
    while (n <= ROM_PATH_MAX && path[n] != '\0') {
        unsigned char b = (unsigned char)path[n];
        if (b < 0x20u || b == 0x7Fu) return 0;
        n++;
    }
    if (n == 0 || n > ROM_PATH_MAX) return 0;
    if (is_ws(path[0]) || is_ws(path[n - 1])) return 0;
    return 1;
}

long setup_config_set_rom(const char *in, size_t in_len, const char *rom_path, char *out, size_t out_cap)
{
    cfg_out o;
    size_t plen = 0, pos = 0, guard;
    int top = 1, done = 0;
    if (rom_path == NULL || out == NULL || (in == NULL && in_len != 0)) return -1;
    if (!setup_rom_path_ok(rom_path) || in_len > CFG_MAX * 2u) return -1;
    plen = strlen(rom_path);
    o.out = out; o.cap = out_cap; o.len = 0; o.bad = 0;
    for (guard = 0; guard <= in_len && pos < in_len; guard++) {   /* each pass consumes >= 1 byte */
        size_t e = pos, term;
        int kind;
        while (e < in_len && in[e] != '\n') e++;
        term = (e < in_len) ? 1u : 0u;            /* '\n' present; a CR before it stays in the line text */
        kind = top ? cfg_classify(in + pos, e - pos) : 0;
        if (kind == 1 && !done) {
            cfg_put(&o, "rom = ", 6); cfg_put(&o, rom_path, plen); cfg_put(&o, "\n", 1);
            done = 1;
        }
        if (kind == 1) top = 0;
        if (kind == 2) {
            if (!done) {
                size_t cr = (e > pos && in[e - 1] == '\r') ? 1u : 0u;
                cfg_put(&o, "rom = ", 6); cfg_put(&o, rom_path, plen);
                if (cr) cfg_put(&o, "\r", 1);
                if (term) cfg_put(&o, "\n", 1);
                done = 1;
            }                                       /* a later duplicate top-level rom line is dropped */
        } else {
            cfg_put(&o, in + pos, e - pos + term);
        }
        pos = e + term;
    }
    if (!done) {
        if (o.len != 0 && o.out[o.len - 1] != '\n') cfg_put(&o, "\n", 1);
        cfg_put(&o, "rom = ", 6); cfg_put(&o, rom_path, plen); cfg_put(&o, "\n", 1);
    }
    return o.bad ? -1 : (long)o.len;
}

/* ---------- backup names ---------- */

int setup_bak_name(const char *base, int (*exists)(const char *path, void *ctx), void *ctx, char *out, size_t out_cap)
{
    size_t blen = 0, digits;
    int n;
    if (base == NULL || exists == NULL || out == NULL) return -1;
    while (blen < CFG_MAX && base[blen] != '\0') blen++;
    if (blen == CFG_MAX) return -1;
    for (n = 1; n <= 999; n++) {
        size_t total;
        digits = (n >= 100) ? 3u : (n >= 10) ? 2u : 1u;
        total = blen + 5u + digits + 1u;               /* ".bak-" + digits + NUL */
        if (total > out_cap) return -1;
        memcpy(out, base, blen);
        memcpy(out + blen, ".bak-", 5);
        if (digits == 3) out[blen + 5] = (char)('0' + n / 100);
        if (digits >= 2) out[blen + 5 + digits - 2] = (char)('0' + (n / 10) % 10);
        out[blen + 5 + digits - 1] = (char)('0' + n % 10);
        out[blen + 5 + digits] = '\0';
        if (exists(out, ctx) == 0) return n;
    }
    return -1;
}
