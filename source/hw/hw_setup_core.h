/* hw_setup_core.h: the pure-C half of the first-launch setup screen (release P4). No libctru, no I/O: callers pass bytes in. */
#ifndef HW_SETUP_CORE_H
#define HW_SETUP_CORE_H
#include <stddef.h>
#include <stdint.h>

/* SHA-1 (FIPS 180-4), our own implementation. */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t used; } setup_sha1;
void setup_sha1_init(setup_sha1 *c);
void setup_sha1_update(setup_sha1 *c, const void *data, size_t n);
void setup_sha1_final(setup_sha1 *c, uint8_t out[20]);

/* ROM header (first 0x200 bytes): 1 when the game code is "CPUE" and the header CRC-16 at 0x15E matches bytes 0..0x15D. */
int setup_rom_header_ok(const uint8_t hdr[0x200]);

/* NitroFS: given the FNT and FAT (read by the caller at the offsets in the header), find a file by its ROM path
 * ("/data/UTF16.dat"). Returns 1 and sets *start and *end (ROM byte offsets) when found, 0 when not, -1 on a malformed table. */
int setup_nitrofs_find(const uint8_t *fnt, uint32_t fnt_len, const uint8_t *fat, uint32_t fat_len,
                       const char *path, uint32_t *start, uint32_t *end);

/* The ROM-check table, generated at build time from the user's own pret checkout (never committed). */
typedef struct { const char *rom_path; uint8_t sha1[20]; } setup_fs_entry;
extern const setup_fs_entry setup_fs_table[];
extern const uint32_t setup_fs_count;

/* Save files. Given the file size and its last min(size,128) bytes: SETUP_SAVE_RAW (exactly 524288 bytes),
 * SETUP_SAVE_DSV (DeSmuME footer recognised by its magic + size field, raw part >= 524288: *raw_len = 524288),
 * SETUP_SAVE_BAD otherwise. */
enum { SETUP_SAVE_BAD = 0, SETUP_SAVE_RAW = 1, SETUP_SAVE_DSV = 2 };
int setup_save_kind(uint64_t file_size, const uint8_t *tail, size_t tail_len, uint64_t *raw_len);

/* config.ini: copy `in` to `out` with the `rom =` line in the top-level section set to `rom_path` (replaced if present,
 * added before the first [section] otherwise); every other line, key and section byte-identical. Returns the output
 * length, or -1 if rom_path is empty / longer than 80 chars / contains CR or LF, or the result exceeds out_cap or 4096. */
long setup_config_set_rom(const char *in, size_t in_len, const char *rom_path, char *out, size_t out_cap);

/* A path that may be written as `rom = <path>` and read back unchanged: 1..80 bytes, every byte 0x20..0x7E or 0x80..0xFF (UTF-8 passes
 * through), and no space or tab as the first or last byte (the config reader trims them). Returns 1 when usable, else 0. */
int setup_rom_path_ok(const char *path);

/* Backup names: the first n >= 1 for which exists(base ".bak-<n>") is 0 (exists is the caller's probe). Writes
 * "<base>.bak-<n>" into out; returns n, or -1 if out_cap is too small or n would pass 999. */
int setup_bak_name(const char *base, int (*exists)(const char *path, void *ctx), void *ctx, char *out, size_t out_cap);

#endif
