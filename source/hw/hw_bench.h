/* hw_bench.h -- bet 2.7a: the hardware-session bench. The kit (make hw-kit) puts a bench.txt next to the app; every launch runs ONE entry (a tag plus config.ini key overrides), the
 * next launch the next entry, and each run's logs are copied into bench/<tag>/ when it ends, so the whole session is "launch, play the script, exit, launch again" and the returned
 * bench/ folder holds every number. Pure C part: the bench.txt parser, the start counter and a SHA-256 (the kit's save must be the save on the SD). The file glue is hw_benchio.c. */
#ifndef HW_BENCH_H
#define HW_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWBENCH_MAX   8u
#define HWBENCH_TEXT_MAX 2048u   /* the largest bench.txt the app reads (comments included); the private hardware-session kit refuses a longer one */
#define HWBENCH_TAG   16u    /* tag characters + NUL; a tag is [A-Za-z0-9_-], 1..15 long */
#define HWBENCH_KEYS  128u   /* the overrides as config.ini text ("key = value\n" lines) */

typedef struct HwBenchEntry {
    char tag[HWBENCH_TAG];
    char keys[HWBENCH_KEYS];
} HwBenchEntry;

/* bench.txt: one entry per line, `TAG key=value key=value ...`; `#` starts a comment, blank lines are skipped. The overrides become `key = value` lines. Returns the number of
   entries, or -1 with *bad_line (1-based) set when a line is malformed, a tag repeats, there are more than max entries or the overrides do not fit. */
int hwbench_parse(HwBenchEntry *e, uint32_t max, const char *text, size_t len, uint32_t *bad_line);

/* bench.cnt: the number of launches already started (decimal). Garbage or empty reads as 0. */
uint32_t hwbench_counter_parse(const char *text, size_t len);
size_t hwbench_counter_format(uint32_t counter, char *buf, size_t cap);

/* SHA-256 */
typedef struct HwSha256 {
    uint32_t h[8];
    uint8_t block[64];
    uint32_t fill;
    uint64_t total;
} HwSha256;
void hwsha256_init(HwSha256 *c);
void hwsha256_update(HwSha256 *c, const void *data, size_t len);
void hwsha256_final(HwSha256 *c, uint8_t out[32]);
void hwsha256_hex(const uint8_t digest[32], char out[65]);
/* 1 when the first 64 characters of `text` (a sha256sum line: hex, then optional blanks and a name) are the digest, case-insensitively */
int hwsha256_matches(const uint8_t digest[32], const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
