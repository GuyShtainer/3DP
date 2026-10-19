/* hw_savewb.c -- bet 2.6b: the backup write-back (hw_savewb.h). Rules: ~/.claude/c-golden-rules.md.
 *
 * Flow. The SDK brackets every backup request with CARD_LockBackup / CARD_UnlockBackup; their hooks feed hw_save.c, which says a flush is DUE once a flash write landed, no
 * lock is held and 30 DS vblanks passed with no further change. The game thread then (1) raises `snap_busy`, (2) wakes the worker. The worker copies the flash image to its
 * own buffer, clears `snap_busy`, and (best effort, bet 2.6c) copies the old <path> to <path>.orig on the first write of the run when no .orig exists, re-reads that copy and compares
 * it; then writes <path>.tmp whole, fsyncs, re-reads it and compares with the copy, then replaces <path> with the .tmp. The game thread picks the result up at its next frame and logs it.
 * A failed .orig copy is logged once and never blocks the save (review 2.6 B1). A failed write-back is retried with the back-off of hw_save.h and, from the second failure in a row, shown
 * to the player (hwctr_console_notice). At boot hwwb_boot_path adopts a stranded .tmp (review 2.6 B2).
 *
 * Why the worker's copy cannot race a game write: flash_write / flash_erase are only reached from a CARD backup request, and the SDK holds the backup lock around every request
 * (CARD_LockBackup ... CARD_UnlockBackup, card_common.c). A flush is handed out only while the lock depth is 0, so no request is in flight when `snap_busy` goes up; the game
 * thread is the only caller of the lock hook, and hwwb_lock waits for `snap_busy` to drop before it returns, so no request can start during the copy. The copy is one memcpy of
 * 512 KB (well under a millisecond on the ARM11); everything slow (SD writes) happens after `snap_busy` dropped, on the worker's private copy. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_ctr.h"
#include "hw_log.h"
#include "hw_save.h"
#include "hw_savewb.h"

#define CHUNK 0x8000u   /* SD file work in 32 KB pieces */
#define PATH_MAX_WB 96u
#define WB_LOCK_WAIT_MS 2000u   /* the backup lock waits at most this long for the snapshot copy (1 ms sleeps); then it proceeds and the flush is discarded */
#define WB_STOP_WAIT_MS 20000u  /* the exit waits at most this long for a flush in flight, and again for the final one (1 ms sleeps) */
#define WB_TICKS_PER_MS 268112u /* sys ticks per millisecond */

typedef struct WbResult {
    uint32_t ok, bytes, ms10, orig;   /* orig: 0 none to keep, 1 written, 2 kept (existed), 3 FAILED (best effort: never fails the flush) */
    char why[48];                     /* first failing step, "" when ok */
} WbResult;

static struct {
    HwSave sm;
    const uint8_t *flash;
    uint32_t bytes;
    char path[PATH_MAX_WB], tmp[PATH_MAX_WB], orig[PATH_MAX_WB];
    uint8_t *snap;                    /* the worker's private copy (allocated once, hwwb_init) */
    uint8_t *buf;                     /* 2 x CHUNK of file work space (allocated once, hwwb_init) */
    void *thread;
    void *self;                       /* the worker's own handle (set before the first wake) */
    int core;
    uint32_t snap_busy;               /* atomic: the copy is running; the lock hook waits */
    uint32_t torn;                    /* atomic: the lock gave up waiting for the copy, so the snapshot may be torn: the flush is discarded */
    uint32_t job_done;                /* atomic: the worker published `res` */
    uint32_t orig_tried;              /* the run's .orig decision was made (a failed copy is NOT retried) */
    uint32_t orig_state, orig_logged; /* 1 written, 2 kept, 3 failed; set by the worker (published by job_done), logged once */
    WbResult res;
    HwWbStats st;
    uint32_t flush_no;
    int attached;
} g;

/* ---- the worker ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* 0 = `to` is a copy of `from`; reads at most limit bytes (+1 chunk to see EOF): a longer source fails */
static int copy_file(const char *from, const char *to, uint8_t *buf, uint32_t limit)
{
    void *in = hwctr_file_open(from), *out;
    uint32_t off = 0u, i, max_iter = limit / CHUNK + 1u;
    int rc = -1;

    assert(from != NULL && to != NULL && buf != NULL);
    if (in == NULL) {
        return -1;
    }
    out = hwctr_file_create(to);
    if (out == NULL) {
        hwctr_file_close(in);
        return -1;
    }
    for (i = 0u; i <= max_iter; i++) {
        int n = hwctr_file_pread(in, off, buf, CHUNK);

        if (n < 0 || (n > 0 && hwctr_file_append(out, buf, (uint32_t)n) != n)) {
            break;
        }
        off += (uint32_t)n;
        if ((uint32_t)n < CHUNK) {
            rc = 0;   /* EOF inside the bound */
            break;
        }
    }
    if (rc == 0 && hwctr_file_sync(out) != 0) {
        rc = -1;
    }
    hwctr_file_close(in);
    hwctr_file_close(out);
    return rc;
}

/* 0 = both files hold the same bytes and the same length (buf = two CHUNK halves) */
static int files_equal(const char *a, const char *b, uint8_t *buf, uint32_t limit)
{
    void *fa = hwctr_file_open(a), *fb = hwctr_file_open(b);
    uint32_t off, i, max_iter = limit / CHUNK + 1u;
    int rc = -1;

    assert(a != NULL && b != NULL && buf != NULL);
    if (fa != NULL && fb != NULL) {
        rc = 1;   /* "no verdict yet" */
        for (i = 0u, off = 0u; i <= max_iter && rc == 1; i++, off += CHUNK) {
            int na = hwctr_file_pread(fa, off, buf, CHUNK), nb = hwctr_file_pread(fb, off, buf + CHUNK, CHUNK);

            if (na < 0 || na != nb || memcmp(buf, buf + CHUNK, (size_t)na) != 0) {
                rc = -1;
            } else if ((uint32_t)na < CHUNK) {
                rc = 0;
            }
        }
        if (rc == 1) {
            rc = -1;   /* longer than the bound */
        }
    }
    hwctr_file_close(fa);
    hwctr_file_close(fb);
    return rc;
}

/* 0 = the file holds exactly snap[0..bytes) and nothing more */
static int verify_file(const char *path, const uint8_t *snap, uint32_t bytes, uint8_t *buf)
{
    void *f = hwctr_file_open(path);
    uint32_t off;
    int rc = 0;

    assert(path != NULL && snap != NULL && buf != NULL);
    if (f == NULL) {
        return -1;
    }
    for (off = 0u; off < bytes && rc == 0; off += CHUNK) {
        uint32_t want = bytes - off < CHUNK ? bytes - off : CHUNK;

        if (hwctr_file_pread(f, off, buf, want) != (int)want || memcmp(buf, snap + off, want) != 0) {
            rc = -1;
        }
    }
    if (rc == 0 && hwctr_file_pread(f, bytes, buf, 1u) != 0) {
        rc = -1;   /* longer than the image */
    }
    hwctr_file_close(f);
    return rc;
}

static void fail(WbResult *r, const char *why)
{
    assert(r != NULL && why != NULL);
    r->ok = 0u;
    (void)strncpy(r->why, why, sizeof r->why - 1u);
    r->why[sizeof r->why - 1u] = 0;
}

/* The first write of this run keeps the file the run started with, once, never over an older .orig. BEST EFFORT (review 2.6 B1): a failed copy is recorded
   (orig_state 3), its partial file removed, never retried this run, and never fails the flush: the original stays on the SD card until the rename anyway. */
static void do_orig(WbResult *r, uint8_t *buf)
{
    assert(r != NULL && buf != NULL);
    if (g.orig_tried != 0u) {
        return;
    }
    g.orig_tried = 1u;
    if (hwctr_file_size(g.orig) >= 0) {
        r->orig = 2u;
        g.orig_state = 2u;
        return;
    }
    if (hwctr_file_size(g.path) < 0) {
        return;   /* no original to keep (a first run) */
    }
    if (copy_file(g.path, g.orig, buf, g.bytes) == 0 && files_equal(g.path, g.orig, buf, g.bytes) == 0) {
        r->orig = 1u;
        g.orig_state = 1u;
        return;
    }
    (void)hwctr_file_remove(g.orig);   /* a partial copy must not pass for the original next time (a failed remove leaves it; the log line says the copy failed) */
    r->orig = 3u;
    g.orig_state = 3u;
}

/* snapshot -> <path>.tmp, fsync, re-read and compare. NULL = ok, else the fixed failure text */
static const char *write_tmp(uint8_t *buf)
{
    void *f = hwctr_file_create(g.tmp);
    uint32_t off;

    assert(buf != NULL && g.snap != NULL);
    if (f == NULL) {
        return "cannot create the .tmp";
    }
    for (off = 0u; off < g.bytes; off += CHUNK) {
        uint32_t n = g.bytes - off < CHUNK ? g.bytes - off : CHUNK;

        if (hwctr_file_append(f, g.snap + off, n) != (int)n) {
            hwctr_file_close(f);
            return "short write to the .tmp";
        }
    }
    if (hwctr_file_sync(f) != 0) {
        hwctr_file_close(f);
        return "fsync of the .tmp failed";
    }
    hwctr_file_close(f);
    return verify_file(g.tmp, g.snap, g.bytes, buf) != 0 ? "re-read of the .tmp differs" : NULL;
}

/* replace the save with the verified .tmp and verify the result. NULL = ok */
static const char *commit_tmp(uint8_t *buf)
{
    assert(buf != NULL && g.snap != NULL);
    if (hwctr_file_rename(g.tmp, g.path) != 0) {
        return "rename over the save failed";
    }
    return verify_file(g.path, g.snap, g.bytes, buf) != 0 ? "re-read after the rename differs" : NULL;
}

/* the whole job; runs on the worker (or, at exit, on the caller) with the snapshot already taken */
static void do_job(WbResult *r, uint8_t *buf)
{
    const char *why;

    assert(r != NULL && buf != NULL);
    memset(r, 0, sizeof *r);
    r->bytes = g.bytes;
    do_orig(r, buf);
    why = write_tmp(buf);
    if (why == NULL && __atomic_load_n(&g.torn, __ATOMIC_ACQUIRE) != 0u) {
        why = "snapshot raced a game write";   /* the lock hook gave up waiting for the copy: this image may be torn, never rename it into place */
    }
    if (why == NULL) {
        why = commit_tmp(buf);
    }
    if (why != NULL) {
        fail(r, why);
    } else {
        r->ok = 1u;
    }
}

static void worker_run(void)
{
    uint64_t t0 = hwctr_sys_tick(), t1;
    WbResult r;

    assert(g.snap != NULL && g.buf != NULL);
    memcpy(g.snap, g.flash, g.bytes);   /* the snapshot: see the argument at the top of the file */
    __atomic_store_n(&g.snap_busy, 0u, __ATOMIC_RELEASE);
    do_job(&r, g.buf);
    t1 = hwctr_sys_tick();
    r.ms10 = (uint32_t)((t1 - t0) * 10u / WB_TICKS_PER_MS);   /* sys ticks -> tenths of a millisecond */
    g.res = r;
    g.st.worker_ticks += t1 - t0;
    __atomic_store_n(&g.job_done, 1u, __ATOMIC_RELEASE);
}

static void worker_thread(void *arg)
{
    void *self = *(void *const *)arg;

    for (;;) {
        worker_run();
        hwctr_thread_park(self);
    }
}

/* ---- boot recovery (review 2.6 B2) ------------------------------------------------------------------------------------------------------------------------------------ */

/* Which file the boot should load. Normally `path`. If the save is missing and a complete <path>.tmp exists (a power cut between the remove and the rename of the last write-back),
   the .tmp is renamed into place first; if that rename fails the .tmp itself is loaded. Called before hwwb_init, may be called more than once. */
const char *hwwb_boot_path(const char *path, uint32_t bytes)
{
    static char tmp[PATH_MAX_WB];
    uint32_t act;
    int tsz;

    assert(path != NULL && bytes != 0u);
    assert(strlen(path) + 8u < PATH_MAX_WB);
    (void)snprintf(tmp, sizeof tmp, "%s%s", path, HWSAVEWB_SUFFIX_TMP);
    tsz = hwctr_file_size(tmp);
    act = hwsave_boot_action(hwctr_file_size(path) >= 0, tsz, bytes);
    if (act == HWSAVE_BOOT_ADOPT_TMP) {
        if (hwctr_file_rename(tmp, path) == 0) {
            HWLOG("save recovery: .sav missing, complete %d B .tmp adopted (cut between remove and rename), loading it", tsz);
            return path;
        }
        HWLOG("save recovery: .sav missing, complete .tmp found but the rename failed: loading the .tmp directly");
        return tmp;
    }
    if (act == HWSAVE_BOOT_ERASED_BAD_TMP) {
        HWLOG("save recovery: .sav missing, .tmp is %d B not %u B: not trusted, the flash starts erased", tsz, (unsigned)bytes);
    }
    return path;
}

/* ---- the game thread ------------------------------------------------------------------------------------------------------------------------------------------------------- */

void hwwb_init(const uint8_t *flash, uint32_t bytes, const char *path, uint32_t counter)
{
    assert(flash != NULL && path != NULL && bytes != 0u);
    assert(strlen(path) + 8u < PATH_MAX_WB);
    if (g.attached) {
        return;
    }
    memset(&g, 0, sizeof g);
    g.flash = flash;
    g.bytes = bytes;
    g.core = -1;
    (void)snprintf(g.path, sizeof g.path, "%s", path);
    (void)snprintf(g.tmp, sizeof g.tmp, "%s%s", path, HWSAVEWB_SUFFIX_TMP);
    (void)snprintf(g.orig, sizeof g.orig, "%s%s", path, HWSAVEWB_SUFFIX_ORIG);
    g.snap = (uint8_t *)malloc(bytes);   /* once, here (c-golden-rules 3): no allocation, so no allocation failure, in the middle of a save */
    g.buf = (uint8_t *)malloc(2u * CHUNK);
    if (g.snap == NULL || g.buf == NULL) {
        free(g.snap);
        free(g.buf);
        g.snap = NULL;
        g.buf = NULL;
        HWLOG("save write-back: no memory for the %u B snapshot + file buffer: every flush will fail and be reported", (unsigned)bytes);
    }
    hwsave_init(&g.sm, counter);
    g.attached = 1;
}

void hwwb_lock(void)
{
    uint64_t t0;

    if (!g.attached) {
        return;
    }
    t0 = hwctr_sys_tick();
    if (__atomic_load_n(&g.snap_busy, __ATOMIC_ACQUIRE) != 0u) {   /* the snapshot copy is running: wait for it (<= 1 ms normally, bounded at WB_LOCK_WAIT_MS) */
        uint32_t spins = 0u;

        g.st.lock_waits++;
        while (__atomic_load_n(&g.snap_busy, __ATOMIC_ACQUIRE) != 0u && spins < WB_LOCK_WAIT_MS) {
            hwctr_sleep_sys(WB_TICKS_PER_MS);
            spins++;
        }
        if (__atomic_load_n(&g.snap_busy, __ATOMIC_ACQUIRE) != 0u) {   /* a starved worker: do not kill the game's save, discard this flush instead (review 2.6 N1) */
            __atomic_store_n(&g.torn, 1u, __ATOMIC_RELEASE);
            HWLOG("save write-back: the snapshot copy did not finish within %u ms; the save goes on, this flush is discarded and retried", (unsigned)WB_LOCK_WAIT_MS);
        }
    }
    hwsave_lock(&g.sm);
    g.st.game_ticks += hwctr_sys_tick() - t0;
    g.st.game_calls++;
}

void hwwb_unlock(uint32_t counter, uint32_t vb)
{
    uint64_t t0;

    if (!g.attached) {
        return;
    }
    t0 = hwctr_sys_tick();
    hwsave_unlock(&g.sm, counter, vb);
    g.st.game_ticks += hwctr_sys_tick() - t0;
    g.st.game_calls++;
}

static void log_result(const WbResult *r, uint32_t no)
{
    assert(r != NULL && no >= 1u);
    /* hwlog records the FIRST call of each site: one site per flush number, so every flush keeps its line (flush 4 and later share the last site) */
    if (r->ok == 0u) {
        HWLOG("save write-back FAILED: %s; retried with a back-off (%u.%u ms, flush #%u)", r->why, (unsigned)(r->ms10 / 10u), (unsigned)(r->ms10 % 10u), (unsigned)no);
        return;
    }
    switch (no) {
    case 1u:
        HWLOG("save write-back: %u B, verify ok, %u.%u ms, flush #%u", (unsigned)r->bytes, (unsigned)(r->ms10 / 10u), (unsigned)(r->ms10 % 10u), (unsigned)no);
        break;
    case 2u:
        HWLOG("save write-back: %u B, verify ok, %u.%u ms, flush #%u", (unsigned)r->bytes, (unsigned)(r->ms10 / 10u), (unsigned)(r->ms10 % 10u), (unsigned)no);
        break;
    case 3u:
        HWLOG("save write-back: %u B, verify ok, %u.%u ms, flush #%u", (unsigned)r->bytes, (unsigned)(r->ms10 / 10u), (unsigned)(r->ms10 % 10u), (unsigned)no);
        break;
    default:
        HWLOG("save write-back: %u B, verify ok, %u.%u ms, flush #%u (later flushes of the run share this line)", (unsigned)r->bytes, (unsigned)(r->ms10 / 10u),
              (unsigned)(r->ms10 % 10u), (unsigned)no);
        break;
    }
}

static void log_orig(void)
{
    if (g.orig_logged != 0u) {
        return;
    }
    if (g.orig_state == 1u) {
        HWLOG("save backup copy: the file this run started with is kept as %s (copied and re-read: identical)", g.orig);
    } else if (g.orig_state == 2u) {
        HWLOG("save backup copy: %s already exists, kept (the oldest original stays)", g.orig);
    } else if (g.orig_state == 3u) {
        HWLOG("save backup copy FAILED: %s could not be written or did not re-read identical; not retried this run, the save write-back goes on without it", g.orig);
    } else {
        return;
    }
    g.orig_logged = 1u;
}

/* the player-visible line (review 2.6 B1): a fixed text on the bottom-screen console, brought up by hwctr_console_notice */
static void show_notice(uint32_t notice, const char *why)
{
    char line[160];

    assert(why != NULL);
    if (notice == HWSAVE_NOTICE_FAIL2) {
        (void)snprintf(line, sizeof line, "\n3DP: save NOT written to the SD card (%s). Still retrying; your game is safe to keep playing, but check the card.\n", why);
        HWLOG("save notice shown to the player after the second failure in a row: %s", why);
    } else {
        (void)snprintf(line, sizeof line, "\n3DP: save write-back gave up (%s). It is tried again at your next in-game save and when the game closes.\n", why);
        HWLOG("save notice shown to the player after the seventh failure in a row: gave up until the next save: %s", why);
    }
    HWCOUNT_ADD("save: player notices shown (console, bottom screen)", 1u);
    hwctr_console_notice(line);
}

/* the single exit of a flush on the game thread: tell the state machine, show the notice when it asks */
static void finish_flush(uint32_t ok, const char *why, uint32_t vb)
{
    uint32_t notice = hwsave_done(&g.sm, ok, vb);

    if (notice != HWSAVE_NOTICE_NONE) {
        show_notice(notice, why);
    }
}

static void harvest(uint32_t vb)
{
    WbResult r;

    if (__atomic_load_n(&g.job_done, __ATOMIC_ACQUIRE) == 0u) {
        return;
    }
    r = g.res;
    g.job_done = 0u;
    if (r.ok != 0u) {
        g.st.flushes_ok++;
        g.flush_no++;
        HWCOUNT_ADD("save: write-back flushes (verified)", 1u);
    } else {
        g.st.flushes_failed++;
        HWCOUNT_ADD("save: write-back flushes FAILED", 1u);
    }
    log_result(&r, g.flush_no + (r.ok == 0u ? 1u : 0u));
    log_orig();
    g.st.orig_copied = g.orig_state;
    finish_flush(r.ok, r.why, vb);
}

/* hand the flush to the worker (creating it at the first use); 0 = could not start (stays pending, counted as a failure) */
static int start_job(uint32_t vb)
{
    if (g.snap == NULL || g.buf == NULL) {
        g.st.flushes_failed++;
        HWLOG("save write-back FAILED: no memory for the %u B snapshot", (unsigned)g.bytes);
        finish_flush(0u, "no memory for the snapshot", vb);
        return 0;
    }
    __atomic_store_n(&g.torn, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g.snap_busy, 1u, __ATOMIC_RELEASE);
    if (g.thread == NULL) {
        g.thread = hwctr_thread_create_core(worker_thread, &g.self, 0x10000u, 2);
        if (g.thread == NULL) {
            __atomic_store_n(&g.snap_busy, 0u, __ATOMIC_RELEASE);
            g.st.flushes_failed++;
            HWLOG("save write-back FAILED: no thread for the worker");
            finish_flush(0u, "no thread for the worker", vb);
            return 0;
        }
        g.self = g.thread;
        g.core = hwctr_thread_core(g.thread);
        g.st.worker_core = g.core;
    }
    hwctr_thread_wake(g.thread);
    return 1;
}

void hwwb_frame(uint32_t counter, uint32_t vb)
{
    uint64_t t0;

    if (!g.attached) {
        return;
    }
    t0 = hwctr_sys_tick();
    harvest(vb);
    if (hwsave_tick(&g.sm, counter, vb) != 0u) {
        (void)start_job(vb);
    }
    g.st.game_ticks += hwctr_sys_tick() - t0;
    g.st.game_calls++;
}

/* wait (<= WB_STOP_WAIT_MS) for the flush in flight; 1 = it finished */
static int wait_in_flight(uint32_t vb)
{
    uint32_t spins = 0u;

    while (g.sm.in_flight != 0u && spins < WB_STOP_WAIT_MS) {
        harvest(vb);
        if (g.sm.in_flight != 0u) {
            hwctr_sleep_sys(WB_TICKS_PER_MS);
            spins++;
        }
    }
    return g.sm.in_flight == 0u;
}

void hwwb_stop(uint32_t counter, uint32_t vb)
{
    if (!g.attached) {
        return;
    }
    if (!wait_in_flight(vb)) {   /* review 2.6 N2: the owed flush is dropped, and now it says so */
        HWLOG("save write-back: the flush in flight did not finish within %u ms at exit; the owed flush is dropped", (unsigned)WB_STOP_WAIT_MS);
        return;
    }
    if (g.sm.depth != 0u) {
        HWLOG("save write-back: the app ended with the backup lock held (a save was in progress): nothing flushed");
        return;
    }
    if (hwsave_flush_now(&g.sm, counter, vb) != 0u && start_job(vb) != 0 && !wait_in_flight(vb)) {
        HWLOG("save write-back: the final flush did not finish within %u ms at exit; the app ends without its result", (unsigned)WB_STOP_WAIT_MS);
    }
}

void hwwb_stats(HwWbStats *out)
{
    assert(out != NULL);
    g.st.locks = g.sm.locks;
    g.st.unlocks = g.sm.unlocks;
    g.st.coalesced = g.sm.coalesced;
    *out = g.st;
    out->worker_ran_on = g.thread != NULL ? hwctr_thread_ran_on(g.thread) : -1;
}
