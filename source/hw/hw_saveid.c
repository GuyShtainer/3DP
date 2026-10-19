/* hw_saveid.c -- the console identity in a Platinum save image (bet 2.6a). Pure C99, no allocation, every loop bounded. */
#include <assert.h>
#include <string.h>

#include "hw_saveid.h"

#define SLOT_BYTES    0x40000u
#define NORMAL_BYTES  0xCF2Cu
#define FOOTER_BYTES  0x14u
#define SIGNATURE     0x20060623u
#define OFS_RTC       0x00u
#define OFS_MAC       0x08u
#define OFS_TRAINER   0x78u

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t crc16_ccitt(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFu;
    size_t i;
    int b;

    assert(p != NULL);
    for (i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 8;
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
        }
        crc &= 0xFFFFu;
    }
    return (uint16_t)crc;
}

/* one slot's normal block: 1 when valid, with its counters */
static int block_valid(const uint8_t *blk, uint32_t *save_counter, uint32_t *block_counter)
{
    const uint8_t *f = blk + NORMAL_BYTES - FOOTER_BYTES;
    uint16_t stored = (uint16_t)((uint16_t)f[0x12] | ((uint16_t)f[0x13] << 8));

    assert(blk != NULL && save_counter != NULL && block_counter != NULL);
    if (le32(f + 0x0C) != SIGNATURE || le32(f + 0x08) != NORMAL_BYTES || f[0x10] != 0u) {
        return 0;
    }
    if (crc16_ccitt(blk, NORMAL_BYTES - FOOTER_BYTES) != stored) {
        return 0;
    }
    *save_counter = le32(f);
    *block_counter = le32(f + 4);
    return 1;
}

int hwsaveid_read(const uint8_t *img, size_t len, HwSaveId *out)
{
    uint32_t sc[2] = { 0, 0 }, bc[2] = { 0, 0 };
    int ok[2], best = -1;
    uint32_t i;
    const uint8_t *b;
    uint64_t rtc = 0;

    assert(out != NULL);
    if (img == NULL || len != HWSAVEID_IMAGE_BYTES) {
        return 0;
    }
    for (i = 0; i < 2u; i++) {
        ok[i] = block_valid(img + i * SLOT_BYTES, &sc[i], &bc[i]);
    }
    if (ok[0]) {
        best = 0;
    }
    if (ok[1] && (best < 0 || sc[1] > sc[0] || (sc[1] == sc[0] && bc[1] > bc[0]))) {
        best = 1;
    }
    if (best < 0) {
        return 0;
    }
    b = img + (uint32_t)best * SLOT_BYTES;
    for (i = 0; i < 8u; i++) {
        rtc |= (uint64_t)b[OFS_RTC + i] << (8u * i);
    }
    memset(out, 0, sizeof *out);
    out->rtc_offset = (int64_t)rtc;
    memcpy(out->mac, b + OFS_MAC, 6);
    out->tid = (uint32_t)b[OFS_TRAINER] | ((uint32_t)b[OFS_TRAINER + 1u] << 8);
    out->sid = (uint32_t)b[OFS_TRAINER + 2u] | ((uint32_t)b[OFS_TRAINER + 3u] << 8);
    out->slot = (uint8_t)(best + 1);
    out->save_counter = sc[best];
    out->block_counter = bc[best];
    return 1;
}
