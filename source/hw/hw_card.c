/* hw_card.c -- the DS card bus: normal-mode ROM commands over a block cache, and the backup flash. Pure C (golden rules: no goto, bounded
 * loops, no allocation, two asserts per function). Design: the private design notes. */
#include <assert.h>
#include <string.h>

#include "hw_card.h"

#define CMD_HEADER  0x00u
#define CMD_ID_NORM 0xb8u
#define CMD_PAGE    0xb7u
#define MAX_WORDS   0x1000u /* 16 KB: the largest transfer the control word's size field describes (0x100 << 6 bytes = 0x1000 words) */
#define LOW_ROM     0x8000u /* a real cart redirects 0xB7 below this offset (secure area); we return the dump bytes and count it */

static uint32_t be32(const uint8_t *p)
{
    assert(p != NULL);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    assert(p != NULL);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void hwcard_init(HwCard *c, const HwCardOps *ops, const uint8_t *io)
{
    assert(c != NULL);
    assert(ops != NULL && io != NULL);
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    c->io = io;
}

void hwcard_reset(HwCard *c)
{
    assert(c != NULL);
    assert(c->io != NULL);
    c->ctrl = 0u;
    c->words = 0u;
    c->pos = 0u;
    c->cmd_lo = 0u;
    c->cmd_off = 0u;
}

/* True when the ROM file really ends at off (one probe byte reads as 0), so a short block is EOF and not a transient failure. */
static int at_eof(HwCard *c, uint32_t off)
{
    uint8_t probe;

    assert(c != NULL);
    assert(c->ops.pread != NULL);
    return c->ops.pread(c->ops.user, off, &probe, 1u) == 0;
}

/* The cache block `blk` (HWCARD_BLOCK bytes of the ROM file; bytes past the end of the file read 0xFF). A failed read, or a short one that
 * is not the end of the file, is served as 0xFF (what the SDK would see from a dead card) but is NOT cached: the slot stays invalid and the
 * next access retries the SD read. */
static const uint8_t *rom_block(HwCard *c, uint32_t blk)
{
    uint32_t slot = blk % HWCARD_SLOTS;

    assert(slot < HWCARD_SLOTS);
    assert(c != NULL);
    if (!c->slot_valid[slot] || c->slot_tag[slot] != blk) {
        uint32_t base = blk * HWCARD_BLOCK;
        int n = c->ops.pread != NULL ? c->ops.pread(c->ops.user, base, c->slot_data[slot], HWCARD_BLOCK) : -1;
        uint32_t got = n < 0 ? 0u : (uint32_t)n;
        int good = n >= 0 && (got >= HWCARD_BLOCK || at_eof(c, base + got));

        c->cache_misses++;
        if (got > HWCARD_BLOCK) {
            got = HWCARD_BLOCK;
        }
        if (got < HWCARD_BLOCK) {
            memset(&c->slot_data[slot][got], 0xff, HWCARD_BLOCK - got);
        }
        if (!good) {
            c->read_errors++;
        }
        c->slot_tag[slot] = blk;
        c->slot_valid[slot] = good ? 1u : 0u;
    } else {
        c->cache_hits++;
    }
    return c->slot_data[slot];
}

/* The little-endian word at the 4-aligned ROM offset off (a word never straddles two blocks). */
static uint32_t rom_word(HwCard *c, uint32_t off)
{
    assert(c != NULL);
    assert((off & 3u) == 0u);
    return le32(rom_block(c, off / HWCARD_BLOCK) + off % HWCARD_BLOCK);
}

void hwcard_ctrl_write(HwCard *c, uint32_t ctrl)
{
    unsigned szf;
    const uint8_t *cmd;

    assert(c != NULL);
    assert(c->io != NULL);
    c->ctrl = ctrl;
    if ((ctrl & HWCARD_START) == 0u) {
        return;
    }
    cmd = c->io + HWR_CARD_CMD;
    c->cmds++;
    c->pos = 0u;
    szf = (ctrl >> 24) & 7u;
    c->words = (szf == 0u) ? 0u : (szf == 7u) ? 1u : ((0x100u << szf) / 4u);
    c->cmd_lo = cmd[0];
    c->cmd_off = 0u;
    switch (cmd[0]) {
    case CMD_PAGE:
        c->page_cmds++;
        c->cmd_off = be32(cmd + 1) & ~3u;
        if (c->cmd_off < LOW_ROM) {
            c->low_reads++;
        }
        break;
    case CMD_ID_NORM:
        c->id_cmds++;
        break;
    case CMD_HEADER:
        c->header_cmds++;
        break;
    default:
        /* KEY1 / KEY2 class (0x3c, 0x4x, 0x1x, 0xax, 0xdx ...): not in the SDK's normal-mode path. */
        c->key_cmds++;
        c->cmd_lo = 0xffu;
        break;
    }
}

uint32_t hwcard_ctrl_read(HwCard *c)
{
    assert(c != NULL);
    assert(c->pos <= c->words || c->words == 0u);
    if (c->pos < c->words) {
        return c->ctrl | HWCARD_DATA_READY | HWCARD_START;
    }
    return c->ctrl & ~(HWCARD_START | HWCARD_DATA_READY);
}

uint32_t hwcard_data_read(HwCard *c)
{
    uint32_t w;

    assert(c != NULL);
    assert(c->words <= MAX_WORDS);
    if (c->pos >= c->words) {
        c->over_reads++;
        return 0xffffffffu;
    }
    switch (c->cmd_lo) {
    case CMD_PAGE:
        w = rom_word(c, c->cmd_off + c->pos * 4u);
        break;
    case CMD_HEADER:
        w = rom_word(c, (c->pos * 4u) % 0x1000u);   /* the header repeats every 0x1000 bytes */
        break;
    case CMD_ID_NORM:
        w = HWCARD_CHIP_ID;
        break;
    default:
        w = 0xffffffffu;
        break;
    }
    c->pos++;
    return w;
}

/* ---- flash ----------------------------------------------------------------------------------------------------------------- */
void hwcard_flash_attach(HwCard *c, uint8_t *image, uint32_t size, uint32_t sector_size)
{
    assert(c != NULL);
    assert(image != NULL && size != 0u && sector_size != 0u);
    c->flash = image;
    c->flash_size = size;
    c->flash_sector = sector_size;
}

static int range_ok(const HwCard *c, uint32_t off, uint32_t len)
{
    assert(c != NULL);
    return off <= c->flash_size && len <= c->flash_size - off;
}

static int32_t flash_read(HwCard *c, const HwCardCmd *cmd, void *(*mem)(void *u, uint32_t addr, uint32_t len), void *mem_user)
{
    uint8_t *m;

    assert(c != NULL && cmd != NULL);
    assert(mem != NULL);
    m = (uint8_t *)mem(mem_user, cmd->dst, cmd->len);
    if (m == NULL || !range_ok(c, cmd->src, cmd->len)) {
        return HWCARD_RESULT_INVALID_PARAM;
    }
    memcpy(m, c->flash + cmd->src, cmd->len);
    c->flash_reads++;
    return HWCARD_RESULT_SUCCESS;
}

/* WRITE (erase page + program: the bytes become the data), PROGRAM (clears bits only) and VERIFY. */
static int32_t flash_write(HwCard *c, unsigned req, const HwCardCmd *cmd, void *(*mem)(void *u, uint32_t addr, uint32_t len), void *mem_user)
{
    const uint8_t *m;
    uint32_t i;

    assert(c != NULL && cmd != NULL);
    assert(mem != NULL);
    m = (const uint8_t *)mem(mem_user, cmd->src, cmd->len);
    if (m == NULL || !range_ok(c, cmd->dst, cmd->len)) {
        return HWCARD_RESULT_INVALID_PARAM;
    }
    if (req == HWCARD_REQ_VERIFY_BACKUP) {
        if (memcmp(c->flash + cmd->dst, m, cmd->len) != 0) {
            c->flash_verify_fail++;
            return HWCARD_RESULT_FAILURE;
        }
    } else if (req == HWCARD_REQ_WRITE_BACKUP) {
        memcpy(c->flash + cmd->dst, m, cmd->len);
        c->flash_writes++;
    } else {
        for (i = 0; i < cmd->len; i++) {
            c->flash[cmd->dst + i] &= m[i];
        }
        c->flash_writes++;
    }
    return HWCARD_RESULT_SUCCESS;
}

static int32_t flash_erase(HwCard *c, unsigned req, const HwCardCmd *cmd)
{
    uint32_t unit;
    uint32_t base;

    assert(c != NULL && cmd != NULL);
    if (req == HWCARD_REQ_ERASE_CHIP) {
        memset(c->flash, 0xff, c->flash_size);
        c->flash_erases++;
        return HWCARD_RESULT_SUCCESS;
    }
    unit = (req == HWCARD_REQ_ERASE_PAGE) ? 256u : (req == HWCARD_REQ_ERASE_SECTOR) ? c->flash_sector : 4096u;
    base = cmd->dst - (cmd->dst % unit);
    if (!range_ok(c, base, unit)) {
        return HWCARD_RESULT_INVALID_PARAM;
    }
    memset(c->flash + base, 0xff, unit);
    c->flash_erases++;
    return HWCARD_RESULT_SUCCESS;
}

int32_t hwcard_flash_request(HwCard *c, unsigned req, HwCardCmd *cmd, void *(*mem)(void *u, uint32_t addr, uint32_t len), void *mem_user)
{
    uint8_t *m;

    assert(c != NULL && cmd != NULL);
    assert(mem != NULL);
    if (c->flash == NULL) {
        return HWCARD_RESULT_UNSUPPORTED;
    }
    switch (req) {
    case HWCARD_REQ_IDENTIFY:
    case HWCARD_REQ_WRITE_STATUS:
        return HWCARD_RESULT_SUCCESS;
    case HWCARD_REQ_READ_STATUS:
        m = (uint8_t *)mem(mem_user, cmd->dst, 1u);
        if (m == NULL) {
            return HWCARD_RESULT_INVALID_PARAM;
        }
        *m = 0u;
        return HWCARD_RESULT_SUCCESS;
    case HWCARD_REQ_READ_BACKUP:
        return flash_read(c, cmd, mem, mem_user);
    case HWCARD_REQ_WRITE_BACKUP:
    case HWCARD_REQ_PROGRAM_BACKUP:
    case HWCARD_REQ_VERIFY_BACKUP:
        return flash_write(c, req, cmd, mem, mem_user);
    case HWCARD_REQ_ERASE_PAGE:
    case HWCARD_REQ_ERASE_SECTOR:
    case HWCARD_REQ_ERASE_SUBSECTOR:
    case HWCARD_REQ_ERASE_CHIP:
        return flash_erase(c, req, cmd);
    default:
        return HWCARD_RESULT_UNSUPPORTED;
    }
}
