/* hw_vram.h -- the DS VRAM bank model. Pure C: <stdint.h> / <string.h> / hw_log.h only.
 *
 * Nine banks A..I (656 KB) are the PHYSICAL store. The store is the LCDC window array `g_hwvram_lcdc` (banks at their LCDC offsets: A 0x00000, B 0x20000,
 * C 0x40000, D 0x60000, E 0x80000, F 0x90000, G 0x94000, H 0x98000, I 0xA0000); there is no second copy. VRAMCNT_A..I (0x04000240..49) decide where a bank
 * appears:
 *   - in a CPU-visible WINDOW array (main BG 0x06000000 / main OBJ 0x06400000 / sub BG 0x06200000 / sub OBJ 0x06600000): compiled code writes VRAM with plain
 *     stores to fixed host addresses, which nothing can intercept, so the bank's bytes are COPIED between the store and the window array when VRAMCNT
 *     changes (the "remap copy"). HWVRAM_SetCnt is called synchronously from every VRAMCNT store in the SDK (nitrosdk-vramcnt.patch):
 *       FLUSH  the bank's old window range -> its place in the store,
 *       ZERO   the vacated window range, except 16 KB chunks another enabled bank still covers (an unmapped DS read is 0),
 *       LOAD   the store -> the new window range.
 *     At most one bank's size (128 KB) is copied per change; a few changes per frame.
 *   - as a read-only VIEW for the hardware readers (texture slots 0-3, texture palette slots 0-5, the four extended-palette windows): HWVRAM_TexSlot ...
 *     return a pointer straight into the store, resolved from the CURRENT VRAMCNT at call time; views never copy. NULL when no bank is mapped there.
 *   - as LCDC (the store itself, nothing to copy), as ARM7 memory (C, D: no ARM9 window) or disabled.
 *
 * DIRTY BITMAP: one bit per 1 KB page, per bank. Set by DMA writes (hw_devs.c, through HwDmaOps.wrote) and by the SDK's MIi_Cpu* bulk helpers
 * (HWVRAM_Touch in mi_memory.c), at the moment of the write; every remap marks the whole bank (what a view shows changed). PLAIN CPU STORES ARE NOT
 * HOOKED, by design (they cannot be); the bitmap is the texture cache's fast path, the cache still hashes content. HWVRAM_DirtyPages reads and clears.
 *
 * KNOWN DIVERGENCE (review 2 B3): because the store doubles as the LCDC window, a CPU READ through LCDC of a bank that is currently mapped elsewhere
 * returns the store's bytes (stale for a bank mapped to a BG/OBJ window with unflushed CPU writes); the DS returns 0. A CPU WRITE through LCDC to such a
 * bank lands in the store and is overwritten by the next flush if the window was written in between. Nothing on the path does either: texture uploads map
 * the bank to LCDC first (GX_BeginLoadTex) and back after (GX_EndLoadTex).
 * TWO MORE DIVERGENCES (review 2.1 V-S1, bet 2.1e), neither on the path as far as the census and the code show (the game uploads through GX_BeginLoadTex /
 * LoadTex / EndLoadTex, gfd_VramTransferManager.c:24-33):
 *   (1) A CPU WRITE through the LCDC window (0x06800000 + the bank's LCDC offset) to a bank that is currently mapped as TEXTURE or TEXTURE PALETTE lands in the
 *       store here, and the texture / palette view (resolved from the store at use time) SHOWS it. On the DS the LCDC window reaches a bank only while its MST
 *       is 0 (LCDC); with the bank mapped to the texture unit the CPU access is dropped (a read returns 0), so the texture is NOT changed. The same holds for
 *       a bank mapped to a BG / OBJ window or to the ARM7: the LCDC write reaches the store only (see above for the flush that may overwrite it).
 *   (2) A plain store into a WINDOW array range that NO enabled bank covers (main BG / main OBJ / sub BG / sub OBJ, 0x06000000 / 0x06400000 / 0x06200000 /
 *       0x06600000) persists in the host array here: nothing hooks it, a later read returns it, and only a LOAD onto that range (a bank mapped there) or the
 *       ZERO of a vacated range overwrites it. On the DS the write is ignored and a read of an unmapped range returns 0.
 *   Both are "garbage in, different garbage out" cases for code that already breaks on the DS; if a census or an oracle diff ever shows one on the path, it
 *   is a finding for Phase 3, like an overlap.
 * Two banks enabled onto overlapping ranges of one window / view are counted (`overlaps`) and logged once: the path has none (census), a hit is a finding
 * for Phase 3, not a crash. The ARM7 mapping of C / D takes no ARM9 window and is not an overlap. */
#ifndef HW_VRAM_H
#define HW_VRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWVRAM_BANKS       9u
#define HWVRAM_BANK_A      0u
#define HWVRAM_BANK_B      1u
#define HWVRAM_BANK_C      2u
#define HWVRAM_BANK_D      3u
#define HWVRAM_BANK_E      4u
#define HWVRAM_BANK_F      5u
#define HWVRAM_BANK_G      6u
#define HWVRAM_BANK_H      7u
#define HWVRAM_BANK_I      8u

#define HWVRAM_STORE_SIZE  0xA4000u
#define HWVRAM_PAGE_SHIFT  10u                       /* 1 KB dirty pages */
#define HWVRAM_PAGE_WORDS  4u                        /* 128 pages = the largest bank (128 KB) */

#define HWVRAM_WIN_MAIN_BG  0u                       /* 0x06000000, 512 KB */
#define HWVRAM_WIN_MAIN_OBJ 1u                       /* 0x06400000, 256 KB */
#define HWVRAM_WIN_SUB_BG   2u                       /* 0x06200000, 128 KB */
#define HWVRAM_WIN_SUB_OBJ  3u                       /* 0x06600000, 128 KB */
#define HWVRAM_WINDOWS      4u
#define HWVRAM_WIN_MAIN_BG_SIZE  0x80000u
#define HWVRAM_WIN_MAIN_OBJ_SIZE 0x40000u
#define HWVRAM_WIN_SUB_BG_SIZE   0x20000u
#define HWVRAM_WIN_SUB_OBJ_SIZE  0x20000u

#define HWVRAM_TEX_SLOT_SIZE   0x20000u              /* texture slots 0-3, banks A-D */
#define HWVRAM_TEXPLTT_SIZE    0x4000u               /* texture palette slots 0-5, 16 KB each (E = 0-3, F / G one slot each) */
#define HWVRAM_EXTPLTT_SLOT    0x2000u               /* extended BG palette slots 0-3, 8 KB each; the OBJ ext palette is one 8 KB slot */

/* The host arrays the model works on: the store (= g_hwvram_lcdc, HWVRAM_STORE_SIZE bytes) and the four CPU-visible windows. */
typedef struct HwVramMem {
    uint8_t *store;
    uint8_t *win[HWVRAM_WINDOWS];
} HwVramMem;

typedef struct HwVramStats {
    uint32_t calls;             /* HWVRAM_SetCnt calls, including a rewrite of the same value */
    uint32_t changed;           /* calls that changed the bank's mapping or value */
    uint32_t frames;            /* HWVRAM_FrameEnd calls */
    uint32_t calls_last, calls_peak, frames_with_calls;   /* calls in the latest completed frame, the largest frame, frames that had any */
    uint32_t overlaps, bad_mst;
    uint32_t flush_bytes, load_bytes, zero_bytes;
    uint32_t touches;           /* HWVRAM_Touch calls that landed in VRAM */
    uint32_t pages_dirtied;     /* pages newly marked since init */
    uint32_t dirty_last, dirty_peak;   /* pages newly marked in the latest frame / the largest frame */
} HwVramStats;

/* All banks disabled, store and windows zeroed (the soft reset calls it again). */
void HWVRAM_Init(const HwVramMem *mem);

/* The synchronous VRAMCNT hook: `bank` 0..8, `value` the byte just written. Also exported to the SDK (port-shim/include/hw_dev_hooks.h). */
void HWVRAM_SetCnt(uint32_t bank, uint32_t value);
/* The value the model last saw for `bank` (hw_devs.c compares it with the register byte: a VRAMCNT store that bypassed the hook shows up there). */
uint8_t HWVRAM_Cnt(uint32_t bank);

/* Read-only views into the store, resolved from the current VRAMCNT. NULL when no bank is mapped there. The pointer is valid for HWVRAM_TEX_SLOT_SIZE /
   HWVRAM_TEXPLTT_SIZE / HWVRAM_EXTPLTT_SLOT bytes (the OBJ ext palette for 8 KB). */
const uint8_t *HWVRAM_TexSlot(uint32_t slot);                       /* 0..3 */
const uint8_t *HWVRAM_TexPltt(uint32_t slot);                       /* 0..5 */
const uint8_t *HWVRAM_BgExtPltt(uint32_t engine, uint32_t slot);    /* engine 0 main (E, F, G), 1 sub (H); slot 0..3 */
const uint8_t *HWVRAM_ObjExtPltt(uint32_t engine);                  /* engine 0 main (F, G), 1 sub (I) */
/* The CPU-visible window array (HWVRAM_WIN_*), for the 2D engines. NULL for an id out of range or before HWVRAM_Init. */
const uint8_t *HWVRAM_Window(uint32_t id);
/* Which 16 KB chunks of window `id` the 2D engines can address: bit c set = chunk c (window offset c * 0x4000) is covered by an enabled bank mapped there, from the
   CURRENT VRAMCNT. 0 for an id out of range or before HWVRAM_Init. The 2D record (gpu2d.h) copies exactly these chunks, so its per-frame byte count follows the bindings. */
uint32_t HWVRAM_WindowChunks(uint32_t id);

/* Dirty bitmap. Touch: a bulk write of `bytes` at host address `dst` just happened; ignored unless it lies in the store or a window. Pages are marked in
   every enabled bank covering the range. DirtyPages: copy the bank's bitmap into `bits[HWVRAM_PAGE_WORDS]` (may be NULL), clear it, return the page count. */
void HWVRAM_Touch(const void *dst, uint32_t bytes);
uint32_t HWVRAM_DirtyPages(uint32_t bank, uint32_t *bits);

/* An observer (bet 2.1c, hw_census.c): called at the top of every HWVRAM_SetCnt (including a rewrite of the same value) with the bank and the byte. It only
   observes. NULL = none (the default); survives HWVRAM_Init. Documented function-pointer dispatch (golden rule 9). */
typedef void (*HwVramObserver)(uint32_t bank, uint32_t value);
void HWVRAM_SetObserver(HwVramObserver fn);

/* Per-frame bookkeeping (hw_devs.c's frame hook) and the totals. */
void HWVRAM_FrameEnd(void);
void HWVRAM_Stats(HwVramStats *out);

#ifdef __cplusplus
}
#endif

#endif
