/* hw_dgt_abi.c -- the DGT_Hash* entry points of the binary-only libdgt (9 symbols) over hw_dgt.c. SDK-world file. The SDK contexts
 * (DGTHash1Context 88 bytes, DGTHash2Context >= 92) hold the plain-C states directly: nothing else reads their fields. */
#include <assert.h>

#include <nitro.h>

#include "hw_dgt.h"
#include "hw_log.h"

typedef char dgt1_fits[sizeof(HwMd5) <= sizeof(DGTHash1Context) ? 1 : -1];
typedef char dgt2_fits[sizeof(HwSha1) <= sizeof(DGTHash2Context) ? 1 : -1];

void DGT_Hash1Reset(DGTHash1Context *ctx)
{
    assert(ctx != NULL);
    hwdgt_md5_init((HwMd5 *)ctx);
}

void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *data, unsigned long len)
{
    assert(ctx != NULL);
    hwdgt_md5_update((HwMd5 *)ctx, data, len);
}

void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx)
{
    assert(ctx != NULL && digest != NULL);
    hwdgt_md5_final((HwMd5 *)ctx, (uint8_t *)digest);
}

void DGT_Hash2Reset(DGTHash2Context *ctx)
{
    assert(ctx != NULL);
    hwdgt_sha1_init((HwSha1 *)ctx);
}

void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *data, unsigned long len)
{
    assert(ctx != NULL);
    hwdgt_sha1_update((HwSha1 *)ctx, data, len);
}

void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest)
{
    assert(ctx != NULL && digest != NULL);
    hwdgt_sha1_final((HwSha1 *)ctx, digest);
}

void DGT_Hash1CalcHmac(void *digest, void *bin_ptr, int bin_len, void *key_ptr, int key_len)
{
    assert(digest != NULL);
    assert(bin_len >= 0 && key_len >= 0);
    hwdgt_hmac_md5((uint8_t *)digest, bin_ptr, (size_t)bin_len, key_ptr, (size_t)key_len);
}

void DGT_Hash2CalcHmac(void *digest, void *bin_ptr, int bin_len, void *key_ptr, int key_len)
{
    assert(digest != NULL);
    assert(bin_len >= 0 && key_len >= 0);
    hwdgt_hmac_sha1((uint8_t *)digest, bin_ptr, (size_t)bin_len, key_ptr, (size_t)key_len);
}

int DGT_SetOverlayTableMode(int flag)
{
    HWLOG("DGT_SetOverlayTableMode(%d): overlay digests are not checked on the 3DS", flag);
    return 0;
}
