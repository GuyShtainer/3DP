/* hw_ctr.h -- the one seam between the SDK-world hardware layer and libctru. Plain C, no libctru types: hw_ctr.c (compiled WITHOUT
 * port-shim/include on the include path) implements it; the host test supplies a fake. Everything the DS-era code needs from the
 * 3DS goes through these functions. */
#ifndef HW_CTR_H
#define HW_CTR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWCTR_APP_DIR "sdmc:/3ds/3DP"
#define HWCTR_ROM_FILE HWCTR_APP_DIR "/platinum.nds"   /* the player's own dump: the card bus reads it, nothing is ever copied */
#define HWCTR_SAVE_FILE HWCTR_APP_DIR "/platinum.sav"  /* the 512 KB flash image (read at boot, writes stay in RAM) */

int64_t hwctr_rtc_secs(void);                 /* the 3DS clock (local time) as seconds since 2000-01-01 */
uint64_t hwctr_sys_tick(void);                 /* svcGetSystemTick (268 111 856 Hz) */
void hwctr_wait_vblank(void);
uint64_t hwctr_idle_ticks(void);               /* 2.7(b) N2: cumulative ticks spent in hwctr_wait_vblank and hwctr_sleep_sys (the game thread's idle time) */
void hwctr_set_frame_hook(void (*hook)(void));  /* called on the game host thread after every gspWaitForVBlank */                  /* gspWaitForVBlank */
void hwctr_sleep_sys(uint64_t sys_ticks);      /* svcSleepThread, converted */

/* host threads for the cooperative contexts: the handle is a parked thread (starts blocked; the first wake runs entry(arg)) */
void *hwctr_main_handle(void);
void *hwctr_thread_create(void (*entry)(void *), void *arg, uint32_t stack_bytes);
void hwctr_thread_wake(void *handle);
void hwctr_thread_park(void *handle);
void hwctr_thread_exit_current(void *handle);  /* never returns */
void *hwctr_thread_create_core(void (*entry)(void *), void *arg, uint32_t stack_bytes, int core);   /* bet 2.3c: as hwctr_thread_create, on `core` (2 = the New 3DS extra core, 1 = the system core APT_SetAppCpuTimeLimit opens to the application, 0 = the game's own core) when the system allows, else the default core */
int hwctr_thread_core(void *handle);           /* the core the thread was created on (-2 = default: the core request was refused or not made) */
int hwctr_thread_ran_on(void *handle);         /* bet 2.7a: the core (svcGetProcessorID) the thread was running on when it first ran, -1 = it has not run yet */
int hwctr_proc_id(void);                       /* svcGetProcessorID of the CALLING thread */
void hwctr_thread_kill(void *handle);          /* end a PARKED thread (not the caller), no unwinding; the main thread's handle is left parked */

/* bet 2.7a: the run header's facts, read once at boot (hwctr_info also calls APT_SetAppCpuTimeLimit(30) and keeps its result code). is_new3ds: 1 / 0 / -1 (the query failed). */
typedef struct HwCtrInfo {
    int is_new3ds;
    int homebrew;          /* 1 = launched through the Homebrew Launcher (a .3dsx), 0 = an installed title (the .cia) */
    int32_t cpu_limit_rc;  /* APT_SetAppCpuTimeLimit(30) */
    int slider_pct;        /* the 3D slider, 0..100 */
    uint32_t linear_free;  /* linearSpaceFree(), bytes */
    int proc_id;           /* svcGetProcessorID of the caller (the main thread at boot) */
    int emu;               /* the svcGetSystemInfo(0x20000,0) probe: 1 = an emulator (rc >= 0 and v != 0), 0 = hardware */
    int32_t emu_rc;        /* the probe's raw result code (Luma answers 1) */
    int64_t emu_v;         /* the probe's raw value (Azahar 2, Citra 1, Luma 0) */
} HwCtrInfo;
void hwctr_info(HwCtrInfo *out);
uint32_t hwctr_linear_free(void);
int hwctr_slider_pct(void);
uint32_t hwctr_heap_free(uint32_t *total);   /* review 2.8 N7: heap bytes not in use (newlib uordblks against envGetHeapSize), *total = the heap size */
uint32_t hwctr_measure_mhz100(void);           /* the measured CPU clock, MHz x 100 (80400 = 804 MHz); about 4 ms */

/* sdmc files: return the byte count, or -1 */
int hwctr_file_read(const char *path, void *buf, uint32_t cap);
int hwctr_file_write(const char *path, const void *buf, uint32_t len);
void hwctr_mkdirs(const char *dir);
/* a read-only file kept open: NULL on failure; pread = bytes read at offset off (< len at end of file), or -1 */
void *hwctr_file_open(const char *path);
int hwctr_file_pread(void *file, uint32_t off, void *dst, uint32_t len);
void hwctr_file_close(void *file);
/* a file written in pieces (the gxtrace): create/truncate -> NULL on failure; append = bytes written (< len on failure); close = hwctr_file_close */
void *hwctr_file_create(const char *path);
int hwctr_file_append(void *file, const void *buf, uint32_t len);

/* release P4b (the first-launch setup screen): a directory listing and the pre-game input poll. dir_list fills at most `max` entries (name only, without the path) and returns the number filled, or -1 when the directory cannot be opened. hid_wait = (wait != 0: gspWaitForVBlank) + hidScanInput; it returns
   the 3DS buttons held (libctru KEY_* bits) and sets *alive to 0 when the system asks the application to quit (HOME / power). The frame hook is NOT called and the idle counter is untouched. */
#define HWCTR_NAME_MAX 81   /* names of up to 80 bytes: the longest `rom =` path is 80, so a longer name can never be a usable ROM */
typedef struct HwCtrDirEnt {
    char name[HWCTR_NAME_MAX];
    uint8_t is_dir;
} HwCtrDirEnt;
/* The entries of `dir` (no . / ..) into out[0..max): the count, or -1 when the folder cannot be opened. *skipped (may be NULL) = the entries NOT returned: a name of HWCTR_NAME_MAX bytes or more, or past `max`. */
int hwctr_dir_list(const char *dir, HwCtrDirEnt *out, int max, int *skipped);
uint32_t hwctr_hid_wait(int wait, int *alive);

/* bet 2.6b (the save write-back): hwctr_file_sync = flush the stdio buffer and fsync (0 = the data reached the SD driver); hwctr_file_rename replaces `to` (a rename onto an existing
   file fails on the SD, so `to` is removed first: 0 = renamed); hwctr_file_remove: 0 = removed or absent. */
int hwctr_file_sync(void *file);
int hwctr_file_rename(const char *from, const char *to);
int hwctr_file_remove(const char *path);
/* bet 2.6c: the size of a file in bytes, -1 when it does not exist (or is a directory / too large for an int). libctru's sdmc device has no atomic replace (FSUSER_RenameFile onto an
   existing file fails), so hwctr_file_rename keeps remove + rename and the boot adopts a stranded .tmp (hw_savewb.c hwwb_recover). */
int hwctr_file_size(const char *path);
/* the screens (bet 2.3c). hwctr_gfx_init once, after gfxInitDefault (before any console text). hwctr_gfx_present is the render thread's: it writes both 256 x 192
   images (0xFFRRGGBB words) into the libctru framebuffers (hw_screen.h's mapping), flushes and swaps; while the text console owns the bottom screen only the top is
   written. hwctr_console_chord(held) is the input frame's: held = ZL and ZR both down; its press edge toggles the console. */
void hwctr_gfx_init(void);
void hwctr_gfx_present(const uint32_t *top, const uint32_t *bot);
void hwctr_gfx_present_wide(const uint32_t *left, const uint32_t *right, const uint32_t *bot);   /* bet 2.8: 400 x 240 top (right != NULL: stereo), bottom as above */
void hwctr_console_chord(unsigned held);

/* bottom-screen text console (shown at power-on until the first game frame, then on the ZL+ZR toggle; the text is kept either way) */
void hwctr_console_write(const char *text);
void hwctr_console_notice(const char *text);   /* bet 2.6c: write the text and bring the console up on the bottom screen (ZL+ZR hides it): the player-visible failure line */
void hwctr_console_save(void);                 /* write the tail of the console text to HWCTR_APP_DIR/console.txt */
void hwctr_fatal(const char *message);         /* print, then park forever (the app is dead; the log is on the sdmc) */

#ifdef __cplusplus
}
#endif

#endif
