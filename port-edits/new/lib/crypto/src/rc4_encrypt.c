#include <nitro/types.h>

#include "crypto/rc4.h"

/*
 * C replacements for lib/crypto/asm/rc4.s and rc4_fast.s (hand-written ARM in the original). Standard RC4
 * PRGA. The SDK's context formats are kept: CRYPTORC4Context holds byte state, CRYPTORC4FastContext holds
 * each state byte and the i/j indices in the top byte of a word (value << 24), so a context initialized
 * by CRYPTO_RC4FastInit can be passed straight in and the stored i/j stay in that form.
 */

void CRYPTO_RC4Encrypt(CRYPTORC4Context *ctx, const void *in, u32 len, void *out)
{
    const u8 *src = (const u8 *)in;
    u8 *dst = (u8 *)out;
    u8 *s = ctx->s;
    u8 i = ctx->i;
    u8 j = ctx->j;

    while (len-- > 0) {
        u8 si, sj;

        i++;
        si = s[i];
        j = (u8)(j + si);
        sj = s[j];
        s[i] = sj;
        s[j] = si;
        *dst++ = (u8)(*src++ ^ s[(u8)(si + sj)]);
    }

    ctx->i = i;
    ctx->j = j;
}

void CRYPTO_RC4FastEncrypt(CRYPTORC4FastContext *ctx, const void *in, u32 len, void *out)
{
    const u8 *src = (const u8 *)in;
    u8 *dst = (u8 *)out;
    u32 *s = ctx->s;
    u32 i = ctx->i >> 24;
    u32 j = ctx->j >> 24;

    while (len-- > 0) {
        u32 si, sj;

        i = (i + 1) & 0xff;
        si = s[i];
        j = (j + (si >> 24)) & 0xff;
        sj = s[j];
        s[i] = sj;
        s[j] = si;
        *dst++ = (u8)(*src++ ^ (s[((si >> 24) + (sj >> 24)) & 0xff] >> 24));
    }

    ctx->i = i << 24;
    ctx->j = j << 24;
}
