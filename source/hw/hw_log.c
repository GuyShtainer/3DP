/* hw_log.c -- see hw_log.h. Pure C; fixed storage, no allocation (golden rule 3). */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hw_ctr.h"
#include "hw_log.h"

typedef struct HwLogEntry {
    HwLogSite *site;     /* NULL = free */
    uint32_t order;      /* insertion number, for "latest first" */
    char text[HWLOG_TEXT];
} HwLogEntry;

static HwLogEntry s_ring[HWLOG_RING];
static uint32_t s_next;           /* next insertion number */
static uint32_t s_calls;
static uint32_t s_dirty;
static uint32_t s_last_flush;     /* vblank count of the last dump */

typedef struct HwCallSite {
    const char *what;
    uintptr_t caller;
    HwLogSite site;
} HwCallSite;

static HwCallSite s_callers[HWLOG_CALLERS];
static uint32_t s_ncallers;

static void (*s_trace_sink)(const char *);
static uint32_t s_trace_n;
static HwCountSite *s_counts;

void hwtrace_set_sink(void (*sink)(const char *line))
{
    s_trace_sink = sink;
}

void hwtrace(const char *fmt, ...)
{
    char line[HWLOG_TEXT];
    va_list ap;

    assert(fmt != NULL);
    s_trace_n++;
    if (s_trace_sink == NULL) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s_trace_sink(line);
}

uint32_t hwtrace_count(void)
{
    return s_trace_n;
}

static unsigned s_task_high[2];

void hwtask_note(unsigned list, unsigned used, unsigned cap)
{
    static const char *const names[2] = { "vblank", "post-vblank" };

    assert(list < 2u);
    if (used > s_task_high[list]) {
        s_task_high[list] = used;
        hwtrace("tasks: %s list high-water %u of %u (the DS holds 32)", names[list], used, cap);
    }
}

unsigned hwtask_high(unsigned list)
{
    assert(list < 2u);
    return s_task_high[list];
}

void hwcount_hit(HwCountSite *site)
{
    assert(site != NULL && site->name != NULL);
    if (site->count == 0u) {   /* first hit: register (counts are never reset, so a site is linked exactly once) */
        site->next = s_counts;
        s_counts = site;
    }
    site->count++;
}

void hwcount_add(HwCountSite *site, uint32_t n)
{
    assert(site != NULL && site->name != NULL);
    if (n == 0u) {
        return;
    }
    if (site->count == 0u) {
        site->next = s_counts;
        s_counts = site;
    }
    site->count += n;
}

size_t hwcount_format(char *buf, size_t cap)
{
    size_t len = 0;
    const HwCountSite *prev = NULL;

    assert(buf != NULL && cap > 0);
    buf[0] = 0;
    for (;;) {   /* selection by name order: bounded by the number of registered sites */
        const HwCountSite *best = NULL, *c;
        int w;

        for (c = s_counts; c != NULL; c = c->next) {
            if (prev != NULL && (strcmp(c->name, prev->name) < 0 || (strcmp(c->name, prev->name) == 0 && (uintptr_t)c <= (uintptr_t)prev))) {
                continue;
            }
            if (best == NULL || strcmp(c->name, best->name) < 0 || (strcmp(c->name, best->name) == 0 && (uintptr_t)c < (uintptr_t)best)) {
                best = c;
            }
        }
        if (best == NULL || len + 1 >= cap) {
            break;
        }
        w = snprintf(buf + len, cap - len, "%s\t%u\t%s:%d\n", best->name, (unsigned)best->count, best->file, best->line);
        if (w < 0) {
            break;
        }
        len = (size_t)w >= cap - len ? cap - 1 : len + (size_t)w;
        prev = best;
    }
    return len;
}

int hwcount_dump(void)
{
    static char out[32768];
    size_t n;

    hwctr_mkdirs(HWCTR_APP_DIR);
    n = hwcount_format(out, sizeof out);
    return hwctr_file_write(HWCOUNT_FILE, out, (uint32_t)n) < 0 ? -1 : 0;
}

void hwlog_reset(void)
{
    uint32_t i;

    for (i = 0; i < HWLOG_RING; i++) {
        if (s_ring[i].site) {
            s_ring[i].site->slot = -1;
            s_ring[i].site->count = 0;
        }
        s_ring[i].site = NULL;
    }
    memset(s_callers, 0, sizeof s_callers);
    s_ncallers = 0;
    s_next = 0;
    s_calls = 0;
    s_dirty = 0;
    s_last_flush = 0;
}

static uint32_t pick_slot(void)
{
    uint32_t i, best = 0;
    uint32_t oldest = UINT32_MAX;

    for (i = 0; i < HWLOG_RING; i++) {
        if (!s_ring[i].site) {
            return i;
        }
        if (s_ring[i].order < oldest) {
            oldest = s_ring[i].order;
            best = i;
        }
    }
    return best;
}

void hwlog_emit(HwLogSite *site, const char *fmt, ...)
{
    va_list ap;
    uint32_t slot;

    assert(site != NULL && fmt != NULL);
    s_calls++;
    site->count++;
    s_dirty = 1;
    if (site->count != 1u) {
        return;   /* a later call from a known site: counted only */
    }
    slot = pick_slot();
    assert(slot < HWLOG_RING);
    if (s_ring[slot].site) {
        s_ring[slot].site->slot = -1;   /* evicted: its count keeps running, its line is gone */
    }
    s_ring[slot].site = site;
    s_ring[slot].order = s_next++;
    site->slot = (int32_t)slot;
    va_start(ap, fmt);
    (void)vsnprintf(s_ring[slot].text, sizeof s_ring[slot].text, fmt, ap);
    va_end(ap);
}

void hwlog_call(const char *file, int line, const char *what, void *caller)
{
    uint32_t i;
    HwCallSite *c = NULL;

    assert(what != NULL);
    assert(s_ncallers <= HWLOG_CALLERS);
    for (i = 0; i < s_ncallers; i++) {
        if (strcmp(s_callers[i].what, what) == 0 && s_callers[i].caller == (uintptr_t)caller) {
            c = &s_callers[i];
            break;
        }
    }
    if (!c) {
        if (s_ncallers >= HWLOG_CALLERS) {
            s_calls++;   /* table full: counted, not recorded */
            return;
        }
        c = &s_callers[s_ncallers++];
        c->what = what;
        c->caller = (uintptr_t)caller;
        c->site.file = file;
        c->site.line = line;
        c->site.count = 0;
        c->site.slot = -1;
    }
    hwlog_emit(&c->site, "%s (stub) called from %p", what, caller);
}

uint32_t hwlog_sites(void)
{
    uint32_t i, n = 0;

    for (i = 0; i < HWLOG_RING; i++) {
        if (s_ring[i].site) {
            n++;
        }
    }
    return n;
}

uint32_t hwlog_calls(void)
{
    return s_calls;
}

/* entries oldest first */
static uint32_t nth_oldest(uint32_t k)
{
    uint32_t i, j, best = HWLOG_RING;
    uint32_t floor_order = 0;
    int have_floor = 0;

    for (j = 0; j <= k; j++) {
        uint32_t cand = HWLOG_RING;
        for (i = 0; i < HWLOG_RING; i++) {
            if (!s_ring[i].site || (have_floor && s_ring[i].order <= floor_order)) {
                continue;
            }
            if (cand == HWLOG_RING || s_ring[i].order < s_ring[cand].order) {
                cand = i;
            }
        }
        if (cand == HWLOG_RING) {
            return HWLOG_RING;
        }
        best = cand;
        floor_order = s_ring[cand].order;
        have_floor = 1;
    }
    return best;
}

size_t hwlog_format(char *buf, size_t cap)
{
    size_t len = 0;
    uint32_t k, n = hwlog_sites();

    assert(buf != NULL && cap > 0);
    buf[0] = 0;
    for (k = 0; k < n; k++) {
        uint32_t s = nth_oldest(k);
        int w;

        if (s >= HWLOG_RING || len + 1 >= cap) {
            break;
        }
        w = snprintf(buf + len, cap - len, "[x%u] %s:%d %s\n", (unsigned)s_ring[s].site->count, s_ring[s].site->file,
                     s_ring[s].site->line, s_ring[s].text);
        if (w < 0) {
            break;
        }
        len = (size_t)w >= cap - len ? cap - 1 : len + (size_t)w;
    }
    return len;
}

int hwlog_dump(void)
{
    static char out[HWLOG_RING * (HWLOG_TEXT + 96u)];
    size_t n;

    hwctr_mkdirs(HWCTR_APP_DIR);
    n = hwlog_format(out, sizeof out);
    s_dirty = 0;
    (void)hwcount_dump();
    return hwctr_file_write(HWLOG_FILE, out, (uint32_t)n) < 0 ? -1 : 0;
}

void hwlog_flush_if_due(uint32_t vblank_count)
{
    if (s_dirty && vblank_count - s_last_flush >= 60u) {
        s_last_flush = vblank_count;
        (void)hwlog_dump();
    }
}

void hwlog_show(uint32_t max_lines)
{
    uint32_t n = hwlog_sites(), k, first;
    char line[HWLOG_TEXT + 64u];

    first = n > max_lines ? n - max_lines : 0;
    for (k = first; k < n; k++) {
        uint32_t s = nth_oldest(k);
        if (s >= HWLOG_RING) {
            break;
        }
        (void)snprintf(line, sizeof line, "[x%u] %.38s\n", (unsigned)s_ring[s].site->count, s_ring[s].text);
        hwctr_console_write(line);
    }
}
