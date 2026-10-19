/* ovl_boot_test.c -- host tests for bet 1.4 gates B and C over the pure units hw_ovl.c, hw_boot.c and hw_ctx.c (no SDK, no libctru):
 *  B  three fake overlays with .data / .bss / .sinit ranges: init takes one pristine copy, a load restores .data and zeroes .bss (and only that
 *     overlay's), a start runs the static initialisers, an unload leaves the RAM, a bad id and a bad table are refused;
 *  C  a fake NitroMain runs twice through the soft-reset path: the second run sees the main binary's .data/.bss restored, every overlay unloaded,
 *     the reset parameter preserved, the hardware-layer state (outside the restore ranges) untouched except what reseed clears, a different host
 *     thread (fresh stack), and the contexts it had left parked are ended (killed without unwinding).
 * Expected values are the literals below, never read back from the code under test. Prints "N checks, 0 failures". */
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../source/hw/hw_boot.h"
#include "../../source/hw/hw_ovl.h"
#include "../../source/hw/hw_pure.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long detail)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        if (g_fails <= 40) {
            fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
        }
    }
}

/* ---- fake linker output: the "main binary" and three overlays ------------------------------------------------------------------ */
static char main_data[16] = "MAIN-DATA-0123";
static char main_bss[16];
static char o_data[3][16] = { "OVL0-DATA", "OVL1-DATA-LONGER", "OVL2" };
static char o_bss[3][8];
static int s_sinit_runs[3];
static void sinit_a(void) { s_sinit_runs[2] += 1; }
static void sinit_b(void) { s_sinit_runs[2] += 10; }
static void sinit_1(void) { s_sinit_runs[1] += 100; }
static void (*o1_sinit[1])(void) = { sinit_1 };
static void (*o2_sinit[3])(void) = { sinit_a, NULL, sinit_b };
static void (*main_sinit[1])(void);

static HwOvlSyms g_syms[3];
static HwOvlSyms g_main_syms;
static HwOvlSection g_sec[3];
static uint8_t g_pristine[256];
static HwOvl g_ovl;

static void build_syms(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        g_syms[i].data_start = o_data[i];
        g_syms[i].data_end = o_data[i] + sizeof o_data[i];
        g_syms[i].bss_start = o_bss[i];
        g_syms[i].bss_end = o_bss[i] + sizeof o_bss[i];
        g_syms[i].sinit_start = NULL;
        g_syms[i].sinit_end = NULL;
        g_syms[i].name = "ovl";
    }
    g_syms[0].bss_end = g_syms[0].bss_start;                 /* overlay 0 has no .bss */
    g_syms[1].sinit_start = o1_sinit;
    g_syms[1].sinit_end = o1_sinit + 1;
    g_syms[2].sinit_start = o2_sinit;
    g_syms[2].sinit_end = o2_sinit + 3;
    g_main_syms.data_start = main_data;
    g_main_syms.data_end = main_data + sizeof main_data;
    g_main_syms.bss_start = main_bss;
    g_main_syms.bss_end = main_bss + sizeof main_bss;
    g_main_syms.sinit_start = main_sinit;
    g_main_syms.sinit_end = main_sinit;
    g_main_syms.name = "main";
}

static void test_overlays(void)
{
    int32_t used;
    HwOvlSyms bad;
    uint8_t tiny[40];
    HwOvl o2;
    HwOvlSection s2[3];

    build_syms();
    used = hwovl_init(&g_ovl, g_sec, 3, g_syms, &g_main_syms, g_pristine, sizeof g_pristine);
    check(used == 16 + 16 + 16 + 16, "init: pristine bytes = main + 3 overlay .data (16 each)", used);
    check(g_ovl.main.data_size == 16 && g_ovl.sec[1].data_size == 16 && g_ovl.sec[0].bss_size == 0 && g_ovl.sec[2].bss_size == 8, "init: sizes from symbols", 0);
    check(g_ovl.sec[1].sinit_count == 1 && g_ovl.sec[2].sinit_count == 3 && g_ovl.sec[0].sinit_count == 0, "init: sinit counts", 0);
    check(memcmp(g_ovl.sec[1].pristine, "OVL1-DATA-LONGER", 16) == 0, "pristine holds the link-time image", 0);
    check(g_ovl.pristine_used == 64, "pristine total reported", g_ovl.pristine_used);

    used = hwovl_init(&o2, s2, 3, g_syms, &g_main_syms, tiny, sizeof tiny);
    check(used == -1, "init: a pristine buffer that is too small is refused", used);
    bad = g_syms[1];
    bad.data_end = bad.data_start - 4;
    {
        HwOvlSyms mix[3];
        mix[0] = g_syms[0];
        mix[1] = bad;
        mix[2] = g_syms[2];
        used = hwovl_init(&o2, s2, 3, mix, &g_main_syms, g_pristine + 100, 150);
        check(used == -1, "init: end before start is refused", used);
    }

    /* mutate everything, then load overlay 1 only */
    strcpy(o_data[0], "dirty0");
    strcpy(o_data[1], "dirty1");
    strcpy(o_data[2], "dirty2");
    memset(o_bss[1], 0x5a, sizeof o_bss[1]);
    memset(o_bss[2], 0x5a, sizeof o_bss[2]);
    strcpy(main_data, "dirty-main");
    check(hwovl_load(&g_ovl, 1) == 0, "load 1 ok", 0);
    check(memcmp(o_data[1], "OVL1-DATA-LONGER", 16) == 0, "load restores that overlay's .data", 0);
    check(o_bss[1][0] == 0 && o_bss[1][7] == 0, "load zeroes that overlay's .bss", 0);
    check(strcmp(o_data[0], "dirty0") == 0 && strcmp(o_data[2], "dirty2") == 0 && o_bss[2][0] == 0x5a, "load leaves the other overlays alone", 0);
    check(strcmp(main_data, "dirty-main") == 0, "an overlay load never touches the main binary", 0);
    check(g_ovl.sec[1].loaded == 1 && g_ovl.sec[1].loads == 1 && g_ovl.sec[2].loaded == 0, "loaded flags", 0);
    strcpy(o_data[1], "again");
    check(hwovl_load(&g_ovl, 1) == 0 && memcmp(o_data[1], "OVL1-DATA-LONGER", 16) == 0, "a second load restores again", 0);
    check(g_ovl.sec[1].loads == 2, "load counter", g_ovl.sec[1].loads);
    check(hwovl_load(&g_ovl, 0) == 0 && memcmp(o_data[0], "OVL0-DATA", 10) == 0, "overlay without .bss loads", 0);

    check(hwovl_start(&g_ovl, 2) == 2 && s_sinit_runs[2] == 11, "start runs both non-NULL static initialisers in order", s_sinit_runs[2]);
    check(hwovl_start(&g_ovl, 1) == 1 && s_sinit_runs[1] == 100, "start overlay 1 runs its one", s_sinit_runs[1]);
    check(g_ovl.sec[2].started == 1 && g_ovl.sec[0].started == 0, "started flags", 0);
    check(hwovl_unload(&g_ovl, 1) == 0 && g_ovl.sec[1].loaded == 0 && g_ovl.sec[1].started == 0, "unload clears the flags", 0);
    check(memcmp(o_data[1], "OVL1-DATA-LONGER", 16) == 0, "unload leaves the RAM as it is", 0);
    check(hwovl_load(&g_ovl, 3) == -1 && hwovl_start(&g_ovl, 200) == -1 && hwovl_unload(&g_ovl, 3) == -1, "a bad overlay id is refused", 0);

    hwovl_reset_overlays(&g_ovl);
    check(memcmp(o_data[2], "OVL2", 5) == 0 && o_bss[2][0] == 0 && !g_ovl.sec[2].loaded && !g_ovl.sec[0].loaded, "reset_overlays restores and unloads all", 0);
    hwovl_reset_main(&g_ovl);
    check(strcmp(main_data, "MAIN-DATA-0123") == 0 && main_bss[0] == 0, "reset_main restores .data, zeroes .bss", 0);
}

/* ---- gate C: pthread backend with kill ------------------------------------------------------------------------------------------ */
typedef struct Pt {
    pthread_mutex_t m;
    pthread_cond_t c;
    int flag, dying, live;
    pthread_t th;
    void (*entry)(void *);
    void *arg;
} Pt;

static Pt g_pt[16];
static int g_npt, g_kills, g_exited_by_kill;

static void pt_park(void *hh, void *u)
{
    Pt *h = (Pt *)hh;

    (void)u;
    pthread_mutex_lock(&h->m);
    while (!h->flag) {
        pthread_cond_wait(&h->c, &h->m);
    }
    h->flag = 0;
    if (h->dying) {
        pthread_mutex_unlock(&h->m);
        __sync_fetch_and_add(&g_exited_by_kill, 1);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&h->m);
}

static void pt_wake(void *hh, void *u)
{
    Pt *h = (Pt *)hh;

    (void)u;
    pthread_mutex_lock(&h->m);
    h->flag = 1;
    pthread_cond_signal(&h->c);
    pthread_mutex_unlock(&h->m);
}

static void *pt_body(void *p)
{
    Pt *h = (Pt *)p;

    pt_park(h, NULL);
    h->entry(h->arg);
    return NULL;
}

static void pt_init(Pt *h)
{
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    h->flag = h->dying = 0;
}

static void *pt_create(void (*entry)(void *), void *arg, uint32_t stack, void *u)
{
    Pt *h = &g_pt[g_npt++];

    (void)stack;
    (void)u;
    assert(g_npt <= 16);
    pt_init(h);
    h->entry = entry;
    h->arg = arg;
    h->live = 1;
    pthread_create(&h->th, NULL, pt_body, h);
    return h;
}

static void pt_exit(void *hh, void *u)
{
    (void)hh;
    (void)u;
    pthread_exit(NULL);
}

static void pt_kill(void *hh, void *u)
{
    Pt *h = (Pt *)hh;

    (void)u;
    __sync_fetch_and_add(&g_kills, 1);
    pthread_mutex_lock(&h->m);
    h->dying = 1;
    h->flag = 1;
    pthread_cond_signal(&h->c);
    pthread_mutex_unlock(&h->m);
}

static HwCtx g_ctx;
static const HwCtxBackend g_be = { pt_create, pt_wake, pt_park, pt_exit, pt_kill, NULL };
static HwBoot g_boot;
static volatile uint32_t g_param_word;        /* HW_RESET_PARAMETER_BUF */
static int g_hw_counter;                      /* hardware-layer state: outside every restore range, survives a reset */
static int g_hw_cleared;                      /* what reseed clears */
static int g_reseeds;
static pthread_t g_run_thread[3];
static char *g_run_local[3];
static int g_run_no;
static pthread_mutex_t g_done_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_c = PTHREAD_COND_INITIALIZER;
static int g_done;
static int g_phase_ok[3];
static int g_seq_reseed_after_restore;        /* reseed saw the .data already restored */

static void reseed(void *u)
{
    (void)u;
    g_reseeds++;
    g_seq_reseed_after_restore = strcmp(main_data, "MAIN-DATA-0123") == 0 && main_bss[0] == 0;
    g_hw_cleared = 0;
    g_param_word = 0;                          /* hw_sys_reseed zeroes the system page, the parameter word included */
}

typedef struct Own {
    uint32_t w11, w12;
} Own;
static Own g_game_own, g_b_own;
static volatile int g_b_started;

static void b_entry(void *arg)
{
    (void)arg;
    g_b_started = 1;
    (void)hwctx_save(&g_ctx, &g_b_own, &g_b_own.w11, &g_b_own.w12);
    (void)hwctx_load(&g_ctx, &g_game_own, g_game_own.w11, g_game_own.w12, 0, 0, 0, 0, 0);   /* hand back; B stays parked for good */
    g_phase_ok[2] = 99;                        /* must never run: B is killed while parked, nothing above its park may execute */
}

/* the fake NitroMain */
static void fake_nitro_main(void)
{
    char local;
    int n = ++g_run_no;

    g_run_thread[n - 1] = pthread_self();
    g_run_local[n - 1] = &local;
    if (n == 1) {
        g_hw_counter = 7;
        g_hw_cleared = 5;
        check(strcmp(main_data, "MAIN-DATA-0123") == 0 && main_bss[0] == 0, "run 1: starts on the link-time image", 0);
        strcpy(main_data, "scribbled-by-1");
        memset(main_bss, 0x77, sizeof main_bss);
        strcpy(o_data[2], "ov2-dirty");
        check(hwovl_load(&g_ovl, 2) == 0 && g_ovl.sec[2].loaded, "run 1: loads overlay 2", 0);
        g_ovl.sec[2].started = 1;
        (void)hwctx_save(&g_ctx, &g_game_own, &g_game_own.w11, &g_game_own.w12);
        (void)hwctx_load(&g_ctx, &g_b_own, 0, 0, (uintptr_t)b_entry, 0, 0, 0x10000, 0);   /* start a second context, it hands back */
        check(g_b_started == 1, "run 1: a second context exists (parked)", 0);
        g_param_word = 0xCAFE0001u;            /* OS_ResetSystem(parameter) stores it, then the hook runs */
        (void)hwboot_reset(&g_boot, g_param_word);
        g_phase_ok[1] = 99;                    /* the caller must never resume: no unwinding */
        return;
    }
    /* run 2 */
    check(n == 2, "run 2: NitroMain re-entered exactly once more", n);
    check(strcmp(main_data, "MAIN-DATA-0123") == 0, "run 2: main .data restored to the link-time image", 0);
    check(main_bss[0] == 0 && main_bss[15] == 0, "run 2: main .bss zeroed", 0);
    check(memcmp(o_data[2], "OVL2", 5) == 0 && !g_ovl.sec[2].loaded && !g_ovl.sec[2].started, "run 2: every overlay unloaded and restored", 0);
    check(g_param_word == 0xCAFE0001u, "run 2: the reset parameter is preserved (OS_GetResetParameter)", (long)g_param_word);
    check(g_seq_reseed_after_restore, "run 2: the hardware reseed ran after the restore", 0);
    check(g_hw_counter == 7, "run 2: hardware-layer state outside the restore ranges survived", g_hw_counter);
    check(g_hw_cleared == 0 && g_reseeds == 1, "run 2: reseed cleared what it owns, once", g_reseeds);
    check(!pthread_equal(g_run_thread[0], g_run_thread[1]), "run 2: a fresh host thread (fresh stack)", 0);
    check(g_run_local[0] != g_run_local[1], "run 2: a different stack address", 0);
    check(g_boot.boots == 2 && g_ctx.restarts == 2, "boot counters: NitroMain entered twice (power-on + one reset), two restarts", g_boot.boots);
    check(g_kills == 1, "run 2: the parked second context was killed", g_kills);
    check(g_phase_ok[1] == 0, "the resetting caller never resumed", g_phase_ok[1]);
    pthread_mutex_lock(&g_done_m);
    g_done = 1;
    pthread_cond_signal(&g_done_c);
    pthread_mutex_unlock(&g_done_m);
}

static void *supervisor(void *u)
{
    (void)u;
    pt_init(&g_pt[g_npt]);
    g_pt[g_npt].live = 1;
    hwctx_init(&g_ctx, &g_be, &g_pt[g_npt]);
    g_npt++;
    {
        HwBootOps ops = { reseed, NULL };

        hwovl_reset_overlays(&g_ovl);
        hwovl_reset_main(&g_ovl);
        hwboot_init(&g_boot, &g_ovl, &g_ctx, &ops, fake_nitro_main, &g_param_word, 0x10000);
    }
    (void)hwboot_start(&g_boot);   /* the supervisor (the process's main context) parks for good on success */
    return NULL;
}

static int s_rf_kills;
static void *rf_create(void (*e)(void *), void *a, uint32_t st, void *u)
{
    (void)e;
    (void)a;
    (void)st;
    return *(int *)u ? NULL : (void *)1;
}
static void rf_nop(void *h, void *u) { (void)h; (void)u; }
static void rf_kill(void *h, void *u) { (void)h; (void)u; s_rf_kills++; }
static void rf_entry(void *a) { (void)a; }

static void test_restart_failure(void)
{
    /* no thread can be made: hwctx_restart returns -1 and changes nothing (the other contexts are not killed) */
    static HwCtx c;
    static int fail = 1;
    HwCtxBackend be = { rf_create, rf_nop, rf_nop, rf_nop, rf_kill, &fail };

    hwctx_init(&c, &be, (void *)2);
    c.rec[1].used = 1;
    c.rec[1].h = (void *)3;
    check(hwctx_restart(&c, rf_entry, NULL, 0x10000) == -1, "restart without a creatable thread returns -1", 0);
    check(s_rf_kills == 0 && c.rec[1].used && c.rec[0].used && c.cur == 0 && c.restarts == 0, "...and has not killed or retired anything", s_rf_kills);
}

int main(void)
{
    pthread_t sup;
    struct timespec ts;

    alarm(30);
    test_overlays();
    test_restart_failure();
    pthread_create(&sup, NULL, supervisor, NULL);
    pthread_mutex_lock(&g_done_m);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    while (!g_done) {
        if (pthread_cond_timedwait(&g_done_c, &g_done_m, &ts) != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&g_done_m);
    check(g_done == 1, "the fake NitroMain ran twice through the reset path", g_done);
    usleep(50000);
    check(g_exited_by_kill == 1, "the killed context's thread ended without running anything above its park", g_exited_by_kill);
    check(g_phase_ok[2] == 0, "the killed context never resumed its body", g_phase_ok[2]);
    printf("ovl_boot_test: %ld checks, %ld failures\n", g_checks, g_fails);
    fflush(stdout);
    _exit(g_fails ? 1 : 0);
}
