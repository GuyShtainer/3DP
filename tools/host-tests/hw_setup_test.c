/* hw_setup_test.c -- release P4b: hw_setup.c over a fake hw_ctr seam backed by a temp directory ("sdmc:/" -> $ROOT/). One process per case (hw_setup keeps its state in statics).
 * usage: hw_setup_test CASE [ROM]   CASE = resolve | notneeded-rom | notneeded-off | notneeded-bench | import | new | wrongrom | select | home | split | cfgkeys
 *        | pathrt | writerom | caps | longname | folders | badname | importfail | utf8 | cjk | spaces   (RF2; only import/new/wrongrom/select/utf8/cjk/spaces need the ROM)
 * The ROM cases need the player's own dump (roms/platinum-us.nds); without one they print SKIP. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hw_cfg.h"
#include "hw_cfgio.h"
#include "hw_ctr.h"
#include "hw_pure.h"
#include "hw_setup.h"
#include "hw_setup_core.h"

static char g_root[256];
static char g_con[65536];
static int g_hid_alive = 1;
static long g_checks, g_fails;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- the fake seam */
static void hostpath(const char *p, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_root, strncmp(p, "sdmc:/", 6) == 0 ? p + 6 : p);
}

uint64_t hwctr_sys_tick(void) { static uint64_t t; t += 268111856ull / 100ull; return t; }
void hwctr_console_write(const char *text) { strncat(g_con, text, sizeof g_con - strlen(g_con) - 1u); }
static int g_hid_calls, g_hid_die_at;   /* g_hid_die_at > 0: HOME is pressed at that poll */
uint32_t hwctr_hid_wait(int wait, int *alive) { (void)wait; g_hid_calls++; *alive = g_hid_alive && !(g_hid_die_at > 0 && g_hid_calls >= g_hid_die_at); return 0; }

int hwctr_file_size(const char *path)
{
    char h[512];
    struct stat st;

    hostpath(path, h, sizeof h);
    return stat(h, &st) == 0 && S_ISREG(st.st_mode) ? (int)st.st_size : -1;
}

int hwctr_file_read(const char *path, void *buf, uint32_t cap)
{
    char h[512];
    FILE *f;
    size_t n;

    hostpath(path, h, sizeof h);
    f = fopen(h, "rb");
    if (!f) {
        return -1;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}

void hwctr_mkdirs(const char *dir)
{
    char h[512], *p;

    hostpath(dir, h, sizeof h);
    for (p = h + strlen(g_root) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(h, 0777);
            *p = '/';
        }
    }
    mkdir(h, 0777);
}

int hwctr_file_write(const char *path, const void *buf, uint32_t len)
{
    char h[512];
    FILE *f;
    size_t n;

    hostpath(path, h, sizeof h);
    f = fopen(h, "wb");
    if (!f) {
        return -1;
    }
    n = fwrite(buf, 1, len, f);
    return fclose(f) == 0 && n == len ? (int)n : -1;
}

void *hwctr_file_open(const char *path) { char h[512]; hostpath(path, h, sizeof h); return fopen(h, "rb"); }
void *hwctr_file_create(const char *path) { char h[512]; hostpath(path, h, sizeof h); return fopen(h, "wb"); }
int hwctr_file_pread(void *file, uint32_t off, void *dst, uint32_t len)
{
    if (fseek((FILE *)file, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    return (int)fread(dst, 1, len, (FILE *)file);
}
int hwctr_file_append(void *file, const void *buf, uint32_t len) { return (int)fwrite(buf, 1, len, (FILE *)file); }
int hwctr_file_sync(void *file) { return fflush((FILE *)file); }
void hwctr_file_close(void *file) { fclose((FILE *)file); }
int hwctr_file_remove(const char *path) { char h[512]; hostpath(path, h, sizeof h); return unlink(h) == 0 || access(h, F_OK) != 0 ? 0 : -1; }
static unsigned g_rename_fail;   /* bit k set = the (k+1)th rename call after arming fails (after removing its target, as the real SD does) */
static unsigned g_rename_calls;
int hwctr_file_rename(const char *from, const char *to)
{
    char a[512], b[512];

    if (hwctr_file_remove(to) != 0) {
        return -1;
    }
    if (g_rename_calls < 32u && ((g_rename_fail >> g_rename_calls++) & 1u)) {
        return -1;
    }
    hostpath(from, a, sizeof a);
    hostpath(to, b, sizeof b);
    return rename(a, b) == 0 ? 0 : -1;
}

int hwctr_dir_list(const char *dir, HwCtrDirEnt *out, int max, int *skipped)
{
    char h[512];
    DIR *d;
    struct dirent *e;
    int n = 0, lost = 0;

    hostpath(dir, h, sizeof h);
    d = opendir(h);
    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char full[1024];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        if (strlen(e->d_name) >= HWCTR_NAME_MAX || n >= max) {
            lost++;
            continue;
        }
        snprintf(full, sizeof full, "%s/%s", h, e->d_name);
        memset(&out[n], 0, sizeof out[n]);
        strcpy(out[n].name, e->d_name);
        out[n].is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        n++;
    }
    closedir(d);
    if (skipped) {
        *skipped = lost;
    }
    return n;
}

/* ---------------------------------------------------------------- helpers */
static void put(const char *rel, const void *data, size_t len)
{
    char h[512], d[512], *s;
    FILE *f;

    snprintf(d, sizeof d, "sdmc:/%s", rel);
    s = strrchr(d, '/');
    *s = '\0';
    hwctr_mkdirs(d);
    snprintf(h, sizeof h, "%s/%s", g_root, rel);
    f = fopen(h, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

static int slurp(const char *rel, uint8_t *buf, size_t cap)
{
    char p[512];

    snprintf(p, sizeof p, "sdmc:/%s", rel);
    return hwctr_file_read(p, buf, (uint32_t)cap);
}

static int exists(const char *rel) { char p[512]; snprintf(p, sizeof p, "sdmc:/%s", rel); return hwctr_file_size(p) >= 0; }

/* a 0x200-byte header that setup_rom_header_ok accepts (game code CPUE + the header CRC-16): enough for the launch-time header check */
static void put_fake_rom(const char *rel)
{
    uint8_t hdr[0x200];
    uint16_t crc = 0xFFFFu;
    int i, b;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr + 0x0C, "CPUE", 4);
    for (i = 0; i < 0x15E; i++) {
        crc ^= hdr[i];
        for (b = 0; b < 8; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    hdr[0x15E] = (uint8_t)crc; hdr[0x15F] = (uint8_t)(crc >> 8);
    put(rel, hdr, sizeof hdr);
}

static uint8_t g_sav[524288];

static void make_save(uint8_t seed) { size_t i; for (i = 0; i < sizeof g_sav; i++) g_sav[i] = (uint8_t)(i * 7u + seed); }

/* a tapped key at step t: the script the app reads (frames below the #game marker) */
static void script(const char *keys)
{
    char buf[2048] = "";
    size_t n;

    strcat(buf, keys);
    strcat(buf, "#game\n99999 NONE\n");
    n = strlen(buf);
    put("3ds/3DP/input.txt", buf, n);
}

static void copy_rom(const char *src, const char *rel, uint64_t flip)
{
    char h[512];
    FILE *in = fopen(src, "rb"), *out;
    static uint8_t buf[1 << 20];
    size_t n;
    uint64_t off = 0;

    snprintf(h, sizeof h, "%s/%s", g_root, rel);
    hwctr_mkdirs("sdmc:/roms");
    out = fopen(h, "wb");
    if (!in || !out) {
        printf("FAIL: cannot copy the ROM\n");
        exit(1);
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (flip && off <= flip && off + n > flip) {
            buf[flip - off] ^= 1u;   /* a byte inside the first table file (flip_offset): the check must refuse, with the "differs" reason */
        }
        fwrite(buf, 1, n, out);
        off += n;
    }
    fclose(in);
    fclose(out);
}

/* the ROM offset of a byte in the middle of the first file the table names (found through the ROM's own file system), or 0 */
static uint64_t flip_offset(const char *rom)
{
    static uint8_t fnt[65536], fat[65536];
    uint8_t hdr[0x200];
    uint32_t fl, tl, st, en;
    FILE *f = fopen(rom, "rb");
    uint64_t at = 0;

    if (!f || fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        if (f) fclose(f);
        return 0;
    }
    fl = hdr[0x44] | (hdr[0x45] << 8) | (hdr[0x46] << 16); tl = hdr[0x4c] | (hdr[0x4d] << 8) | (hdr[0x4e] << 16);
    if (fl <= sizeof fnt && tl <= sizeof fat) {
        uint32_t fo = hdr[0x40] | (hdr[0x41] << 8) | (hdr[0x42] << 16) | ((uint32_t)hdr[0x43] << 24), to = hdr[0x48] | (hdr[0x49] << 8) | (hdr[0x4a] << 16) | ((uint32_t)hdr[0x4b] << 24);

        if (fseek(f, (long)fo, SEEK_SET) == 0 && fread(fnt, 1, fl, f) == fl && fseek(f, (long)to, SEEK_SET) == 0 && fread(fat, 1, tl, f) == tl &&
            setup_nitrofs_find(fnt, fl, fat, tl, setup_fs_table[0].rom_path, &st, &en) == 1 && en > st) {
            at = st + (uint64_t)(en - st) / 2u;
        }
    }
    fclose(f);
    return at;
}

static void tap(char *buf, int *t, const char *key)
{
    char l[64];

    snprintf(l, sizeof l, "%d %s\n%d NONE\n", *t, key, *t + 2);
    strcat(buf, l);
    *t += 6;
}

static void make_script(const char *taps)   /* taps: space-separated key names */
{
    char buf[2048] = "0 NONE\n", copy[256], *tok;
    int t = 6;

    snprintf(copy, sizeof copy, "%s", taps);
    for (tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
        tap(buf, &t, tok);
    }
    script(buf);
}

/* ---------------------------------------------------------------- cases */
static void case_resolve(void)
{
    HwCfg c;
    char out[96];

    hwcfg_defaults(&c);
    check(hw_setup_resolve_rom(&c, out, sizeof out) == 0 && strcmp(out, HWCTR_ROM_FILE) == 0, "no ROM anywhere: 0 and the fixed name");
    put("3ds/3DP/platinum.nds", "x", 1);
    check(hw_setup_resolve_rom(&c, out, sizeof out) == 1 && strcmp(out, HWCTR_ROM_FILE) == 0, "the fixed name exists: 1");
    c.rom_set = 1;
    snprintf(c.rom, sizeof c.rom, "sdmc:/nope.nds");
    check(hw_setup_resolve_rom(&c, out, sizeof out) == 1 && strcmp(out, HWCTR_ROM_FILE) == 0, "rom= missing: falls back to the fixed name");
    put("roms/a.nds", "y", 1);
    snprintf(c.rom, sizeof c.rom, "sdmc:/roms/a.nds");
    check(hw_setup_resolve_rom(&c, out, sizeof out) == 1 && strcmp(out, "sdmc:/roms/a.nds") == 0, "rom= exists: that path wins");
}

static void case_notneeded(const char *which)
{
    if (!strcmp(which, "rom")) {
        put_fake_rom("3ds/3DP/platinum.nds");
    } else if (!strcmp(which, "off")) {
        put("3ds/3DP/config.ini", "setup = 0\n", 10);
    } else {
        put("3ds/3DP/bench.txt", "x\n", 2);
    }
    check(hw_setup_run() == HWSETUP_NOT_NEEDED, "setup is not needed");
    check(g_con[0] == '\0', "nothing was written to the console");
}

static int rom_case_prepare(const char *rom, int flip)
{
    if (!rom || !*rom) {
        printf("SKIP: no ROM\n");
        return 0;
    }
    copy_rom(rom, "roms/x.nds", flip ? flip_offset(rom) : 0u);
    make_save(3);
    put("saves/x.sav", g_sav, sizeof g_sav);
    return 1;
}

static void case_import(const char *rom)
{
    static uint8_t got[524288];
    uint8_t cfg[512] = "";

    if (!rom_case_prepare(rom, 0)) return;
    make_script("A DOWN A A");
    check(hw_setup_run() == HWSETUP_DONE, "import ends done");
    check(slurp("3ds/3DP/platinum.sav", got, sizeof got) == (int)sizeof got && memcmp(got, g_sav, sizeof got) == 0, "platinum.sav is a byte copy");
    check(slurp("saves/x.sav", got, sizeof got) == (int)sizeof got && memcmp(got, g_sav, sizeof got) == 0, "the source save is untouched");
    check(slurp("3ds/3DP/config.ini", cfg, sizeof cfg - 1u) > 0 && strstr((char *)cfg, "rom = sdmc:/roms/x.nds") != NULL, "config.ini has the rom line");
    check(!exists("3ds/3DP/platinum.sav.import"), "no .import leftover");
    check(strstr(g_con, "Checking your ROM") != NULL, "the check screen was shown");
}

static void case_new(const char *rom)
{
    static uint8_t got[524288];
    uint8_t tmp[4096];

    if (!rom_case_prepare(rom, 0)) return;
    put("3ds/3DP/platinum.sav", g_sav, sizeof g_sav);
    memset(tmp, 0xAA, sizeof tmp);
    put("3ds/3DP/platinum.sav.tmp", tmp, sizeof tmp);
    put("3ds/3DP/platinum.sav.orig", g_sav, sizeof g_sav);
    put("3ds/3DP/platinum.sav.bak-1", "old", 3);   /* the first free number is 2 */
    make_script("A DOWN A A A");   /* ROM; down to [ Start a new game ] (row 0 is Keep); confirm; done */
    check(hw_setup_run() == HWSETUP_DONE, "new ends done");
    check(!exists("3ds/3DP/platinum.sav") && !exists("3ds/3DP/platinum.sav.tmp") && !exists("3ds/3DP/platinum.sav.orig"), "sav, .tmp and .orig are gone");
    check(slurp("3ds/3DP/platinum.sav.bak-2", got, sizeof got) == (int)sizeof got && memcmp(got, g_sav, sizeof got) == 0, "the old save is .bak-2");
    check(slurp("3ds/3DP/platinum.sav.tmp.bak-1", got, sizeof tmp) == (int)sizeof tmp && got[0] == 0xAA, "the stranded .tmp is .tmp.bak-1");
    check(exists("3ds/3DP/platinum.sav.orig.bak-1"), "the .orig is .orig.bak-1");
    check(slurp("3ds/3DP/platinum.sav.bak-1", got, 3) == 3, "the earlier .bak-1 is untouched");
}

static void case_wrongrom(const char *rom)
{
    uint8_t cfg[256];

    if (!rom_case_prepare(rom, 1)) return;
    make_script("A A");
    check(hw_setup_run() == HWSETUP_QUIT, "a wrong ROM ends in quit (the script ended)");
    check(slurp("3ds/3DP/config.ini", cfg, sizeof cfg) < 0 || strstr((char *)cfg, "rom = ") == NULL, "no rom line was written");
    check(!exists("3ds/3DP/platinum.sav"), "no save was written");
    check(strstr(g_con, "was not accepted") != NULL, "the refusal was shown");
    check(strstr(g_con, "a game file differs from the expected data") != NULL, "the refusal REASON is the hash mismatch (not any other refusal)");
}

static void case_select(const char *rom)
{
    static uint8_t got[524288];
    uint8_t cfg[256];

    if (!rom_case_prepare(rom, 0)) return;
    put("3ds/3DP/config.ini", "rom = sdmc:/roms/x.nds\n", 23);
    put("3ds/3DP/platinum.sav", g_sav, sizeof g_sav);
    {
        char buf[2048] = "0 SELECT\n2 NONE\n";
        int t = 6;
        tap(buf, &t, "START");
        tap(buf, &t, "START");
        tap(buf, &t, "A");
        script(buf);
    }
    check(hw_setup_run() == HWSETUP_DONE, "SELECT at launch opens setup, START keeps both, ends done");
    check(slurp("3ds/3DP/config.ini", cfg, sizeof cfg - 1u) == 23, "config.ini unchanged");
    check(slurp("3ds/3DP/platinum.sav", got, sizeof got) == (int)sizeof got && memcmp(got, g_sav, sizeof got) == 0, "the current save is untouched");
    check(!exists("3ds/3DP/platinum.sav.bak-1"), "no .bak file");
}

static void case_home(void)
{
    g_hid_alive = 0;
    check(hw_setup_run() == HWSETUP_QUIT, "HOME ends in quit");
}

static void case_split(void)
{
    const char *a = "# s\n0 A\n#game\n60 B\n";
    const char *b = "#game\r\n60 B\n";
    const char *c = "0 A\n#gamex\n60 B\n";
    const char *d = "#game";

    check(hwin_script_split(a, (uint32_t)strlen(a)) == (uint32_t)(strstr(a, "60 B") - a), "split: offset after the marker line");
    check(hwin_script_split(b, (uint32_t)strlen(b)) == (uint32_t)(strstr(b, "60 B") - b), "split: a CR before the newline is allowed");
    check(hwin_script_split(c, (uint32_t)strlen(c)) == 0, "split: #gamex is not the marker");
    check(hwin_script_split(d, (uint32_t)strlen(d)) == (uint32_t)strlen(d), "split: marker at the very end");
    check(hwin_script_split("0 A\n", 4) == 0, "split: no marker is 0");
}

static uint32_t parse(HwCfg *c, const char *t)
{
    uint32_t skipped = 0;

    return hwcfg_parse(c, t, strlen(t), &skipped);
}

static void case_cfgkeys(void)
{
    HwCfg c;
    char big[200];

    hwcfg_defaults(&c);
    check(parse(&c, "rom = sdmc:/roms/a.nds\nsetup = 0\n") >= 2 && c.rom_set && strcmp(c.rom, "sdmc:/roms/a.nds") == 0 && c.setup_off == 1, "rom and setup=0 parse");
    hwcfg_defaults(&c);
    check(parse(&c, "setup = 1\n") >= 1 && c.setup_off == 0 && !c.rom_set, "setup=1 keeps setup on");
    hwcfg_defaults(&c);
    memset(big, 'a', 99);
    big[99] = '\0';
    {
        char line[256];
        snprintf(line, sizeof line, "rom = %s\n", big);
        parse(&c, line);
    }
    check(!c.rom_set, "a rom path over 80 bytes is ignored");
    hwcfg_defaults(&c);
    parse(&c, "rom = \nsetup = 7\n");
    check(!c.rom_set && c.setup_off == 0, "an empty rom and a bad setup value are ignored");
}


/* ---------------------------------------------------------------- RF2 cases */
static void read_cfg(HwCfg *c)
{
    static char t[4200];
    int n = hwctr_file_read("sdmc:/3ds/3DP/config.ini", t, sizeof t - 1u);

    hwcfg_defaults(c);
    if (n > 0) {
        (void)hwcfg_parse(c, t, (size_t)n, NULL);
    }
}

/* H1: every accepted path writes, swaps and reads back exactly through the real parser; the refused ones are refused with a reason, and no file is left half-written */
static void case_pathrt(void)
{
    static const char *const good[] = {
        "sdmc:/roms/Pok\xc3\xa9mon Platinum.nds",                                   /* e-acute */
        "sdmc:/roms/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3 \xe3\x83\x97\xe3\x83\xa9.nds",   /* CJK */
        "sdmc:/roms/Platinum .nds",                                                  /* a space before the extension */
        "sdmc:/roms/ Platinum.nds",                                                  /* a space at the start of the name */
        "sdmc:/roms/a=b#c;d.nds",
    };
    static const char *const bad[] = { "sdmc:/roms/a\x7f.nds", "sdmc:/roms/a\x01.nds", "sdmc:/roms/p.nds ", " sdmc:/roms/p.nds", "" };
    HwCfg c;
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        put("3ds/3DP/config.ini", "owner_name = Guy\n", 17);
        check(hw_setup_write_rom(good[i]) == 0, "a UTF-8 / blank-bearing path is written");
        read_cfg(&c);
        check(c.rom_set && strcmp(c.rom, good[i]) == 0, "the path reads back byte for byte through hwcfg_parse");
        check(!exists("3ds/3DP/config.ini.tmp") && !exists("3ds/3DP/config.ini.bak"), "no .tmp / .bak left after a good write");
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        put("3ds/3DP/config.ini", "owner_name = Guy\n", 17);
        check(hw_setup_write_rom(bad[i]) == -1 && hw_setup_last_why()[0] != '\0', "an unholdable path is refused with a reason");
        read_cfg(&c);
        check(!c.rom_set && exists("3ds/3DP/config.ini"), "and config.ini is left alone");
    }
    {   /* the parser itself: 0x7F and controls are not accepted, 0x80+ is */
        const char *t = "rom = sdmc:/a\x7f.nds\n";
        hwcfg_defaults(&c);
        (void)hwcfg_parse(&c, t, strlen(t), NULL);
        check(!c.rom_set, "hwcfg: a 0x7F byte in rom = is ignored");
        t = "rom = sdmc:/\xc3\xa9.nds\n";
        hwcfg_defaults(&c);
        (void)hwcfg_parse(&c, t, strlen(t), NULL);
        check(c.rom_set && strcmp(c.rom, "sdmc:/\xc3\xa9.nds") == 0, "hwcfg: a UTF-8 rom = path is accepted");
    }
}

static void rm_rel(const char *rel)
{
    char p[512];

    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    (void)unlink(p);
}

static const char *last_of(const char *hay, const char *needle)   /* the last occurrence, or NULL */
{
    const char *at = NULL, *f = hay;

    while ((f = strstr(f, needle)) != NULL) {
        at = f++;
    }
    return at;
}

/* M2: a failing swap never leaves the player without a config.ini */
static void case_writerom(void)
{
    static uint8_t t[4200];
    HwCfg c;
    const char *old = "owner_name = Guy\nlanguage = 1\n[save:1:2]\nmac = 00:11:22:33:44:55\n";

    /* a: the second rename (tmp -> config.ini) fails: the OLD file is back, whole */
    put("3ds/3DP/config.ini", old, strlen(old));
    g_rename_fail = 1u << 1; g_rename_calls = 0;
    check(hw_setup_write_rom("sdmc:/roms/a.nds") == -1, "a failing swap is reported");
    g_rename_fail = 0;
    check(slurp("3ds/3DP/config.ini", t, sizeof t) == (int)strlen(old) && memcmp(t, old, strlen(old)) == 0, "failed swap: config.ini is the old file, byte for byte");
    /* b: the swap AND the restore fail (card trouble): no config.ini, but .tmp + .bak hold both; the next launch gets one back */
    g_rename_fail = (1u << 1) | (1u << 2); g_rename_calls = 0;
    check(hw_setup_write_rom("sdmc:/roms/b.nds") == -1, "a swap that cannot even restore is reported");
    g_rename_fail = 0;
    check(!exists("3ds/3DP/config.ini") && exists("3ds/3DP/config.ini.tmp") && exists("3ds/3DP/config.ini.bak"), "worst case: config.ini absent, .tmp and .bak both complete");
    put("3ds/3DP/bench.txt", "x\n", 2);   /* a bench launch: hw_setup_run reads config.ini (and so recovers it), then passes straight through */
    check(hw_setup_run() == HWSETUP_NOT_NEEDED, "the next launch runs");
    read_cfg(&c);
    check(c.rom_set && strcmp(c.rom, "sdmc:/roms/b.nds") == 0, "the next launch promoted the new .tmp");
    check(slurp("3ds/3DP/config.ini", t, sizeof t) > 0 && strstr((char *)t, "[save:1:2]") != NULL, "and the identity section survived");
    /* c: only the .bak is left: it is promoted */
    rm_rel("3ds/3DP/config.ini");
    rm_rel("3ds/3DP/config.ini.tmp");
    put("3ds/3DP/config.ini.bak", old, strlen(old));
    (void)hw_setup_run();
    check(slurp("3ds/3DP/config.ini", t, sizeof t) == (int)strlen(old) && memcmp(t, old, strlen(old)) == 0, "only a .bak left: it becomes config.ini");
    /* d: the first rename (config.ini -> .bak) fails: config.ini is untouched */
    g_rename_fail = 1u; g_rename_calls = 0;
    check(hw_setup_write_rom("sdmc:/roms/c.nds") == -1, "a failing first rename is reported");
    g_rename_fail = 0;
    check(slurp("3ds/3DP/config.ini", t, sizeof t) == (int)strlen(old) && memcmp(t, old, strlen(old)) == 0, "config.ini untouched when the first rename fails");
    /* e: a good write leaves no .bak / .tmp */
    check(hw_setup_write_rom("sdmc:/roms/d.nds") == 0 && !exists("3ds/3DP/config.ini.bak") && !exists("3ds/3DP/config.ini.tmp"), "a good write cleans up");
}

/* L3: a config near the 4096-byte cap gets a clear on-screen reason, never a silent failure */
static void case_caps(void)
{
    static char big[4200];
    size_t n;
    HwCfg c;

    memset(big, 'x', sizeof big);
    n = 4060;   /* leaves 35 bytes: "rom = sdmc:/roms/Platinum-long-name.nds\n" does not fit */
    big[n - 1] = '\n';
    put("3ds/3DP/config.ini", big, n);
    check(hw_setup_write_rom("sdmc:/roms/Platinum-long-name.nds") == -1, "a nearly full config refuses the ROM line");
    check(strstr(hw_setup_last_why(), "nearly full (4060 of 4095 bytes)") != NULL, "the reason names the sizes");
    check(strstr(hw_setup_last_why(), "Delete old [save:...] sections") != NULL, "and says what to do");
    check(hwctr_file_size("sdmc:/3ds/3DP/config.ini") == (int)n, "config.ini is untouched");
    n = 4096;
    put("3ds/3DP/config.ini", big, n);
    check(hw_setup_write_rom("sdmc:/roms/a.nds") == -1 && strstr(hw_setup_last_why(), "config.ini is 4096 bytes (the game reads at most 4095)") != NULL, "a config over the cap is refused with its size");
    n = 4000;   /* still room: written */
    big[n - 1] = '\n';
    put("3ds/3DP/config.ini", big, n);
    check(hw_setup_write_rom("sdmc:/roms/a.nds") == 0, "a config with room is written");
    read_cfg(&c);
    check(c.rom_set, "and reads back");
}

/* M1: the scan caps are on screen */
static void mk_files(const char *dirrel, int count, const char *prefix)
{
    char rel[512];
    int i;

    for (i = 0; i < count; i++) {
        snprintf(rel, sizeof rel, "%s/%s%04d.txt", dirrel, prefix, i);
        put(rel, "x", 1);
    }
}

static void quit_script(void)
{
    make_script("B B");
}

static void case_longname(void)
{
    char name[200];

    memset(name, 'n', 100);
    snprintf(name + 100, sizeof name - 100, ".nds");
    put("roms/ok.txt", "x", 1);
    {   char rel[400]; snprintf(rel, sizeof rel, "roms/%s", name); put(rel, "x", 1); }
    quit_script();
    check(hw_setup_run() == HWSETUP_QUIT, "no ROM: the script ends in quit");
    check(strstr(g_con, "1 files skipped (name too long / too many files)") != NULL, "a name over the limit is reported on the ROM menu");
    check(strstr(g_con, "(nothing found)") == NULL, "never a bare (nothing found)");
}

static void case_folders(void)
{
    mk_files("roms/many", 512 + 37, "f");
    quit_script();
    check(hw_setup_run() == HWSETUP_QUIT, "no ROM: the script ends in quit");
    check(strstr(g_con, "37 files skipped (name too long / too many files)") != NULL, "a folder past 512 entries says how many were skipped (37)");
    check(strstr(g_con, "(nothing found)") == NULL, "never a bare (nothing found)");
}

static void case_queue(void)
{
    char path[256];
    int i;

    for (i = 0; i < 520; i++) {   /* 520 sub-folders of one folder: the 512-folder queue cannot hold them */
        snprintf(path, sizeof path, "sdmc:/dirs/d%03d", i);
        hwctr_mkdirs(path);
    }
    quit_script();
    check(hw_setup_run() == HWSETUP_QUIT, "no ROM: the script ends in quit");
    check(strstr(g_con, "files skipped (name too long / too many files)") != NULL, "a full folder queue is reported too");
    check(strstr(g_con, "(nothing found)") == NULL, "never a bare (nothing found)");
}

static void case_badname(void)
{
    /* a Platinum-looking file with an unholdable name is counted, not hidden: needs only a header, so build a fake one in place */
    uint8_t hdr[0x200];
    uint16_t crc = 0xFFFFu;
    int i, b;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr + 0x0C, "CPUE", 4);
    for (i = 0; i < 0x15E; i++) {
        crc ^= hdr[i];
        for (b = 0; b < 8; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    hdr[0x15E] = (uint8_t)crc; hdr[0x15F] = (uint8_t)(crc >> 8);
    put("roms/bad\x7f" "name.nds", hdr, sizeof hdr);
    quit_script();
    check(hw_setup_run() == HWSETUP_QUIT, "no usable ROM: quit");
    check(strstr(g_con, "1 ROM names use characters config.ini cannot hold") != NULL, "an unholdable ROM name is reported");
    check(strstr(g_con, "(nothing found)") == NULL, "never a bare (nothing found)");
}

/* L1: a failed import puts the old save back and the menu forgets nothing */
static void case_importfail(void)
{
    static uint8_t got[524288], old[524288];
    uint8_t src[524288];
    char buf[2048] = "0 SELECT\n2 NONE\n";
    int t = 6;
    size_t i;

    put_fake_rom("roms/a.nds");   /* usable: the launch-time header check passes */
    put("3ds/3DP/config.ini", "rom = sdmc:/roms/a.nds\n", 23);
    for (i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 5u + 1u), old[i] = (uint8_t)(i * 3u + 9u);
    put("saves/x.sav", src, sizeof src);
    put("3ds/3DP/platinum.sav", old, sizeof old);
    tap(buf, &t, "START");   /* keep the ROM */
    tap(buf, &t, "DOWN");
    tap(buf, &t, "DOWN");    /* rows: keep, new, saves/x.sav */
    tap(buf, &t, "A");       /* import saves/x.sav */
    tap(buf, &t, "A");       /* confirm: the swap then fails */
    tap(buf, &t, "A");       /* dismiss the message; the script then ends */
    script(buf);
    g_rename_fail = 1u << 1; g_rename_calls = 0;   /* call 0 = platinum.sav -> .bak-1, call 1 = .import -> platinum.sav (fails) */
    check(hw_setup_run() == HWSETUP_QUIT, "the script ends in quit after the failure message");
    g_rename_fail = 0;
    check(slurp("3ds/3DP/platinum.sav", got, sizeof got) == (int)sizeof got && memcmp(got, old, sizeof got) == 0, "the old save is back, byte for byte");
    check(!exists("3ds/3DP/platinum.sav.bak-1") && !exists("3ds/3DP/platinum.sav.import"), "no stray .bak-1 / .import");
    check(strstr(g_con, "your current save is unchanged") != NULL, "the message says the save is unchanged");
    check(strstr(g_con, "START/B: keep the current save") != NULL, "and the menu still offers keeping it");
}

static void case_importfail2(void)
{
    static uint8_t got[524288], old[524288];
    uint8_t src[524288];
    char buf[2048] = "0 SELECT\n2 NONE\n";
    int t = 6;
    size_t i;

    put_fake_rom("roms/a.nds");
    put("3ds/3DP/config.ini", "rom = sdmc:/roms/a.nds\n", 23);
    for (i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 5u + 1u), old[i] = (uint8_t)(i * 3u + 9u);
    put("saves/x.sav", src, sizeof src);
    put("3ds/3DP/platinum.sav", old, sizeof old);
    tap(buf, &t, "START");
    tap(buf, &t, "DOWN");
    tap(buf, &t, "DOWN");
    tap(buf, &t, "A");
    tap(buf, &t, "A");
    tap(buf, &t, "A");
    script(buf);
    g_rename_fail = (1u << 1) | (1u << 2); g_rename_calls = 0;   /* the swap fails, and so does the restore */
    check(hw_setup_run() == HWSETUP_QUIT, "the script ends in quit");
    g_rename_fail = 0;
    check(!exists("3ds/3DP/platinum.sav") && slurp("3ds/3DP/platinum.sav.bak-1", got, sizeof got) == (int)sizeof got && memcmp(got, old, sizeof got) == 0, "the old save is kept whole as .bak-1");
    check(strstr(g_con, "kept as sdmc:/3ds/3DP/platinum.sav.bak-1") != NULL, "the message names the backup");
    check(last_of(g_con, "Choose a save") != NULL && strstr(last_of(g_con, "Choose a save"), "keep the current save") == NULL, "the last save menu no longer offers keeping a save that is gone (have is recomputed)");
}

/* S4: every crash window of the ONE config writer (hw_sys uses it too): whatever renames fail, after the next launch's recovery config.ini is whole, old or new */
static void case_cfgio(void)
{
    static uint8_t t[4200];
    const char *old = "owner_name = Guy\n[save:1:2]\nmac = 00:11:22:33:44:55\n", *nw = "owner_name = Guy\n[save:1:2]\nmac = 00:11:22:33:44:55\n[save:3:4]\nmac = 66:77:88:99:aa:bb\n";
    unsigned mask;
    int r, n;

    for (mask = 0; mask < 16u; mask++) {
        put("3ds/3DP/config.ini", old, strlen(old));
        rm_rel("3ds/3DP/config.ini.tmp");
        rm_rel("3ds/3DP/config.ini.bak");
        g_rename_fail = mask; g_rename_calls = 0;
        r = hwcfgio_write(nw, strlen(nw));
        g_rename_fail = 0;
        (void)hwcfgio_recover();   /* the next launch */
        n = slurp("3ds/3DP/config.ini", t, sizeof t);
        check(n == (int)strlen(old) && memcmp(t, old, (size_t)n) == 0 ? 1 : (n == (int)strlen(nw) && memcmp(t, nw, (size_t)n) == 0), "config.ini is whole (old or new) after the crash window");
        check(r != 0 || (n == (int)strlen(nw) && memcmp(t, nw, (size_t)n) == 0), "a write that reported success left the new file");
    }
    /* no config.ini at all (first launch): the defaults write is atomic too */
    rm_rel("3ds/3DP/config.ini");
    check(hwcfgio_write(nw, strlen(nw)) == 0 && exists("3ds/3DP/config.ini") && !exists("3ds/3DP/config.ini.tmp") && !exists("3ds/3DP/config.ini.bak"), "a first write needs no .bak and leaves no .tmp");
    check(hwcfgio_write(NULL, 0) == -1 && hwcfgio_write(nw, 5000) == -1, "bad arguments are refused");
}

/* L2: HOME during the long ROM check is served inside the check, and nothing is written */
static void case_homecheck(const char *rom)
{
    HwCfg c;

    if (!rom_case_prepare(rom, 0)) return;
    make_script("A A A");
    g_hid_die_at = 30;   /* the menu needs ~8 polls; the check polls every 16th chunk */
    check(hw_setup_run() == HWSETUP_QUIT, "HOME during the check ends in quit");
    check(g_hid_calls < 200, "and it stopped inside the check (not after reading the whole ROM)");
    read_cfg(&c);
    check(!c.rom_set && !exists("3ds/3DP/platinum.sav"), "nothing was written");
}

/* S1-S3 share a start: a usable (header-valid) ROM named in config.ini, SELECT at launch, START keeps it, then the save menu */
static void save_menu_start(char *buf, int *t)
{
    put_fake_rom("roms/a.nds");
    put("3ds/3DP/config.ini", "rom = sdmc:/roms/a.nds\n", 23);
    strcpy(buf, "0 SELECT\n2 NONE\n");
    *t = 6;
    tap(buf, t, "START");
}

/* S1: with a save present the first row is Keep and is the default: one A press changes nothing */
static void case_s1keep(void)
{
    static uint8_t got[524288];
    char buf[2048];
    int t;

    save_menu_start(buf, &t);
    make_save(5);
    put("3ds/3DP/platinum.sav", g_sav, sizeof g_sav);
    tap(buf, &t, "A");   /* the default row */
    tap(buf, &t, "A");   /* done */
    script(buf);
    check(hw_setup_run() == HWSETUP_DONE, "an A press on the default row ends done");
    check(slurp("3ds/3DP/platinum.sav", got, sizeof got) == (int)sizeof got && memcmp(got, g_sav, sizeof got) == 0, "the save is untouched");
    check(!exists("3ds/3DP/platinum.sav.bak-1"), "no backup was made");
    check(strstr(g_con, "> [ Keep the current save ]") != NULL, "the cursor starts on Keep");
}

/* S1: start-new asks first; B cancels and nothing moves; A moves the save to the named .bak-N */
static void case_s1confirm(void)
{
    char buf[2048];
    int t;

    save_menu_start(buf, &t);
    make_save(5);
    put("3ds/3DP/platinum.sav", g_sav, sizeof g_sav);
    tap(buf, &t, "DOWN");
    tap(buf, &t, "A");   /* start a new game: asks */
    tap(buf, &t, "B");   /* no */
    tap(buf, &t, "START");   /* back at the menu: keep */
    tap(buf, &t, "A");
    script(buf);
    check(hw_setup_run() == HWSETUP_DONE, "B on the confirmation returns to the menu");
    check(exists("3ds/3DP/platinum.sav") && !exists("3ds/3DP/platinum.sav.bak-1"), "nothing was moved");
    check(strstr(g_con, "Your current save will be moved to\nplatinum.sav.bak-1") != NULL, "the confirmation names the backup");
}

/* S1: a backup is listed and restoring it moves the current save aside first */
static void case_s1restore(void)
{
    static uint8_t got[524288], cur[524288], old[524288];
    char buf[2048];
    int t;
    size_t i;

    save_menu_start(buf, &t);
    for (i = 0; i < sizeof cur; i++) cur[i] = (uint8_t)(i * 3u + 1u), old[i] = (uint8_t)(i * 7u + 2u);
    put("3ds/3DP/platinum.sav", cur, sizeof cur);
    put("3ds/3DP/platinum.sav.bak-1", old, sizeof old);
    tap(buf, &t, "DOWN");
    tap(buf, &t, "DOWN");   /* rows: keep, new, [restore] bak-1 */
    tap(buf, &t, "A");
    tap(buf, &t, "A");      /* confirm */
    tap(buf, &t, "A");      /* done */
    script(buf);
    check(hw_setup_run() == HWSETUP_DONE, "restore ends done");
    check(strstr(g_con, "[restore] platinum.sav.bak-1") != NULL, "the backup is listed");
    check(slurp("3ds/3DP/platinum.sav", got, sizeof got) == (int)sizeof got && memcmp(got, old, sizeof got) == 0, "platinum.sav is the restored backup");
    check(slurp("3ds/3DP/platinum.sav.bak-2", got, sizeof got) == (int)sizeof got && memcmp(got, cur, sizeof got) == 0, "the save that was current is now .bak-2");
    check(slurp("3ds/3DP/platinum.sav.bak-1", got, sizeof got) == (int)sizeof got && memcmp(got, old, sizeof got) == 0, "the restored backup stays");
}

/* S2: a header that is not Platinum's opens setup at every launch, with the reason; a good one passes straight through */
static void case_s2badrom(void)
{
    put("3ds/3DP/platinum.nds", "this is not a rom, just some bytes that exist\n", 46);
    quit_script();
    check(hw_setup_run() == HWSETUP_QUIT, "an existing but wrong file opens setup (the script then ends)");
    check(strstr(g_con, "is not a readable Platinum (USA) ROM") != NULL, "the reason is on screen");
    check(strstr(g_con, "START/B: keep the current ROM") == NULL, "and keeping it is not offered");
}

/* S3: a save that is not exactly 512 KB is counted on the save menu */
static void case_s3odd(void)
{
    static uint8_t one[1048576];
    char buf[2048];
    int t;

    save_menu_start(buf, &t);
    tap(buf, &t, "B");
    script(buf);
    put("saves/padded.sav", one, sizeof one);
    check(hw_setup_run() == HWSETUP_QUIT, "no save and the script ends in quit");
    check(strstr(g_con, "1 save files skipped (not exactly 512 KB") != NULL, "the padded dump is counted on the save menu");
}

/* H1 with the real ROM: a dump named with non-ASCII bytes or blanks is found, checked, written, re-read, and the flow ends done */
static void case_namedrom(const char *rom, const char *rel, const char *want)
{
    char line[256];
    HwCfg c;

    if (!rom || !*rom) {
        printf("SKIP: no ROM\n");
        return;
    }
    copy_rom(rom, rel, 0u);
    make_save(3);
    put("saves/x.sav", g_sav, sizeof g_sav);
    make_script("A A A");
    check(hw_setup_run() == HWSETUP_DONE, "a ROM with this name goes through setup to done");
    read_cfg(&c);
    snprintf(line, sizeof line, "%s", want);
    check(c.rom_set && strcmp(c.rom, line) == 0, "config.ini reads back with the exact path");
    check(hw_setup_resolve_rom(&c, line, sizeof line) == 1 && strcmp(line, want) == 0, "and the card bus would open that file");
}

int main(int argc, char **argv)
{
    const char *cs = argc > 1 ? argv[1] : "";
    const char *rom = argc > 2 ? argv[2] : "";
    char tmpl[] = "/tmp/hw_setup_test_XXXXXX";
    char cmd[300];

    if (!mkdtemp(tmpl)) {
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    hwctr_mkdirs("sdmc:/3ds/3DP");
    if (!strcmp(cs, "resolve")) case_resolve();
    else if (!strncmp(cs, "notneeded-", 10)) case_notneeded(cs + 10);
    else if (!strcmp(cs, "import")) case_import(rom);
    else if (!strcmp(cs, "new")) case_new(rom);
    else if (!strcmp(cs, "wrongrom")) case_wrongrom(rom);
    else if (!strcmp(cs, "select")) case_select(rom);
    else if (!strcmp(cs, "home")) case_home();
    else if (!strcmp(cs, "split")) case_split();
    else if (!strcmp(cs, "cfgkeys")) case_cfgkeys();
    else if (!strcmp(cs, "pathrt")) case_pathrt();
    else if (!strcmp(cs, "writerom")) case_writerom();
    else if (!strcmp(cs, "caps")) case_caps();
    else if (!strcmp(cs, "longname")) case_longname();
    else if (!strcmp(cs, "folders")) case_folders();
    else if (!strcmp(cs, "s1keep")) case_s1keep();
    else if (!strcmp(cs, "s1confirm")) case_s1confirm();
    else if (!strcmp(cs, "s1restore")) case_s1restore();
    else if (!strcmp(cs, "s2badrom")) case_s2badrom();
    else if (!strcmp(cs, "s3odd")) case_s3odd();
    else if (!strcmp(cs, "cfgio")) case_cfgio();
    else if (!strcmp(cs, "homecheck")) case_homecheck(rom);
    else if (!strcmp(cs, "queue")) case_queue();
    else if (!strcmp(cs, "badname")) case_badname();
    else if (!strcmp(cs, "importfail")) case_importfail();
    else if (!strcmp(cs, "importfail2")) case_importfail2();
    else if (!strcmp(cs, "utf8")) case_namedrom(rom, "roms/Pok\xc3\xa9mon Platinum.nds", "sdmc:/roms/Pok\xc3\xa9mon Platinum.nds");
    else if (!strcmp(cs, "cjk")) case_namedrom(rom, "roms/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3 \xe3\x83\x97\xe3\x83\xa9.nds", "sdmc:/roms/\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3 \xe3\x83\x97\xe3\x83\xa9.nds");
    else if (!strcmp(cs, "spaces")) case_namedrom(rom, "roms/ Platinum .nds", "sdmc:/roms/ Platinum .nds");
    else {
        printf("unknown case\n");
        return 2;
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
    (void)system(cmd);
    printf("hw_setup_test %s: %ld checks, %ld failed\n", cs, g_checks, g_fails);
    return g_fails ? 1 : 0;
}
