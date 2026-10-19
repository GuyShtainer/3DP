/* hw_saveid_test.c -- bet 2.6a: the console identity in a Platinum save (hw_saveid.c) and the per-save config.ini section (hw_cfg.c). Native, no SDK, no libctru.
 * A synthetic 512 KB save is built here with a known identity (its own CRC16, written independently of hw_saveid.c); checks: the parse returns the identity, the newest-slot
 * rule (saveCounter, then blockCounter, a tie to slot 1, one corrupt slot), every refusal (flipped body byte, wrong signature, wrong block id, wrong size, NULL), the
 * section format/lookup round trip, unknown keys and sections ignored, the old flat keys unchanged, the adoption decision (new / section / opt-out / partial section),
 * the clock offset left alone. With `--identity FILE` it prints the identity of a real save in the form the private save-tool tests compare with an independent reader.
 * Prints "hw_saveid_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_cfg.h"
#include "hw_saveid.h"

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

#define NORMAL 0xCF2Cu
#define FOOT   0x14u

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFFu;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        c = (uint16_t)(c ^ (uint16_t)(p[i] << 8));
        for (b = 0; b < 8; b++) {
            c = (uint16_t)((c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1));
        }
    }
    return c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* one slot's normal block at img + base, with the identity and valid footer */
static void make_slot(uint8_t *img, uint32_t base, uint32_t save_counter, uint32_t block_counter, int64_t rtc, const uint8_t mac[6], uint16_t tid, uint16_t sid)
{
    uint8_t *b = img + base, *f = b + NORMAL - FOOT;
    uint16_t c;
    uint32_t i;

    memset(b, 0, NORMAL);
    for (i = 0; i < 8u; i++) {
        b[i] = (uint8_t)((uint64_t)rtc >> (8u * i));
    }
    memcpy(b + 8, mac, 6);
    b[0x78] = (uint8_t)tid; b[0x79] = (uint8_t)(tid >> 8); b[0x7A] = (uint8_t)sid; b[0x7B] = (uint8_t)(sid >> 8);
    for (i = 0x100; i < NORMAL - FOOT; i += 7u) {
        b[i] = (uint8_t)(i * 31u);   /* some body so the CRC is not of zeros */
    }
    put32(f, save_counter); put32(f + 4, block_counter); put32(f + 8, NORMAL); put32(f + 12, 0x20060623u);
    f[0x10] = 0;
    c = crc16(b, NORMAL - FOOT);
    f[0x12] = (uint8_t)c; f[0x13] = (uint8_t)(c >> 8);
}

static uint8_t g_img[0x80000];
static const uint8_t MAC_A[6] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
static const uint8_t MAC_B[6] = { 0xe0, 0x0c, 0x7f, 0x5d, 0x4f, 0xdb };

static void test_parse(void)
{
    HwSaveId id;
    uint8_t copy[0x80000];

    memset(g_img, 0xff, sizeof g_img);
    make_slot(g_img, 0, 37, 142, 0xFF00000000LL, MAC_A, 25460, 20201);
    make_slot(g_img, 0x40000, 37, 141, 0xFF00000000LL, MAC_A, 25460, 20201);
    check(hwsaveid_read(g_img, sizeof g_img, &id) == 1, "valid save parses", 0);
    check(id.tid == 25460u && id.sid == 20201u, "TID / SID read", (long)id.tid);
    check(memcmp(id.mac, MAC_A, 6) == 0, "MAC read", id.mac[0]);
    check(id.rtc_offset == 0xFF00000000LL, "RTC offset read (the 64-bit value of Guy's save)", (long)id.rtc_offset);
    check(id.slot == 1u && id.save_counter == 37u && id.block_counter == 142u, "slot 1 wins on blockCounter", id.slot);

    memcpy(copy, g_img, sizeof copy);
    copy[0x10] ^= 1u;   /* a flipped byte in slot 1's body: that slot's CRC no longer holds */
    check(hwsaveid_read(copy, sizeof copy, &id) == 1 && id.slot == 2u, "flipped byte in slot 1: slot 2 is used", id.slot);
    copy[0x40010] ^= 1u;
    memset(&id, 0x5a, sizeof id);
    check(hwsaveid_read(copy, sizeof copy, &id) == 0, "a flipped byte in both slots is refused", 0);
    check(id.tid == 0x5a5a5a5au, "a refusal leaves *out untouched", (long)id.tid);

    memcpy(copy, g_img, sizeof copy);
    copy[NORMAL - FOOT + 0x0C] ^= 1u; copy[0x40000 + NORMAL - FOOT + 0x0C] ^= 1u;
    check(hwsaveid_read(copy, sizeof copy, &id) == 0, "wrong signature refused", 0);
    memcpy(copy, g_img, sizeof copy);
    copy[NORMAL - FOOT + 0x10] = 1; copy[0x40000 + NORMAL - FOOT + 0x10] = 1;
    check(hwsaveid_read(copy, sizeof copy, &id) == 0, "wrong block id refused (a storage footer is not a normal block)", 0);
    check(hwsaveid_read(g_img, sizeof g_img - 1u, &id) == 0, "wrong size refused", 0);
    check(hwsaveid_read(NULL, sizeof g_img, &id) == 0, "NULL refused", 0);
    memset(copy, 0xff, sizeof copy);
    check(hwsaveid_read(copy, sizeof copy, &id) == 0, "an erased flash (all 0xFF) has no identity", 0);
}

static void test_newest(void)
{
    HwSaveId id;

    make_slot(g_img, 0, 10, 5, 0, MAC_A, 1, 2);
    make_slot(g_img, 0x40000, 11, 1, 7, MAC_B, 3, 4);
    check(hwsaveid_read(g_img, sizeof g_img, &id) == 1 && id.slot == 2u && memcmp(id.mac, MAC_B, 6) == 0 && id.rtc_offset == 7 && id.tid == 3u && id.sid == 4u,
          "higher saveCounter wins (slot 2)", id.slot);
    make_slot(g_img, 0x40000, 10, 5, 7, MAC_B, 3, 4);
    check(hwsaveid_read(g_img, sizeof g_img, &id) == 1 && id.slot == 1u, "a tie goes to slot 1", id.slot);
    make_slot(g_img, 0x40000, 10, 6, 7, MAC_B, 3, 4);
    check(hwsaveid_read(g_img, sizeof g_img, &id) == 1 && id.slot == 2u, "equal saveCounter: higher blockCounter wins", id.slot);
    make_slot(g_img, 0, 5, 5, -1, MAC_A, 1, 2);
    make_slot(g_img, 0x40000, 4, 5, 7, MAC_B, 3, 4);
    check(hwsaveid_read(g_img, sizeof g_img, &id) == 1 && id.slot == 1u && id.rtc_offset == -1, "a negative RTC offset is sign-extended", (long)id.rtc_offset);
}

static void test_section(void)
{
    char buf[1024], file[4096];
    HwCfgSave s;
    HwCfg c;
    size_t n, fl;
    uint32_t skipped = 9;

    n = hwcfg_format_save(25460, 20201, MAC_A, 0xFF00000000LL, buf, sizeof buf);
    check(n > 0u && n < sizeof buf && buf[n - 1u] == '\n' && buf[n] == '\0', "section formatted", (long)n);
    check(strstr(buf, "[save 25460-20201]\n") != NULL && strstr(buf, "mac = 00:01:02:03:04:05\n") != NULL && strstr(buf, "rtc_offset = 1095216660480\n") != NULL &&
          strstr(buf, "adopted_from_save = 1\n") != NULL, "section text", 0);
    check(hwcfg_format_save(70000, 1, MAC_A, 0, buf, sizeof buf) == 0, "a TID above 16 bits refused", 0);
    check(hwcfg_format_save(1, 1, MAC_A, 0, buf, 20) == 0, "a short buffer refused, never truncated", 0);

    hwcfg_defaults(&c);
    fl = hwcfg_format(&c, file, sizeof file);
    memcpy(file + fl, buf, n + 1u);   /* the real file: the flat keys, then the section just formatted */
    fl += n;
    check(hwcfg_save_lookup(file, fl, 25460, 20201, &s) == 1 && s.found && s.adopted && s.mac_set && s.rtc_set && memcmp(s.mac, MAC_A, 6) == 0 &&
          s.rtc_offset == 0xFF00000000LL, "format -> lookup round trip", 0);
    check(hwcfg_save_lookup(file, fl, 25460, 20202, &s) == 0 && !s.found, "another SID: not found", 0);
    check(hwcfg_save_lookup(file, fl, 1, 20201, &s) == 0, "another TID: not found", 0);

    /* flat keys: unchanged, and a section after them neither adds nor breaks anything */
    hwcfg_defaults(&c);
    check(hwcfg_parse(&c, file, fl, &skipped) == 8u && skipped == 0u, "the flat keys still parse (8 keys, none skipped) with a section after them", (long)skipped);
    check(c.rtc_offset == 0 && c.mac[0] == 0x00 && c.mac[1] == 0x09 && c.id_rtc_set == 0, "the section does not leak into the flat keys", c.mac[1]);
    {
        static const char old_flat[] = "owner_name = GUY\nmac = 12:34:56:78:9a:bc\nrtc_offset = -42\n";

        hwcfg_defaults(&c);
        check(hwcfg_parse(&c, old_flat, sizeof old_flat - 1u, &skipped) == 3u && c.mac[0] == 0x12 && c.rtc_offset == -42 && skipped == 0u, "an old flat file keeps its meaning", 0);
    }
    {   /* unknown keys and sections ignored, ids in either order of sections, first duplicate wins */
        static const char txt[] =
            "rtc_offset = 5\n"
            "[something else]\nmac = 99:99:99:99:99:99\n"
            "[save 1-2]\nfuture_key = 1\nmac = 11:22:33:44:55:66\nrtc_offset = bogus\n"
            "[save 3-4]\nmac = aa:aa:aa:aa:aa:aa\nadopted_from_save = 0\n"
            "[save 1-2]\nmac = 77:77:77:77:77:77\n";
        hwcfg_defaults(&c);
        check(hwcfg_parse(&c, txt, sizeof txt - 1u, &skipped) == 1u && c.rtc_offset == 5 && skipped == 0u, "flat parse stops at the first section header", (long)skipped);
        check(hwcfg_save_lookup(txt, sizeof txt - 1u, 1, 2, &s) && s.mac_set && s.mac[0] == 0x11 && !s.rtc_set && s.adopted, "unknown key ignored, bad value skipped, first duplicate wins", s.mac[0]);
        check(hwcfg_save_lookup(txt, sizeof txt - 1u, 3, 4, &s) && !s.adopted, "adopted_from_save = 0 read", 0);
        check(hwcfg_save_lookup(txt, sizeof txt - 1u, 9, 9, &s) == 0, "a section for a save not in the file is not found", 0);
    }
    check(hwcfg_save_lookup("[save 1-2", 9, 1, 2, &s) == 0 && hwcfg_save_lookup("[save x-2]\n", 11, 1, 2, &s) == 0 && hwcfg_save_lookup("[save 1-2-3]\n", 13, 1, 2, &s) == 0,
          "malformed headers do not match", 0);
}

static void test_adopt(void)
{
    HwCfg c;
    static const uint8_t SAVE_MAC[6] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    static const char custom[] = "[save 25460-20201]\nmac = de:ad:be:ef:00:01\nadopted_from_save = 1\n";   /* the user overrode the MAC, left the offset out */
    static const char optout[] = "mac = 12:34:56:78:9a:bc\n[save 25460-20201]\nadopted_from_save = 0\n";

    hwcfg_defaults(&c);
    check(hwcfg_adopt_identity(&c, "", 0, 25460, 20201, SAVE_MAC, 77) == HWCFG_ID_NEW && memcmp(c.mac, SAVE_MAC, 6) == 0 && c.id_rtc_set && c.id_rtc_offset == 77,
          "no section: the save's identity is adopted (NEW)", 0);
    check(c.rtc_offset == 0, "the clock offset is not touched by adoption", (long)c.rtc_offset);
    hwcfg_defaults(&c);
    check(hwcfg_adopt_identity(&c, custom, sizeof custom - 1u, 25460, 20201, SAVE_MAC, 77) == HWCFG_ID_SECTION && c.mac[0] == 0xde && c.id_rtc_offset == 77 && c.id_rtc_set,
          "a section overrides the MAC; the missing rtc_offset falls back to the save's", c.mac[0]);
    hwcfg_defaults(&c);
    check(hwcfg_adopt_identity(&c, optout, sizeof optout - 1u, 25460, 20201, SAVE_MAC, 77) == HWCFG_ID_OPTOUT && c.mac[0] == 0x00 && c.mac[1] == 0x09 && !c.id_rtc_set,
          "adopted_from_save = 0: the config is left alone", 0);
    hwcfg_defaults(&c);
    check(hwcfg_adopt_identity(&c, optout, sizeof optout - 1u, 1, 1, SAVE_MAC, 77) == HWCFG_ID_NEW, "the opt-out is per save", 0);
}

/* Bet 2.7a: the render_core / hwcheck keys: defaults, range, bad values skipped. */
static void test_hw27_keys(void)
{
    HwCfg c;
    uint32_t skipped = 0;
    static const char ok[] = "render_core = 0\nhwcheck = 2\n";
    static const char bad[] = "render_core = 3\nhwcheck = 9\nrender_core = x\n";

    hwcfg_defaults(&c);
    check(c.render_core == 2u && c.hwcheck == 0u, "render_core defaults to 2, hwcheck to 0", c.render_core);
    check(hwcfg_parse(&c, ok, sizeof ok - 1u, &skipped) == 2u && c.render_core == 0u && c.hwcheck == 2u && skipped == 0u, "render_core = 0 and hwcheck = 2 parse", c.render_core);
    hwcfg_defaults(&c);
    check(hwcfg_parse(&c, bad, sizeof bad - 1u, &skipped) == 0u && skipped == 3u && c.render_core == 2u && c.hwcheck == 0u, "out-of-range values are skipped and counted", (long)skipped);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--identity") == 0) {   /* cross-check hook for savetools_test.py: the identity of a real save, one line */
        static uint8_t buf[0x80000];
        HwSaveId id;
        FILE *f = fopen(argv[2], "rb");
        size_t n = f != NULL ? fread(buf, 1, sizeof buf, f) : 0u;

        if (f != NULL) {
            (void)fclose(f);
        }
        if (n != sizeof buf || !hwsaveid_read(buf, n, &id)) {
            printf("refused\n");
            return 1;
        }
        printf("slot=%u tid=%u sid=%u mac=%02x:%02x:%02x:%02x:%02x:%02x rtc_offset=%lld\n", (unsigned)id.slot, (unsigned)id.tid, (unsigned)id.sid, id.mac[0], id.mac[1], id.mac[2],
               id.mac[3], id.mac[4], id.mac[5], (long long)id.rtc_offset);
        return 0;
    }
    test_parse();
    test_newest();
    test_section();
    test_adopt();
    test_hw27_keys();
    printf("hw_saveid_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
