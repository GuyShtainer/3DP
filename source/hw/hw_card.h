/* hw_card.h -- the DS card bus. Pure C: no SDK, no libctru. Two units behind the patched SDK:
 *  ROM    the card registers MCCNT1 / MCCMD / MCD1 as the SDK's own card_rom.c drives them (command 0xB7 read page, 0xB8 read id,
 *         0x00 read header), served from a ROM file through a block cache. Only normal-mode commands exist; a KEY1 / KEY2 command is
 *         counted and answered with 0xFFFFFFFF (the SDK never issues one: the dump is decrypted).
 *  FLASH  the loopback ARM7's backup chip: a 512 KB flash in a RAM image (the .sav), driven by the CARD request protocol of card_request.c. */
#ifndef HW_CARD_H
#define HW_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWCARD_CHIP_ID      0x00001FC2u   /* our choice (any value works): HW_RED_RESERVED and the 0xB8 command agree on it */
#define HWCARD_BLOCK        4096u
#define HWCARD_SLOTS        16u           /* direct-mapped, 64 KB */
#define HWCARD_HEADER_BYTES 0x200u

#define HWCARD_START        0x80000000u
#define HWCARD_DATA_READY   0x00800000u

#define HWR_CARD_CMD        0x1a8u        /* offset of the 8 command bytes in the I/O block */

typedef struct HwCardOps {
    /* read up to len bytes of the ROM file at off: the byte count (< len at end of file), or -1 */
    int (*pread)(void *u, uint32_t off, void *dst, uint32_t len);
    void *user;
} HwCardOps;

typedef struct HwCard {
    HwCardOps ops;
    const uint8_t *io;
    uint32_t ctrl;
    uint32_t cmd_lo, cmd_off;
    uint32_t words, pos;
    uint8_t slot_data[HWCARD_SLOTS][HWCARD_BLOCK];
    uint32_t slot_tag[HWCARD_SLOTS];
    uint8_t slot_valid[HWCARD_SLOTS];
    uint32_t cmds, page_cmds, id_cmds, header_cmds, key_cmds, over_reads;
    uint32_t cache_hits, cache_misses, read_errors, low_reads;
    uint8_t *flash;
    uint32_t flash_size, flash_sector;
    uint32_t flash_reads, flash_writes, flash_erases, flash_verify_fail;
} HwCard;

void hwcard_init(HwCard *c, const HwCardOps *ops, const uint8_t *io);
/* Forget the in-flight transfer. The block cache and the flash image survive (the ROM and the chip do). */
void hwcard_reset(HwCard *c);

void hwcard_ctrl_write(HwCard *c, uint32_t ctrl);
uint32_t hwcard_ctrl_read(HwCard *c);
uint32_t hwcard_data_read(HwCard *c);

/* ---- backup flash (card_request.c protocol) ---- */
#define HWCARD_REQ_IDENTIFY       2u
#define HWCARD_REQ_READ_BACKUP    6u
#define HWCARD_REQ_WRITE_BACKUP   7u
#define HWCARD_REQ_PROGRAM_BACKUP 8u
#define HWCARD_REQ_VERIFY_BACKUP  9u
#define HWCARD_REQ_ERASE_PAGE     10u
#define HWCARD_REQ_ERASE_SECTOR   11u
#define HWCARD_REQ_ERASE_CHIP     12u
#define HWCARD_REQ_READ_STATUS    13u
#define HWCARD_REQ_WRITE_STATUS   14u
#define HWCARD_REQ_ERASE_SUBSECTOR 15u

#define HWCARD_RESULT_SUCCESS       0
#define HWCARD_RESULT_FAILURE       1
#define HWCARD_RESULT_INVALID_PARAM 2
#define HWCARD_RESULT_UNSUPPORTED   3

/* First words of the SDK's CARDiCommandArg (hw_devs.c asserts the layout). */
typedef struct HwCardCmd {
    int32_t result;
    uint32_t type;
    uint32_t id;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    uint32_t total_size;
    uint32_t sect_size;
} HwCardCmd;

void hwcard_flash_attach(HwCard *c, uint8_t *image, uint32_t size, uint32_t sector_size);
/* `mem` resolves a DS address range to host memory (NULL if unmapped). Returns a HWCARD_RESULT_*. */
int32_t hwcard_flash_request(HwCard *c, unsigned req, HwCardCmd *cmd, void *(*mem)(void *u, uint32_t addr, uint32_t len), void *mem_user);

#ifdef __cplusplus
}
#endif

#endif
