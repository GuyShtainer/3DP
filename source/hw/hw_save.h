/* hw_save.h -- bet 2.6b: the CARD backup write-back decision. Pure C99 (stdint only), host-tested by
 * tools/host-tests/hw_save_test.c. The SDK's CARD_LockBackup / CARD_UnlockBackup call the hooks HWDEV_BackupLock / HWDEV_BackupUnlock (port-patches/nitrosdk-devices.patch);
 * hw_devs.c forwards them here together with the hw_card write counter (flash_writes + flash_erases). The state machine only DECIDES; the 512 KB file write is the worker's.
 *
 *   lock     the SDK holds the backup resource: a request may be in flight, the flash image may change, nothing is snapshotted.
 *   unlock   if the write counter moved since the last baseline, a flush is PENDING and the debounce window restarts at this vblank.
 *   tick     once per input frame with the DS vblank count: a flush is DUE when pending, unlocked and DEBOUNCE vblanks have passed with no further change. A write
 *            seen outside any lock (the counter moved with no unlock) also becomes pending, stamped at the tick that saw it.
 * Counters are uint32 and compared only for inequality, so a wrap is harmless; vblank distances are uint32 subtractions, wrap-safe for a window of 30. */
#ifndef HW_SAVE_H
#define HW_SAVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWSAVE_DEBOUNCE 30u   /* DS vblanks between the last flash change and the file write: one file write per in-game save (a save is several lock/write/unlock rounds) */

/* B1 (review 2.6): a failed write-back is retried with a back-off, not every window forever. After failure number k (k = 1, 2, ...) the next attempt waits HWSAVE_DEBOUNCE << min(k, 6)
 * vblanks: 60, 120, 240, 480, 960, 1920 (about 1, 2, 4, 8, 16, 32 s). After HWSAVE_RETRY_MAX + 1 = 7 failures in a row the machine stops retrying (gave_up) until the game changes the flash again
 * (a new save) or the exit path asks (hwsave_flush_now ignores the stop). hwsave_done tells the caller when the player must be told: HWSAVE_NOTICE_FAIL2 on the second failure in a row,
 * HWSAVE_NOTICE_GAVE_UP on the seventh. */
#define HWSAVE_RETRY_MAX  6u
#define HWSAVE_BACKOFF_SHIFT_MAX 6u
#define HWSAVE_NOTICE_NONE    0u
#define HWSAVE_NOTICE_FAIL2   1u
#define HWSAVE_NOTICE_GAVE_UP 2u

/* B2 (review 2.6): what the boot does about the save file. `tmp_size` is the size of platinum.sav.tmp, -1 when it does not exist. A .tmp without its .sav can only come from a power cut between the
 * remove and the rename of a verified write-back (the remove happens only after the .tmp was verified), so a complete one is adopted. A torn .tmp from a cut during the write always has its .sav beside it. */
#define HWSAVE_BOOT_LOAD          0u   /* platinum.sav exists: load it, whatever a .tmp says (both present = a crash mid-verify: the .sav is the last verified file) */
#define HWSAVE_BOOT_ADOPT_TMP     1u   /* no .sav, a .tmp of exactly image_bytes: rename it into place, log it, then load */
#define HWSAVE_BOOT_ERASED        2u   /* neither file: a first run, the flash starts erased */
#define HWSAVE_BOOT_ERASED_BAD_TMP 3u  /* no .sav and a .tmp of the wrong size: not trustworthy, erased flash and a log line */
uint32_t hwsave_boot_action(int sav_exists, int64_t tmp_size, uint32_t image_bytes);

typedef struct HwSave {
    uint32_t depth;          /* lock nesting (the SDK does not nest; counted so a bug shows as an imbalance, not as a stuck flag) */
    uint32_t seen;           /* the write counter at the last baseline (an unlock, a tick that saw an unlocked change, or a completed flush's snapshot) */
    uint32_t pending;        /* 1 = the image differs from the file */
    uint32_t last_change;    /* vblank of the last change that set / extended `pending` */
    uint32_t snap_counter;   /* the counter value the in-flight snapshot was taken at (valid between due and done) */
    uint32_t in_flight;      /* 1 = a flush was handed out and not yet completed */
    uint32_t locks, unlocks, unbalanced;   /* statistics: lock calls, unlock calls, unlock without lock */
    uint32_t flushes_due;    /* flushes handed out */
    uint32_t fails;          /* consecutive failed flushes (reset by a verified one): the back-off exponent */
    uint32_t gave_up;        /* 1 = HWSAVE_RETRY_MAX retries failed: no more hand-outs until the game changes the flash again */
    uint32_t coalesced;      /* changes that landed while a flush was already pending (they extended the window instead of making another file write) */
} HwSave;

void hwsave_init(HwSave *s, uint32_t counter);
void hwsave_lock(HwSave *s);
void hwsave_unlock(HwSave *s, uint32_t counter, uint32_t vb);
/* 1 = take the snapshot NOW and write it (the caller must call hwsave_done when the file is written); 0 = nothing to do. Never 1 while locked or while a flush is in flight. */
uint32_t hwsave_tick(HwSave *s, uint32_t counter, uint32_t vb);
/* the worker finished (ok = verified write): a failed write leaves the flush pending and restarts the window at vb, so it is retried after the back-off above. Returns a HWSAVE_NOTICE_* code:
 * the caller shows the player a notice when it is not NONE. */
uint32_t hwsave_done(HwSave *s, uint32_t ok, uint32_t vb);
/* the exit path: 1 = a flush is owed regardless of the debounce (pending, unlocked, none in flight, or the counter moved) */
uint32_t hwsave_owed(const HwSave *s, uint32_t counter);
/* the exit path: like hwsave_tick with the debounce waived. 1 = hand out the flush now (same rules: never while locked or in flight). */
uint32_t hwsave_flush_now(HwSave *s, uint32_t counter, uint32_t vb);

#ifdef __cplusplus
}
#endif

#endif
