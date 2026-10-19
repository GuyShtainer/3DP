/* hw_card_test.c -- bet 1.4 gate A, host half (review 1.4): the card bus of source/hw/hw_card.c alone. Native, one thread, no SDK, no libctru.
 * The SDK half (the real fs + card sources over this unit) is the private emulator gate `emu-card-fs` (on the ARM11 under Azahar).
 *
 *   hw_card_test                      in-memory ROM fixture (3 x 64 KB + 100 bytes: EOF falls mid-block, byte i = hash(i)) with fault injection
 *   hw_card_test --rom ROM REFDIR     also: hw_card over the real ROM file must match the pages.bin of a reference script in the private tree byte for byte
 *                                     (the offsets come from an independent FAT reader)
 *
 * Cases a-h of the review spec: command encoding, MCCNT1 protocol, ID/header, unknown commands, the block cache, read errors that are NOT cached,
 * the low-read tripwire, and the backup flash. Prints "hw_card_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_card.h"

#define FIX_SIZE   (3u * 65536u + 100u)
#define CTRL_SZ(n) ((uint32_t)(n) << 24)
#define MROMOP_G_READ_PAGE 0xb7000000u
#define LATENCY1_MASK      0x00001fffu
#define CARD_COMMAND_ID    0x07000000u

static long g_checks, g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

/* ---- the fixture ROM behind ops.pread ----------------------------------------------------------------------------------------- */
static uint8_t g_rom[FIX_SIZE];

typedef enum { INJ_NONE, INJ_FAIL, INJ_SHORT } Inject;
static Inject g_inject;
static uint32_t g_preads;

static uint8_t fix_byte(uint32_t i)
{
    return i < FIX_SIZE ? g_rom[i] : 0xffu;
}

static uint32_t fix_word(uint32_t off)
{
    assert((off & 3u) == 0u);
    return (uint32_t)fix_byte(off) | ((uint32_t)fix_byte(off + 1u) << 8) | ((uint32_t)fix_byte(off + 2u) << 16) | ((uint32_t)fix_byte(off + 3u) << 24);
}

static int fix_pread(void *u, uint32_t off, void *dst, uint32_t len)
{
    uint32_t n;

    (void)u;
    assert(dst != NULL);
    g_preads++;
    if (g_inject == INJ_FAIL && len > 1u) {
        g_inject = INJ_NONE;
        return -1;
    }
    if (off >= FIX_SIZE) {
        return 0;
    }
    n = FIX_SIZE - off < len ? FIX_SIZE - off : len;
    if (g_inject == INJ_SHORT && len > 1u && n > 2u) {
        g_inject = INJ_NONE;
        n /= 2u;   /* a short count that is not the end of the file */
    }
    memcpy(dst, g_rom + off, n);
    return (int)n;
}

static void fixture_init(void)
{
    uint32_t i;

    for (i = 0; i < FIX_SIZE; i++) {
        g_rom[i] = (uint8_t)(((i * 2654435761u) >> 13) ^ i);
    }
}

/* ---- driving the bus the way the SDK does ------------------------------------------------------------------------------------- */
static uint8_t g_io[0x400];   /* the register file: the 8 command bytes sit at HWR_CARD_CMD */
static HwCard g_card;

static void be_put(uint8_t *p, uint32_t v)
{
    assert(p != NULL);
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* CARDi_SetRomOp(MROMOP_G_READ_PAGE | (src >> 8), src << 24), card_rom.c:49-70 */
static void set_page_cmd(uint32_t src)
{
    be_put(g_io + HWR_CARD_CMD, MROMOP_G_READ_PAGE | (src >> 8));
    be_put(g_io + HWR_CARD_CMD + 4, src << 24);
}

static void set_cmd_byte(uint8_t op)
{
    memset(g_io + HWR_CARD_CMD, 0, 8);
    g_io[HWR_CARD_CMD] = op;
}

static void bus_init(const HwCardOps *ops)
{
    assert(ops != NULL);
    memset(g_io, 0, sizeof g_io);
    hwcard_init(&g_card, ops, g_io);
}

static uint32_t drain(unsigned limit)
{
    uint32_t n = 0;

    assert(limit <= 0x2000u);
    assert(limit > 0u);
    while ((hwcard_ctrl_read(&g_card) & HWCARD_START) != 0u && n < limit) {
        (void)hwcard_data_read(&g_card);
        n++;
    }
    return n;
}

/* a. address encoding */
static int cmd_decodes_to(uint32_t src)
{
    const uint8_t *c = g_io + HWR_CARD_CMD;
    uint32_t v = ((uint32_t)c[1] << 24) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 8) | c[4];

    return c[0] == 0xb7u && v == src && c[5] == 0u && c[6] == 0u && c[7] == 0u;
}

static void test_encoding(void)
{
    static const uint32_t srcs[] = { 0x8000u, 0x1fe00u, 0x00fffe00u, 0x07fffe00u };
    unsigned k, w;
    int all;

    for (k = 0; k < sizeof srcs / sizeof srcs[0]; k++) {
        HwCardOps ops = { fix_pread, NULL };

        bus_init(&ops);
        set_page_cmd(srcs[k]);
        check(cmd_decodes_to(srcs[k]), "page command bytes = 0xB7 + big-endian src", (long)srcs[k]);
        hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(1));
        all = 1;
        for (w = 0; w < 128u; w++) {
            if (hwcard_data_read(&g_card) != fix_word(srcs[k] + w * 4u)) {
                all = 0;
            }
        }
        check(all, "128 words equal the fixture at src", (long)srcs[k]);
        check(g_card.page_cmds == 1u && g_card.low_reads == 0u && g_card.over_reads == 0u, "one page command, not a low read", (long)g_card.low_reads);
    }
}

/* b. protocol */
static void test_protocol(void)
{
    static const uint32_t want[8] = { 0u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 1u };
    HwCardOps ops = { fix_pread, NULL };
    unsigned szf, w;
    uint32_t st, n;

    bus_init(&ops);
    set_page_cmd(0x8000u);
    hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(1));
    st = hwcard_ctrl_read(&g_card);
    check((st & HWCARD_START) != 0u && (st & HWCARD_DATA_READY) != 0u, "START|READY right after the command", (long)st);
    for (w = 0; w < 127u; w++) {
        (void)hwcard_data_read(&g_card);
    }
    st = hwcard_ctrl_read(&g_card);
    check((st & HWCARD_START) != 0u && (st & HWCARD_DATA_READY) != 0u, "START|READY still up for the 128th word", (long)st);
    (void)hwcard_data_read(&g_card);
    st = hwcard_ctrl_read(&g_card);
    check((st & (HWCARD_START | HWCARD_DATA_READY)) == 0u, "neither bit after the 128th word", (long)st);
    check(g_card.over_reads == 0u, "no over-read yet", 0);
    check(hwcard_data_read(&g_card) == 0xffffffffu && g_card.over_reads == 1u, "the 129th read gives 0xFFFFFFFF and counts one over-read",
          (long)g_card.over_reads);
    for (szf = 0; szf < 8u; szf++) {
        set_page_cmd(0x8000u);
        hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(szf));
        n = drain(0x2000u);
        check(n == want[szf], "size field -> word count", (long)szf * 10000L + (long)n);
    }
    {
        uint32_t cmds = g_card.cmds, words = g_card.words;

        set_page_cmd(0x9000u);
        hwcard_ctrl_write(&g_card, CTRL_SZ(1));   /* no START: nothing latches */
        check(g_card.cmds == cmds && g_card.words == words && (hwcard_ctrl_read(&g_card) & HWCARD_START) == 0u, "a write without START latches nothing",
              (long)g_card.cmds);
    }
}

/* c. ID and header */
static void test_id_header(void)
{
    HwCardOps ops = { fix_pread, NULL };
    uint32_t id_ctrl = HWCARD_START | (CARD_COMMAND_ID & ~LATENCY1_MASK);
    uint32_t pos;
    int ok = 1;

    bus_init(&ops);
    set_cmd_byte(0xb8u);
    hwcard_ctrl_write(&g_card, id_ctrl);
    check(hwcard_data_read(&g_card) == HWCARD_CHIP_ID, "0xB8 gives HWCARD_CHIP_ID", 0);
    check((hwcard_ctrl_read(&g_card) & HWCARD_START) == 0u && g_card.id_cmds == 1u, "the ID transfer is one word", 0);
    set_cmd_byte(0x00u);
    hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(6));
    for (pos = 0; pos < 4096u; pos++) {
        if (hwcard_data_read(&g_card) != fix_word((pos * 4u) % 0x1000u)) {
            ok = 0;
        }
    }
    check(ok && g_card.header_cmds == 1u, "0x00 gives the header, repeating every 0x1000 bytes", 0);
}

/* d. unknown commands */
static void test_unknown(void)
{
    static const uint8_t ops_[] = { 0x3c, 0x9f, 0xa0 };
    HwCardOps ops = { fix_pread, NULL };
    unsigned k;

    bus_init(&ops);
    for (k = 0; k < sizeof ops_; k++) {
        set_cmd_byte(ops_[k]);
        hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(1));
        check(hwcard_data_read(&g_card) == 0xffffffffu, "an unknown command reads 0xFFFFFFFF", ops_[k]);
        check(g_card.key_cmds == k + 1u && g_card.over_reads == 0u, "counted in key_cmds", ops_[k]);
    }
}

/* e. cache */
static uint32_t word_at(uint32_t off)
{
    set_page_cmd(off);
    hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(1));
    return hwcard_data_read(&g_card);
}

static void test_cache(void)
{
    HwCardOps ops = { fix_pread, NULL };
    uint32_t b, base = 8u;   /* blocks 8..23 hold file offsets 0x8000..0x17fff */
    int ok = 1;

    bus_init(&ops);
    for (b = base; b < base + 16u; b++) {
        ok &= word_at(b * HWCARD_BLOCK) == fix_word(b * HWCARD_BLOCK);
    }
    check(ok && g_card.cache_misses == 16u && g_card.cache_hits == 0u, "16 distinct blocks: 16 misses", (long)g_card.cache_misses);
    for (b = base; b < base + 16u; b++) {
        ok &= word_at(b * HWCARD_BLOCK + 8u) == fix_word(b * HWCARD_BLOCK + 8u);
    }
    check(ok && g_card.cache_misses == 16u && g_card.cache_hits == 16u, "then 16 hits", (long)g_card.cache_hits);
    (void)word_at((base + 16u) * HWCARD_BLOCK);   /* block b + 16 evicts block b */
    check(g_card.cache_misses == 17u, "b+16 misses", (long)g_card.cache_misses);
    (void)word_at(base * HWCARD_BLOCK);
    check(g_card.cache_misses == 18u, "b was evicted by b+16", (long)g_card.cache_misses);
    (void)word_at((base + 16u) * HWCARD_BLOCK);
    check(g_card.cache_misses == 19u, "and b+16 by b (direct-mapped)", (long)g_card.cache_misses);
    check(word_at(48u * HWCARD_BLOCK + 96u) == fix_word(48u * HWCARD_BLOCK + 96u), "the last real word before EOF", 0);
    check(word_at(48u * HWCARD_BLOCK + 100u) == 0xffffffffu, "bytes past EOF read 0xFF (mid-block)", 0);
    check(word_at(60u * HWCARD_BLOCK) == 0xffffffffu && g_card.read_errors == 0u, "a block wholly past EOF reads 0xFF and is no error",
          (long)g_card.read_errors);
}

/* f. errors are served but never cached */
static void test_errors(void)
{
    HwCardOps ops = { fix_pread, NULL };
    uint32_t off = 20u * HWCARD_BLOCK, misses;

    bus_init(&ops);
    g_inject = INJ_FAIL;
    check(word_at(off) == 0xffffffffu, "pread -1: the SDK sees 0xFF", 0);
    check(g_card.read_errors == 1u && g_card.slot_valid[20u % HWCARD_SLOTS] == 0u, "read_errors 1, the slot stays invalid", (long)g_card.read_errors);
    misses = g_card.cache_misses;
    check(word_at(off) == fix_word(off), "the next read retries and gets the real bytes", 0);
    check(g_card.cache_misses == misses + 1u && g_card.read_errors == 1u && g_card.slot_valid[20u % HWCARD_SLOTS] == 1u, "retry is a miss, now cached",
          (long)g_card.cache_misses);
    g_inject = INJ_SHORT;
    off = 21u * HWCARD_BLOCK + 3000u;   /* beyond the half that the short read delivers */
    check(word_at(off) == 0xffffffffu, "short read mid-file: the missing half reads 0xFF", 0);
    check(g_card.read_errors == 2u && g_card.slot_valid[21u % HWCARD_SLOTS] == 0u, "counted as an error, not cached", (long)g_card.read_errors);
    check(word_at(off) == fix_word(off) && g_card.slot_valid[21u % HWCARD_SLOTS] == 1u, "the next read gets the real bytes", 0);
    {
        HwCardOps none = { NULL, NULL };

        bus_init(&none);
        check(word_at(0x8000u) == 0xffffffffu && g_card.read_errors == 1u && g_card.slot_valid[0] == 0u, "no reader: error, 0xFF, nothing cached", 0);
    }
}

/* g. low reads */
static void test_low_reads(void)
{
    HwCardOps ops = { fix_pread, NULL };

    bus_init(&ops);
    (void)word_at(0x7e00u);
    check(g_card.low_reads == 1u, "page command below 0x8000 counts a low read", (long)g_card.low_reads);
    (void)word_at(0x8000u);
    check(g_card.low_reads == 1u, "0x8000 is not a low read", (long)g_card.low_reads);
    (void)word_at(0u);
    check(g_card.low_reads == 2u, "src 0 counts", (long)g_card.low_reads);
}

/* h. backup flash */
#define FLASH_BYTES  0x80000u
#define FLASH_SECTOR 0x10000u
#define ARENA_BYTES  0x2000u
static uint8_t g_flash[FLASH_BYTES];
static uint8_t g_arena[ARENA_BYTES];

static void *arena_mem(void *u, uint32_t addr, uint32_t len)
{
    (void)u;
    return (addr <= ARENA_BYTES && len <= ARENA_BYTES - addr) ? (void *)(g_arena + addr) : NULL;
}

static int32_t freq(unsigned req, uint32_t src, uint32_t dst, uint32_t len)
{
    HwCardCmd cmd;

    memset(&cmd, 0, sizeof cmd);
    cmd.src = src;
    cmd.dst = dst;
    cmd.len = len;
    return hwcard_flash_request(&g_card, req, &cmd, arena_mem, NULL);
}

static void test_flash_reads(void)
{
    uint32_t i;
    int ok = 1;

    memset(g_arena, 0x77, sizeof g_arena);
    check(freq(HWCARD_REQ_IDENTIFY, 0, 0, 0) == HWCARD_RESULT_SUCCESS && freq(HWCARD_REQ_WRITE_STATUS, 0, 0, 0) == HWCARD_RESULT_SUCCESS,
          "identify and write-status succeed", 0);
    check(freq(HWCARD_REQ_READ_STATUS, 0, 0x100u, 1) == HWCARD_RESULT_SUCCESS && g_arena[0x100] == 0u, "read-status writes 0", 0);
    check(freq(HWCARD_REQ_READ_STATUS, 0, ARENA_BYTES + 1u, 1) == HWCARD_RESULT_INVALID_PARAM, "read-status into unmapped memory is INVALID_PARAM", 0);
    for (i = 0; i < FLASH_BYTES; i++) {
        g_flash[i] = (uint8_t)(i * 5u + (i >> 8));
    }
    check(freq(HWCARD_REQ_READ_BACKUP, 0x7ff00u, 0, 0x100u) == HWCARD_RESULT_SUCCESS && memcmp(g_arena, g_flash + 0x7ff00u, 0x100u) == 0,
          "read at the end of the chip (0x7ff00 + 0x100)", 0);
    check(freq(HWCARD_REQ_READ_BACKUP, 0x7ff00u, 0, 0x101u) == HWCARD_RESULT_INVALID_PARAM, "0x7ff00 + 0x101 is INVALID_PARAM", 0);
    check(freq(HWCARD_REQ_READ_BACKUP, 0x80001u, 0, 0u) == HWCARD_RESULT_INVALID_PARAM, "a start past the chip is INVALID_PARAM", 0);
    check(freq(HWCARD_REQ_READ_BACKUP, 0xffffff00u, 0, 0x200u) == HWCARD_RESULT_INVALID_PARAM, "no 32-bit wrap: src + len overflowing is INVALID_PARAM", 0);
    check(freq(HWCARD_REQ_READ_BACKUP, 0u, ARENA_BYTES - 4u, 8u) == HWCARD_RESULT_INVALID_PARAM, "unmapped destination is INVALID_PARAM", 0);
    for (i = 0; i < 0x100u; i++) {
        ok &= g_arena[i] == g_flash[0x7ff00u + i];
    }
    check(ok, "the failed requests left the arena alone", 0);
}

static void test_flash_writes(void)
{
    uint32_t i;
    int ok = 1;

    for (i = 0; i < 256u; i++) {
        g_arena[i] = (uint8_t)(i ^ 0x5au);
    }
    check(freq(HWCARD_REQ_WRITE_BACKUP, 0u, 0x20000u, 256u) == HWCARD_RESULT_SUCCESS, "write-backup", 0);
    for (i = 0; i < 256u; i++) {
        ok &= g_flash[0x20000u + i] == (uint8_t)(i ^ 0x5au);
    }
    check(ok, "write-backup: the bytes become the data", 0);
    memset(g_flash + 0x30000u, 0xf0, 16);
    memset(g_arena, 0x3c, 16);
    check(freq(HWCARD_REQ_PROGRAM_BACKUP, 0u, 0x30000u, 16u) == HWCARD_RESULT_SUCCESS && g_flash[0x30000u] == (0xf0u & 0x3cu) && g_flash[0x3000fu] == 0x30u,
          "program ANDs bits (0xF0 & 0x3C = 0x30)", g_flash[0x30000u]);
    memcpy(g_arena, g_flash + 0x30000u, 16);
    check(freq(HWCARD_REQ_VERIFY_BACKUP, 0u, 0x30000u, 16u) == HWCARD_RESULT_SUCCESS && g_card.flash_verify_fail == 0u, "verify equal succeeds", 0);
    g_arena[7] ^= 1u;
    check(freq(HWCARD_REQ_VERIFY_BACKUP, 0u, 0x30000u, 16u) == HWCARD_RESULT_FAILURE && g_card.flash_verify_fail == 1u, "verify fails on a 1-bit difference",
          (long)g_card.flash_verify_fail);
    check(freq(HWCARD_REQ_WRITE_BACKUP, 0u, 0x7ff80u, 0x100u) == HWCARD_RESULT_INVALID_PARAM, "write past the chip is INVALID_PARAM", 0);
    check(freq(HWCARD_REQ_WRITE_BACKUP, 0u, 0x7ff00u, 0x100u) == HWCARD_RESULT_SUCCESS, "write ending exactly at the chip end succeeds", 0);
}

static int all_ff(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    assert(p != NULL);
    for (i = 0; i < n; i++) {
        if (p[i] != 0xffu) {
            return 0;
        }
    }
    return 1;
}

static void test_flash_erase(void)
{
    uint32_t e0;

    memset(g_flash, 0x00, sizeof g_flash);
    check(freq(HWCARD_REQ_ERASE_PAGE, 0, 0x1234u, 0) == HWCARD_RESULT_SUCCESS && all_ff(g_flash + 0x1200u, 256u) && g_flash[0x11ffu] == 0u &&
              g_flash[0x1300u] == 0u,
          "erase-page: the 256-byte unit holding dst, nothing else", 0);
    check(freq(HWCARD_REQ_ERASE_SUBSECTOR, 0, 0x5fffu, 0) == HWCARD_RESULT_SUCCESS && all_ff(g_flash + 0x5000u, 4096u) && g_flash[0x4fffu] == 0u &&
              g_flash[0x6000u] == 0u,
          "erase-subsector: the 4 KB unit holding dst", 0);
    check(freq(HWCARD_REQ_ERASE_SECTOR, 0, 0x2ffffu, 0) == HWCARD_RESULT_SUCCESS && all_ff(g_flash + 0x20000u, FLASH_SECTOR) && g_flash[0x1ffffu] == 0u &&
              g_flash[0x30000u] == 0u,
          "erase-sector: the 64 KB unit holding dst", 0);
    check(freq(HWCARD_REQ_ERASE_SECTOR, 0, FLASH_BYTES, 0) == HWCARD_RESULT_INVALID_PARAM, "erase-sector at the chip end is INVALID_PARAM", 0);
    check(freq(HWCARD_REQ_ERASE_PAGE, 0, 0xffffff00u, 0) == HWCARD_RESULT_INVALID_PARAM, "erase-page far outside is INVALID_PARAM", 0);
    e0 = g_card.flash_erases;
    check(freq(HWCARD_REQ_ERASE_CHIP, 0, 0, 0) == HWCARD_RESULT_SUCCESS && all_ff(g_flash, FLASH_BYTES) && g_card.flash_erases == e0 + 1u, "erase-chip", 0);
    check(freq(99u, 0, 0, 0) == HWCARD_RESULT_UNSUPPORTED, "an unknown request is UNSUPPORTED", 0);
}

static void test_flash(void)
{
    HwCardOps ops = { fix_pread, NULL };
    HwCard bare;

    bus_init(&ops);
    memset(&bare, 0, sizeof bare);
    hwcard_init(&bare, &ops, g_io);
    check(hwcard_flash_request(&bare, HWCARD_REQ_IDENTIFY, &(HwCardCmd){ 0 }, arena_mem, NULL) == HWCARD_RESULT_UNSUPPORTED, "no flash attached: UNSUPPORTED", 0);
    hwcard_flash_attach(&g_card, g_flash, FLASH_BYTES, FLASH_SECTOR);
    test_flash_reads();
    test_flash_writes();
    test_flash_erase();
}

static void test_reset_keeps(void)
{
    uint32_t hits;

    memset(g_flash, 0xa5, 64);
    (void)word_at(30u * HWCARD_BLOCK);
    hits = g_card.cache_hits;
    hwcard_reset(&g_card);
    check((hwcard_ctrl_read(&g_card) & HWCARD_START) == 0u, "reset drops the transfer in flight", 0);
    check(g_flash[0] == 0xa5u && g_card.flash == g_flash, "reset keeps the flash image", 0);
    (void)word_at(30u * HWCARD_BLOCK + 4u);
    check(g_card.cache_hits == hits + 1u, "reset keeps the block cache", (long)g_card.cache_hits);
}

/* ---- the optional real-ROM leg ------------------------------------------------------------------------------------------------ */
static FILE *g_romfile;

static int file_pread(void *u, uint32_t off, void *dst, uint32_t len)
{
    (void)u;
    assert(dst != NULL);
    if (fseek(g_romfile, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    return (int)fread(dst, 1, len, g_romfile);
}

static void test_real_rom(const char *rom, const char *refdir)
{
    char path[512];
    FILE *txt, *bin;
    HwCardOps ops = { file_pread, NULL };
    unsigned long off;
    unsigned pages = 0, bad = 0;
    uint8_t want[512], got[512];
    unsigned w;

    snprintf(path, sizeof path, "%s/pages.txt", refdir);
    txt = fopen(path, "r");
    snprintf(path, sizeof path, "%s/pages.bin", refdir);
    bin = fopen(path, "rb");
    g_romfile = fopen(rom, "rb");
    check(txt != NULL && bin != NULL && g_romfile != NULL, "--rom: ROM and reference pages open", 0);
    if (txt == NULL || bin == NULL || g_romfile == NULL) {
        return;
    }
    bus_init(&ops);
    while (pages < 64u && fscanf(txt, "%lx", &off) == 1) {
        if (fread(want, 1, sizeof want, bin) != sizeof want) {
            break;
        }
        set_page_cmd((uint32_t)off);
        hwcard_ctrl_write(&g_card, HWCARD_START | CTRL_SZ(1));
        for (w = 0; w < 128u; w++) {
            uint32_t v = hwcard_data_read(&g_card);

            got[w * 4u] = (uint8_t)v;
            got[w * 4u + 1u] = (uint8_t)(v >> 8);
            got[w * 4u + 2u] = (uint8_t)(v >> 16);
            got[w * 4u + 3u] = (uint8_t)(v >> 24);
        }
        bad += memcmp(got, want, sizeof want) != 0;
        pages++;
    }
    check(pages == 32u && bad == 0u, "--rom: 32 FAT-derived pages match an independent reader byte for byte", (long)pages * 1000L + (long)bad);
    check(g_card.read_errors == 0u && g_card.over_reads == 0u, "--rom: no read error", (long)g_card.read_errors);
    (void)fclose(txt);
    (void)fclose(bin);
    (void)fclose(g_romfile);
}

int main(int argc, char **argv)
{
    assert(sizeof g_io > HWR_CARD_CMD + 8u);
    assert(FIX_SIZE % HWCARD_BLOCK == 100u);
    fixture_init();
    test_encoding();
    test_protocol();
    test_id_header();
    test_unknown();
    test_cache();
    test_errors();
    test_low_reads();
    test_flash();
    test_reset_keeps();
    if (argc >= 4 && strcmp(argv[1], "--rom") == 0) {
        test_real_rom(argv[2], argv[3]);
    }
    printf("hw_card_test: %ld checks, %ld failures%s\n", g_checks, g_fails, argc >= 4 ? " (with --rom)" : "");
    return g_fails != 0 ? 1 : 0;
}
