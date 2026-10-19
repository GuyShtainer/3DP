/* hw_save_test.c -- bet 2.6b: the backup write-back state machine of source/hw/hw_save.c. Native, no SDK, no libctru. Cases: a lock/write/unlock round makes a flush pending
 * and it is due only after the 30-vblank debounce; a save is SIX rounds and yields ONE flush; a second save inside the window coalesces, one after it flushes again;
 * SaveDataState_Cancel-shaped rounds (lock + unlock with no write) never flush; nothing is due while locked or while a flush is in flight; a write outside any lock becomes
 * pending at the tick that saw it; a failed write retries after another window; the counter wrap; the exit rule (owed). Prints "hw_save_test: N checks, F failures". */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hw_save.h"

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

/* the world: the vblank count and the hw_card write counter (flash_writes + flash_erases) */
static HwSave s;
static uint32_t ctr, vb;

static void reset(uint32_t start_ctr)
{
    ctr = start_ctr;
    vb = 1000u;
    hwsave_init(&s, ctr);
}

static uint32_t tick(void)
{
    return hwsave_tick(&s, ctr, vb);
}

/* a backup operation: lock, `writes` flash writes/erases, unlock, one vblank elapses inside it */
static void round_(uint32_t writes)
{
    hwsave_lock(&s);
    vb++;
    ctr += writes;
    hwsave_unlock(&s, ctr, vb);
}

/* run the ticks of `n` vblanks; returns how many flushes came due */
static uint32_t run(uint32_t n)
{
    uint32_t i, due = 0u;

    for (i = 0; i < n; i++) {
        vb++;
        due += tick();
        if (s.in_flight != 0u) {
            hwsave_done(&s, 1u, vb);   /* the worker finishes at once in this model */
        }
    }
    return due;
}

static void t_basic(void)
{
    uint32_t i, due;

    reset(0u);
    check(tick() == 0u, "idle: nothing due", 0);
    round_(0u);
    check(s.pending == 0u, "a read-only round (lock, no write, unlock) is not pending", s.pending);
    check(run(100u) == 0u, "read-only rounds never flush", 0);
    round_(3u);
    check(s.pending == 1u, "a round with writes is pending", s.pending);
    check(tick() == 0u, "not due right after the unlock", 0);
    vb += HWSAVE_DEBOUNCE - 1u;
    check(tick() == 0u, "not due 29 vblanks after the unlock", 0);
    vb++;
    check(tick() == 1u, "due 30 vblanks after the unlock", 0);
    check(s.in_flight == 1u && s.pending == 0u, "handed out: in flight, no longer pending", s.in_flight);
    check(tick() == 0u, "not handed out twice", 0);
    hwsave_done(&s, 1u, vb);
    check(run(200u) == 0u, "after a verified flush nothing more is due", 0);
    check(s.flushes_due == 1u, "one flush in total", s.flushes_due);
    /* a real save: six lock/write/unlock rounds a few vblanks apart */
    reset(500u);
    for (i = 0; i < 6u; i++) {
        round_(2u);
        vb += 4u;
        check(tick() == 0u, "inside a save: never due between its rounds", (long)i);
    }
    due = run(100u);
    check(due == 1u, "a six-round save is ONE flush", due);
    check(s.coalesced == 5u, "five of the six rounds were coalesced", s.coalesced);
}

static void t_second_save(void)
{
    uint32_t due;

    reset(0u);
    round_(4u);
    due = run(20u);   /* 20 vblanks later: still inside the window */
    check(due == 0u, "no flush yet at +20", due);
    round_(4u);       /* a second save inside the window */
    due = run(29u);
    check(due == 0u, "the window restarted at the second save", due);
    due = run(1u);
    check(due == 1u, "the two saves flush once, 30 vblanks after the second", due);
    check(s.flushes_due == 1u && s.coalesced == 1u, "coalesced count 1, flushes 1", s.coalesced);
    round_(4u);       /* a third save after the window: its own flush */
    due = run(40u);
    check(due == 1u && s.flushes_due == 2u, "a save after the window flushes again", s.flushes_due);
}

static void t_cancel(void)
{
    uint32_t i;

    reset(0u);
    for (i = 0; i < 5u; i++) {   /* SaveDataState_Cancel: CARD_LockBackup ... CARD_UnlockBackup with no write between */
        hwsave_lock(&s);
        vb += 2u;
        hwsave_unlock(&s, ctr, vb);
        vb += 2u;
    }
    check(s.pending == 0u && run(100u) == 0u, "cancel-shaped rounds never flush", s.pending);
    check(s.locks == 5u && s.unlocks == 5u && s.unbalanced == 0u, "5 locks, 5 unlocks, balanced", s.locks);
    /* a cancel AFTER a real save: the write's flush is unaffected and still comes once */
    round_(3u);
    hwsave_lock(&s);
    hwsave_unlock(&s, ctr, vb);
    check(run(40u) == 1u, "a cancel after a save leaves one flush for the save", 0);
}

static void t_locked(void)
{
    uint32_t i;

    reset(0u);
    hwsave_lock(&s);
    ctr += 7u;
    for (i = 0; i < 200u; i++) {   /* a long backup operation: writes land, the lock is held, many vblanks pass */
        vb++;
        check(tick() == 0u, "nothing is due while the backup lock is held", (long)i);
    }
    check(s.pending == 0u, "the writes of an open round are not pending yet (the unlock decides)", s.pending);
    hwsave_unlock(&s, ctr, vb);
    check(s.pending == 1u, "pending after the unlock", s.pending);
    check(tick() == 0u, "the debounce counts from the unlock, not from the lock", 0);
    vb += HWSAVE_DEBOUNCE;
    check(tick() == 1u, "due 30 vblanks after the unlock", 0);
    /* in flight: a new round and its writes do not hand out a second flush */
    round_(2u);
    vb += HWSAVE_DEBOUNCE + 5u;
    check(tick() == 0u, "nothing handed out while the previous flush is in flight", 0);
    hwsave_done(&s, 1u, vb);
    check(s.pending == 1u, "the round that landed during the flight is pending after it", s.pending);
    check(tick() == 1u, "and flushes at the next tick (its window already elapsed)", 0);
    hwsave_done(&s, 1u, vb);
}

static void t_outside(void)
{
    uint32_t due;

    reset(10u);
    ctr += 2u;   /* a write with no lock (a hook the SDK path did not take): pending at the tick that saw it */
    check(tick() == 0u && s.pending == 1u, "a write outside any lock is pending at the tick that saw it", s.pending);
    due = run(31u);
    check(due == 1u, "and flushes one window later", due);
    /* an unlock the machine never saw the lock of */
    reset(0u);
    hwsave_unlock(&s, ctr, vb);
    check(s.unbalanced == 1u && s.depth == 0u, "unbalanced unlock counted, depth stays 0", s.depth);
}

static void t_fail(void)
{
    reset(0u);
    round_(1u);
    vb += HWSAVE_DEBOUNCE;
    check(tick() == 1u, "due", 0);
    check(hwsave_done(&s, 0u, vb) == HWSAVE_NOTICE_NONE, "the first failure shows no notice", 0);   /* verify failed */
    check(s.pending == 1u && s.in_flight == 0u, "a failed write stays pending", s.pending);
    check(tick() == 0u, "not retried at once", 0);
    vb += HWSAVE_DEBOUNCE;
    check(tick() == 0u, "after one failure the retry waits 60 vblanks, not 30", 0);
    vb += HWSAVE_DEBOUNCE;
    check(tick() == 1u, "retried after the doubled window", 0);
    check(hwsave_done(&s, 1u, vb) == HWSAVE_NOTICE_NONE && s.fails == 0u, "a verified flush clears the failure streak", s.fails);
    check(run(100u) == 0u && s.flushes_due == 2u, "then quiet; two hand-outs in total", s.flushes_due);
}

/* B1: the schedule after failure k is 30 << min(k, 6) vblanks; the notice comes on the second failure in a row; after seven the machine stops until the game writes again */
static void t_backoff(void)
{
    static const uint32_t want[] = { 60u, 120u, 240u, 480u, 960u, 1920u };
    uint32_t k, notice;

    reset(0u);
    round_(1u);
    vb += HWSAVE_DEBOUNCE;
    check(tick() == 1u, "backoff: first hand-out after the debounce", 0);
    for (k = 0u; k < 6u; k++) {
        notice = hwsave_done(&s, 0u, vb);
        check(notice == (k == 1u ? HWSAVE_NOTICE_FAIL2 : HWSAVE_NOTICE_NONE), "backoff: notice only on the second failure in a row", (long)k);
        vb += want[k] - 1u;
        check(tick() == 0u, "backoff: not due one vblank early", (long)k);
        vb += 1u;
        check(tick() == 1u, "backoff: due exactly at the scheduled window", (long)k);
    }
    notice = hwsave_done(&s, 0u, vb);   /* the seventh failure in a row */
    check(notice == HWSAVE_NOTICE_GAVE_UP && s.gave_up == 1u && s.fails == 7u, "backoff: the seventh failure gives up and says so", s.fails);
    vb += 100000u;
    check(tick() == 0u, "backoff: gave up, never due again however long it waits (no endless retry)", 0);
    check(hwsave_owed(&s, ctr) == 1u, "backoff: the flush is still owed at exit", 0);
    check(hwsave_flush_now(&s, ctr, vb) == 1u, "backoff: the exit path still tries once", 0);
    check(hwsave_done(&s, 0u, vb) == HWSAVE_NOTICE_NONE && s.fails == 8u, "backoff: a further failure shows no second give-up notice", s.fails);
    vb += 10u;
    round_(1u);   /* the game saves again */
    check(s.gave_up == 0u, "backoff: a new save re-arms the retries", s.gave_up);
    vb += 1920u;
    check(tick() == 1u, "backoff: and the new attempt waits the capped 1920 vblanks", 0);
    check(hwsave_done(&s, 1u, vb) == HWSAVE_NOTICE_NONE && s.fails == 0u && s.gave_up == 0u, "backoff: success clears everything", 0);
}

/* B2: the boot decision table */
static void t_boot(void)
{
    const uint32_t img = 524288u;

    check(hwsave_boot_action(0, -1, img) == HWSAVE_BOOT_ERASED, "boot: neither file: erased flash", 0);
    check(hwsave_boot_action(0, (int64_t)img, img) == HWSAVE_BOOT_ADOPT_TMP, "boot: no .sav and a complete .tmp: adopt it", 0);
    check(hwsave_boot_action(0, (int64_t)img - 1, img) == HWSAVE_BOOT_ERASED_BAD_TMP, "boot: no .sav and a short .tmp: erased flash + log", 0);
    check(hwsave_boot_action(0, (int64_t)img + 1, img) == HWSAVE_BOOT_ERASED_BAD_TMP, "boot: no .sav and a long .tmp: erased flash + log", 0);
    check(hwsave_boot_action(0, 0, img) == HWSAVE_BOOT_ERASED_BAD_TMP, "boot: no .sav and an empty .tmp: erased flash + log", 0);
    check(hwsave_boot_action(1, -1, img) == HWSAVE_BOOT_LOAD, "boot: .sav alone: load it", 0);
    check(hwsave_boot_action(1, (int64_t)img, img) == HWSAVE_BOOT_LOAD, "boot: both present (a crash mid-verify): keep the .sav, ignore the .tmp", 0);
    check(hwsave_boot_action(1, 100, img) == HWSAVE_BOOT_LOAD, "boot: .sav plus a torn .tmp: load the .sav", 0);
}

static void t_wrap(void)
{
    uint32_t due;

    reset(0xfffffffeu);   /* the counter wraps through 0 during the round */
    round_(5u);
    check(ctr == 3u && s.pending == 1u, "pending across the counter wrap", ctr);
    due = run(31u);
    check(due == 1u, "flushed across the wrap", due);
    reset(0u);
    vb = 0xfffffff0u;     /* the vblank count wraps inside the window */
    round_(1u);
    check(run(20u) == 0u, "no flush at +20 across the vblank wrap", 0);
    check(run(12u) == 1u, "flush at +30 across the vblank wrap", 0);
}

static void t_owed(void)
{
    reset(0u);
    check(hwsave_owed(&s, ctr) == 0u, "exit: nothing owed when clean", 0);
    round_(2u);
    check(hwsave_owed(&s, ctr) == 1u, "exit: a pending flush is owed before the window ends", 0);
    hwsave_lock(&s);
    check(hwsave_owed(&s, ctr) == 0u, "exit: not owed while locked (a request may be in flight)", 0);
    hwsave_unlock(&s, ctr, vb);
    ctr += 1u;
    check(hwsave_owed(&s, ctr) == 1u, "exit: an unseen counter move is owed", 0);
}

static void t_now(void)
{
    reset(0u);
    check(hwsave_flush_now(&s, ctr, vb) == 0u, "exit flush: nothing when clean", 0);
    round_(2u);
    hwsave_lock(&s);
    check(hwsave_flush_now(&s, ctr, vb) == 0u, "exit flush: refused while locked", 0);
    hwsave_unlock(&s, ctr, vb);
    check(hwsave_flush_now(&s, ctr, vb) == 1u, "exit flush: handed out inside the debounce window", 0);
    check(hwsave_flush_now(&s, ctr, vb) == 0u, "exit flush: not twice (in flight)", 0);
    hwsave_done(&s, 1u, vb);
    ctr += 1u;   /* an unseen write */
    check(hwsave_flush_now(&s, ctr, vb) == 1u, "exit flush: an unseen counter move is flushed", 0);
}

int main(void)
{
    t_basic();
    t_second_save();
    t_cancel();
    t_locked();
    t_outside();
    t_fail();
    t_backoff();
    t_boot();
    t_wrap();
    t_owed();
    t_now();
    printf("hw_save_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
