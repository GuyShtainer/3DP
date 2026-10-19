/* hw_benchio.c -- see hw_benchio.h. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hw_bench.h"
#include "hw_benchio.h"
#include "hw_ctr.h"

#define CHUNK 8192u

static const char *const LOGS[] = { "hwlog.txt", "hwcount.txt", "boot.out", "console.txt", "keys.txt" };
#define NLOGS (sizeof LOGS / sizeof LOGS[0])

/* SHA-256 of a file, streamed; 0 on success, -1 when it cannot be read */
static int sha_file(const char *path, uint8_t out[32])
{
    static uint8_t buf[CHUNK];
    HwSha256 c;
    void *f = hwctr_file_open(path);
    uint32_t off = 0;
    int n;

    if (f == NULL) {
        return -1;
    }
    hwsha256_init(&c);
    do {
        n = hwctr_file_pread(f, off, buf, CHUNK);
        if (n < 0) {
            hwctr_file_close(f);
            return -1;
        }
        hwsha256_update(&c, buf, (size_t)n);
        off += (uint32_t)n;
    } while (n == (int)CHUNK);
    hwctr_file_close(f);
    hwsha256_final(&c, out);
    return 0;
}

/* copy src to dst, streamed; 0 on success */
static int copy_file(const char *src, const char *dst)
{
    static uint8_t buf[CHUNK];
    void *in = hwctr_file_open(src), *out;
    uint32_t off = 0;
    int n, rc = 0;

    if (in == NULL) {
        return -1;
    }
    out = hwctr_file_create(dst);
    if (out == NULL) {
        hwctr_file_close(in);
        return -1;
    }
    do {
        n = hwctr_file_pread(in, off, buf, CHUNK);
        if (n < 0 || (n > 0 && hwctr_file_append(out, buf, (uint32_t)n) != n)) {
            rc = -1;
            break;
        }
        off += (uint32_t)n;
    } while (n == (int)CHUNK);
    hwctr_file_close(in);
    hwctr_file_close(out);
    return rc;
}

static void copy_logs(const char *tag)
{
    char src[96], dst[96], dir[80];
    unsigned i;

    snprintf(dir, sizeof dir, HWBENCH_DIR "/%s", tag);
    hwctr_mkdirs(HWBENCH_DIR);
    hwctr_mkdirs(dir);
    for (i = 0; i < NLOGS; i++) {
        snprintf(src, sizeof src, HWCTR_APP_DIR "/%s", LOGS[i]);
        snprintf(dst, sizeof dst, "%s/%s", dir, LOGS[i]);
        (void)copy_file(src, dst);   /* a missing log (no keys.txt in a run that never reached the field) is simply absent in the folder */
    }
}

static void refuse(HwBenchRun *run, const char *why)
{
    run->refused = 1;
    snprintf(run->note, sizeof run->note, "bench refused: %s", why);
}

/* N7 (review 2.7a): every override of every entry must be one hwcfg_parse accepts; a misspelled key would otherwise run silently as the default config under the new tag.
   The keys of an entry are `key = value` lines; each is tried alone on a scratch copy so the refusal can name the key. Returns 0 when all are valid. */
static int validate_entry(const HwBenchEntry *e, const HwCfg *cfg, char *why, size_t cap)
{
    const char *p = e->keys;

    while (*p != '\0') {
        char line[48];
        size_t n = 0;
        HwCfg scratch = *cfg;
        uint32_t skipped = 0;

        while (p[n] != '\0' && p[n] != '\n' && n < sizeof line - 1u) {
            line[n] = p[n];
            n++;
        }
        line[n] = '\0';
        (void)hwcfg_parse(&scratch, line, n, &skipped);
        if (skipped != 0u) {
            snprintf(why, cap, "entry %s: '%.47s' is not a valid config key=value", e->tag, line);
            return -1;
        }
        p += n;
        while (*p == '\n') {
            p++;
        }
    }
    return 0;
}

/* The previous launch ran entry k-1: its DONE marker says it ended normally; without it the leftover logs are salvaged with a CRASHED marker. */
static void salvage_previous(HwBenchRun *run, const char *prev)
{
    char path[96], marker[96];

    snprintf(path, sizeof path, HWBENCH_DIR "/%s/DONE", prev);
    if (hwctr_file_size(path) < 0) {
        copy_logs(prev);
        snprintf(marker, sizeof marker, HWBENCH_DIR "/%s/CRASHED", prev);
        (void)hwctr_file_write(marker, "no DONE marker: the run ended without finishing; its leftover logs were salvaged\n", 79u);
        run->salvaged = 1;
    }
}

/* The save must be the kit's: the run may write the card, and a bench run on someone's own save is not what the session is for. 0 = ok (run->sav_start set). */
static int check_save(HwBenchRun *run)
{
    uint8_t d[32];
    char want[160];
    int wl = hwctr_file_read(HWBENCH_SHA_FILE, want, sizeof want - 1u);

    if (wl < 0) {
        refuse(run, "platinum.sav.sha256 is missing (copy the whole kit, including your own platinum.nds)");
        return -1;
    }
    if (sha_file(HWCTR_SAVE_FILE, d) != 0) {
        refuse(run, "platinum.sav is missing");
        return -1;
    }
    if (!hwsha256_matches(d, want, (size_t)wl)) {
        refuse(run, "platinum.sav is not the kit's save (its SHA-256 differs from platinum.sav.sha256): rename your own save away and copy the kit's");
        return -1;
    }
    memcpy(run->sav_start, d, sizeof d);
    return 0;
}

void hwbench_start(HwBenchRun *run, HwCfg *cfg)
{
    static char text[HWBENCH_TEXT_MAX];   /* F-2b: bench.txt grew past 1 KB with bet 2.8's entries and was silently cut mid-line ("line 13 is not valid"); the private hardware-session kit checks the file fits */
    static HwBenchEntry entries[HWBENCH_MAX];   /* static: hw_sys_init runs on a small host-thread stack (a 1.2 KB array here hung the boot on Azahar) */
    char path[96], cnt[16], why[160];
    uint32_t bad = 0, k;
    int n, len, i;

    assert(run != NULL && cfg != NULL);
    memset(run, 0, sizeof *run);
    len = hwctr_file_read(HWBENCH_LIST_FILE, text, sizeof text - 1u);
    if (len < 0) {
        return;   /* no bench.txt: a normal launch */
    }
    n = hwbench_parse(entries, HWBENCH_MAX, text, (size_t)len, &bad);
    if (n < 0) {
        snprintf(why, sizeof why, "bench.txt line %u is not valid", (unsigned)bad);
        refuse(run, why);
        return;
    }
    for (i = 0; i < n; i++) {
        if (validate_entry(&entries[i], cfg, why, sizeof why) != 0) {
            refuse(run, why);
            return;
        }
    }
    run->total = n;
    len = hwctr_file_read(HWBENCH_CNT_FILE, cnt, sizeof cnt - 1u);
    k = len > 0 ? hwbench_counter_parse(cnt, (size_t)len) : 0u;
    if (k > 0u && k <= (uint32_t)n) {
        salvage_previous(run, entries[k - 1u].tag);
    }
    if (k >= (uint32_t)n) {
        snprintf(run->note, sizeof run->note, "bench: all %d entries are done (bench.cnt %u): a normal launch. Delete bench.cnt to run the bench again", n, (unsigned)k);
        return;
    }
    if (check_save(run) != 0) {
        return;
    }
    {   /* START: the counter advances now, so a crash cannot loop on the same entry */
        char out[16];
        size_t ol = hwbench_counter_format(k + 1u, out, sizeof out);

        (void)hwctr_file_write(HWBENCH_CNT_FILE, out, (uint32_t)ol);
    }
    snprintf(path, sizeof path, HWBENCH_DIR "/%s/DONE", entries[k].tag);   /* N6: a stale DONE of an earlier session must not pass for this run's marker */
    (void)hwctr_file_remove(path);
    run->active = 1;
    run->index = (int)k;
    snprintf(run->tag, sizeof run->tag, "%s", entries[k].tag);
    (void)hwcfg_parse(cfg, entries[k].keys, strlen(entries[k].keys), NULL);
    snprintf(run->note, sizeof run->note, "bench: entry %s (%d of %d)%s", run->tag, (int)k + 1, n, run->salvaged ? "; the previous entry had crashed and was salvaged" : "");
}

void hwbench_finish(const HwBenchRun *run, const char *why)
{
    uint8_t d[32];
    char hex[65], dir[80], path[96], text[320];
    int same = 0, n;

    assert(run != NULL && why != NULL);
    if (!run->active) {
        return;
    }
    copy_logs(run->tag);
    if (sha_file(HWCTR_SAVE_FILE, d) == 0) {
        same = memcmp(d, run->sav_start, sizeof d) == 0;
        hwsha256_hex(d, hex);
    } else {
        snprintf(hex, sizeof hex, "unreadable");
    }
    snprintf(dir, sizeof dir, HWBENCH_DIR "/%s", run->tag);
    snprintf(path, sizeof path, "%s/DONE", dir);
    n = snprintf(text, sizeof text, "tag=%s\nindex=%d\ntotal=%d\nwhy=%s\nsav_unchanged=%d\nsav_sha256=%s\nsalvaged_previous=%d\n", run->tag, run->index, run->total, why, same, hex, run->salvaged);
    if (n > 0) {
        (void)hwctr_file_write(path, text, (uint32_t)n);
    }
}
