/* setup_core_test.c: host tests for source/hw/hw_setup_core.c (release P4a). usage: setup_core_test [ROM.nds]
 * With a ROM it also checks the real header and all table paths end to end (NitroFS lookup + SHA-1). Our own code. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hw_setup_core.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }

static void hex(const uint8_t d[20], char out[41])
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 20; i++) { out[2 * i] = H[d[i] >> 4]; out[2 * i + 1] = H[d[i] & 15]; }
    out[40] = 0;
}

static void sha_hex(const void *d, size_t n, char out[41])
{
    setup_sha1 c; uint8_t r[20];
    setup_sha1_init(&c); setup_sha1_update(&c, d, n); setup_sha1_final(&c, r); hex(r, out);
}

static void test_sha1(void)
{
    char h[41]; setup_sha1 c; uint8_t r[20]; static uint8_t big[1000000]; uint8_t rest[1000];
    size_t i, off;
    sha_hex("abc", 3, h); CHECK(strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    sha_hex("", 0, h); CHECK(strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
    sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
    memset(big, 'a', sizeof big);
    sha_hex(big, sizeof big, h); CHECK(strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0);
    /* split-update equivalence over random chunkings, lengths around the block edges */
    for (i = 0; i < 300; i++) {
        size_t n = (i < 140) ? i : rnd() % 1000; char a[41], b[41];
        for (off = 0; off < n; off++) rest[off] = (uint8_t)rnd();
        sha_hex(rest, n, a);
        setup_sha1_init(&c);
        for (off = 0; off < n;) { size_t k = rnd() % 130; if (k > n - off) k = n - off; setup_sha1_update(&c, rest + off, k); off += k; }
        setup_sha1_final(&c, r); hex(r, b);
        CHECK(strcmp(a, b) == 0);
    }
}

static void fix_crc(uint8_t *h)
{
    uint16_t crc = 0xFFFF; int i, b;
    for (i = 0; i < 0x15E; i++) { crc ^= h[i]; for (b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1); }
    h[0x15E] = (uint8_t)crc; h[0x15F] = (uint8_t)(crc >> 8);
}

static void test_header(const char *rom_path)
{
    uint8_t h[0x200]; int i; FILE *f;
    memset(h, 0, sizeof h); memcpy(h, "POKEMON PL", 10); memcpy(h + 0x0C, "CPUE", 4); h[0x20] = 7;
    fix_crc(h); CHECK(setup_rom_header_ok(h) == 1);
    for (i = 0; i < 0x15E; i += 17) { h[i] ^= 0x10; if (i >= 0x0C && i < 0x10) { h[i] ^= 0x10; continue; } CHECK(setup_rom_header_ok(h) == 0); h[i] ^= 0x10; }
    h[0x15E] ^= 1; CHECK(setup_rom_header_ok(h) == 0); h[0x15E] ^= 1; CHECK(setup_rom_header_ok(h) == 1);
    memcpy(h + 0x0C, "CPUJ", 4); fix_crc(h); CHECK(setup_rom_header_ok(h) == 0);   /* right CRC, wrong game code */
    memcpy(h + 0x0C, "ADAE", 4); fix_crc(h); CHECK(setup_rom_header_ok(h) == 0);
    CHECK(setup_rom_header_ok(NULL) == 0);
    f = rom_path ? fopen(rom_path, "rb") : NULL;
    if (f) { uint8_t rh[0x200]; CHECK(fread(rh, 1, sizeof rh, f) == sizeof rh); CHECK(setup_rom_header_ok(rh) == 1); (void)fclose(f); }
    else printf("SKIP: no ROM, real-header check skipped\n");
}

/* synthetic table: / { a.txt(id0), d/ { b.bin(id1) } } */
static const uint8_t k_fnt[] = {
    16,0,0,0, 0,0, 2,0,                      /* root: sub_off 16, first file 0, 2 dirs */
    27,0,0,0, 1,0, 0,0xF0,                   /* d: sub_off 27, first file 1, parent root */
    5,'a','.','t','x','t', 0x81,'d',1,0xF0, 0,   /* root sub-table (16..26) */
    5,'b','.','b','i','n', 0 };              /* d sub-table (27..) */
static const uint8_t k_fat[] = { 0x10,0,0,0, 0x20,0,0,0, 0x00,0x01,0,0, 0x80,0x01,0,0 };   /* a.txt 0x10..0x20, b.bin 0x100..0x180 */

static void test_nitrofs_synth(void)
{
    uint32_t s = 0, e = 0; uint8_t t[sizeof k_fnt]; uint8_t fat[16];
    memcpy(fat, k_fat, 16);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/a.txt", &s, &e) == 1 && s == 0x10 && e == 0x20);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/d/b.bin", &s, &e) == 1 && s == 0x100 && e == 0x180);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/missing", &s, &e) == 0);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/d/missing", &s, &e) == 0);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/d", &s, &e) == 0);       /* a directory is not a file */
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/a.txt/x", &s, &e) == 0); /* a file is not a directory */
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "a.txt", &s, &e) == 0);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "/", &s, &e) == 0);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 16, "//a.txt", &s, &e) == 0);
    CHECK(setup_nitrofs_find(k_fnt, sizeof k_fnt, fat, 8, "/d/b.bin", &s, &e) == -1);  /* id 1 outside a 1-entry FAT */
    /* truncated: every cut of the table before the d sub-table's end either finds, reports absent, or is -1; never reads past the end */
    {   size_t cut; int r;
        for (cut = 0; cut < sizeof k_fnt; cut++) { memcpy(t, k_fnt, cut); r = setup_nitrofs_find(t, (uint32_t)cut, fat, 16, "/d/b.bin", &s, &e); CHECK(r == -1 || r == 0 || r == 1); }
        memcpy(t, k_fnt, sizeof t); r = setup_nitrofs_find(t, (uint32_t)sizeof t - 1, fat, 16, "/d/zzz", &s, &e); CHECK(r == -1);   /* no terminator */
    }
    /* looping: root's sub-table is a run of entries with no terminator; and a sub_off pointing past the table */
    {   uint8_t l[64]; size_t i;
        memset(l, 0, sizeof l);
        l[0] = 8; l[6] = 1;                                   /* root: sub_off 8, 1 dir */
        for (i = 8; i + 2 <= sizeof l; i += 2) { l[i] = 1; l[i + 1] = 'x'; }
        CHECK(setup_nitrofs_find(l, sizeof l, k_fat, 16, "/zzz", &s, &e) == -1);
        l[0] = 200; CHECK(setup_nitrofs_find(l, sizeof l, k_fat, 16, "/zzz", &s, &e) == -1);
        memcpy(l, k_fnt, sizeof k_fnt); l[24] = 0; l[25] = 0xF0;                                       /* a self-referential 'd' pointing back at the root */
        CHECK(setup_nitrofs_find(l, sizeof k_fnt, fat, 16, "/d/d/d/d/d/a.txt", &s, &e) == 1);   /* finite by path length */
        l[6] = 0; l[7] = 0; CHECK(setup_nitrofs_find(l, sizeof k_fnt, fat, 16, "/a.txt", &s, &e) == -1);   /* zero dirs */
    }
    CHECK(setup_nitrofs_find(NULL, 0, fat, 16, "/a", &s, &e) == -1);
}

static void test_nitrofs_rom(const char *rom_path)
{
    FILE *f; uint8_t *rom; long size; uint32_t fo, fl, ao, al, i, found = 0, bad_hash = 0; clock_t t0;
    if (!rom_path || !(f = fopen(rom_path, "rb"))) { printf("SKIP: no ROM, table-path check skipped\n"); return; }
    CHECK(fseek(f, 0, SEEK_END) == 0); size = ftell(f); CHECK(fseek(f, 0, SEEK_SET) == 0);
    rom = (uint8_t *)malloc((size_t)size);
    if (!rom || size < 0x200 || fread(rom, 1, (size_t)size, f) != (size_t)size) { printf("FAIL: cannot read the ROM\n"); g_fail++; (void)fclose(f); free(rom); return; }
    (void)fclose(f);
    fo = rom[0x40] | (rom[0x41] << 8) | (rom[0x42] << 16) | ((uint32_t)rom[0x43] << 24); fl = rom[0x44] | (rom[0x45] << 8) | (rom[0x46] << 16) | ((uint32_t)rom[0x47] << 24);
    ao = rom[0x48] | (rom[0x49] << 8) | (rom[0x4A] << 16) | ((uint32_t)rom[0x4B] << 24); al = rom[0x4C] | (rom[0x4D] << 8) | (rom[0x4E] << 16) | ((uint32_t)rom[0x4F] << 24);
    CHECK((uint64_t)fo + fl <= (uint64_t)size && (uint64_t)ao + al <= (uint64_t)size);
    CHECK(setup_fs_count == 340);
    t0 = clock();
    for (i = 0; i < setup_fs_count; i++) {
        uint32_t s = 0, e = 0; uint8_t d[20]; setup_sha1 c;
        if (setup_nitrofs_find(rom + fo, fl, rom + ao, al, setup_fs_table[i].rom_path, &s, &e) != 1 || e > (uint64_t)size) { printf("not found: %s\n", setup_fs_table[i].rom_path); continue; }
        found++;
        setup_sha1_init(&c); setup_sha1_update(&c, rom + s, e - s); setup_sha1_final(&c, d);
        if (memcmp(d, setup_fs_table[i].sha1, 20) != 0) { bad_hash++; printf("hash mismatch: %s\n", setup_fs_table[i].rom_path); }
    }
    printf("real ROM: %u of %u table paths found, %u hash mismatches, find+SHA-1 wall time %.2f s on this host\n", found, setup_fs_count, bad_hash,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    CHECK(found == 340 && bad_hash == 0);
    free(rom);
}

static void make_dsv(uint8_t *buf, size_t raw, uint32_t written, uint32_t pad, uint32_t version)
{
    static const char snip[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
    uint8_t *f = buf + raw; uint32_t v[6]; int i;
    v[0] = written; v[1] = pad; v[2] = 3; v[3] = 2; v[4] = (uint32_t)raw; v[5] = version;
    memset(buf, 0x5A, raw);
    memcpy(f, snip, 82);
    for (i = 0; i < 6; i++) { f[82 + 4 * i] = (uint8_t)v[i]; f[83 + 4 * i] = (uint8_t)(v[i] >> 8); f[84 + 4 * i] = (uint8_t)(v[i] >> 16); f[85 + 4 * i] = (uint8_t)(v[i] >> 24); }
    memcpy(f + 106, "|-DESMUME SAVE-|", 16);
}

static void test_save_kind(void)
{
    static uint8_t b[524288 + 122 + 4096]; uint64_t raw = 7; uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    CHECK(setup_save_kind(524288, tail, 128, &raw) == SETUP_SAVE_RAW && raw == 524288);
    make_dsv(b, 524288, 524288, 524288, 0);
    memcpy(tail, b + 524288 + 122 - 128, 128);
    CHECK(setup_save_kind(524288 + 122, tail, 128, &raw) == SETUP_SAVE_DSV && raw == 524288);
    CHECK(setup_save_kind(524288 + 122, b + 524288, 122, &raw) == SETUP_SAVE_DSV);       /* exact-size tail */
    CHECK(setup_save_kind(524288 + 122, b + 524288, 121, &raw) == SETUP_SAVE_BAD);       /* tail too short */
    CHECK(setup_save_kind(524288 + 122, NULL, 0, &raw) == SETUP_SAVE_BAD && raw == 0);
    make_dsv(b, 524288 + 4096, 524288, 524288 + 4096, 0);                                   /* a longer raw part is truncated */
    CHECK(setup_save_kind(524288 + 4096 + 122, b + 524288 + 4096, 122, &raw) == SETUP_SAVE_DSV && raw == 524288);
    make_dsv(b, 524288, 524288, 524288 + 1, 0);                                             /* bad size field */
    CHECK(setup_save_kind(524288 + 122, b + 524288, 122, &raw) == SETUP_SAVE_BAD);
    make_dsv(b, 524288, 524289, 524288, 0);                                                 /* written > padded */
    CHECK(setup_save_kind(524288 + 122, b + 524288, 122, &raw) == SETUP_SAVE_BAD);
    make_dsv(b, 524288, 524288, 524288, 1);                                                 /* unknown version */
    CHECK(setup_save_kind(524288 + 122, b + 524288, 122, &raw) == SETUP_SAVE_BAD);
    make_dsv(b, 262144, 262144, 262144, 0);                                                 /* raw part too small */
    CHECK(setup_save_kind(262144 + 122, b + 262144, 122, &raw) == SETUP_SAVE_BAD);
    make_dsv(b, 524288, 524288, 524288, 0); b[524288 + 110] ^= 1;                           /* bad cookie */
    CHECK(setup_save_kind(524288 + 122, b + 524288, 122, &raw) == SETUP_SAVE_BAD);
    memset(b, 0, sizeof b);
    CHECK(setup_save_kind(100, b, 100, &raw) == SETUP_SAVE_BAD);                            /* short */
    CHECK(setup_save_kind(0, b, 0, &raw) == SETUP_SAVE_BAD);
    CHECK(setup_save_kind(524288 + 122, b, 122, &raw) == SETUP_SAVE_BAD);                   /* long, no footer */
    CHECK(setup_save_kind(1048576, b, 128, &raw) == SETUP_SAVE_BAD);
    CHECK(setup_save_kind(524288 + 1, b, 128, &raw) == SETUP_SAVE_BAD);
}

static long cfg(const char *in, const char *rom, char *out, size_t cap) { return setup_config_set_rom(in, strlen(in), rom, out, cap); }

static void test_config(void)
{
    char o[8192]; char big[5000]; long n; const char *p = "sdmc:/roms/platinum.nds";
    const char *fx = "# 3DP config\nwidescreen = 1\n\n[save:ABCD1234]\nname = Guy\nrom_hint = keep\n[other]\nrom = inner\n";
    char exp[512];
    /* add: no rom key, goes before the first section; everything else untouched */
    n = cfg(fx, p, o, sizeof o);
    snprintf(exp, sizeof exp, "# 3DP config\nwidescreen = 1\n\nrom = %s\n[save:ABCD1234]\nname = Guy\nrom_hint = keep\n[other]\nrom = inner\n", p);
    CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    /* replace: a top-level rom line; the [other] rom stays */
    {   const char *in2 = "rom = old.nds\nwidescreen = 1\n[other]\nrom = inner\n";
        n = cfg(in2, p, o, sizeof o);
        snprintf(exp, sizeof exp, "rom = %s\nwidescreen = 1\n[other]\nrom = inner\n", p);
        CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
        /* round trip: setting the same value again is the identity */
        o[n] = 0; n = cfg(o, p, big, sizeof big); CHECK(n == (long)strlen(exp) && memcmp(big, exp, (size_t)n) == 0);
    }
    CHECK(cfg("  ROMx = 1\nrom_x = 2\nrom\n", p, o, sizeof o) > 0 && strstr(o, "rom_x = 2\n") && strstr(o, "ROMx = 1\n"));   /* look-alikes are not the key */
    n = cfg("; c\nrom=a\n", p, o, sizeof o); snprintf(exp, sizeof exp, "; c\nrom = %s\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    n = cfg("\trom\t=\tx\n", p, o, sizeof o); snprintf(exp, sizeof exp, "rom = %s\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    n = cfg("a = 1", p, o, sizeof o); snprintf(exp, sizeof exp, "a = 1\nrom = %s\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);   /* no trailing newline */
    n = cfg("", p, o, sizeof o); snprintf(exp, sizeof exp, "rom = %s\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    n = cfg("rom = x", p, o, sizeof o); snprintf(exp, sizeof exp, "rom = %s", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);   /* last line, no terminator, kept */
    n = cfg("rom = x\r\nb = 2\r\n", p, o, sizeof o); snprintf(exp, sizeof exp, "rom = %s\r\nb = 2\r\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    n = cfg("[s]\nrom = 1\n", p, o, sizeof o); snprintf(exp, sizeof exp, "rom = %s\n[s]\nrom = 1\n", p); CHECK(n == (long)strlen(exp) && memcmp(o, exp, (size_t)n) == 0);
    /* refusals */
    {   char p80[82], p81[83]; memset(p80, 'a', 80); p80[80] = 0; memset(p81, 'a', 81); p81[81] = 0;
        CHECK(cfg("", p80, o, sizeof o) == 87); CHECK(cfg("", p81, o, sizeof o) == -1); }
    CHECK(cfg("", "", o, sizeof o) == -1); CHECK(cfg("", "a\nb", o, sizeof o) == -1); CHECK(cfg("", "a\rb", o, sizeof o) == -1);
    CHECK(setup_config_set_rom(NULL, 0, NULL, o, sizeof o) == -1);
    /* RF2 H1: UTF-8 passes through, edge blanks / controls / 0x7F do not */
    CHECK(setup_rom_path_ok("sdmc:/roms/Pok\xc3\xa9mon Platinum.nds") == 1);                 /* e-acute */
    CHECK(setup_rom_path_ok("sdmc:/roms/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3.nds") == 1);   /* CJK katakana */
    CHECK(setup_rom_path_ok("sdmc:/roms/Platinum .nds") == 1 && setup_rom_path_ok("sdmc:/roms/ Platinum.nds") == 1);   /* inner blanks are fine */
    CHECK(setup_rom_path_ok("sdmc:/roms/Platinum.nds ") == 0 && setup_rom_path_ok(" sdmc:/roms/p.nds") == 0 && setup_rom_path_ok("\tsdmc:/p.nds") == 0);
    CHECK(setup_rom_path_ok("sdmc:/roms/a\x7f.nds") == 0 && setup_rom_path_ok("sdmc:/roms/a\x01.nds") == 0 && setup_rom_path_ok("") == 0 && setup_rom_path_ok(NULL) == 0);
    CHECK(cfg("", "sdmc:/roms/Pok\xc3\xa9mon Platinum.nds", o, sizeof o) > 0 && strstr(o, "rom = sdmc:/roms/Pok\xc3\xa9mon Platinum.nds\n") != NULL);
    CHECK(cfg("", "sdmc:/roms/a\x7f.nds", o, sizeof o) == -1 && cfg("", "sdmc:/p.nds ", o, sizeof o) == -1);
    memset(big, 'x', sizeof big); big[4999] = 0;
    CHECK(cfg(big, p, o, sizeof o) == -1);                                        /* result over 4096 */
    memset(big, 'x', 4096 - 6 - strlen(p) - 2); big[4096 - 6 - strlen(p) - 2] = '\n'; big[4096 - 6 - strlen(p) - 1] = 0;
    n = cfg(big, p, o, sizeof o); CHECK(n == 4096);                           /* just under the cap fits */
    memset(big, 'x', 4096 - 6 - strlen(p) - 1); big[4096 - 6 - strlen(p) - 1] = 0;
    CHECK(cfg(big, p, o, sizeof o) == -1);                                        /* the added newline pushes it over */
    n = cfg("a = 1\n", p, o, sizeof o); CHECK(cfg("a = 1\n", p, o, (size_t)n) == n); CHECK(cfg("a = 1\n", p, o, (size_t)n - 1) == -1);   /* out_cap exact / one short */
    CHECK(cfg("a = 1\n", p, o, 0) == -1);
}

static int ex_set(const char *path, void *ctx)
{
    const char *const *names = (const char *const *)ctx; int i;
    for (i = 0; names[i]; i++) if (strcmp(names[i], path) == 0) return 1;
    return 0;
}

static void test_bak(void)
{
    char o[64]; const char *none[] = { NULL }; const char *a[] = { "s.sav.bak-1", "s.sav.bak-2", NULL };
    const char *gap[] = { "s.sav.bak-1", "s.sav.bak-3", NULL };
    CHECK(setup_bak_name("s.sav", ex_set, none, o, sizeof o) == 1 && strcmp(o, "s.sav.bak-1") == 0);
    CHECK(setup_bak_name("s.sav", ex_set, a, o, sizeof o) == 3 && strcmp(o, "s.sav.bak-3") == 0);
    CHECK(setup_bak_name("s.sav", ex_set, gap, o, sizeof o) == 2 && strcmp(o, "s.sav.bak-2") == 0);
    {   static char names[1000][16]; static const char *list[1001]; int i; char *base = "x";
        for (i = 1; i <= 999; i++) { snprintf(names[i - 1], 16, "x.bak-%d", i); list[i - 1] = names[i - 1]; } list[999] = NULL;
        CHECK(setup_bak_name(base, ex_set, list, o, sizeof o) == -1);                  /* all 999 taken */
        list[998] = NULL; CHECK(setup_bak_name(base, ex_set, list, o, sizeof o) == 999 && strcmp(o, "x.bak-999") == 0);
        list[98] = NULL; /* 1..98 present, 99 free */
        CHECK(setup_bak_name(base, ex_set, list, o, sizeof o) == 99 && strcmp(o, "x.bak-99") == 0);
    }
    CHECK(setup_bak_name("s.sav", ex_set, none, o, 11) == -1);       /* needs 12 */
    CHECK(setup_bak_name("s.sav", ex_set, none, o, 12) == 1);
    CHECK(setup_bak_name("s.sav", ex_set, a, o, 12) == 3);
    CHECK(setup_bak_name("s.sav", NULL, none, o, sizeof o) == -1);
}

#define FUZZ_ROUNDS 10000
static void test_fuzz(void)
{
    int round; uint32_t s, e; uint8_t fnt[96], fat[40]; char in[200], out[300], path[40];
    static const char alpha[] = "rom=[]\r\n x#;\t:a1";
    static const char *paths[] = { "/a.txt", "/d/b.bin", "/d/d/x", "/zz", "/", "", "/d/", "/a.txt/b" };
    for (round = 0; round < FUZZ_ROUNDS; round++) {
        size_t i, fl = rnd() % sizeof fnt, al = rnd() % sizeof fat, n = rnd() % sizeof in;
        for (i = 0; i < fl; i++) fnt[i] = (uint8_t)rnd();
        for (i = 0; i < al; i++) fat[i] = (uint8_t)rnd();
        if (round % 2 == 0 && fl >= sizeof k_fnt) { memcpy(fnt, k_fnt, sizeof k_fnt); for (i = rnd() % 4; i > 0; i--) fnt[rnd() % sizeof k_fnt] = (uint8_t)rnd(); }
        {   int r = setup_nitrofs_find(fnt, (uint32_t)fl, fat, (uint32_t)al, paths[rnd() % 8], &s, &e); CHECK(r >= -1 && r <= 1); }
        for (i = 0; i < 39; i++) path[i] = (char)(1 + rnd() % 126); path[rnd() % 39] = 0;
        (void)setup_nitrofs_find(fnt, (uint32_t)fl, fat, (uint32_t)al, path, &s, &e);
        for (i = 0; i < n; i++) in[i] = (round % 3) ? alpha[rnd() % (sizeof alpha - 1)] : (char)rnd();
        {   long r = setup_config_set_rom(in, n, "sdmc:/p.nds", out, sizeof out - (rnd() % 120));
            CHECK(r == -1 || (r >= 12 && r <= 4096 && (size_t)r <= sizeof out)); }
        /* idempotent on success: a second pass over the output changes nothing */
        {   long r1 = setup_config_set_rom(in, n, "sdmc:/p.nds", out, sizeof out);
            if (r1 > 0) { char again[300]; long r2 = setup_config_set_rom(out, (size_t)r1, "sdmc:/p.nds", again, sizeof again);
                CHECK(r2 == r1 && memcmp(again, out, (size_t)r1) == 0); } }
    }
}

int main(int argc, char **argv)
{
    const char *rom = argc > 1 ? argv[1] : NULL;
    test_sha1(); test_header(rom); test_nitrofs_synth(); test_nitrofs_rom(rom);
    test_save_kind(); test_config(); test_bak(); test_fuzz();
    printf("setup_core_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
