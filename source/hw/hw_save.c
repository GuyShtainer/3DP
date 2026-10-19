/* hw_save.c -- bet 2.6b: the backup write-back state machine (hw_save.h). Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <string.h>

#include "hw_save.h"

uint32_t hwsave_boot_action(int sav_exists, int64_t tmp_size, uint32_t image_bytes)
{
    assert(image_bytes != 0u);
    assert(tmp_size >= -1);
    if (sav_exists != 0) {
        return HWSAVE_BOOT_LOAD;
    }
    if (tmp_size == (int64_t)image_bytes) {
        return HWSAVE_BOOT_ADOPT_TMP;
    }
    return tmp_size >= 0 ? HWSAVE_BOOT_ERASED_BAD_TMP : HWSAVE_BOOT_ERASED;
}

/* the wait before the next hand-out: the debounce, doubled per consecutive failure (capped) */
static uint32_t window_of(const HwSave *s)
{
    uint32_t k = s->fails < HWSAVE_BACKOFF_SHIFT_MAX ? s->fails : HWSAVE_BACKOFF_SHIFT_MAX;

    assert(s != NULL);
    assert(k <= HWSAVE_BACKOFF_SHIFT_MAX);
    return HWSAVE_DEBOUNCE << k;
}

void hwsave_init(HwSave *s, uint32_t counter)
{
    assert(s != NULL);
    memset(s, 0, sizeof *s);
    s->seen = counter;
}

void hwsave_lock(HwSave *s)
{
    assert(s != NULL);
    s->locks++;
    s->depth++;
}

/* a change the machine just saw: pending, and the window restarts; a change on top of a pending one is coalesced */
static void changed(HwSave *s, uint32_t counter, uint32_t vb)
{
    if (s->pending != 0u) {
        s->coalesced++;
    }
    s->pending = 1u;
    s->last_change = vb;
    s->seen = counter;
    s->gave_up = 0u;   /* a new change by the game: the retries start again (the back-off exponent stays until a flush verifies) */
}

void hwsave_unlock(HwSave *s, uint32_t counter, uint32_t vb)
{
    assert(s != NULL);
    s->unlocks++;
    if (s->depth == 0u) {
        s->unbalanced++;   /* an unlock the machine never saw the lock of (the lock fired before the hooks existed): the change check below still applies */
    } else {
        s->depth--;
    }
    if (counter != s->seen) {
        changed(s, counter, vb);
    }
}

uint32_t hwsave_tick(HwSave *s, uint32_t counter, uint32_t vb)
{
    assert(s != NULL);
    if (s->depth != 0u || s->in_flight != 0u) {
        return 0u;   /* a backup request may be in flight, or the worker still owns the previous snapshot */
    }
    if (counter != s->seen) {
        changed(s, counter, vb);   /* a write outside any lock: it cannot be told from a half-finished round, so it restarts the window */
    }
    if (s->pending == 0u || s->gave_up != 0u || (uint32_t)(vb - s->last_change) < window_of(s)) {
        return 0u;
    }
    s->snap_counter = counter;
    s->in_flight = 1u;
    s->pending = 0u;
    s->flushes_due++;
    return 1u;
}

uint32_t hwsave_done(HwSave *s, uint32_t ok, uint32_t vb)
{
    assert(s != NULL && s->in_flight != 0u);
    assert(ok <= 1u);
    s->in_flight = 0u;
    if (ok != 0u) {
        s->fails = 0u;
        s->gave_up = 0u;
        return HWSAVE_NOTICE_NONE;
    }
    s->pending = 1u;
    s->last_change = vb;   /* retried after the back-off window, not every frame */
    if (s->fails < 0xFFFFFFFFu) {
        s->fails++;
    }
    if (s->fails > HWSAVE_RETRY_MAX) {
        s->gave_up = 1u;
        return s->fails == HWSAVE_RETRY_MAX + 1u ? HWSAVE_NOTICE_GAVE_UP : HWSAVE_NOTICE_NONE;
    }
    return s->fails == 2u ? HWSAVE_NOTICE_FAIL2 : HWSAVE_NOTICE_NONE;
}

uint32_t hwsave_owed(const HwSave *s, uint32_t counter)
{
    assert(s != NULL);
    return (s->pending != 0u || counter != s->seen) && s->depth == 0u && s->in_flight == 0u ? 1u : 0u;
}

uint32_t hwsave_flush_now(HwSave *s, uint32_t counter, uint32_t vb)
{
    assert(s != NULL);
    if (s->depth != 0u || s->in_flight != 0u) {
        return 0u;
    }
    if (counter != s->seen) {
        changed(s, counter, vb);
    }
    if (s->pending == 0u) {
        return 0u;
    }
    s->snap_counter = counter;
    s->in_flight = 1u;
    s->pending = 0u;
    s->flushes_due++;
    return 1u;
}
