/* hw_benchio_test.c -- bet 2.7a: the bench glue (hw_benchio.c) over a temp directory standing in for the SD card: entry selection, the counter advancing at START, the save check,
 * the overrides, salvage of a run that never finished, the copy of the logs and the DONE marker. The hwctr_* file calls are faked here (sdmc:/3ds/3DP -> $BENCH_ROOT). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hw_bench.h"
#include "hw_benchio.h"
#include "hw_ctr.h"

static char g_root[256];
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void real(const char *p, char *out, size_t cap)
{
    const char *pre = "sdmc:/3ds/3DP";
    snprintf(out, cap, "%s%s", g_root, strncmp(p, pre, strlen(pre)) == 0 ? p + strlen(pre) : p);
}
int hwctr_file_read(const char *path, void *buf, uint32_t cap)
{
    char rp[512]; FILE *f; size_t n;
    real(path, rp, sizeof rp);
    f = fopen(rp, "rb");
    if (!f) return -1;
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}
int hwctr_file_write(const char *path, const void *buf, uint32_t len)
{
    char rp[512]; FILE *f;
    real(path, rp, sizeof rp);
    f = fopen(rp, "wb");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return (int)len;
}
void hwctr_mkdirs(const char *dir)
{
    char rp[512];
    real(dir, rp, sizeof rp);
    mkdir(rp, 0755);
}
void *hwctr_file_open(const char *path)
{
    char rp[512];
    real(path, rp, sizeof rp);
    return fopen(rp, "rb");
}
int hwctr_file_pread(void *f, uint32_t off, void *dst, uint32_t len)
{
    if (fseek((FILE *)f, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(dst, 1, len, (FILE *)f);
}
void hwctr_file_close(void *f) { fclose((FILE *)f); }
void *hwctr_file_create(const char *path)
{
    char rp[512];
    real(path, rp, sizeof rp);
    return fopen(rp, "wb");
}
int hwctr_file_append(void *f, const void *buf, uint32_t len) { return (int)fwrite(buf, 1, len, (FILE *)f); }
int hwctr_file_size(const char *path)
{
    char rp[512]; struct stat st;
    real(path, rp, sizeof rp);
    return stat(rp, &st) == 0 && S_ISREG(st.st_mode) ? (int)st.st_size : -1;
}

int hwctr_file_remove(const char *path)
{
    char rp[512];
    real(path, rp, sizeof rp);
    return remove(rp) == 0 || errno == ENOENT ? 0 : -1;
}

static void put(const char *name, const char *text)
{
    char p[300];
    snprintf(p, sizeof p, "sdmc:/3ds/3DP/%s", name);
    hwctr_file_write(p, text, (uint32_t)strlen(text));
}
static int has(const char *name)
{
    char p[300];
    snprintf(p, sizeof p, "sdmc:/3ds/3DP/%s", name);
    return hwctr_file_size(p) >= 0;
}
static void get(const char *name, char *buf, size_t cap)
{
    char p[300];
    int n;
    snprintf(p, sizeof p, "sdmc:/3ds/3DP/%s", name);
    n = hwctr_file_read(p, buf, (uint32_t)cap - 1u);
    buf[n > 0 ? n : 0] = '\0';
}

int main(int argc, char **argv)
{
    HwBenchRun run;
    HwCfg cfg;
    char buf[400], cmd[700];   /* two copies of g_root (256) plus the command words: a deep checkout path must not truncate the quoted command */
    HwSha256 c;
    uint8_t d[32];
    char hex[65];

    snprintf(g_root, sizeof g_root, "%s/benchio_%d", argc > 1 ? argv[1] : "/tmp", (int)getpid());   /* build.sh passes its output directory */
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_root, g_root);
    if (system(cmd) != 0) return 2;

    /* no bench.txt: a normal launch, nothing read or written */
    hwcfg_defaults(&cfg);
    hwbench_start(&run, &cfg);
    CHECK(!run.active && !run.refused && !has("bench.cnt"), "no bench.txt = normal launch");

    /* a malformed bench.txt refuses */
    put("bench.txt", "A render_core\n");
    hwbench_start(&run, &cfg);
    CHECK(run.refused && strstr(run.note, "line 1") != NULL, "malformed bench.txt refused: %s", run.note);

    /* N7: a misspelled key (or a bad value) refuses the whole bench.txt and names the key; the counter does not advance */
    put("bench.txt", "A render_core=2\nB rendr_core=0\n");
    hwbench_start(&run, &cfg);
    CHECK(run.refused && strstr(run.note, "entry B") != NULL && strstr(run.note, "rendr_core") != NULL && !has("bench.cnt"), "skipped key refused, key named: %s", run.note);
    put("bench.txt", "A render_core=7\n");
    hwbench_start(&run, &cfg);
    CHECK(run.refused && strstr(run.note, "render_core") != NULL, "bad value refused, key named: %s", run.note);

    put("bench.txt", "# kit\nA render_core=2 hwcheck=1\nB render_core=0\n");
    put("platinum.sav", "the kit's save");
    hwsha256_init(&c);
    hwsha256_update(&c, "the kit's save", 14);
    hwsha256_final(&c, d);
    hwsha256_hex(d, hex);

    /* no sha file, then a wrong one: refused, the counter does not advance */
    hwbench_start(&run, &cfg);
    CHECK(run.refused && strstr(run.note, "sha256 is missing") != NULL && !has("bench.cnt"), "missing sha file refused: %s", run.note);
    put("platinum.sav.sha256", "0000000000000000000000000000000000000000000000000000000000000000  platinum.sav\n");
    hwbench_start(&run, &cfg);
    CHECK(run.refused && strstr(run.note, "not the kit's save") != NULL && !has("bench.cnt"), "wrong save refused: %s", run.note);

    snprintf(buf, sizeof buf, "%s  platinum.sav\n", hex);
    put("platinum.sav.sha256", buf);
    put("hwlog.txt", "log of A\n");
    put("boot.out", "boot A\n");

    /* N6: a stale DONE of an earlier session (bench.cnt was deleted, bench/ was not) is removed at START */
    hwctr_mkdirs("sdmc:/3ds/3DP/bench");
    hwctr_mkdirs("sdmc:/3ds/3DP/bench/A");
    put("bench/A/DONE", "stale\n");
    /* launch 1 = entry A: overrides applied, counter at 1 */
    hwcfg_defaults(&cfg);
    hwbench_start(&run, &cfg);
    CHECK(run.active && !run.refused && strcmp(run.tag, "A") == 0 && run.index == 0 && run.total == 2, "entry A selected: tag %s", run.tag);
    CHECK(cfg.render_core == 2u && cfg.hwcheck == 1u, "A overrides applied");
    CHECK(!has("bench/A/DONE"), "the stale DONE of A was deleted at START");
    get("bench.cnt", buf, sizeof buf);
    CHECK(strcmp(buf, "1\n") == 0, "counter advanced at START: '%s'", buf);
    hwbench_finish(&run, "autoexit");
    CHECK(has("bench/A/hwlog.txt") && has("bench/A/boot.out") && has("bench/A/DONE") && !has("bench/A/keys.txt"), "A's logs copied, DONE written, absent log absent");
    get("bench/A/DONE", buf, sizeof buf);
    CHECK(strstr(buf, "tag=A") && strstr(buf, "why=autoexit") && strstr(buf, "sav_unchanged=1"), "DONE content: %s", buf);

    /* launch 2 = entry B */
    put("hwlog.txt", "log of B\n");
    hwcfg_defaults(&cfg);
    hwbench_start(&run, &cfg);
    CHECK(run.active && strcmp(run.tag, "B") == 0 && cfg.render_core == 0u && cfg.hwcheck == 0u, "entry B selected, overrides applied");
    /* B changes the save (a write-back): DONE says so */
    put("platinum.sav", "changed");
    hwbench_finish(&run, "HOME/exit");
    get("bench/B/DONE", buf, sizeof buf);
    CHECK(strstr(buf, "sav_unchanged=0") != NULL, "a changed save is reported: %s", buf);

    /* launch 3 = all done: a normal launch, no overrides, no refusal even though the save changed */
    hwcfg_defaults(&cfg);
    hwbench_start(&run, &cfg);
    CHECK(!run.active && !run.refused && strstr(run.note, "all 2 entries are done") != NULL && cfg.render_core == 2u, "exhausted bench is a normal launch: %s", run.note);
    hwbench_finish(&run, "x");   /* a no-op */

    /* salvage: counter 1 (entry A was started) but A's DONE is gone -> the leftover logs are copied with CRASHED, and launch goes on with B */
    snprintf(cmd, sizeof cmd, "rm -rf %s/bench && echo 1 > %s/bench.cnt", g_root, g_root);
    system(cmd);
    put("platinum.sav", "the kit's save");
    put("hwlog.txt", "leftover of A\n");
    hwcfg_defaults(&cfg);
    hwbench_start(&run, &cfg);
    CHECK(run.active && strcmp(run.tag, "B") == 0 && run.salvaged, "salvage then entry B");
    get("bench/A/hwlog.txt", buf, sizeof buf);
    CHECK(strcmp(buf, "leftover of A\n") == 0 && has("bench/A/CRASHED") && !has("bench/A/DONE"), "A salvaged with a CRASHED marker");

    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    system(cmd);
    printf("hw_benchio_test: %d checks, %d failures\n", checks, fails);
    return fails != 0 ? 1 : 0;
}
