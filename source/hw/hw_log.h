/* hw_log.h -- the hardware layer's log: each call site is recorded ONCE (its first message goes into a ring buffer) and counted on every
 * later call, so a stubbed DWC call made 60 times a second costs one line and a counter. Dumped to sdmc:/3ds/3DP/hwlog.txt and
 * shown on the bottom console. Pure C (no SDK, no libctru): the file and console I/O go through hw_ctr.h. */
#ifndef HW_LOG_H
#define HW_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWLOG_RING        256u   /* distinct call sites kept; the oldest is overwritten when full */
#define HWLOG_TEXT        160u
#define HWLOG_FILE        HWCTR_APP_DIR "/hwlog.txt"

typedef struct HwLogSite {
    const char *file;
    int line;
    uint32_t count;     /* calls, including the first */
    int32_t slot;       /* ring slot of the recorded line, -1 if never recorded or evicted */
} HwLogSite;

/* HWLOG("DWC_Init called (%d)", x): one static HwLogSite per expansion (the macro is a statement, GNU named variadics) */
#define HWLOG(fmt, ...)                                                                              \
    do {                                                                                              \
        static HwLogSite hwlog_site_ = { __FILE__, __LINE__, 0u, -1 };                                \
        hwlog_emit(&hwlog_site_, fmt, ##__VA_ARGS__);                                                 \
    } while (0)

/* HWLOG_CALL("DWC_Init"): for a stub, one record per (stub, CALLER) pair: the line names the game call site (the return address),
   later calls from the same site only count. Up to HWLOG_CALLERS pairs; more are counted in hwlog_calls() only. */
#define HWLOG_CALLERS 192u
#define HWLOG_CALL(name) hwlog_call(__FILE__, __LINE__, (name), __builtin_return_address(0))
void hwlog_call(const char *file, int line, const char *what, void *caller);

void hwlog_emit(HwLogSite *site, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hwlog_reset(void);
uint32_t hwlog_sites(void);        /* distinct sites recorded (not evicted) */
uint32_t hwlog_calls(void);        /* total calls */
size_t hwlog_format(char *buf, size_t cap);   /* the whole dump as text; returns the length (truncates at cap - 1) */
int hwlog_dump(void);              /* write HWLOG_FILE (creates the directory); 0 on success */
void hwlog_flush_if_due(uint32_t vblank_count);   /* dump at most once per 60 vblanks, and only if something changed */
void hwlog_show(uint32_t max_lines);              /* the latest lines on the bottom console */

/* An ORDERED trace line (bet 1.6b): unlike HWLOG (one line per call site) every call is kept, in order, and handed to the sink installed by
   port_main.c (boot.out). Used for the overlay load/unload log and the application switches. A NULL sink drops the line (tools, host tests). */
void hwtrace_set_sink(void (*sink)(const char *line));
void hwtrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t hwtrace_count(void);

/* Bet 2.2c, F8: queue headroom, trace only. The game calls hwtask_note(list, used, cap) with the number of tasks in use in a SysTaskManager list (0 = vblank, 1 = post-vblank);
   a new high-water mark emits one `tasks:` trace line, hwtask_high(list) reads it back for the hwlog. */
void hwtask_note(unsigned list, unsigned used, unsigned cap);
unsigned hwtask_high(unsigned list);

/* HWCOUNT("name"): a per-site hit counter for hardware-layer entry points that are not stubs (PXI send, DMA start, overlay load...). One static
   record per expansion, registered on its first hit; dumped by hwcount_dump() as "name<TAB>count<TAB>file:line" lines, sorted by name. */
typedef struct HwCountSite {
    const char *name;
    const char *file;
    int line;
    uint32_t count;
    struct HwCountSite *next;   /* the registration list */
} HwCountSite;

#define HWCOUNT(nm)                                                                                  \
    do {                                                                                              \
        static HwCountSite hwcount_site_ = { (nm), __FILE__, __LINE__, 0u, NULL };                    \
        hwcount_hit(&hwcount_site_);                                                                   \
    } while (0)
/* HWCOUNT_ADD("name", n): the same site, counting n (registers changed in a frame, bytes moved) */
#define HWCOUNT_ADD(nm, n)                                                                           \
    do {                                                                                              \
        static HwCountSite hwcount_site_ = { (nm), __FILE__, __LINE__, 0u, NULL };                    \
        hwcount_add(&hwcount_site_, (n));                                                              \
    } while (0)
void hwcount_hit(HwCountSite *site);
void hwcount_add(HwCountSite *site, uint32_t n);
size_t hwcount_format(char *buf, size_t cap);   /* "name\tcount\tfile:line\n", sorted by name; returns the length */
int hwcount_dump(void);                          /* HWCOUNT_FILE; 0 on success */
#define HWCOUNT_FILE HWCTR_APP_DIR "/hwcount.txt"

#ifdef __cplusplus
}
#endif

#endif
