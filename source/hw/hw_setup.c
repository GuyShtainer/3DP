/* hw_setup.c -- the first-launch setup screen (release P4b). See hw_setup.h. Our own text and code only.
 *
 * Flow: [ROM list -> checking -> ok / refused] -> config.ini gets `rom =` -> [save list: import / start new] -> done. Every screen is text written through hwctr_console_write
 * (never hwctr_console_notice and never a raw console init: the render thread's bottom-screen policy stays untouched, so the first game frame hides the text).
 * Input: input.txt may carry a setup script in front of a `#game` marker line; setup steps it once per vblank on its own counter. Otherwise the real buttons.
 * The static buffers below are the whole memory budget: no heap, no recursion (the folder scan is a bounded queue). */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw_benchio.h"
#include "hw_cfg.h"
#include "hw_cfgio.h"
#include "hw_ctr.h"
#include "hw_log.h"
#include "hw_pure.h"
#include "hw_setup.h"
#include "hw_setup_core.h"
#include "version.h"

#define CHUNK        65536u
#define ROMS_MAX     64
#define SAVES_MAX    64
#define BAKS_MAX     16            /* platinum.sav.bak-N files offered for restore */
#define LAB_MAX      (SAVES_MAX + BAKS_MAX + 2)   /* keep + new + imports + restores */
#define PATH_CAP     128u
#define DIRS_MAX     512           /* folders queued for the scan (RF2: was 256) */
#define ENT_MAX      512           /* entries read per folder (RF2: was 128); ROMS_MAX / SAVES_MAX below cap the lists */
#define DEPTH_MAX    4u
#define TABLE_MAX    65536u        /* the largest FNT / FAT the check reads (Platinum's: 7 KB / 4 KB) */
#define SAVE_BYTES   524288u
#define CFG_MAX      4096u
#define PAGE         17
#define LABEL_COLS   36u
#define SCRIPT_TAIL  300u          /* a scripted run quits this many steps after its last line */
#define SYS_HZ       268111856ull

#define MENU_BACK  (-1)
#define MENU_START (-2)
#define MENU_QUIT  (-3)

enum { QUIT_NONE = 0, QUIT_HOME = 1, QUIT_SCRIPT = 2 };

/* ---------------------------------------------------------------------------------------------------------------- state */
static HwCfg s_cfg;
static char s_cfgtext[CFG_MAX + 1u];
static size_t s_cfglen;
static int s_cfg_existed;

static HwInScript s_script;
static int s_script_on;
static uint32_t s_step, s_script_last;
static uint16_t s_prev;
static int s_quit;

static uint8_t s_buf[2][CHUNK];
static uint8_t s_fnt[TABLE_MAX], s_fat[TABLE_MAX];

typedef struct ScanDir {
    char path[PATH_CAP];
    uint8_t depth;
} ScanDir;
static ScanDir s_q[DIRS_MAX];
static HwCtrDirEnt s_ent[ENT_MAX];
static char s_roms[ROMS_MAX][PATH_CAP];
static char s_saves[SAVES_MAX][PATH_CAP];
static int s_nroms, s_nsaves, s_rom_ignored, s_rom_long, s_rom_badname;
static int s_skipped;   /* entries the scan could not look at: a name over the seam's limit, a folder past ENT_MAX entries, a full folder queue, a full list. Shown on the menus. */
static char s_lab[LAB_MAX][PATH_CAP];
static char s_why[200];
static char s_extra[320];   /* a line the next menu shows above its list: why the current ROM was refused (S2), how many odd saves were left out (S3) */
static char s_baks[BAKS_MAX][PATH_CAP];
static int s_nbaks, s_save_odd;

static char s_sb[2048];
static size_t s_sb_len;

/* ---------------------------------------------------------------------------------------------------------------- text output */
static void sb_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sb_add(const char *fmt, ...)
{
    va_list ap;
    int n;

    assert(fmt != NULL && s_sb_len < sizeof s_sb);
    va_start(ap, fmt);
    n = vsnprintf(s_sb + s_sb_len, sizeof s_sb - s_sb_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s_sb_len += (size_t)n < sizeof s_sb - s_sb_len ? (size_t)n : sizeof s_sb - s_sb_len - 1u;
    }
}

static void sb_flush(void)
{
    assert(s_sb_len < sizeof s_sb);
    s_sb[s_sb_len] = '\0';
    hwctr_console_write(s_sb);
    s_sb_len = 0;
}

static void sb_screen(const char *title)
{
    assert(title != NULL);
    s_sb_len = 0;
    sb_add("\x1b[2J\x1b[1;1H3DP %s - setup\n%s\n\n", APP_VERSION_STR, title);
}

/* the label of an entry shortened from the left to LABEL_COLS columns; a byte the console font cannot draw (outside 0x20..0x7E: the bytes of a UTF-8 name, a control byte)
 * shows as '?', so the escape bytes of a hostile name can never reach the terminal either */
static const char *shown(const char *label, char *tmp, size_t cap)
{
    size_t n = strlen(label), i, from = n > LABEL_COLS ? n - (LABEL_COLS - 1u) : 0u, o = 0;

    assert(cap > LABEL_COLS + 1u);
    if (n > LABEL_COLS) {
        tmp[o++] = '~';
    }
    for (i = from; i < n && o < cap - 1u; i++) {
        unsigned char b = (unsigned char)label[i];

        tmp[o++] = (b >= 0x20u && b < 0x7fu) ? (char)b : '?';
    }
    tmp[o] = '\0';
    return tmp;
}

/* ---------------------------------------------------------------------------------------------------------------- config and the ROM path */
int hw_setup_resolve_rom(const HwCfg *cfg, char *out, size_t cap)
{
    assert(cfg != NULL && out != NULL && cap > sizeof HWCTR_ROM_FILE);
    if (cfg->rom_set && hwctr_file_size(cfg->rom) >= 0) {
        (void)snprintf(out, cap, "%s", cfg->rom);
        return 1;
    }
    (void)snprintf(out, cap, "%s", HWCTR_ROM_FILE);
    return hwctr_file_size(HWCTR_ROM_FILE) >= 0 ? 1 : 0;
}

/* config.ini into s_cfgtext / s_cfg; a missing file reads as the formatted defaults (s_cfg_existed 0). Returns 0, or -1 when the file is there but cannot be used as it is. */
static int cfg_load(void)
{
    int sz = hwctr_file_size(HWCFG_FILE);
    int n;

    hwcfg_defaults(&s_cfg);
    s_cfglen = 0;
    s_cfg_existed = 0;
    if (sz < 0 && hwcfgio_recover()) {
        sz = hwctr_file_size(HWCFG_FILE);
    }
    if (sz >= 0) {
        s_cfg_existed = 1;
        n = hwctr_file_read(HWCFG_FILE, s_cfgtext, CFG_MAX);
        if (n < 0 || sz >= (int)CFG_MAX) {
            s_cfglen = sz >= (int)CFG_MAX ? (size_t)sz : 0u;   /* config_write_rom words the refusal from this */
            return -1;
        }
        s_cfglen = (size_t)n;
        (void)hwcfg_parse(&s_cfg, s_cfgtext, s_cfglen, NULL);
        return 0;
    }
    s_cfglen = hwcfg_format(&s_cfg, s_cfgtext, CFG_MAX);
    return s_cfglen > 0u ? 0 : -1;
}

/* config.ini read back through the real parser (hwcfg_parse): 1 when it holds `rom = <rompath>` exactly */
static int config_reads_rom(const char *rompath)
{
    static char back[CFG_MAX + 1u];
    static HwCfg chk;
    int n = hwctr_file_read(HWCFG_FILE, back, CFG_MAX);

    if (n <= 0) {
        return 0;
    }
    hwcfg_defaults(&chk);
    (void)hwcfg_parse(&chk, back, (size_t)n, NULL);
    return chk.rom_set && strcmp(chk.rom, rompath) == 0;
}

/* `rom = <path>` into config.ini by the verified-write pattern (.tmp, re-read, rename). Returns 0 or -1 (s_why says why). */
int hw_setup_write_rom(const char *rompath)
{
    static char out[CFG_MAX + 1u];
    long n;

    assert(rompath != NULL);
    if (cfg_load() != 0) {
        if (s_cfglen >= CFG_MAX) {
            (void)snprintf(s_why, sizeof s_why, "config.ini is %u bytes (the game reads at most %u). Delete old [save:...] sections from it, or the file itself, then try again.",
                           (unsigned)s_cfglen, (unsigned)(CFG_MAX - 1u));
        } else {
            (void)snprintf(s_why, sizeof s_why, "config.ini could not be read");
        }
        return -1;
    }
    n = setup_config_set_rom(s_cfgtext, s_cfglen, rompath, out, sizeof out);
    if (n < 0) {
        if (setup_rom_path_ok(rompath)) {   /* the path is fine: the file with the new line would pass the cap */
            (void)snprintf(s_why, sizeof s_why, "config.ini is nearly full (%u of %u bytes): the ROM line does not fit. Delete old [save:...] sections from it, or the file itself, then try again.",
                           (unsigned)s_cfglen, (unsigned)(CFG_MAX - 1u));
        } else {
            (void)snprintf(s_why, sizeof s_why, "the ROM path cannot be stored in config.ini");
        }
        return -1;
    }
    if (hwcfgio_write(out, (size_t)n) != 0) {
        (void)snprintf(s_why, sizeof s_why, "config.ini could not be written");
        return -1;
    }
    if (!config_reads_rom(rompath)) {   /* the file is on the SD, but the game's own reader would not get this path back: say so instead of looping at every launch */
        (void)snprintf(s_why, sizeof s_why, "config.ini was written, but reads back without this ROM path");
        return -1;
    }
    return 0;
}

const char *hw_setup_last_why(void)
{
    return s_why;
}

/* ---------------------------------------------------------------------------------------------------------------- input */
static void script_load(void)
{
    static char text[8192];
    uint32_t bad = 0, off;
    int n = hwctr_file_read(HWCTR_APP_DIR "/input.txt", text, sizeof text);
    HwInScript game;

    s_script_on = 0;
    if (n <= 0) {
        return;
    }
    off = hwin_script_split(text, (uint32_t)n);
    if (off == 0u || hwin_script_parse(&s_script, text, off, &bad) <= 0) {
        return;
    }
    /* only lines below the game's first line belong to setup */
    if (hwin_script_parse(&game, text + off, (uint32_t)n - off, &bad) > 0 && s_script.frame[s_script.n - 1u] >= game.frame[0]) {
        return;
    }
    s_script_last = s_script.frame[s_script.n - 1u];
    s_script_on = 1;
}

/* One step: one vblank. Returns the keys that went down since the last step (DS layout). HOME or the end of the script set s_quit. */
static uint16_t press(void)
{
    uint16_t keys;
    int alive = 1;
    uint32_t held = hwctr_hid_wait(1, &alive);

    if (s_script_on) {
        keys = hwin_script_at(&s_script, s_step).keys;
        if (s_step > s_script_last + SCRIPT_TAIL) {
            s_quit = QUIT_SCRIPT;
        }
    } else {
        keys = hwin_keys_from_ctr(held);
    }
    s_step++;
    if (!alive) {
        s_quit = QUIT_HOME;
    }
    keys = (uint16_t)(keys & HWIN_ALL);
    {
        uint16_t down = (uint16_t)(keys & ~s_prev);

        s_prev = keys;
        return down;
    }
}

/* SELECT at launch: the script's first state, else the buttons read now (no vblank wait: a launch that needs no setup must not shift by a frame). */
static int launch_select(void)
{
    int alive = 1;
    uint16_t keys;

    if (s_script_on) {
        keys = hwin_script_at(&s_script, 0u).keys;
    } else {
        keys = hwin_keys_from_ctr(hwctr_hid_wait(0, &alive));
    }
    s_prev = (uint16_t)(keys & HWIN_ALL);
    return (keys & HWIN_SELECT) != 0u;
}

static int wait_for(uint16_t mask)
{
    uint16_t k;

    while (s_quit == QUIT_NONE) {
        k = press();
        if ((k & mask) != 0u) {
            return (int)(k & mask);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------- the folder scan */
static int ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext), i;

    if (n < e) {
        return 0;
    }
    for (i = 0; i < e; i++) {
        char a = s[n - e + i], b = ext[i];

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int skip_dir(const char *name)
{
    static const char *const skip[] = { "Nintendo 3DS", "luma" };
    size_t i;

    assert(name != NULL);
    if (name[0] == '.') {
        return 1;
    }
    for (i = 0; i < sizeof skip / sizeof skip[0]; i++) {
        const char *a = name, *b = skip[i];

        while (*a != '\0' && *b != '\0' && (*a | 0x20) == (*b | 0x20)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return 1;
        }
    }
    return 0;
}

static int join_path(char *out, const char *dir, const char *name)
{
    size_t d = strlen(dir);
    int n = snprintf(out, PATH_CAP, d > 0u && dir[d - 1u] == '/' ? "%s%s" : "%s/%s", dir, name);

    return n > 0 && (size_t)n < PATH_CAP;
}

/* the first 0x200 bytes of a file through the seam; 1 when they read as a Platinum (USA) header */
static int rom_header_of(const char *path)
{
    uint8_t hdr[0x200];
    void *f = hwctr_file_open(path);
    int n;

    if (f == NULL) {
        return 0;
    }
    n = hwctr_file_pread(f, 0u, hdr, sizeof hdr);
    hwctr_file_close(f);
    return n == (int)sizeof hdr && setup_rom_header_ok(hdr);
}

static void note_rom(const char *full)
{
    int sz = hwctr_file_size(full);

    if (sz < 0x200 || !rom_header_of(full)) {
        s_rom_ignored++;
        HWLOG("setup: rom-skip %s: size %d, %s", full, sz, sz < 0x200 ? "too small or unreadable" : "header is not a Platinum (USA) header or unreadable");
    } else if (strlen(full) > HWCFG_ROM_MAX) {
        s_rom_long++;
    } else if (!setup_rom_path_ok(full)) {   /* a control byte, DEL, or a blank first/last byte: config.ini could not hold it unchanged (UTF-8 bytes are fine) */
        s_rom_badname++;
    } else if (s_nroms < ROMS_MAX) {
        (void)snprintf(s_roms[s_nroms++], PATH_CAP, "%s", full);
    } else {
        s_skipped++;   /* a good dump past the list's room */
    }
}

/* a *.sav / *.dsv that is a whole Platinum save (raw or with a DeSmuME footer); never our own save file */
static void note_save(const char *full)
{
    uint8_t tail[128];
    uint64_t raw = 0;
    int size = hwctr_file_size(full);
    size_t tl = 128u;
    void *f;
    int n;

    if (size < (int)SAVE_BYTES || strcmp(full, HWCTR_SAVE_FILE) == 0) {
        return;
    }
    if (size > (int)SAVE_BYTES && size < (int)SAVE_BYTES + 122) {   /* longer than a save but too short to carry a DeSmuME footer: an odd dump */
        s_save_odd++;
        return;
    }
    if (s_nsaves >= SAVES_MAX) {
        s_skipped++;
        return;
    }
    f = hwctr_file_open(full);
    if (f == NULL) {
        return;
    }
    n = hwctr_file_pread(f, (uint32_t)size - (uint32_t)tl, tail, (uint32_t)tl);
    hwctr_file_close(f);
    if (n == (int)tl && setup_save_kind((uint64_t)size, tail, tl, &raw) != SETUP_SAVE_BAD) {
        (void)snprintf(s_saves[s_nsaves++], PATH_CAP, "%s", full);
    } else if (n == (int)tl) {
        s_save_odd++;   /* 1 MB padded flashcart dumps and the like: counted, and the save menu says so */
    }
}

static void scan_dir(int qi, int *nq)
{
    int lost = 0, n = hwctr_dir_list(s_q[qi].path, s_ent, ENT_MAX, &lost), i;
    char full[PATH_CAP];

    s_skipped += lost;
    for (i = 0; i < n; i++) {
        if (!join_path(full, s_q[qi].path, s_ent[i].name)) {
            s_skipped++;
            continue;
        }
        if (s_ent[i].is_dir) {
            if (s_q[qi].depth < DEPTH_MAX && !skip_dir(s_ent[i].name)) {
                if (*nq < DIRS_MAX) {
                    (void)snprintf(s_q[*nq].path, PATH_CAP, "%s", full);
                    s_q[*nq].depth = (uint8_t)(s_q[qi].depth + 1u);
                    (*nq)++;
                } else {
                    s_skipped++;   /* the folder queue is full: this folder is never looked into */
                }
            }
        } else if (ends_with(s_ent[i].name, ".nds")) {
            note_rom(full);
        } else if (ends_with(s_ent[i].name, ".sav") || ends_with(s_ent[i].name, ".dsv")) {
            note_save(full);
        }
    }
}

static void sort_paths(char (*list)[PATH_CAP], int n)
{
    char t[PATH_CAP];
    int i, j;

    assert(n >= 0);
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && strcmp(list[j - 1], list[j]) > 0; j--) {
            memcpy(t, list[j], PATH_CAP);
            memcpy(list[j], list[j - 1], PATH_CAP);
            memcpy(list[j - 1], t, PATH_CAP);
        }
    }
}

/* at most DIRS_MAX folders, each read once, depth <= DEPTH_MAX under sdmc:/ */
static void scan_all(void)
{
    int nq = 1, qi;

    s_nroms = s_nsaves = s_rom_ignored = s_rom_long = s_rom_badname = s_skipped = s_save_odd = 0;
    (void)snprintf(s_q[0].path, PATH_CAP, "sdmc:/");
    s_q[0].depth = 0;
    for (qi = 0; qi < nq && qi < DIRS_MAX; qi++) {
        scan_dir(qi, &nq);
    }
    sort_paths(s_roms, s_nroms);
    sort_paths(s_saves, s_nsaves);
}

/* ---------------------------------------------------------------------------------------------------------------- menus */
static void draw_menu(const char *title, const char *hint, int n, int sel, int top)
{
    char tmp[PATH_CAP];
    int i;

    sb_screen(title);
    for (i = top; i < n && i < top + PAGE; i++) {
        sb_add("%c %s\n", i == sel ? '>' : ' ', shown(s_lab[i], tmp, sizeof tmp));
    }
    if (s_extra[0] != '\0') {
        sb_add("\n%s\n", s_extra);
    }
    if (n == 0) {
        sb_add(s_skipped + s_rom_long + s_rom_badname > 0 ? "  (nothing usable was listed)\n" : "  (nothing found)\n");
    }
    if (s_skipped + s_rom_long > 0) {   /* never a silent cap: say how many entries the scan could not list */
        sb_add("\n%d files skipped (name too long / too many files)\n", s_skipped + s_rom_long);
    }
    if (s_rom_badname > 0) {
        sb_add("%d ROM names use characters config.ini cannot hold\n", s_rom_badname);
    }
    sb_add("\n%s\n", hint);
    sb_flush();
}

/* s_lab[0..n) as a list. Returns the chosen index, MENU_BACK (B), MENU_START (START) or MENU_QUIT. */
static int menu(const char *title, const char *hint, int n)
{
    int sel = 0, top = 0, dirty = 1;
    uint16_t k;

    assert(n >= 0 && n <= LAB_MAX && title != NULL && hint != NULL);
    while (s_quit == QUIT_NONE) {
        if (dirty) {
            draw_menu(title, hint, n, sel, top);
            dirty = 0;
        }
        k = press();
        if (s_quit != QUIT_NONE) {
            break;
        }
        if ((k & HWIN_DOWN) != 0u && sel + 1 < n) {
            sel++;
            dirty = 1;
        }
        if ((k & HWIN_UP) != 0u && sel > 0) {
            sel--;
            dirty = 1;
        }
        if (sel < top) {
            top = sel;
        }
        if (sel >= top + PAGE) {
            top = sel - PAGE + 1;
        }
        if ((k & HWIN_A) != 0u && n > 0) {
            return sel;
        }
        if ((k & (HWIN_B | HWIN_START)) != 0u) {
            return (k & HWIN_B) != 0u ? MENU_BACK : MENU_START;
        }
    }
    return MENU_QUIT;
}

static void message(const char *title, const char *text)
{
    sb_screen(title);
    sb_add("%s\n\nPress A.\n", text);
    sb_flush();
    (void)wait_for(HWIN_A | HWIN_B | HWIN_START);
}

/* ---------------------------------------------------------------------------------------------------------------- the ROM check */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int refuse(const char *why)
{
    (void)snprintf(s_why, sizeof s_why, "%s", why);
    return 0;
}

/* the FNT and FAT named by the header, into s_fnt / s_fat. 1 or 0 (s_why set) */
static int load_tables(void *f, const uint8_t *hdr, uint32_t *fnt_len, uint32_t *fat_len)
{
    *fnt_len = le32(hdr + 0x44);
    *fat_len = le32(hdr + 0x4c);
    if (*fnt_len == 0u || *fnt_len > TABLE_MAX || *fat_len == 0u || *fat_len > TABLE_MAX) {
        return refuse("the file table is not Platinum's");
    }
    if (hwctr_file_pread(f, le32(hdr + 0x40), s_fnt, *fnt_len) != (int)*fnt_len || hwctr_file_pread(f, le32(hdr + 0x48), s_fat, *fat_len) != (int)*fat_len) {
        return refuse("the file table could not be read");
    }
    return 1;
}

/* every table file located (before anything is hashed): the byte total, or -1 (s_why set) */
static int64_t locate_all(uint32_t fnt_len, uint32_t fat_len)
{
    int64_t total = 0;
    uint32_t i, st, en;

    if (setup_fs_count == 0u) {
        (void)refuse("this build has no ROM table");
        return -1;
    }
    for (i = 0; i < setup_fs_count; i++) {
        int r = setup_nitrofs_find(s_fnt, fnt_len, s_fat, fat_len, setup_fs_table[i].rom_path, &st, &en);

        if (r != 1 || en < st) {
            (void)refuse(r == 0 ? "a game file is missing" : "the file table is damaged");
            return -1;
        }
        total += (int64_t)(en - st);
    }
    return total;
}

static void show_progress(uint32_t done, uint32_t total, int *last_pct)
{
    int pct = total == 0u ? 100 : (int)(((uint64_t)done * 100u) / total);

    if (pct != *last_pct) {
        *last_pct = pct;
        s_sb_len = 0;
        sb_add("\x1b[10;1HChecking: %3d%%", pct);
        sb_flush();
    }
}

#define POLL_EVERY 16u   /* chunks (1 MB) between two HOME / B polls while the check runs */

/* HOME (or the lid, or power) and B stay served during the long read: 1 when the check must stop (s_why / s_quit set) */
static int check_cancelled(void)
{
    int alive = 1;
    uint32_t held = hwctr_hid_wait(0, &alive);

    if (!alive) {
        s_quit = QUIT_HOME;
        (void)refuse("cancelled");
        return 1;
    }
    if (!s_script_on && (hwin_keys_from_ctr(held) & HWIN_B) != 0u) {
        (void)refuse("cancelled (B)");
        return 1;
    }
    return 0;
}

/* SHA-1 of the bytes [st, en) of the ROM; 1 when it equals `want` */
static int hash_span(void *f, uint32_t st, uint32_t en, const uint8_t *want, uint32_t *done, uint32_t total, int *pct)
{
    setup_sha1 c;
    uint8_t out[20];
    uint32_t off = st;
    static uint32_t polls;

    setup_sha1_init(&c);
    while (off < en) {
        uint32_t n = en - off < CHUNK ? en - off : CHUNK;

        if (++polls % POLL_EVERY == 0u && check_cancelled()) {
            return 0;
        }

        if (hwctr_file_pread(f, off, s_buf[0], n) != (int)n) {
            return refuse("the ROM could not be read");
        }
        setup_sha1_update(&c, s_buf[0], n);
        off += n;
        *done += n;
        show_progress(*done, total, pct);
    }
    setup_sha1_final(&c, out);
    return memcmp(out, want, 20) == 0 ? 1 : refuse("a game file differs from the expected data");
}

static int hash_all(void *f, uint32_t fnt_len, uint32_t fat_len, uint32_t total)
{
    uint32_t i, st, en, done = 0;
    int pct = -1;

    for (i = 0; i < setup_fs_count; i++) {
        if (setup_nitrofs_find(s_fnt, fnt_len, s_fat, fat_len, setup_fs_table[i].rom_path, &st, &en) != 1 ||
            !hash_span(f, st, en, setup_fs_table[i].sha1, &done, total, &pct)) {
            if (s_why[0] == '\0') {
                (void)refuse("the file table changed during the check");
            }
            return 0;
        }
    }
    return 1;
}

/* header, then the game's file system against the table. 1 = a Platinum (USA) image; 0 = refused, s_why says why. *bytes: bytes hashed. */
static int check_rom(const char *path, uint32_t *bytes)
{
    uint8_t hdr[0x200];
    uint32_t fnt_len = 0, fat_len = 0;
    int64_t total = 0;
    void *f = hwctr_file_open(path);
    int ok = 0;

    s_why[0] = '\0';
    *bytes = 0;
    if (f == NULL) {
        (void)refuse("the file could not be opened");
        return 0;
    }
    if (hwctr_file_pread(f, 0u, hdr, sizeof hdr) != (int)sizeof hdr || !setup_rom_header_ok(hdr)) {
        (void)refuse("not a Platinum (USA) ROM");
    } else if (load_tables(f, hdr, &fnt_len, &fat_len) && (total = locate_all(fnt_len, fat_len)) >= 0) {
        if (total > 0x7fffffffLL) {
            (void)refuse("the game files are too large");
        } else {
            sb_screen("Checking your ROM");
            sb_add("This reads every game file once.\nPlease wait.\n");
            sb_flush();
            ok = hash_all(f, fnt_len, fat_len, (uint32_t)total);
            *bytes = (uint32_t)total;
        }
    }
    hwctr_file_close(f);
    return ok;
}

/* ---------------------------------------------------------------------------------------------------------------- the ROM step */
/* 1 = a ROM was picked, checked and written to config.ini; 0 = keep what is there; -1 = quit */
static int rom_step(int usable)
{
    int again = 1;

    while (again && s_quit == QUIT_NONE) {
        int i, pick;
        uint32_t bytes = 0, t0;

        scan_all();
        for (i = 0; i < s_nroms; i++) {
            memcpy(s_lab[i], s_roms[i], PATH_CAP);
        }
        HWLOG("setup: rom-list %d found, %d not Platinum, %d path too long, %d unusable name, %d skipped", s_nroms, s_rom_ignored, s_rom_long, s_rom_badname, s_skipped);
        pick = menu("Choose your Platinum ROM (.nds)", usable ? "A: choose  START/B: keep the current ROM" : "A: choose  B: scan again\nCopy your own dump to the SD card.", s_nroms);
        if (pick == MENU_QUIT) {
            return -1;
        }
        if (pick < 0) {
            if (usable) {
                return 0;
            }
            continue;
        }
        t0 = (uint32_t)(hwctr_sys_tick() / (SYS_HZ / 1000ull));
        if (check_rom(s_roms[pick], &bytes) && hw_setup_write_rom(s_roms[pick]) == 0) {
            uint32_t ms = (uint32_t)(hwctr_sys_tick() / (SYS_HZ / 1000ull)) - t0;

            HWLOG("setup: rom-ok %s, %u KB hashed in %u ms", s_roms[pick], (unsigned)(bytes / 1024u), (unsigned)ms);
            HWLOG("setup: config-written rom = %s", s_roms[pick]);
            return 1;
        }
        HWLOG("setup: rom-refused %s: %s", s_roms[pick], s_why);
        message("This ROM was not accepted", s_why);
    }
    return -1;
}

/* ---------------------------------------------------------------------------------------------------------------- the save step */
static int file_exists_cb(const char *path, void *ctx)
{
    (void)ctx;
    return hwctr_file_size(path) >= 0;
}

/* what move_saves_aside moved, so a failed step can put it back */
typedef struct Moved {
    const char *orig;
    char bak[PATH_CAP];
    int done;
} Moved;
static Moved s_mv[3];

/* an existing file moves to <path>.bak-<n> (recorded in *m); absent = fine. 0 or -1 */
static int move_aside(const char *path, Moved *m)
{
    m->orig = path;
    m->done = 0;
    if (hwctr_file_size(path) < 0) {
        return 0;
    }
    if (setup_bak_name(path, file_exists_cb, NULL, m->bak, sizeof m->bak) < 0 || hwctr_file_rename(path, m->bak) != 0) {
        return -1;
    }
    m->done = 1;
    return 0;
}

/* the save, its half-written replacement and its first-write copy: all out of the way, so none is adopted for the new save */
static int move_saves_aside(void)
{
    memset(s_mv, 0, sizeof s_mv);
    return move_aside(HWCTR_SAVE_FILE, &s_mv[0]) == 0 && move_aside(HWCTR_SAVE_FILE ".tmp", &s_mv[1]) == 0 && move_aside(HWCTR_SAVE_FILE ".orig", &s_mv[2]) == 0 ? 0 : -1;
}

/* undo move_saves_aside (newest first). 0 when every moved file is back; else -1 and s_why names the backup that stayed behind */
static int restore_saves(void)
{
    int i, bad = 0;

    for (i = 2; i >= 0; i--) {
        if (s_mv[i].done) {
            if (hwctr_file_rename(s_mv[i].bak, s_mv[i].orig) == 0) {
                s_mv[i].done = 0;
            } else if (!bad) {
                bad = 1;
                (void)snprintf(s_why, sizeof s_why, "your old save could not be put back; it is kept as %s", s_mv[i].bak);
            }
        }
    }
    return bad ? -1 : 0;
}

static int copy_bytes(const char *src, const char *dst, uint32_t len)
{
    void *in = hwctr_file_open(src);
    void *out = hwctr_file_create(dst);
    uint32_t off = 0;
    int ok = in != NULL && out != NULL;

    while (ok && off < len) {
        uint32_t n = len - off < CHUNK ? len - off : CHUNK;

        ok = hwctr_file_pread(in, off, s_buf[0], n) == (int)n && hwctr_file_append(out, s_buf[0], n) == (int)n;
        off += n;
    }
    ok = ok && hwctr_file_sync(out) == 0;
    if (in != NULL) {
        hwctr_file_close(in);
    }
    if (out != NULL) {
        hwctr_file_close(out);
    }
    return ok ? 0 : -1;
}

static int same_bytes(const char *a, const char *b, uint32_t len)
{
    void *fa = hwctr_file_open(a);
    void *fb = hwctr_file_open(b);
    uint32_t off = 0;
    int ok = fa != NULL && fb != NULL;

    while (ok && off < len) {
        uint32_t n = len - off < CHUNK ? len - off : CHUNK;

        ok = hwctr_file_pread(fa, off, s_buf[0], n) == (int)n && hwctr_file_pread(fb, off, s_buf[1], n) == (int)n && memcmp(s_buf[0], s_buf[1], n) == 0;
        off += n;
    }
    if (fa != NULL) {
        hwctr_file_close(fa);
    }
    if (fb != NULL) {
        hwctr_file_close(fb);
    }
    return ok;
}

/* import: the first 512 KB of `src` become platinum.sav (verified copy beside it, the old files moved aside, then renamed into place). The source is only ever read. 0 or -1 */
static int import_save(const char *src)
{
    static const char part[] = HWCTR_SAVE_FILE ".import";

    hwctr_mkdirs(HWCTR_APP_DIR);
    (void)hwctr_file_remove(part);
    if (copy_bytes(src, part, SAVE_BYTES) != 0 || !same_bytes(src, part, SAVE_BYTES)) {
        (void)hwctr_file_remove(part);
        (void)snprintf(s_why, sizeof s_why, "the copy could not be verified");
        return -1;
    }
    if (move_saves_aside() != 0 || hwctr_file_rename(part, HWCTR_SAVE_FILE) != 0 || !same_bytes(src, HWCTR_SAVE_FILE, SAVE_BYTES)) {
        (void)hwctr_file_remove(part);
        (void)snprintf(s_why, sizeof s_why, "the old save could not be moved aside");
        if (restore_saves() == 0) {   /* the old save is where it was (a bad new platinum.sav is replaced by it) */
            (void)snprintf(s_why, sizeof s_why, "the save could not be imported; your current save is unchanged");
        }
        return -1;
    }
    return 0;
}

/* the platinum.sav.bak-<digits> files of the app folder that are whole 512 KB saves: s_baks, sorted */
static void scan_baks(void)
{
    static const char pre[] = "platinum.sav.bak-";
    int lost = 0, n = hwctr_dir_list(HWCTR_APP_DIR, s_ent, ENT_MAX, &lost), i;
    char full[PATH_CAP];
    size_t pl = sizeof pre - 1u, k;

    s_nbaks = 0;
    for (i = 0; i < n && s_nbaks < BAKS_MAX; i++) {
        const char *nm = s_ent[i].name;
        int digits = 1;

        if (s_ent[i].is_dir || strncmp(nm, pre, pl) != 0 || nm[pl] == '\0') {
            continue;
        }
        for (k = pl; nm[k] != '\0'; k++) {
            digits = digits && nm[k] >= '0' && nm[k] <= '9';
        }
        if (digits && join_path(full, HWCTR_APP_DIR, nm) && hwctr_file_size(full) == (int)SAVE_BYTES) {
            (void)snprintf(s_baks[s_nbaks++], PATH_CAP, "%s", full);
        }
    }
    sort_paths(s_baks, s_nbaks);
}

/* before a step that moves the current save aside: "A: yes  B: no". 1 = go on, 0 = no, -1 = quit */
static int confirm_move(void)
{
    char bak[PATH_CAP];
    const char *name;

    if (setup_bak_name(HWCTR_SAVE_FILE, file_exists_cb, NULL, bak, sizeof bak) < 0) {
        (void)snprintf(s_why, sizeof s_why, "there is no free name for a backup of your save");
        return 0;
    }
    name = strrchr(bak, '/');
    sb_screen("Replace your current save?");
    sb_add("Your current save will be moved to\n%s\nand nothing is deleted.\n\nContinue?\n\nA: yes  B: no\n", name != NULL ? name + 1 : bak);
    sb_flush();
    {
        int k = wait_for(HWIN_A | HWIN_B | HWIN_START);

        return s_quit != QUIT_NONE ? -1 : (k & HWIN_A) != 0 ? 1 : 0;
    }
}

/* a new game: the save files move aside; a partial move is undone. 0 or -1 */
static int start_new(void)
{
    if (move_saves_aside() == 0) {
        return 0;
    }
    (void)snprintf(s_why, sizeof s_why, "an old save could not be moved aside");
    if (restore_saves() == 0) {
        (void)snprintf(s_why, sizeof s_why, "the save could not be moved aside; your current save is unchanged");
    }
    return -1;
}

/* 1 = a save was imported, restored or a new game chosen (the files are in place); 0 = nothing changed; -1 = quit.
 * Rows: [ Keep the current save ] (only when a save exists: the default row, so a quick A press changes nothing), [ Start a new game ], the importable
 * saves, then the platinum.sav.bak-N backups ("[restore]"). Anything that moves an existing save aside asks first. */
static int save_step(void)
{
    int have, i, pick, keep, nrow, first_new, first_save, first_bak;

    for (;;) {
        have = hwctr_file_size(HWCTR_SAVE_FILE) >= 0;   /* every pass: a failed step may have left no save behind */
        scan_all();
        scan_baks();
        keep = have ? 1 : 0;
        first_new = keep;
        first_save = first_new + 1;
        first_bak = first_save + s_nsaves;
        nrow = first_bak + s_nbaks;
        if (keep) {
            (void)snprintf(s_lab[0], PATH_CAP, "[ Keep the current save ]");
        }
        (void)snprintf(s_lab[first_new], PATH_CAP, "[ Start a new game ]");
        for (i = 0; i < s_nsaves; i++) {
            memcpy(s_lab[first_save + i], s_saves[i], PATH_CAP);
        }
        for (i = 0; i < s_nbaks; i++) {
            (void)snprintf(s_lab[first_bak + i], PATH_CAP, "[restore] %s", s_baks[i] + sizeof HWCTR_APP_DIR);
        }
        s_extra[0] = '\0';
        if (s_save_odd > 0) {
            (void)snprintf(s_extra, sizeof s_extra, "%d save files skipped (not exactly 512 KB: cut a padded dump to 512 KB first)", s_save_odd);
        }
        pick = menu("Choose a save", have ? "A: choose  START/B: keep the current save\nAn old save is kept as .bak-N." : "A: choose\nAn old save is kept as .bak-N.", nrow);
        s_extra[0] = '\0';
        if (pick == MENU_QUIT) {
            return -1;
        }
        if (pick < 0) {
            if (have) {
                return 0;
            }
            continue;
        }
        if (keep && pick == 0) {
            HWLOG("setup: save-keep");
            return 0;
        }
        if (have) {
            int c = confirm_move();

            if (c < 0) {
                return -1;
            }
            if (c == 0) {
                continue;
            }
        }
        if (pick == first_new ? start_new() == 0 : import_save(pick < first_bak ? s_saves[pick - first_save] : s_baks[pick - first_bak]) == 0) {
            if (pick == first_new) {
                HWLOG("setup: save-new, no save file");
            } else if (pick < first_bak) {
                HWLOG("setup: save-import %s", s_saves[pick - first_save]);
            } else {
                HWLOG("setup: save-restore %s", s_baks[pick - first_bak]);
            }
            return 1;
        }
        message("The save step failed", s_why[0] != '\0' ? s_why : "an old save could not be moved aside");
        if (s_quit != QUIT_NONE) {
            return -1;
        }
    }
}

/* ---------------------------------------------------------------------------------------------------------------- entry */
static int flow(int usable)
{
    int r = rom_step(usable);

    if (r < 0) {
        return HWSETUP_QUIT;
    }
    if (r == 0 && !usable) {
        return HWSETUP_QUIT;
    }
    s_why[0] = '\0';
    s_extra[0] = '\0';
    if (save_step() < 0) {
        return HWSETUP_QUIT;
    }
    sb_screen("All set");
    sb_add("Setup is done.\nPress A to start the game.\n");
    sb_flush();
    (void)wait_for(HWIN_A | HWIN_START);
    return s_quit == QUIT_NONE ? HWSETUP_DONE : HWSETUP_QUIT;
}

int hw_setup_run(void)
{
    char path[PATH_CAP];
    int usable, sel, r;

    if (cfg_load() != 0 && s_cfg_existed == 0) {
        return HWSETUP_NOT_NEEDED;
    }
    if (s_cfg.setup_off || hwctr_file_size(HWBENCH_LIST_FILE) >= 0) {
        return HWSETUP_NOT_NEEDED;
    }
    script_load();
    sel = launch_select();
    usable = hw_setup_resolve_rom(&s_cfg, path, sizeof path);
    s_extra[0] = '\0';
    if (usable && !rom_header_of(path)) {   /* S2: every launch checks the 0x200-byte header of the ROM it will boot (the 340-file hash stays a pick-time check) */
        char tmp[PATH_CAP];

        (void)snprintf(s_extra, sizeof s_extra, "The current ROM (%s) is not a readable Platinum (USA) ROM, so the game cannot start. Choose your own dump.", shown(path, tmp, sizeof tmp));
        usable = 0;
    }
    if (usable && !sel) {
        return HWSETUP_NOT_NEEDED;
    }
    HWLOG("setup: open (%s)", sel ? "SELECT at launch" : (s_extra[0] != '\0' ? "the current ROM failed the header check" : "no usable ROM"));
    r = flow(usable);
    HWLOG("setup: exit %s", r == HWSETUP_DONE ? "done" : (s_quit == QUIT_SCRIPT ? "quit, the script ended" : "quit"));
    return r;
}
