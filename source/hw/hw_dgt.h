/* hw_dgt.h -- MD5, SHA-1 and HMAC in plain C99 (replacement for the binary-only libdgt of the NitroSDK). Pure: <stdint.h> only, so the
 * host test checks it against the RFC 1321 / FIPS 180 / RFC 2202 vectors. hw_dgt_abi.c maps the DGT_Hash* entry points onto it. */
#ifndef HW_DGT_H
#define HW_DGT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWDGT_MD5_SIZE   16u
#define HWDGT_SHA1_SIZE  20u
#define HWDGT_BLOCK      64u

/* 32-bit fields only (no uint64_t): the SDK contexts these live in are 4-byte aligned */
typedef struct HwMd5 {
    uint32_t st[4];
    uint32_t len_lo, len_hi;     /* message length in bytes */
    uint8_t buf[HWDGT_BLOCK];
} HwMd5;                         /* 88 bytes = sizeof(DGTHash1Context) */

typedef struct HwSha1 {
    uint32_t h[5];
    uint32_t len_lo, len_hi;
    uint8_t buf[HWDGT_BLOCK];
} HwSha1;                        /* 92 bytes <= sizeof(DGTHash2Context) */

void hwdgt_md5_init(HwMd5 *c);
void hwdgt_md5_update(HwMd5 *c, const void *data, size_t len);
void hwdgt_md5_final(HwMd5 *c, uint8_t out[HWDGT_MD5_SIZE]);

void hwdgt_sha1_init(HwSha1 *c);
void hwdgt_sha1_update(HwSha1 *c, const void *data, size_t len);
void hwdgt_sha1_final(HwSha1 *c, uint8_t out[HWDGT_SHA1_SIZE]);

/* HMAC (RFC 2104). A key longer than one block is hashed first. */
void hwdgt_hmac_md5(uint8_t out[HWDGT_MD5_SIZE], const void *data, size_t len, const void *key, size_t key_len);
void hwdgt_hmac_sha1(uint8_t out[HWDGT_SHA1_SIZE], const void *data, size_t len, const void *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif
