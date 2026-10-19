/* hw_bench.c -- see hw_bench.h. */
#include <assert.h>
#include <string.h>

#include "hw_bench.h"

static int is_tag_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int is_key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int is_val_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '.' || c == '-';
}

/* one entry line (no newline) into *e; 1 on success */
static int parse_line(HwBenchEntry *e, const char *s, size_t n)
{
    size_t i = 0, k = 0, used = 0;

    memset(e, 0, sizeof *e);
    while (i < n && is_tag_char(s[i])) {
        if (k + 1u >= HWBENCH_TAG) {
            return 0;
        }
        e->tag[k++] = s[i++];
    }
    if (k == 0u) {
        return 0;
    }
    while (i < n) {
        size_t ks, ke, vs, ve, need;

        if (s[i] != ' ' && s[i] != '\t') {
            return 0;
        }
        while (i < n && (s[i] == ' ' || s[i] == '\t')) {
            i++;
        }
        if (i >= n) {
            break;
        }
        ks = i;
        while (i < n && is_key_char(s[i])) {
            i++;
        }
        ke = i;
        if (ke == ks || i >= n || s[i] != '=') {
            return 0;
        }
        i++;
        vs = i;
        while (i < n && is_val_char(s[i])) {
            i++;
        }
        ve = i;
        if (ve == vs) {
            return 0;
        }
        need = (ke - ks) + 3u + (ve - vs) + 1u;   /* "key = value\n" */
        if (used + need + 1u > HWBENCH_KEYS) {
            return 0;
        }
        memcpy(e->keys + used, s + ks, ke - ks);
        used += ke - ks;
        memcpy(e->keys + used, " = ", 3);
        used += 3u;
        memcpy(e->keys + used, s + vs, ve - vs);
        used += ve - vs;
        e->keys[used++] = '\n';
    }
    e->keys[used] = '\0';
    return 1;
}

int hwbench_parse(HwBenchEntry *e, uint32_t max, const char *text, size_t len, uint32_t *bad_line)
{
    size_t pos = 0;
    uint32_t line = 0, count = 0, j;

    assert(e != NULL && text != NULL);
    if (bad_line != NULL) {
        *bad_line = 0;
    }
    while (pos < len) {
        size_t end = pos, a, b;

        while (end < len && text[end] != '\n') {
            end++;
        }
        line++;
        a = pos;
        b = end;
        if (b > a && text[b - 1u] == '\r') {
            b--;
        }
        while (a < b && (text[a] == ' ' || text[a] == '\t')) {
            a++;
        }
        while (b > a && (text[b - 1u] == ' ' || text[b - 1u] == '\t')) {
            b--;
        }
        pos = end + 1u;
        if (a == b || text[a] == '#') {
            continue;
        }
        if (count >= max || !parse_line(&e[count], text + a, b - a)) {
            if (bad_line != NULL) {
                *bad_line = line;
            }
            return -1;
        }
        for (j = 0; j < count; j++) {
            if (strcmp(e[j].tag, e[count].tag) == 0) {
                if (bad_line != NULL) {
                    *bad_line = line;
                }
                return -1;
            }
        }
        count++;
    }
    return (int)count;
}

uint32_t hwbench_counter_parse(const char *text, size_t len)
{
    uint64_t v = 0;
    size_t i = 0;

    assert(text != NULL);
    while (i < len && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    if (i >= len || text[i] < '0' || text[i] > '9') {
        return 0u;
    }
    for (; i < len && text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10u + (uint64_t)(text[i] - '0');
        if (v > 1000000u) {
            return 0u;
        }
    }
    return (uint32_t)v;
}

size_t hwbench_counter_format(uint32_t counter, char *buf, size_t cap)
{
    char tmp[12];
    size_t n = 0, i;

    assert(buf != NULL);
    if (cap < 12u) {
        return 0;
    }
    do {
        tmp[n++] = (char)('0' + counter % 10u);
        counter /= 10u;
    } while (counter != 0u && n < sizeof tmp - 1u);
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1u - i];
    }
    buf[n++] = '\n';
    buf[n] = '\0';
    return n;
}

/* ---- SHA-256 (FIPS 180-4) ---- */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void compress(HwSha256 *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    unsigned i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)p[4u * i] << 24) | ((uint32_t)p[4u * i + 1u] << 16) | ((uint32_t)p[4u * i + 2u] << 8) | (uint32_t)p[4u * i + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);

        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64u; i++) {
        t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void hwsha256_init(HwSha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

    assert(c != NULL);
    memcpy(c->h, iv, sizeof iv);
    c->fill = 0;
    c->total = 0;
}

void hwsha256_update(HwSha256 *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    assert(c != NULL && (data != NULL || len == 0u));
    c->total += len;
    while (len != 0u) {
        size_t take = 64u - c->fill;

        if (take > len) {
            take = len;
        }
        memcpy(c->block + c->fill, p, take);
        c->fill += (uint32_t)take;
        p += take;
        len -= take;
        if (c->fill == 64u) {
            compress(c, c->block);
            c->fill = 0;
        }
    }
}

void hwsha256_final(HwSha256 *c, uint8_t out[32])
{
    uint64_t bits;
    uint8_t pad[72];
    uint32_t padlen, i;

    assert(c != NULL && out != NULL);
    bits = c->total * 8u;
    padlen = (c->fill < 56u) ? 56u - c->fill : 120u - c->fill;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80u;
    for (i = 0; i < 8u; i++) {
        pad[padlen + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    hwsha256_update(c, pad, padlen + 8u);
    for (i = 0; i < 8u; i++) {
        out[4u * i] = (uint8_t)(c->h[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(c->h[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(c->h[i] >> 8);
        out[4u * i + 3u] = (uint8_t)c->h[i];
    }
}

void hwsha256_hex(const uint8_t digest[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    unsigned i;

    assert(digest != NULL && out != NULL);
    for (i = 0; i < 32u; i++) {
        out[2u * i] = hx[digest[i] >> 4];
        out[2u * i + 1u] = hx[digest[i] & 15u];
    }
    out[64] = '\0';
}

int hwsha256_matches(const uint8_t digest[32], const char *text, size_t len)
{
    char hex[65];
    unsigned i;

    assert(digest != NULL && text != NULL);
    if (len < 64u) {
        return 0;
    }
    hwsha256_hex(digest, hex);
    for (i = 0; i < 64u; i++) {
        char c = text[i];

        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != hex[i]) {
            return 0;
        }
    }
    return len == 64u || text[64] == ' ' || text[64] == '\t' || text[64] == '\n' || text[64] == '\r' || text[64] == '*';
}
