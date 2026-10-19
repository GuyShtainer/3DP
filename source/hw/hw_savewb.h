/* hw_savewb.h -- bet 2.6b: the backup write-back's glue between hw_save.c (the decision), the 512 KB flash image (hw_devs.c's s_flash) and the SD card (hw_ctr.h).
 * Plain C, no SDK, no libctru types. Everything but the worker's file work runs on the game thread; the worker never touches the hw log, HWCOUNT or the flash image
 * after its snapshot. Design and gates: the private design notes. */
#ifndef HW_SAVEWB_H
#define HW_SAVEWB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWSAVEWB_SUFFIX_TMP  ".tmp"
#define HWSAVEWB_SUFFIX_ORIG ".orig"

/* Boot recovery (bet 2.6c, review 2.6 B2): the file the boot must load. `path` normally; when `path` is missing and <path>.tmp is a complete image (a power cut between the remove and
   the rename of a verified write-back) the .tmp is renamed into place and logged; if that rename fails the .tmp path itself is returned. Call before reading the save. */
const char *hwwb_boot_path(const char *path, uint32_t bytes);
/* Attach the flash image (the one the card reads) and its file path. Idempotent for the same arguments. Starts no thread: the worker is created at the first due flush. */
void hwwb_init(const uint8_t *flash, uint32_t bytes, const char *path, uint32_t counter);
/* The SDK's CARD_LockBackup / CARD_UnlockBackup (through HWDEV_BackupLock / Unlock). Lock waits (bounded, 2 s) while a snapshot copy is running; if it gives up the flush is discarded. */
void hwwb_lock(void);
void hwwb_unlock(uint32_t counter, uint32_t vb);
/* Once per input frame, game thread: harvest a finished flush, hand out a due one (debounce + no lock held). */
void hwwb_frame(uint32_t counter, uint32_t vb);
/* Exit: wait for a flush in flight, then flush what is owed (the debounce waived) with the worker; bounded. */
void hwwb_stop(uint32_t counter, uint32_t vb);
/* Counters for the report (game thread). */
typedef struct HwWbStats {
    uint32_t flushes_ok, flushes_failed, lock_waits;
    uint32_t locks, unlocks, coalesced;
    uint32_t orig_copied;         /* 1 = platinum.sav.orig written by this run, 2 = it already existed (kept), 3 = the copy FAILED (not retried), 0 = no original to keep */
    uint64_t worker_ticks;        /* wall time on the worker, sys ticks */
    uint64_t game_ticks;          /* wall time the game thread spent in the lock / unlock / frame hooks, sys ticks */
    uint32_t game_calls;
    int worker_core;              /* -2 = default core, -1 = no worker was needed */
    int worker_ran_on;            /* bet 2.7a: svcGetProcessorID read inside the worker when it first ran, -1 = it never ran */
} HwWbStats;
void hwwb_stats(HwWbStats *out);

#ifdef __cplusplus
}
#endif

#endif
