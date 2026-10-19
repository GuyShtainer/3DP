/* hw_gx.h -- the geometry engine's INPUT layer. Pure C: no SDK, no libctru, only <stdint.h> / <string.h>.
 *
 * The game feeds the DS geometry engine four ways, and all four share one command-packing state, so the order of every word matters:
 *   HWGX_PATH_DMA       DMA channel 3 into the GX FIFO (NNS_G3dGeSendDL display lists)               hw_devs.c gx_sink
 *   HWGX_PATH_CPUSEND   MI_CpuSend32 / MI_CpuCopy32 / MI_CpuCopyFast into the FIFO or a command port nitrosdk-g3-ports.patch (memory.h)
 *   HWGX_PATH_SENDFIFO  GX_SendFifo36B/48B/64B/128B, GXi_NopClearFifo128_                             asm-to-c (gxasm.c, g3x.c), gcc-fixes (gxasm.h)
 *   HWGX_PATH_PORT      a direct store to a command port 0x04000440..0x040005FF (reg_G3_*), or to the FIFO itself (reg_G3X_GXFIFO = w)
 *                                                                                                    nitro{sdk,system}-g3-ports.patch, g3x.c
 * This unit only RECORDS (counts per path, an ordered bounded trace, a canary); it decodes nothing (bet 2.4 does).
 *
 * Canary: the 0x200 bytes of the I/O block at 0x400..0x5FF (the FIFO mirror and every write-only command port) are filled with a pattern. The hooks
 * never write there (a command port's last value goes to a shadow array instead), so ANY change at a frame end is a store that slipped past the hooks.
 * `canary_words` counts the changed words, `canary_frames` the frames that had any; the first offset is kept.
 *
 * Trace file, VERSION 2 (bet 2.2a; config.ini `gxtrace = FROM:COUNT`, a frame window; never unbounded) -> sdmc:/3ds/3DP/gxtrace.bin (hw_devs.c). All little endian.
 * Version 1 (a one-off state dump + stream) could not drive stream replay past the first frame (review 2.1 A-B1) and was REMOVED, not kept: nothing reads it.
 * The byte-level table, the matrix-reload rule and the size numbers are in the private design notes; tools/gxtrace.py is the reference parser.
 *   header, 64 bytes: "GXTR", u32 version (2), u32 from_frame, u32 frame_count (requested), u32 flags (bit 0 = clock is `frame`, bit 1 = state sealed at the snapshot point), u32 entry_bytes (8), u32 regs_off (0x60),
 *                     u32 regs_len (0x3A0), u32 vram_bytes (the LCDC store size, 0xA4000: for reference, never dumped), u32 page_bytes (1024), u32 ds_vblanks_at_open, 5 x u32 0
 *   then per frame of the window the chunks, each `char tag[4]; u32 len; u8 payload[len]` (len a multiple of 4), in exactly this order:
 *     FRAM (32)  u32 frame (absolute host frame), u32 ds_vblanks (the pump's total, across soft resets), u32 index in window, u32 flags (bit 0 = first), 4 x u32 0   <- the event key
 *     STRM       0..n chunks of 8-byte entries { u32 word; u16 addr; u8 path; u8 0 } in PROGRAM ORDER across all four paths (path 0..3 = HWGX_PATH_*; addr 0 = a FIFO word, else the
 *                command-port offset 0x440..0x5FF); one chunk per 16384 entries, so a long frame has several
 *     --- end of the frame's stream (the vblank hook: the host wait, before the pump raises the DS vblank). Everything below is the state at the frame's SNAPSHOT POINT
 *     (bet 2.2c, B3): after the DS vblank handler (SysTaskManager tasks: the billboard texture uploads) and the game's vblank callback (main.c), i.e. what the DS renders at
 *     line 215. Header flags bit 1 says so; FEND word 6 says how each frame was sealed (HWGX_SEAL_*). Words stored between the hook and the snapshot point belong to the NEXT frame. ---
 *     R3D  (0x3A0)  the 3D registers 0x04000060..0x040003FF
 *     VCNT (16)     bytes 0x04000240..0x04000249 (VRAMCNT_A..G, WRAMCNT, VRAMCNT_H, VRAMCNT_I) + 6 x 0
 *     PAGE*         3D regions in ascending id: u32 region (0..3 TEX slot, 4..9 TEXPLTT slot), u32 npages, npages x { u32 page; u8 data[1024] }, pages ascending
 *     R2A  (0x70)   engine A registers 0x04000000..0x0400006F          R2B (0x70) engine B registers 0x04001000..0x0400106F   (the live registers at the seal, i.e. after the frame's last HBlank line)
 *     L2A / L2B     ONLY when header flags bit 2 is set (bet 2.3a): `u32 form` then the payload. form 0 (HWGX_L2_UNIFORM) = one 0x70-byte image (engine A / B) for all 192 lines,
 *                   form 1 (HWGX_L2_LINES) = 192 such images, form 2 (HWGX_L2_NONE) = nothing (the frame was sealed at the next vblank hook). The images are gpu2d.h's per-line
 *                   record: line n taken after line n's HBlank callback, bytes 4..7 of engine A (DISPSTAT, VCOUNT) zeroed. The lines run BEFORE the seal, so they belong to the
 *                   frame whose vblank callback preceded them. The run-length form is the uniform one: a frame with no HBlank handler (the whole census path) costs 0x7C bytes
 *                   per engine instead of 21 508.
 *     PLTT (2048)   the standard palettes (main 1 KB, sub 1 KB)         OAM (2048)  main + sub OAM
 *     PAGE*         2D regions in ascending id: 16 main BG window, 17 main OBJ window, 18 sub BG, 19 sub OBJ, 20..23 main BG ext palette slots, 24..27 sub BG ext palette
 *                   slots, 28 main OBJ ext palette, 29 sub OBJ ext palette
 *     FEND (32)     u32 stream entries (paths 0..3 only), 4 x u32 per-path entries, u32 pages dumped in this frame, u32 seal kind (HWGX_SEAL_*), u32 RDBK entries (path 4)   (the parser checks the counts against the STRM chunks)
 *   RDBK (bet 2.4a, header flags bit 3; STRM entries with path 4, the game READ a geometry-engine register at this stream position): a head { word = n (1..16), addr = register offset
 *   (0x600 GXSTAT, 0x620 POS_RESULT, 0x630/0x632/0x634 VEC_RESULT, 0x640 CLIPMTX, 0x680 VECMTX), pad 1 } then n entries { word = value the port returned (16-bit ones zero-extended),
 *   addr = off + 4 * i, pad 2 }. Slice 2.4c adds the WRITE entry { word = the value the port's engine applied, addr = 0x600 (GXSTAT) or 0x060 (DISP3DCNT), path 4, pad 3 }, standing alone (never
 *   inside a group), also counted in FEND word 7; each window starts with a GXSTAT write entry that carries the FIFO-interrupt bits already set. Every other entry has pad 0.
 *   Replayers that do not read RDBK skip path 4.
 *   A PAGE chunk exists only for pages whose CONTENT changed since the one the reader holds; the reader starts with all-zero regions, so the first frame carries every
 *   non-zero page of everything mapped (a zero page and an unmapped region are the same to the reader). Unmapping a region dumps its pages as zeros.
 *   Matrix stacks are NOT in the trace (no geometry engine before bet 2.4): a window must open on a frame whose stream reloads the projection and the position/vector
 *   matrices before its first BEGIN_VTXS (the MATRIX-RELOAD RULE); tools/gxtrace.py asserts it. */
#ifndef HW_GX_H
#define HW_GX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWGX_PATH_DMA      0u
#define HWGX_PATH_CPUSEND  1u
#define HWGX_PATH_SENDFIFO 2u
#define HWGX_PATH_PORT     3u
#define HWGX_PATHS         4u
#define HWGX_PATH_RDBK     4u   /* trace-only entry kind (bet 2.4a): a game READ of a geometry-engine register; not a store path, never counted in HWGX_PATHS */
#define HWGX_RDBK_MAX      16u  /* most words in one RDBK group (CLIPMTX) */

#define HWGX_FIFO_OFF      0x400u   /* GXFIFO and its mirrors: 0x400..0x43F */
#define HWGX_FIFO_END      0x440u
#define HWGX_PORT_END      0x600u   /* the command ports end at 0x5FF (GXSTAT and the readbacks start at 0x600) */
#define HWGX_CANARY_WORDS  ((HWGX_PORT_END - HWGX_FIFO_OFF) / 4u)

#define HWGX_TRACE_REGS_OFF 0x60u
#define HWGX_TRACE_REGS_LEN 0x3a0u
#define HWGX_TRACE_HDR      64u
#define HWGX_PAGE_BYTES     1024u
#define HWGX_REGS2D_LEN     0x70u
#define HWGX_PLTT_LEN       2048u
#define HWGX_OAM_LEN        2048u
#define HWGX_REGIONS        30u      /* page region ids (see the layout above); 10..15 are unused */
#define HWGX_TOTAL_PAGES    1712u    /* 4*128 + 6*16 + 512+256+128+128 + 10*8 */
#define HWGX_TRACE_ENTRY    8u
#define HWGX_TRACE_VERSION  2u
#define HWGX_TRACE_BUF      16384u   /* entries buffered between flushes */
#define HWGX_FLAG_CLOCK_FRAME  1u    /* header flags bit 0: the clock is `frame` */
#define HWGX_FLAG_RDBK         8u    /* header flags bit 3 (bet 2.4a): the stream may carry RDBK entries (path 4; the writer sets it on every trace) and FEND word 7 counts them */
#define HWGX_FLAG_LINES2D      4u    /* header flags bit 2 (bet 2.3a): every frame carries the per-line 2D chunks L2A / L2B after R2B (needs bit 1: the lines run just before the snapshot seal) */
#define HWGX_L2_UNIFORM        0u    /* L2A / L2B form word: ONE 0x70-byte image stands for all 192 lines (the pump's short-cut: no HBlank handler could run) */
#define HWGX_L2_LINES          1u    /* form word: 192 images of 0x70 bytes, line 0 first */
#define HWGX_L2_NONE           2u    /* form word: no payload: the frame was sealed by the next vblank hook (HWGX_SEAL_NEXT_VBLANK) and the lines of its own frame never ran */
#define HWGX_FLAG_SEAL_SNAPSHOT 2u   /* header flags bit 1 (bet 2.2c, B3): the STATE chunks of a frame are sealed at the game's snapshot point (HWGX_Seal, after the vblank callback), not at the vblank hook */
#define HWGX_SEAL_SNAPSHOT     1u    /* FEND word 6: sealed by HWGX_Seal (the game passed its snapshot point) */
#define HWGX_SEAL_NEXT_VBLANK  2u    /* FEND word 6: the game waited for the next vblank before reaching its snapshot point; sealed at that hook (the pre-2.2c position, plus the vblank handler) */

/* The SDK-side hooks (port-shim/include/hw_dev_hooks.h declares the same two functions and the HWG3_MODE_* twins of these modes; hw_devs.c asserts they
   agree). Bulk sender modes: SEND = every word to the SAME offset (MI_CpuSend32), COPY = word i to off + 4 i (MI_CpuCopy32/Fast), FIFO = the FIFO mirror
   (GX_SendFifo*, GXi_NopClearFifo128_). */
#define HWGX_MODE_SEND 0u
#define HWGX_MODE_COPY 1u
#define HWGX_MODE_FIFO 2u
void HWG3_Port(uint32_t off, uint32_t val);
void HWG3_Words(uint32_t off, const void *src, uint32_t words, uint32_t mode);
void HWGX_StatTrace(uint32_t off, uint32_t value);              /* slice 2.4c: trace-only entry {value, off, path 4, pad 3}: the game wrote GXSTAT / DISP3DCNT */
void HWGX_SetTraceOpenHook(void (*fn)(void));
void HWG3_Readback(uint32_t off, uint32_t words, const void *vals);   /* bet 2.4a: trace-only RDBK entries for a register read, see the trace layout */

typedef struct HwGxIo {   /* the file seam (hw_ctr.h in the app, a fake in the host test): documented dispatch, golden rule 9 */
    void *(*create)(const char *path);
    int (*append)(void *file, const void *buf, uint32_t len);   /* bytes written, or < len on failure */
    void (*close)(void *file);
    void *(*alloc)(uint32_t bytes);   /* the page-hash table of a trace window (13.7 KB): taken when a window is armed, never in bss, so a run without a trace has the layout it had before 2.2a */
} HwGxIo;

typedef struct HwGxStats {
    uint32_t frames;                  /* HWGX_FrameEnd calls */
    uint32_t total[HWGX_PATHS];       /* words per path since init */
    uint32_t last[HWGX_PATHS];        /* words per path in the latest completed frame */
    uint32_t peak[HWGX_PATHS];        /* the largest single frame per path */
    uint32_t stray;                   /* words aimed outside 0x400..0x5FF (not recorded) */
    uint32_t canary_frames, canary_words, canary_first_off, canary_first_val;
    uint32_t canary_offs[8], canary_vals[8];          /* the first 8 DISTINCT offsets that ever changed (find the unhooked writer) */
    uint32_t canary_noffs;
    uint32_t canary_at_slice;         /* of canary_words: found as an ARM7 sound slice began (the rest: at a frame end) */
    uint32_t trace_state;             /* 0 off, 1 waiting for the window, 2 recording, 3 finished, 4 failed */
    uint32_t trace_frames, trace_entries;
    uint32_t trace_bytes;             /* the file so far */
    uint32_t trace_pages;             /* 1 KB state pages dumped over the window */
    uint32_t trace_expect[HWGX_PATHS];   /* the counters' words per path over the window (independent of the trace appends) */
    uint32_t trace_got[HWGX_PATHS];      /* entries appended per path */
    uint32_t seals_snapshot, seals_next_vblank;   /* frames sealed at the snapshot point / at the next vblank hook (bet 2.2c) */
} HwGxStats;

/* Bet 2.2a: where the per-frame snapshot reads its state from (set once, survives HWGX_Init; every field may be NULL = zeros). `region(id)` returns the bytes of page region
   `id` (HWGX_REGIONS ids, sizes fixed by the layout above) or NULL when nothing is mapped there. Documented function-pointer dispatch (golden rule 9). */
typedef struct HwGxSnap {
    const uint8_t *pltt;                      /* the 2 KB standard palettes */
    const uint8_t *oam;                       /* 2 KB OAM */
    const uint8_t *(*region)(uint32_t id);
    uint32_t (*vblanks)(void);                /* DS vblanks raised since power-on (the event key) */
    const uint8_t *(*lines)(uint32_t engine, uint32_t *form);   /* bet 2.3a: the per-line 2D record of engine 0/1 (gpu2d.h): HWGX_L2_UNIFORM -> 0x70 bytes, HWGX_L2_LINES -> 192 x 0x70; NULL = none this frame. NULL fn = no L2 chunks, flag bit 2 clear */
    uint32_t clock_frame;                     /* header flag bit 0 */
} HwGxSnap;
void HWGX_SetSnapshot(const HwGxSnap *s);

/* `regs` = the 64 KB-aligned-or-not I/O block (offset 0 = 0x04000000), `vram` / `vram_bytes` = the LCDC array dumped at a trace start; any may be NULL
   (no canary / no state dump). Zeroes every counter, fills the canary pattern. `io` may be NULL (no trace). */
void HWGX_Init(uint8_t *regs, const uint8_t *vram, uint32_t vram_bytes, const HwGxIo *io);
void HWGX_ArmCanary(void);   /* refill the pattern (after the I/O block was zeroed by a soft reset) */
/* A bounded frame window; count == 0 turns the trace off. The file is created when the window opens. */
/* Bet 2.2c B3 A/B control: 1 = seal at the vblank hook like before 2.2c (header flag bit 1 clear, FEND word 6 = 0). Call before the window opens. */
void HWGX_SetSealMode(int frame_end);
void HWGX_SetTrace(uint32_t from_frame, uint32_t count, const char *path);

void HWGX_FifoWord(uint32_t word, uint32_t path);   /* a word stored into the GX FIFO */
void HWGX_Port(uint32_t off, uint32_t val);          /* a 32-bit store to the I/O block offset `off` in 0x400..0x5FF (0x400..0x43F = the FIFO, path PORT) */
uint32_t HWGX_PortLast(uint32_t off);                /* the last value stored to a command port (the shadow); 0 if never */
void HWGX_TraceStop(void);                           /* close a window still recording (the run ends mid-window) */
void HWGX_SliceBegin(void);                          /* the ARM7 sound slice starts: check the canary, then swap in the ARM7's own view of the shared bytes (its SPU registers) */
void HWGX_SliceEnd(void);                            /* ... and ends: swap its registers out, the canary pattern back in */
void HWGX_SliceBegin(void);                          /* the ARM7 sound slice starts: check the canary, then swap in the ARM7's own view of the shared bytes (its SPU registers) */
void HWGX_SliceEnd(void);                            /* ... and ends: swap its registers out, the canary pattern back in */
void HWGX_FrameEnd(void);                            /* once per frame, at the vblank hook: canary check, per-frame counters, the stream of the recorded frame is closed; its state stays pending */
void HWGX_Seal(void);                                /* the game's snapshot point (after the vblank callback): write the pending frame's state chunks + FEND and open the next frame. No-op when nothing is pending. */
void HWGX_Stats(HwGxStats *out);

/* An observer (bet 2.1c, hw_census.c): called for every word counted on any path, in program order, with addr 0 for a FIFO word or the command-port offset
   (0x440..0x5FF). It only observes. NULL = none (the default); survives HWGX_Init. Documented function-pointer dispatch (golden rule 9). */
typedef void (*HwGxObserver)(uint32_t addr, uint32_t word);
void HWGX_SetObserver(HwGxObserver fn);
/* Bet 2.1c2: a block observer for a run of FIFO words that is already in memory (a DMA burst): `w` stays valid for the call, the words are counted on `path`
   exactly as if each had gone through HWGX_FifoWord. HWGX_FifoBlock returns 1 when it took the run (a block observer is set and no trace window is recording),
   else 0 and counts nothing: the caller sends the words one by one. Without an observer set nothing changes on any path. */
typedef void (*HwGxBlockObserver)(const uint32_t *w, uint32_t n);
void HWGX_SetBlockObserver(HwGxBlockObserver fn);
/* Bet 2.4b: the geometry engine's input. Unlike the observers (the census, which only watches), the feed is the engine itself: every GX word reaches it once, in program
   order, whether or not a trace is recording. `block` takes a DMA burst that is in memory (used only while no trace records; with a trace the words come one by one). */
void HWGX_SetFeed(HwGxObserver word, HwGxBlockObserver block);
int HWGX_FifoBlock(const uint32_t *w, uint32_t n, uint32_t path);

#ifdef __cplusplus
}
#endif

#endif
