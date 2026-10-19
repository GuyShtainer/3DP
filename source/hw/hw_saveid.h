/* hw_saveid.h -- the console identity a Platinum save carries (bet 2.6a): the MAC and the RTC offset the game compared with the console on Continue, plus the
 * trainer's TID/SID that key the config.ini section. Read from the NEWEST VALID normal block of the 512 KB backup image. Pure C99 (no SDK, no libctru):
 * host-tested against a synthetic save (tools/host-tests/hw_saveid_test.c) and, in the private tree, against an independent Python reader on real saves.
 *
 * Layout: two 256 KB slots at 0x00000 and 0x40000; each starts with the normal block (0xCF2C bytes,
 * footer in its last 0x14 bytes: saveCounter, blockCounter, size, signature 0x20060623, block id u8, CRC16-CCITT u16 over the body); SystemData is the first
 * page of the block: +0x00 s64 rtcOffset, +0x08 u8[6] macAddress; the trainer's id (TID low half, SID high half) is the u32 at block +0x78. */
#ifndef HW_SAVEID_H
#define HW_SAVEID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWSAVEID_IMAGE_BYTES   0x80000u   /* the whole backup chip */

typedef struct HwSaveId {
    uint32_t tid, sid;
    uint8_t mac[6];
    int64_t rtc_offset;
    uint8_t slot;                 /* 1 or 2: the slot the identity came from */
    uint32_t save_counter, block_counter;
} HwSaveId;

/* 1 and *out filled when the image is exactly 512 KB and at least one slot's normal block is valid (signature, size, block id, CRC16); the newest valid slot wins
   by the game's rule (higher saveCounter, then higher blockCounter, a tie to slot 1). 0 otherwise (*out untouched): no valid block means no identity, never a guess. */
int hwsaveid_read(const uint8_t *img, size_t len, HwSaveId *out);

#ifdef __cplusplus
}
#endif

#endif
