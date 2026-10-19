/* hw_dgt.c -- MD5 (RFC 1321), SHA-1 (FIPS 180-4) and HMAC (RFC 2104), plain C99. See hw_dgt.h. */
#include <assert.h>
#include <string.h>

#include "hw_dgt.h"

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32u - (n))))

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* add n bytes to the 64-bit length kept as two words */
static void add_len(uint32_t *lo, uint32_t *hi, size_t n)
{
    uint32_t before = *lo;

    *lo += (uint32_t)n;
    *hi += (uint32_t)(((uint64_t)n) >> 32) + (*lo < before ? 1u : 0u);
}

/* ---- MD5 ---------------------------------------------------------------------------------------------------------------- */
static const uint32_t k_md5[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
    0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
    0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};
static const uint8_t r_md5[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                   14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(HwMd5 *c, const uint8_t *p)
{
    uint32_t w[16], a = c->st[0], b = c->st[1], cc = c->st[2], d = c->st[3];
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = get_le32(p + 4u * i);
    }
    for (i = 0; i < 64u; i++) {
        uint32_t f, g, t;

        if (i < 16u) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32u) {
            f = (d & b) | (~d & cc);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48u) {
            f = b ^ cc ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = cc ^ (b | ~d);
            g = (7u * i) & 15u;
        }
        t = d;
        d = cc;
        cc = b;
        b = b + ROL32(a + f + k_md5[i] + w[g], r_md5[i]);
        a = t;
    }
    c->st[0] += a;
    c->st[1] += b;
    c->st[2] += cc;
    c->st[3] += d;
}

void hwdgt_md5_init(HwMd5 *c)
{
    assert(c != NULL);
    assert(sizeof(HwMd5) == 88u);
    c->st[0] = 0x67452301u;
    c->st[1] = 0xefcdab89u;
    c->st[2] = 0x98badcfeu;
    c->st[3] = 0x10325476u;
    c->len_lo = 0;
    c->len_hi = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void hwdgt_md5_update(HwMd5 *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t used;

    assert(c != NULL);
    assert(data != NULL || len == 0u);
    used = c->len_lo & 63u;
    add_len(&c->len_lo, &c->len_hi, len);
    while (len > 0u) {
        size_t take = HWDGT_BLOCK - used;

        if (take > len) {
            take = len;
        }
        memcpy(c->buf + used, p, take);
        used += (uint32_t)take;
        p += take;
        len -= take;
        if (used == HWDGT_BLOCK) {
            md5_block(c, c->buf);
            used = 0;
        }
    }
}

void hwdgt_md5_final(HwMd5 *c, uint8_t out[HWDGT_MD5_SIZE])
{
    uint8_t pad[HWDGT_BLOCK + 8u];
    uint32_t used, padlen, lo, hi;
    uint32_t i;

    assert(c != NULL && out != NULL);
    lo = c->len_lo << 3;
    hi = (c->len_hi << 3) | (c->len_lo >> 29);
    used = c->len_lo & 63u;
    padlen = used < 56u ? 56u - used : 120u - used;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    put_le32(pad + padlen, lo);
    put_le32(pad + padlen + 4u, hi);
    hwdgt_md5_update(c, pad, padlen + 8u);
    for (i = 0; i < 4u; i++) {
        put_le32(out + 4u * i, c->st[i]);
    }
}

/* ---- SHA-1 -------------------------------------------------------------------------------------------------------------- */
static void sha1_block(HwSha1 *c, const uint8_t *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = get_be32(p + 4u * i);
    }
    for (i = 16; i < 80u; i++) {
        w[i] = ROL32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    }
    for (i = 0; i < 80u; i++) {
        uint32_t f, k, t;

        if (i < 20u) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40u) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60u) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6u;
        }
        t = ROL32(a, 5u) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL32(b, 30u);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void hwdgt_sha1_init(HwSha1 *c)
{
    assert(c != NULL);
    assert(sizeof(HwSha1) == 92u);
    c->h[0] = 0x67452301u;
    c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xc3d2e1f0u;
    c->len_lo = 0;
    c->len_hi = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void hwdgt_sha1_update(HwSha1 *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t used;

    assert(c != NULL);
    assert(data != NULL || len == 0u);
    used = c->len_lo & 63u;
    add_len(&c->len_lo, &c->len_hi, len);
    while (len > 0u) {
        size_t take = HWDGT_BLOCK - used;

        if (take > len) {
            take = len;
        }
        memcpy(c->buf + used, p, take);
        used += (uint32_t)take;
        p += take;
        len -= take;
        if (used == HWDGT_BLOCK) {
            sha1_block(c, c->buf);
            used = 0;
        }
    }
}

void hwdgt_sha1_final(HwSha1 *c, uint8_t out[HWDGT_SHA1_SIZE])
{
    uint8_t pad[HWDGT_BLOCK + 8u];
    uint32_t used, padlen, lo, hi;
    uint32_t i;

    assert(c != NULL && out != NULL);
    lo = c->len_lo << 3;
    hi = (c->len_hi << 3) | (c->len_lo >> 29);
    used = c->len_lo & 63u;
    padlen = used < 56u ? 56u - used : 120u - used;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    put_be32(pad + padlen, hi);
    put_be32(pad + padlen + 4u, lo);
    hwdgt_sha1_update(c, pad, padlen + 8u);
    for (i = 0; i < 5u; i++) {
        put_be32(out + 4u * i, c->h[i]);
    }
}

/* ---- HMAC --------------------------------------------------------------------------------------------------------------- */
void hwdgt_hmac_md5(uint8_t out[HWDGT_MD5_SIZE], const void *data, size_t len, const void *key, size_t key_len)
{
    uint8_t k[HWDGT_BLOCK], pad[HWDGT_BLOCK], inner[HWDGT_MD5_SIZE];
    HwMd5 c;
    uint32_t i;

    assert(out != NULL);
    assert(key != NULL || key_len == 0u);
    memset(k, 0, sizeof k);
    if (key_len > HWDGT_BLOCK) {
        hwdgt_md5_init(&c);
        hwdgt_md5_update(&c, key, key_len);
        hwdgt_md5_final(&c, k);
    } else if (key_len > 0u) {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < HWDGT_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    hwdgt_md5_init(&c);
    hwdgt_md5_update(&c, pad, sizeof pad);
    hwdgt_md5_update(&c, data, len);
    hwdgt_md5_final(&c, inner);
    for (i = 0; i < HWDGT_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    hwdgt_md5_init(&c);
    hwdgt_md5_update(&c, pad, sizeof pad);
    hwdgt_md5_update(&c, inner, sizeof inner);
    hwdgt_md5_final(&c, out);
}

void hwdgt_hmac_sha1(uint8_t out[HWDGT_SHA1_SIZE], const void *data, size_t len, const void *key, size_t key_len)
{
    uint8_t k[HWDGT_BLOCK], pad[HWDGT_BLOCK], inner[HWDGT_SHA1_SIZE];
    HwSha1 c;
    uint32_t i;

    assert(out != NULL);
    assert(key != NULL || key_len == 0u);
    memset(k, 0, sizeof k);
    if (key_len > HWDGT_BLOCK) {
        hwdgt_sha1_init(&c);
        hwdgt_sha1_update(&c, key, key_len);
        hwdgt_sha1_final(&c, k);
    } else if (key_len > 0u) {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < HWDGT_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    hwdgt_sha1_init(&c);
    hwdgt_sha1_update(&c, pad, sizeof pad);
    hwdgt_sha1_update(&c, data, len);
    hwdgt_sha1_final(&c, inner);
    for (i = 0; i < HWDGT_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    hwdgt_sha1_init(&c);
    hwdgt_sha1_update(&c, pad, sizeof pad);
    hwdgt_sha1_update(&c, inner, sizeof inner);
    hwdgt_sha1_final(&c, out);
}
