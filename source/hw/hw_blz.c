/* hw_blz.c -- the "BLZ" backward LZ decoder of the DS overlay loader (MIi_UncompressBackward, init/crt0.c) as pure C, so a host test
 * checks it against an independent encoder. The asm decoded an overlay in place, from its end towards its start:
 *   footer = 8 bytes just below `bottom`: word 0 = (footer length << 24) | compressed length (24 bits), word 1 = extra size.
 *   input is read downward from bottom - footer length; output is written downward from bottom + extra size; the decoded image starts at
 *   bottom - compressed length. Each group is one flag byte (bit 7 first) and up to 8 items: flag 0 = one literal byte, flag 1 = a
 *   2-byte block (first byte read = high: count nibble + index bits 11-8; second = index bits 7-0): copy (high >> 4) + 3 bytes, each from
 *   outp[index + 2] at the moment of the write (distance index + 3 bytes above the byte being written), the output pointer falling by one.
 * The asm trusted the stream; this version stops (returns 0) when a write would go below the image start or a block reads outside the
 * image, so a corrupt overlay cannot scribble. The cache flush at the end of the asm has no meaning here. */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Returns the number of bytes of the decoded image (compressed length + extra size), or 0 for a NULL / corrupt stream. */
uint32_t hwblz_uncompress(uint8_t *bottom)
{
    uint32_t info, extra, comp, flen;
    uint8_t *inp, *top, *outp, *end;

    if (bottom == NULL) {
        return 0u;
    }
    info = rd32(bottom - 8);
    extra = rd32(bottom - 4);
    comp = info & 0x00ffffffu;
    flen = info >> 24;
    if (flen < 8u || flen > comp) {
        return 0u;
    }
    inp = bottom - flen;
    top = bottom - comp;
    end = bottom + extra;
    outp = end;
    while (inp > top) {
        uint32_t flag = *--inp;
        uint32_t k;

        for (k = 0; k < 8u && inp > top; k++, flag <<= 1) {
            if (outp <= top) {
                return 0u;
            }
            if (!(flag & 0x80u)) {
                *--outp = *--inp;
            } else {
                uint32_t hi, idx, len;

                if (inp - top < 2) {
                    return 0u;
                }
                hi = *--inp;
                idx = ((uint32_t)*--inp | (hi << 8)) & 0x0fffu;
                idx += 2u;
                len = (hi >> 4) + 3u;
                if ((uint32_t)(outp - top) < len || (uint32_t)(end - outp) <= idx) {
                    return 0u;
                }
                while (len-- > 0u) {
                    outp[-1] = outp[idx];
                    outp--;
                }
            }
        }
    }
    return (uint32_t)(end - top);
}
