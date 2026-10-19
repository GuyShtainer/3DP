/* hw_ctr.c -- libctru implementation of hw_ctr.h. Compiled with the devkitPro include path and WITHOUT port-shim/include
 * (`make port-hw`): the shim's stddef.h / wchar_t.h shadow system headers libctru needs. */
#include <3ds.h>
#include <assert.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hw_ctr.h"
#include "hw_log.h"
#include "hw_screen.h"

typedef struct HwCtrThread {
    LightEvent ev;
    Thread th;
    void (*entry)(void *);
    void *arg;
    int used;
    int core;             /* the core the thread was created on (-2 = the default core) */
    volatile int ran_on;  /* bet 2.7a: svcGetProcessorID read inside the thread when it first ran (-1 = not yet) */
    volatile int dying;   /* set by hwctr_thread_kill before the wake: the woken thread ends instead of returning to its caller */
} HwCtrThread;

#define HWCTR_POOL 64   /* = HWCTX_MAX: the thread hand-off never has more host threads than that */
static HwCtrThread s_pool[HWCTR_POOL];   /* static: no heap allocation in the hand-off path (golden rule 3) */

static HwCtrThread s_main;
static int s_main_init;

int64_t hwctr_rtc_secs(void)
{
    return (int64_t)(osGetTime() / 1000ULL) - 3155673600LL;   /* osGetTime = ms since 1900-01-01; 100 years = 3 155 673 600 s */
}

uint64_t hwctr_sys_tick(void)
{
    return svcGetSystemTick();
}

static void (*s_frame_hook)(void);

void hwctr_set_frame_hook(void (*hook)(void))
{
    s_frame_hook = hook;
}

static uint64_t s_idle;   /* 2.7(b) N2: ticks the pumping thread spent waiting (the vblank wait and the timer sleep); written and read on that thread only */

uint64_t hwctr_idle_ticks(void)
{
    return s_idle;
}

void hwctr_wait_vblank(void)
{
    uint64_t t0 = svcGetSystemTick();

    gspWaitForVBlank();
    s_idle += svcGetSystemTick() - t0;
    if (s_frame_hook != NULL) {
        s_frame_hook();   /* port_main.c: HOME / START+SELECT exit, the frame counter (the main thread is parked, so the pump's thread polls) */
    }
}

void hwctr_sleep_sys(uint64_t sys_ticks)
{
    /* sys ticks at 268 111 856 Hz -> ns: x * 1e9 / 268111856 ~ x * 3.7296 */
    uint64_t t0 = svcGetSystemTick();

    svcSleepThread((s64)((sys_ticks * 1000000000ull) / 268111856ull));
    s_idle += svcGetSystemTick() - t0;
}

void *hwctr_main_handle(void)
{
    if (!s_main_init) {
        LightEvent_Init(&s_main.ev, RESET_ONESHOT);
        s_main_init = 1;
    }
    return &s_main;
}

/* Azahar does not implement SVC 0x11 (GetProcessorID): the call is logged as unimplemented and the game thread stalls after it. svcGetSystemInfo type 0x20000 is the
   Citra/Azahar "information" query. Azahar answers rc 0 with v 2 (Citra v 1); Luma3DS answers rc 1 with v 0 (GetSystemInfo.c, a positive non-error so a caller can tell
   "Luma, not an emulator" by the VALUE); a kernel without the type fails. So an emulator is rc >= 0 AND v != 0 (review 2.7a B1). -1 = core not known. The probe runs once. */
static int s_emu = -1;
static int32_t s_emu_rc;
static int64_t s_emu_v;

static void emu_probe(void)
{
    if (s_emu < 0) {
        s64 v = 0;
        Result rc = svcGetSystemInfo(&v, 0x20000, 0);

        s_emu_rc = (int32_t)rc;
        s_emu_v = (int64_t)v;
        s_emu = (R_SUCCEEDED(rc) && v != 0) ? 1 : 0;
    }
}

static int safe_proc_id(void)
{
    emu_probe();
    return s_emu ? -1 : (int)svcGetProcessorID();
}

static void thread_body(void *p)
{
    HwCtrThread *t = (HwCtrThread *)p;

    LightEvent_Wait(&t->ev);   /* parked until the first hand-off */
    t->ran_on = safe_proc_id();
    if (t->dying) {
        t->used = 0;
        return;
    }
    t->entry(t->arg);
}

static void *thread_create_core(void (*entry)(void *), void *arg, uint32_t stack_bytes, int core)
{
    HwCtrThread *t = NULL;
    s32 prio = 0x30;
    unsigned i;

    assert(entry != NULL);
    for (i = 0; i < HWCTR_POOL; i++) {
        if (!s_pool[i].used) {
            t = &s_pool[i];
            break;
        }
    }
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->core = -2;
    t->ran_on = -1;
    if (R_FAILED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE))) {
        prio = 0x30;
    }
    LightEvent_Init(&t->ev, RESET_ONESHOT);
    t->entry = entry;
    t->arg = arg;
    t->th = NULL;
    if (core >= 0) {   /* bet 2.3c: the render thread asks for `core`, then for core 1 (the application's second core: needs the CPU time limit), then takes the default core; bet 2.7a: core 0 is asked for directly (the A/B control) */
        int c;

        (void)APT_SetAppCpuTimeLimit(30);
        for (c = core; c >= (core == 0 ? 0 : 1) && !t->th; c--) {
            t->th = threadCreate(thread_body, t, stack_bytes < 0x10000u ? 0x10000u : stack_bytes, prio, c, true);
            if (t->th) {
                t->core = c;
            }
        }
    }
    if (!t->th) {
        t->th = threadCreate(thread_body, t, stack_bytes < 0x10000u ? 0x10000u : stack_bytes, prio, -2, true);
    }
    if (!t->th) {
        t->used = 0;
        return NULL;
    }
    return t;
}

void *hwctr_thread_create(void (*entry)(void *), void *arg, uint32_t stack_bytes)
{
    return thread_create_core(entry, arg, stack_bytes, -2);
}

void *hwctr_thread_create_core(void (*entry)(void *), void *arg, uint32_t stack_bytes, int core)
{
    return thread_create_core(entry, arg, stack_bytes, core);
}

int hwctr_thread_ran_on(void *handle)
{
    assert(handle != NULL);
    return ((HwCtrThread *)handle)->ran_on;
}

int hwctr_proc_id(void)
{
    return safe_proc_id();
}

/* Bet 2.7a, the run header's facts. The CPU time limit is set here at boot (the same call and value thread_create_core makes): the rc is what the hardware session needs to see. */
void hwctr_info(HwCtrInfo *out)
{
    float slider = osGet3DSliderState();

    assert(out != NULL);
    memset(out, 0, sizeof *out);
    out->is_new3ds = 0;
    {
        bool n3 = false;

        if (R_SUCCEEDED(APT_CheckNew3DS(&n3))) {
            out->is_new3ds = n3 ? 1 : 0;
        } else {
            out->is_new3ds = -1;
        }
    }
    out->homebrew = envIsHomebrew() ? 1 : 0;
    out->cpu_limit_rc = (int32_t)APT_SetAppCpuTimeLimit(30);
    out->slider_pct = (int)(slider * 100.0f + 0.5f);
    out->linear_free = (uint32_t)linearSpaceFree();
    out->proc_id = safe_proc_id();
    out->emu = s_emu;
    out->emu_rc = s_emu_rc;
    out->emu_v = s_emu_v;
}

uint32_t hwctr_linear_free(void)
{
    return (uint32_t)linearSpaceFree();
}

uint32_t hwctr_heap_free(uint32_t *total)   /* review 2.8 N7: the newlib heap (a fixed block of the process): what is not in use inside the arena + what sbrk can still add */
{
    struct mallinfo mi = mallinfo();
    uint32_t tot = (uint32_t)envGetHeapSize();
    uint32_t used = (uint32_t)mi.uordblks;

    if (total != NULL) {
        *total = tot;
    }
    return used >= tot ? 0u : tot - used;
}

int hwctr_slider_pct(void)
{
    return (int)(osGet3DSliderState() * 100.0f + 0.5f);
}

/* A dependent chain of 256 adds per loop pass (one cycle each on the ARM11 pipeline: a calibration ASSUMPTION, see the private design notes), timed against the
   268 MHz system tick; the fastest of 5 passes is the clock (interrupts only ever slow a pass). MHz * 100, so 80400 = 804.00 MHz. */
static void spin_adds(uint32_t passes)
{
    uint32_t x = 1u;

    __asm__ volatile("1:\n"
                     ".rept 256\n"
                     "add %0, %0, #1\n"
                     ".endr\n"
                     "subs %1, %1, #1\n"
                     "bne 1b\n"
                     : "+r"(x), "+r"(passes)
                     :
                     : "cc");
}

uint32_t hwctr_measure_mhz100(void)
{
    enum { PASSES = 4000, REPS = 5 };
    u64 best = ~0ull;
    int r;

    for (r = 0; r < REPS; r++) {
        u64 t0 = svcGetSystemTick();
        u64 dt;

        spin_adds(PASSES);
        dt = svcGetSystemTick() - t0;
        if (dt < best) {
            best = dt;
        }
    }
    if (best == 0ull) {
        return 0u;
    }
    return (uint32_t)((u64)PASSES * 256u * 26811185600ull / (best * 1000000ull));   /* MHz x 100 = cycles / ticks * 268.111856e6 Hz / 1e6 x 100 (26811185600 = 268.111856e6 x 100) */
}

int hwctr_thread_core(void *handle)
{
    assert(handle != NULL);
    return ((HwCtrThread *)handle)->core;
}

void hwctr_thread_wake(void *handle)
{
    assert(handle != NULL);
    LightEvent_Signal(&((HwCtrThread *)handle)->ev);
}

void hwctr_thread_park(void *handle)
{
    HwCtrThread *t = (HwCtrThread *)handle;

    assert(handle != NULL);
    LightEvent_Wait(&t->ev);
    if (t->dying) {          /* hwctr_thread_kill: end here, nothing above this frame runs */
        t->used = 0;
        threadExit(0);
    }
}

void hwctr_thread_kill(void *handle)
{
    HwCtrThread *t = (HwCtrThread *)handle;

    assert(handle != NULL);
    if (t == &s_main) {
        return;              /* the process's own thread cannot end; it stays parked and nothing wakes it again */
    }
    t->dying = 1;
    LightEvent_Signal(&t->ev);
}

void hwctr_thread_exit_current(void *handle)
{
    assert(handle != NULL);
    if (handle != &s_main) {
        ((HwCtrThread *)handle)->used = 0;   /* detached: libctru frees the stack when the thread function exits; the slot returns to the pool */
    }
    threadExit(0);
    for (;;) {
    }
}

void hwctr_mkdirs(const char *dir)
{
    char tmp[256];
    size_t i, n = strlen(dir);

    assert(dir != NULL);
    if (n >= sizeof tmp) {
        return;
    }
    memcpy(tmp, dir, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            (void)mkdir(tmp, 0777);   /* an existing directory is the normal case: the final mkdir below is the one that is checked by use */
            tmp[i] = '/';
        }
    }
    (void)mkdir(tmp, 0777);
}

int hwctr_file_read(const char *path, void *buf, uint32_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        return -1;
    }
    n = fread(buf, 1, cap, f);
    if (fclose(f) != 0) {
        return -1;
    }
    return (int)n;
}

int hwctr_file_write(const char *path, const void *buf, uint32_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n;

    if (!f) {
        return -1;
    }
    n = fwrite(buf, 1, len, f);
    if (fclose(f) != 0) {
        return -1;
    }
    return n == len ? (int)n : -1;
}

void *hwctr_file_open(const char *path)
{
    assert(path != NULL);
    return fopen(path, "rb");
}

int hwctr_file_pread(void *file, uint32_t off, void *dst, uint32_t len)
{
    size_t n;

    assert(file != NULL && dst != NULL);
    if (fseek((FILE *)file, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    n = fread(dst, 1, len, (FILE *)file);
    if (n < len && ferror((FILE *)file)) {
        return -1;
    }
    return (int)n;
}

void *hwctr_file_create(const char *path)
{
    assert(path != NULL);
    return fopen(path, "wb");
}

int hwctr_file_append(void *file, const void *buf, uint32_t len)
{
    assert(file != NULL && buf != NULL);
    return (int)fwrite(buf, 1, len, (FILE *)file);
}

void hwctr_file_close(void *file)
{
    if (file != NULL) {
        (void)fclose((FILE *)file);
    }
}

int hwctr_file_sync(void *file)
{
    assert(file != NULL);
    if (fflush((FILE *)file) != 0) {
        return -1;
    }
    return fsync(fileno((FILE *)file)) == 0 ? 0 : -1;
}

int hwctr_file_remove(const char *path)
{
    assert(path != NULL);
    return (remove(path) == 0 || access(path, F_OK) != 0) ? 0 : -1;
}

int hwctr_dir_list(const char *dir, HwCtrDirEnt *out, int max, int *skipped)
{
    DIR *d;
    struct dirent *e;
    int n = 0, lost = 0;

    assert(dir != NULL && out != NULL && max > 0);
    d = opendir(dir);
    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (len == 0u || strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (len >= HWCTR_NAME_MAX || n >= max) {   /* too long for the entry, or past the caller's room: counted, never silently dropped */
            lost++;
            continue;
        }
        memcpy(out[n].name, e->d_name, len + 1u);
        out[n].is_dir = e->d_type == DT_DIR ? 1u : 0u;
        n++;
    }
    (void)closedir(d);
    if (skipped != NULL) {
        *skipped = lost;
    }
    return n;
}

uint32_t hwctr_hid_wait(int wait, int *alive)
{
    assert(alive != NULL);
    if (wait != 0) {
        gspWaitForVBlank();
    }
    hidScanInput();
    *alive = aptMainLoop() ? 1 : 0;
    return hidKeysHeld();
}

int hwctr_file_size(const char *path)
{
    struct stat st;

    assert(path != NULL);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 0x7fffffff) {
        return -1;
    }
    return (int)st.st_size;
}

int hwctr_file_rename(const char *from, const char *to)
{
    assert(from != NULL && to != NULL);
    if (hwctr_file_remove(to) != 0) {
        return -1;
    }
    return rename(from, to) == 0 ? 0 : -1;
}

#define CON_KEEP 16384u
static char s_con[CON_KEEP];   /* tail of everything written to the text console (the emulator gate cannot see the screen; the toggle redraws from it) */
static uint32_t s_con_len;

/* ---- the bottom screen: the game, or the text console (bet 2.3c). One lock covers the console text, the policy and every framebuffer access: the render thread
   (hwctr_gfx_present), the game thread (console writes, the ZL+ZR chord) and a fatal error can all reach it. ---- */
static LightLock s_gfx_lock;
static int s_gfx_ready;
static HwScrCon s_scon;
static PrintConsole s_pc;
static int s_con_shown;   /* the bottom framebuffer currently carries the console (RGB565, single buffer) */

void hwctr_gfx_init(void)
{
    LightLock_Init(&s_gfx_lock);
    hwscr_con_init(&s_scon);
    s_gfx_ready = 1;
}

static void gfx_commit_bottom(void)
{
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

/* lock held: make the bottom screen match the policy. Showing the console re-initialises it and reprints the tail; hiding returns the screen to BGR8 / double
   buffered (consoleInit switched it to RGB565) and blanks it until the next presented frame. */
static void console_reconcile(void)
{
    if (s_scon.visible && !s_con_shown) {
        consoleInit(GFX_BOTTOM, &s_pc);
        consoleClear();
        fwrite(s_con, 1, s_con_len, stdout);
        s_con_shown = 1;
        gfx_commit_bottom();
    } else if (!s_scon.visible && s_con_shown) {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        s_con_shown = 0;
        hwscr_black_bgr8(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), HWSCR_BOT_W);
        gfx_commit_bottom();
    }
}

void hwctr_console_write(const char *text)
{
    uint32_t n;

    assert(text != NULL);
    if (s_gfx_ready) {
        LightLock_Lock(&s_gfx_lock);
    }
    n = (uint32_t)strlen(text);
    if (n >= CON_KEEP) {
        text += n - (CON_KEEP - 1u);
        n = CON_KEEP - 1u;
    }
    if (s_con_len + n > CON_KEEP) {
        uint32_t drop = s_con_len + n - CON_KEEP;
        memmove(s_con, s_con + drop, s_con_len - drop);
        s_con_len -= drop;
    }
    memcpy(s_con + s_con_len, text, n);
    s_con_len += n;
    if (s_scon.visible) {
        if (s_con_shown) {
            fwrite(text, 1, n, stdout);
            gfx_commit_bottom();
        } else {
            console_reconcile();   /* the first text: bring the console up (it prints the tail, which already holds this text) */
        }
    }
    if (s_gfx_ready) {
        LightLock_Unlock(&s_gfx_lock);
    }
}

void hwctr_console_notice(const char *text)
{
    assert(text != NULL);
    hwctr_console_write(text);   /* keeps the text whether or not the console is up */
    if (s_gfx_ready) {
        LightLock_Lock(&s_gfx_lock);
        hwscr_con_show(&s_scon);
        console_reconcile();
        LightLock_Unlock(&s_gfx_lock);
    }
}

void hwctr_console_chord(unsigned held)
{
    assert(s_gfx_ready);
    LightLock_Lock(&s_gfx_lock);
    if (hwscr_con_input(&s_scon, held != 0u ? 1u : 0u)) {
        console_reconcile();
    }
    LightLock_Unlock(&s_gfx_lock);
}

/* One composited frame to the screens (the render thread). `top` / `bot` are 256 x 192 0xFFRRGGBB images (hw_screen.h); the bottom one is skipped while the console
   owns that screen. The first presented frame hides the boot console. */
static int s_top_3d;   /* bet 2.8: the top screen is in stereo mode (gfxSet3D) */

static void top_3d(int on)
{
    if (on != s_top_3d) {
        gfxSet3D(on != 0);
        s_top_3d = on;
    }
}

void hwctr_gfx_present(const uint32_t *top, const uint32_t *bot)
{
    assert(top != NULL && bot != NULL && s_gfx_ready);
    LightLock_Lock(&s_gfx_lock);
    (void)hwscr_con_presented(&s_scon);
    console_reconcile();
    top_3d(0);
    hwscr_blit_bgr8(top, gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), HWSCR_TOP_W);
    if (!s_con_shown) {
        hwscr_blit_bgr8(bot, gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), HWSCR_BOT_W);
    }
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, false);
    if (!s_con_shown) {
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    }
    LightLock_Unlock(&s_gfx_lock);
}

/* Bet 2.8: the wide top screen (400 x 240, edge to edge) and the bottom one as hwctr_gfx_present. right != NULL: the top screen goes to stereo mode (gfxSet3D) and gets the
   left / right images; NULL: mono (stereo mode off). */
void hwctr_gfx_present_wide(const uint32_t *left, const uint32_t *right, const uint32_t *bot)
{
    assert(left != NULL && bot != NULL && s_gfx_ready);
    LightLock_Lock(&s_gfx_lock);
    (void)hwscr_con_presented(&s_scon);
    console_reconcile();
    top_3d(right != NULL);
    hwscr_blit_bgr8_wide(left, gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL));
    if (right != NULL) {
        hwscr_blit_bgr8_wide(right, gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL));
    }
    if (!s_con_shown) {
        hwscr_blit_bgr8(bot, gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), HWSCR_BOT_W);
    }
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, right != NULL);
    if (!s_con_shown) {
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    }
    LightLock_Unlock(&s_gfx_lock);
}

void hwctr_console_save(void)
{
    hwctr_mkdirs(HWCTR_APP_DIR);
    (void)hwctr_file_write(HWCTR_APP_DIR "/console.txt", s_con, s_con_len);
}

void hwctr_fatal(const char *message)
{
    if (s_gfx_ready) {
        LightLock_Lock(&s_gfx_lock);
        hwscr_con_force(&s_scon);   /* the app is dead: the text is the only thing worth showing */
        console_reconcile();
        LightLock_Unlock(&s_gfx_lock);
    }
    hwctr_console_write("\nFATAL: ");
    hwctr_console_write(message);
    hwctr_console_write("\n");
    (void)hwlog_dump();
    hwctr_console_save();
    for (;;) {
        gspWaitForVBlank();
    }
}
