/* hw_gx_test.c -- bet 2.1a: the GX input layer of source/hw/hw_gx.c alone. Native, one thread, no SDK, no libctru.
 * Feeds words on all four paths and checks: per-path counts, program order in the trace (one stream across paths, multi-word port sequences kept in order),
 * the canary catching a raw store, the trace window start/stop and file layout, the entry-count identity, failure handling and the config key.
 * Prints "hw_gx_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_cfg.h"
#include "hw_dev_hooks.h"
#include "hw_gx.h"

_Static_assert(HWG3_MODE_SEND == HWGX_MODE_SEND && HWG3_MODE_COPY == HWGX_MODE_COPY && HWG3_MODE_FIFO == HWGX_MODE_FIFO, "hook modes agree");

static long g_checks, g_fails;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

/* ---- the fake file seam ------------------------------------------------------------------------------------------------------ */
#define FILE_CAP (4u * 1024u * 1024u)
static uint8_t g_file[FILE_CAP];
static uint32_t g_flen;
static int g_creates, g_closes, g_fail_after = -1;   /* g_fail_after >= 0: appends fail once that many bytes were written */
static char g_last_path[96];

static void *fk_create(const char *path)
{
    g_creates++;
    g_flen = 0u;
    snprintf(g_last_path, sizeof g_last_path, "%s", path);
    return g_file;
}

static int fk_append(void *f, const void *buf, uint32_t len)
{
    assert(f == g_file);
    if (g_fail_after >= 0 && g_flen + len > (uint32_t)g_fail_after) {
        return -1;
    }
    assert(g_flen + len <= FILE_CAP);
    memcpy(g_file + g_flen, buf, len);
    g_flen += len;
    return (int)len;
}

static void fk_close(void *f)
{
    assert(f == g_file);
    g_closes++;
}

static int g_alloc_fail;
static void *fk_alloc(uint32_t bytes)
{
    return g_alloc_fail ? NULL : malloc(bytes);
}

static const HwGxIo g_io = { fk_create, fk_append, fk_close, fk_alloc };

#define VRAM_BYTES 0x2000u
static uint8_t g_regs[0x2000];
static uint8_t g_vram[VRAM_BYTES];
static uint8_t g_pltt[2048], g_oam[2048];
static uint8_t g_tex0[128u * 1024u], g_bg16[512u * 1024u];
static int g_tex_mapped;
static uint32_t snap_vbl(void);
static const uint8_t *snap_region(uint32_t id);

static void fresh(const HwGxIo *io)
{
    uint32_t i;

    memset(g_regs, 0, sizeof g_regs);
    for (i = 0; i < sizeof g_regs; i++) {
        g_regs[i] = (uint8_t)(i * 7u);   /* a state the trace dump can be compared against */
    }
    for (i = 0; i < VRAM_BYTES; i++) {
        g_vram[i] = (uint8_t)(i ^ 0x5au);
    }
    g_creates = g_closes = 0;
    g_fail_after = -1;
    HWGX_Init(g_regs, g_vram, VRAM_BYTES, io);
    memset(g_pltt, 0, sizeof g_pltt);
    memset(g_oam, 0, sizeof g_oam);
    memset(g_tex0, 0, sizeof g_tex0);
    memset(g_bg16, 0, sizeof g_bg16);
    g_tex_mapped = 0;
    {
        HwGxSnap sn;

        memset(&sn, 0, sizeof sn);   /* bet 2.3a: HwGxSnap grew a `lines` pointer; garbage there is a call through a wild pointer */
        sn.pltt = g_pltt; sn.oam = g_oam; sn.region = snap_region; sn.vblanks = snap_vbl; sn.clock_frame = 1u;
        HWGX_SetSnapshot(&sn);
    }
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- counts and the shadow ---------------------------------------------------------------------------------------------------- */
static void test_counts(void)
{
    static const uint32_t five[5] = { 1, 2, 3, 4, 5 };
    static const uint32_t nine[9] = { 11, 12, 13, 14, 15, 16, 17, 18, 19 };
    HwGxStats st;
    uint32_t i;

    fresh(NULL);
    for (i = 0; i < 3; i++) {
        HWGX_FifoWord(0x100 + i, HWGX_PATH_DMA);
    }
    HWG3_Words(HWGX_FIFO_OFF, five, 5, HWG3_MODE_SEND);          /* MI_CpuSend32 into the FIFO */
    HWG3_Words(0x440, five, 5, HWG3_MODE_COPY);                  /* MI_CpuCopy32 into 5 consecutive ports */
    HWG3_Words(HWGX_FIFO_OFF, nine, 9, HWG3_MODE_FIFO);          /* GX_SendFifo36B */
    HWG3_Port(0x4a4, 0xaa);                                      /* reg_G3_* store */
    HWG3_Port(0x400, 0xbb);                                      /* reg_G3X_GXFIFO = w */
    HWGX_Port(0x5c4, 0xcc);
    HWGX_Stats(&st);
    check(st.total[HWGX_PATH_DMA] == 3, "live DMA words", st.total[0]);
    check(st.total[HWGX_PATH_CPUSEND] == 10, "live CPUSEND words (5 send + 5 copy)", st.total[1]);
    check(st.total[HWGX_PATH_SENDFIFO] == 9, "live SENDFIFO words", st.total[2]);
    check(st.total[HWGX_PATH_PORT] == 3, "live PORT words (2 ports + 1 direct FIFO store)", st.total[3]);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.frames == 1 && st.last[0] == 3 && st.last[1] == 10 && st.last[2] == 9 && st.last[3] == 3, "frame-end per-path words", st.last[1]);
    check(st.total[0] == 3 && st.total[3] == 3, "totals survive the frame end", st.total[3]);
    check(HWGX_PortLast(0x4a4) == 0xaa && HWGX_PortLast(0x5c4) == 0xcc, "shadow keeps the last port value", HWGX_PortLast(0x4a4));
    check(HWGX_PortLast(0x444) == 2 && HWGX_PortLast(0x440) == 1, "copy lands on consecutive ports", HWGX_PortLast(0x444));
    check(rd32(g_regs + 0x4a4) != 0xaa, "the hooks never store into the I/O block", rd32(g_regs + 0x4a4));
    check(st.canary_words == 0 && st.canary_frames == 0, "hooked stores leave the canary alone", st.canary_words);
    HWGX_FifoWord(1, 9);
    HWG3_Port(0x300, 5);
    HWG3_Port(0x600, 5);
    HWGX_Stats(&st);
    check(st.stray == 3, "bad path and out-of-range offsets are stray, not recorded", st.stray);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.last[0] == 0 && st.peak[1] == 10 && st.peak[2] == 9, "an empty frame zeroes `last`, `peak` stays", st.peak[1]);
}

/* ---- the block observer (bet 2.1c2) -------------------------------------------------------------------------------------------- */
static uint32_t g_blk_words, g_blk_calls, g_obs_words;

static void blk_obs(const uint32_t *w, uint32_t n)
{
    (void)w;
    g_blk_words += n;
    g_blk_calls++;
}

static void word_obs(uint32_t addr, uint32_t word)
{
    (void)addr;
    (void)word;
    g_obs_words++;
}

static void test_block(void)
{
    static const uint32_t run[6] = { 1, 2, 3, 4, 5, 6 };
    HwGxStats st;

    fresh(NULL);
    HWGX_SetBlockObserver(NULL);
    HWGX_SetObserver(word_obs);
    check(HWGX_FifoBlock(run, 6, HWGX_PATH_DMA) == 0, "no block observer: the run is declined, nothing counted", 0);
    HWGX_Stats(&st);
    check(st.total[HWGX_PATH_DMA] == 0, "and nothing was counted", st.total[0]);
    HWGX_SetBlockObserver(blk_obs);
    check(HWGX_FifoBlock(run, 6, HWGX_PATH_DMA) == 1 && g_blk_words == 6 && g_blk_calls == 1 && g_obs_words == 0, "a block observer takes the run whole", g_blk_words);
    HWGX_Stats(&st);
    check(st.total[HWGX_PATH_DMA] == 6, "the run is counted on its path", st.total[0]);
    check(HWGX_FifoBlock(run, 6, 9) == 0 && HWGX_FifoBlock(NULL, 6, HWGX_PATH_DMA) == 0, "a bad path or a NULL run is declined", 0);
    HWGX_SetObserver(NULL);
    HWGX_SetBlockObserver(NULL);
    fresh(&g_io);
    HWGX_SetBlockObserver(blk_obs);
    HWGX_SetTrace(0, 4, "t.bin");
    g_blk_words = 0;
    check(HWGX_FifoBlock(run, 6, HWGX_PATH_DMA) == 0 && g_blk_words == 0, "while a trace window records, the run is declined (every word must reach the trace)", g_blk_words);
    HWGX_SetBlockObserver(NULL);
}

/* ---- the canary --------------------------------------------------------------------------------------------------------------- */
static void test_canary(void)
{
    HwGxStats st;

    fresh(NULL);
    check(rd32(g_regs + 0x400) != 0 && rd32(g_regs + 0x5fc) != 0 && rd32(g_regs + 0x400) != rd32(g_regs + 0x404), "pattern armed over 0x400..0x5FF", rd32(g_regs + 0x400));
    check(g_regs[0x3ff] == (uint8_t)(0x3ffu * 7u) && g_regs[0x600] == (uint8_t)(0x600u * 7u), "the pattern stays inside 0x400..0x5FF", g_regs[0x600]);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 0, "quiet frame: no canary hit", st.canary_words);
    g_regs[0x480] = 1;   /* an unhooked raw store */
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 1 && st.canary_frames == 1, "a raw store is detected", st.canary_words);
    check(st.canary_first_off == 0x480 && st.canary_first_val == 0xc4a70001u, "first differing offset and value kept", st.canary_first_off);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 1 && st.canary_frames == 1, "re-armed: the same store is not counted twice", st.canary_words);
    g_regs[0x400] ^= 0xff;
    g_regs[0x5ff] ^= 0xff;
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 3 && st.canary_frames == 2 && st.canary_first_off == 0x480, "two more words, first offset unchanged", st.canary_words);
    memset(g_regs, 0, sizeof g_regs);   /* a soft reset zeroes the block ... */
    HWGX_ArmCanary();                   /* ... and the owner re-arms it */
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 3, "ArmCanary after a reset: no false hits", st.canary_words);

    /* the ARM7 sound slice shares the block but has its own view: it reads 0 (not the pattern), its stores are not hits and survive between slices */
    HWGX_ArmCanary();
    HWGX_SliceBegin();
    check(rd32(g_regs + 0x500) == 0u && rd32(g_regs + 0x400) == 0u, "inside a slice the shared words read 0 (SPU registers), not the pattern", rd32(g_regs + 0x500));
    wr32(g_regs + 0x500, 0x807fu);   /* SOUNDCNT */
    wr32(g_regs + 0x400, 0x1234u);
    HWGX_SliceEnd();
    check(rd32(g_regs + 0x500) == 0xc4a70040u, "outside the slice the canary pattern is back", rd32(g_regs + 0x500));
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 3, "slice stores are not canary hits", st.canary_words);
    HWGX_SliceBegin();
    check(rd32(g_regs + 0x500) == 0x807fu && rd32(g_regs + 0x400) == 0x1234u, "the next slice sees what the driver left", rd32(g_regs + 0x500));
    HWGX_SliceEnd();
    wr32(g_regs + 0x504, 7u);        /* an unhooked ARM9 store before the next slice */
    HWGX_SliceBegin();
    HWGX_SliceEnd();
    HWGX_Stats(&st);
    check(st.canary_words == 4 && st.canary_at_slice == 1, "an ARM9 store before a slice is caught at its start", st.canary_words);
    wr32(g_regs + 0x500, 0u);        /* ... and after a slice, at the frame end */
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.canary_words == 5 && rd32(g_regs + 0x500) == 0xc4a70040u, "an ARM9 store after a slice is caught at the frame end", st.canary_words);
    HWGX_ArmCanary();                /* a reset clears the driver's registers */
    HWGX_SliceBegin();
    check(rd32(g_regs + 0x500) == 0u, "ArmCanary clears the ARM7 view", rd32(g_regs + 0x500));
    HWGX_SliceEnd();
}

/* ---- the trace (version 2: chunked frames, bet 2.2a) --------------------------------------------------------------------- */
static uint32_t g_vbl;
static uint32_t snap_vbl(void) { return g_vbl; }
static const uint8_t *snap_region(uint32_t id)
{
    if (id == 0u && g_tex_mapped) { return g_tex0; }
    if (id == 16u) { return g_bg16; }
    return NULL;
}

/* a chunk walker over g_file */
typedef struct Walk { uint32_t off; char tag[5]; uint32_t len; const uint8_t *pay; } Walk;
static int walk_next(Walk *w)
{
    if (w->off + 8u > g_flen) {
        return 0;
    }
    memcpy(w->tag, g_file + w->off, 4);
    w->tag[4] = 0;
    w->len = rd32(g_file + w->off + 4);
    w->pay = g_file + w->off + 8;
    if (w->off + 8u + w->len > g_flen) {
        return 0;
    }
    w->off += 8u + w->len;
    return 1;
}

#define PER_FRAME 21u   /* 1 + 1 + 9 + 4 + 2 + 3 + 1 */

static void send_frame(uint32_t f)
{
    static const uint32_t m[4] = { 0xe0, 0xe1, 0xe2, 0xe3 };
    uint32_t i;

    HWGX_FifoWord(0xd0000000u | f, HWGX_PATH_DMA);
    HWG3_Port(0x470, 0x10000000u | f);
    for (i = 0; i < 9; i++) {
        HWG3_Port(0x48c, 0x2000u + i);   /* nine words through ONE port: order matters (g3_util.c) */
    }
    HWG3_Words(HWGX_FIFO_OFF, m, 4, HWG3_MODE_SEND);
    HWG3_Words(0x440, m, 2, HWG3_MODE_COPY);
    HWG3_Words(HWGX_FIFO_OFF + 8, m, 3, HWG3_MODE_FIFO);   /* the FIFO mirror: still the FIFO */
    HWG3_Port(HWGX_FIFO_OFF, 0x30000000u | f);
}

/* walk one complete frame starting at w; returns 1 when the tag order is exactly FRAM STRM* R3D VCNT PAGE* R2A R2B PLTT OAM PAGE* FEND; fills the facts */
typedef struct FrameFacts {
    uint32_t frame, vbl, index, first, entries, path[4], pages3d, pages2d, fend_entries, fend_path[4], fend_pages, fend_seal, rdbk, fend_rdbk;
    uint32_t page_regions[40], npage_regions;
    int order_ok;
} FrameFacts;

static int walk_frame(Walk *w, FrameFacts *ff, const uint8_t **stream_out)
{
    static const char *tail[] = { "R3D ", "VCNT", "R2A ", "R2B ", "PLTT", "OAM " };
    uint32_t t = 0;
    uint32_t i;

    memset(ff, 0, sizeof *ff);
    ff->order_ok = 1;
    if (!walk_next(w) || strcmp(w->tag, "FRAM") != 0 || w->len != 32u) { ff->order_ok = 0; return 0; }
    ff->frame = rd32(w->pay); ff->vbl = rd32(w->pay + 4); ff->index = rd32(w->pay + 8); ff->first = rd32(w->pay + 12);
    for (;;) {
        if (!walk_next(w)) { ff->order_ok = 0; return 0; }
        if (strcmp(w->tag, "STRM") == 0 && t == 0u) {
            ff->entries += w->len / 8u;
            for (i = 0; i < w->len / 8u; i++) {
                if (w->pay[i * 8 + 6] == HWGX_PATH_RDBK) { ff->rdbk++; ff->entries--; } else { ff->path[w->pay[i * 8 + 6] & 3]++; }
            }
            if (stream_out != NULL && *stream_out == NULL) { *stream_out = w->pay; }
            continue;
        }
        if (strcmp(w->tag, "PAGE") == 0) {
            uint32_t reg = rd32(w->pay), np = rd32(w->pay + 4);
            if (w->len != 8u + np * 1028u) { ff->order_ok = 0; }
            if (ff->npage_regions < 40u) { ff->page_regions[ff->npage_regions++] = reg; }
            if (t <= 2u) { ff->pages3d += np; } else { ff->pages2d += np; }
            continue;
        }
        if (strcmp(w->tag, "FEND") == 0) {
            ff->fend_entries = rd32(w->pay);
            for (i = 0; i < 4; i++) { ff->fend_path[i] = rd32(w->pay + 4 + 4 * i); }
            ff->fend_pages = rd32(w->pay + 20);
            ff->fend_seal = rd32(w->pay + 24);
            ff->fend_rdbk = rd32(w->pay + 28);
            if (t != 6u) { ff->order_ok = 0; }
            return 1;
        }
        if (t < 6u && strcmp(w->tag, tail[t]) == 0) {
            t++;
            continue;
        }
        ff->order_ok = 0;
        return 0;
    }
}

static void test_trace(void)
{
    HwGxStats st;
    Walk w;
    FrameFacts ff;
    uint32_t f, i;

    fresh(&g_io);
    HWGX_SetTrace(2, 3, "sdmc:/x/gxtrace.bin");
    check(g_creates == 0, "nothing is written before the window", g_creates);
    for (f = 0; f < 7; f++) {
        g_vbl = 100u + f;
        if (f == 2u) {   /* the first frame of the window: tex slot 0 pages 3 and 5, main BG page 7 */
            memset(g_tex0, 0, sizeof g_tex0);
            memset(g_tex0 + 3u * 1024u, 0x33, 1024);
            memset(g_tex0 + 5u * 1024u, 0x55, 1024);
            memset(g_bg16 + 7u * 1024u, 0x77, 1024);
            g_tex_mapped = 1;
        }
        if (f == 4u) {   /* the last: page 5 changes, page 3 stays (not dumped) */
            memset(g_tex0 + 5u * 1024u, 0x56, 1024);
        }
        send_frame(f);
        HWGX_FrameEnd(); HWGX_Seal();
    }
    HWGX_Stats(&st);
    check(g_creates == 1 && g_closes == 1 && strcmp(g_last_path, "sdmc:/x/gxtrace.bin") == 0, "one file, closed at the window end", g_closes);
    check(st.trace_state == 3 && st.trace_frames == 3, "window 2:3 recorded 3 frames and finished", st.trace_frames);
    check(st.trace_entries == 3 * PER_FRAME, "entry count = 3 frames of stores", st.trace_entries);
    for (i = 0; i < 4; i++) {
        check(st.trace_expect[i] == st.trace_got[i], "per-path entries = per-path counters", i);
    }
    check(st.trace_bytes == g_flen, "trace_bytes = file size", st.trace_bytes);
    check(memcmp(g_file, "GXTR", 4) == 0 && rd32(g_file + 4) == 2 && rd32(g_file + 8) == 2 && rd32(g_file + 12) == 3 && rd32(g_file + 16) == 11 && rd32(g_file + 20) == 8, "header fields (version 2, from 2, count 3, flags: frame clock + seal-after-vblank-callback + RDBK, entry 8)", rd32(g_file + 4));
    check(rd32(g_file + 24) == 0x60 && rd32(g_file + 28) == 0x3a0 && rd32(g_file + 32) == VRAM_BYTES && rd32(g_file + 36) == 1024 && rd32(g_file + 40) == 101u, "header: regs/vram extents, page size, vblanks at open", rd32(g_file + 40));
    w.off = HWGX_TRACE_HDR;
    for (f = 2; f < 5; f++) {
        const uint8_t *sp = NULL;

        check(walk_frame(&w, &ff, &sp) == 1 && ff.order_ok, "frame chunks in the documented order", f);
        check(ff.frame == f && ff.vbl == 99u + f && ff.index == f - 2u && ff.first == (f == 2u ? 1u : 0u), "FRAM: frame, event key (DS vblanks at the frame start = the previous frame end), window index, first flag", ff.frame);
        check(ff.entries == PER_FRAME && ff.fend_entries == PER_FRAME, "stream entries = FEND entries", ff.entries);
        for (i = 0; i < 4; i++) {
            check(ff.path[i] == ff.fend_path[i], "stream per-path counts = FEND per-path counts", i);
        }
        check(ff.path[HWGX_PATH_DMA] == 1 && ff.path[HWGX_PATH_CPUSEND] == 6 && ff.path[HWGX_PATH_SENDFIFO] == 3 && ff.path[HWGX_PATH_PORT] == 11, "entries per path in the frame", ff.path[3]);
        check(sp != NULL && rd32(sp) == (0xd0000000u | f) && sp[6] == HWGX_PATH_DMA, "first stream entry is the DMA word", 0);
        if (f == 2u) {
            check(ff.fend_pages == 3u && ff.pages3d == 2u && ff.pages2d == 1u, "first frame: tex 0 pages 3,5 + main BG page 7 (non-zero pages only)", ff.fend_pages);
            check(ff.npage_regions == 2 && ff.page_regions[0] == 0 && ff.page_regions[1] == 16, "page regions ascending, 3D before 2D", ff.npage_regions);
        } else if (f == 3u) {
            check(ff.fend_pages == 0u && ff.npage_regions == 0, "unchanged state: no PAGE chunk", ff.fend_pages);
        } else {
            check(ff.fend_pages == 1u && ff.pages3d == 1u && ff.pages2d == 0u, "only the page whose content changed is dumped", ff.fend_pages);
        }
    }
    check(w.off == g_flen, "walked the whole file", (long)w.off);
    check(st.trace_pages == 4u, "trace_pages counts the dumped pages", st.trace_pages);
    check(g_regs[0x60] == (uint8_t)(0x60u * 7u), "the snapshot does not disturb the registers", 0);
}

/* Bet 2.4a: the RDBK entry kind. A game READ of a geometry-engine register is recorded at its exact stream position, trace-only: head {n, off, path 4, pad 1}, then n values
   {value, off + 4 i, path 4, pad 2}. No counter the port runs on moves, FEND word 7 counts the frame's RDBK entries, and a readback outside a recorded frame is dropped. */
static void test_rdbk(void)
{
    uint32_t clip[16], one = 0x06000000u, vec = 0xfff0u, i, k = 0u;
    HwGxStats st, st2;
    Walk w;
    FrameFacts ff;
    const uint8_t *sp = NULL, *e;

    for (i = 0; i < 16u; i++) {
        clip[i] = 0x1000u * i + 7u;
    }
    fresh(&g_io);
    HWG3_Readback(0x600, 1, &one);   /* no window yet: nothing happens */
    HWGX_Stats(&st);
    check(st.trace_state == 0 && st.trace_entries == 0, "a readback with no trace armed records nothing", st.trace_entries);
    g_vbl = 50;
    HWGX_SetTrace(0, 2, "rdbk");
    HWG3_Port(0x470, 11u);
    HWG3_Readback(0x640, 16, clip);
    HWG3_Port(0x470, 22u);
    HWG3_Readback(0x630, 1, &vec);
    HWG3_Readback(0x600, 1, &one);
    HWGX_FrameEnd();
    HWG3_Readback(0x600, 1, &one);   /* between the frame end and the snapshot point: belongs to the NEXT frame */
    HWGX_Seal();
    HWG3_Port(0x470, 33u);
    HWGX_FrameEnd();
    HWG3_Readback(0x680, 1, &one);   /* after the last recorded frame end: dropped, like any word queued there */
    HWGX_Seal();
    HWGX_Stats(&st);
    HWGX_Stats(&st2);
    check(st.trace_state == 3 && st.trace_frames == 2, "window of 2 frames finished", st.trace_state);
    check(st.trace_entries == 3 && st.total[HWGX_PATH_PORT] == 3 && st.trace_expect[HWGX_PATH_PORT] == st.trace_got[HWGX_PATH_PORT], "RDBK entries are in no store counter", st.trace_entries);
    check(rd32(g_file + 16) == 11u, "header flags carry bit 3", rd32(g_file + 16));
    w.off = HWGX_TRACE_HDR;
    check(walk_frame(&w, &ff, &sp) == 1 && ff.order_ok, "frame 0 chunk order", 0);
    check(ff.entries == 2 && ff.fend_entries == 2 && ff.rdbk == 17u + 2u + 2u && ff.fend_rdbk == 21u, "frame 0: 2 stores, 21 RDBK entries (16+1, 1+1, 1+1), FEND word 7 = 21", ff.fend_rdbk);
    /* the exact stream: store, head+16, store, head+1, head+1 */
    e = sp;
    check(rd32(e) == 11u && e[6] == HWGX_PATH_PORT && e[7] == 0, "entry 0: the first store, pad 0", 0);
    e += 8;
    check(rd32(e) == 16u && (e[4] | (e[5] << 8)) == 0x640 && e[6] == HWGX_PATH_RDBK && e[7] == 1, "head: 16 words at 0x640, path 4, pad 1", rd32(e));
    for (i = 0; i < 16u; i++) {
        e += 8;
        if (!(rd32(e) == clip[i] && (e[4] | (e[5] << 8)) == (int)(0x640 + 4 * i) && e[6] == HWGX_PATH_RDBK && e[7] == 2)) { k++; }
    }
    check(k == 0, "16 value entries: the values the port returned, address off + 4 i, pad 2", k);
    e += 8;
    check(rd32(e) == 22u && e[6] == HWGX_PATH_PORT, "the second store follows the group (program order)", rd32(e));
    e += 8;
    check(rd32(e) == 1u && (e[4] | (e[5] << 8)) == 0x630 && e[7] == 1, "head: 1 word at 0x630", rd32(e));
    e += 8;
    check(rd32(e) == 0xfff0u && (e[4] | (e[5] << 8)) == 0x630 && e[7] == 2, "value 0xfff0 at 0x630", rd32(e));
    e += 8;
    check(rd32(e) == 1u && (e[4] | (e[5] << 8)) == 0x600 && e[7] == 1, "head: GXSTAT", rd32(e));
    e += 8;
    check(rd32(e) == 0x06000000u && (e[4] | (e[5] << 8)) == 0x600 && e[7] == 2, "GXSTAT value", rd32(e));
    check(walk_frame(&w, &ff, &sp) == 1 && ff.order_ok, "frame 1 chunk order", 1);
    check(ff.entries == 1 && ff.rdbk == 2u && ff.fend_rdbk == 2u, "frame 1: the readback between the frame end and the seal is in it; the one after its end is dropped", ff.fend_rdbk);
    check(w.off == g_flen && st.trace_bytes == g_flen, "walked the whole file", (long)w.off);
}

static void test_trace_content(void)
{
    Walk w;
    uint32_t i;
    int got_r3d = 0, got_vc = 0, got_a = 0, got_b = 0, got_p = 0, got_o = 0, got_page_ok = 0;

    fresh(&g_io);
    g_vbl = 7;
    g_tex_mapped = 1;
    memset(g_tex0, 0, sizeof g_tex0);
    memset(g_tex0 + 3u * 1024u, 0x33, 1024);
    for (i = 0; i < 2048; i++) { g_pltt[i] = (uint8_t)(i * 3u); g_oam[i] = (uint8_t)(i * 5u + 1u); }
    HWGX_SetTrace(0, 1, "a");
    HWGX_FrameEnd(); HWGX_Seal();
    w.off = HWGX_TRACE_HDR;
    while (walk_next(&w)) {
        if (strcmp(w.tag, "R3D ") == 0) { got_r3d = w.len == 0x3a0 && memcmp(w.pay, g_regs + 0x60, 0x3a0) == 0; }
        if (strcmp(w.tag, "VCNT") == 0) { got_vc = w.len == 16 && memcmp(w.pay, g_regs + 0x240, 10) == 0 && w.pay[10] == 0 && w.pay[15] == 0; }
        if (strcmp(w.tag, "R2A ") == 0) { got_a = w.len == 0x70 && memcmp(w.pay, g_regs, 0x70) == 0; }
        if (strcmp(w.tag, "R2B ") == 0) { got_b = w.len == 0x70 && memcmp(w.pay, g_regs + 0x1000, 0x70) == 0; }
        if (strcmp(w.tag, "PLTT") == 0) { got_p = w.len == 2048 && memcmp(w.pay, g_pltt, 2048) == 0; }
        if (strcmp(w.tag, "OAM ") == 0) { got_o = w.len == 2048 && memcmp(w.pay, g_oam, 2048) == 0; }
        if (strcmp(w.tag, "PAGE") == 0 && rd32(w.pay) == 0u) { got_page_ok = rd32(w.pay + 4) == 1 && rd32(w.pay + 8) == 3 && memcmp(w.pay + 12, g_tex0 + 3u * 1024u, 1024) == 0; }
    }
    check(got_r3d, "R3D = registers 0x60..0x3FF", 0);
    check(got_vc, "VCNT = VRAMCNT_A..I bytes", 0);
    check(got_a && got_b, "R2A / R2B = engine A at 0, engine B at 0x1000", 0);
    check(got_p && got_o, "PLTT / OAM = the snapshot sources", 0);
    check(got_page_ok, "PAGE payload = u32 page index + 1 KB of the page", 0);

    /* an unmapped region reads as zeros: dumping it after it was mapped sends the zeroed pages */
    fresh(&g_io);
    g_tex_mapped = 1;
    memset(g_tex0, 0, sizeof g_tex0);
    memset(g_tex0 + 9u * 1024u, 0x99, 1024);
    HWGX_SetTrace(0, 2, "a");
    HWGX_FrameEnd(); HWGX_Seal();
    g_tex_mapped = 0;
    HWGX_FrameEnd(); HWGX_Seal();
    {
        FrameFacts ff;
        const uint8_t *sp = NULL;
        int zero_ok = 0;

        w.off = HWGX_TRACE_HDR;
        check(walk_frame(&w, &ff, &sp) == 1 && ff.pages3d == 1, "frame 0 dumps the mapped non-zero page", ff.pages3d);
        {
            Walk w2 = w;
            while (walk_next(&w2)) {
                if (strcmp(w2.tag, "PAGE") == 0 && rd32(w2.pay) == 0u && rd32(w2.pay + 4) == 1 && rd32(w2.pay + 8) == 9) {
                    uint32_t k, nz = 0;
                    for (k = 0; k < 1024; k++) { nz |= w2.pay[12 + k]; }
                    zero_ok = nz == 0;
                }
            }
        }
        check(zero_ok, "unmapping a region dumps its pages as zeros", 0);
    }
}

static void test_trace_edges(void)
{
    HwGxStats st;
    Walk w;
    uint32_t i, strm = 0, ents = 0;

    /* from == 0: opens at once, the first FRAM is frame 0 and flags first */
    fresh(&g_io);
    HWGX_SetTrace(0, 2, "a");
    check(g_creates == 1, "from 0 opens immediately", g_creates);
    send_frame(0);
    HWGX_FrameEnd(); HWGX_Seal();
    send_frame(1);
    HWGX_FrameEnd(); HWGX_Seal();
    send_frame(2);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.trace_state == 3 && st.trace_entries == 2 * PER_FRAME && g_closes == 1, "0:2 records exactly 2 frames", st.trace_entries);
    check(memcmp(g_file + HWGX_TRACE_HDR, "FRAM", 4) == 0 && rd32(g_file + HWGX_TRACE_HDR + 8) == 0 && rd32(g_file + HWGX_TRACE_HDR + 16) == 0 && rd32(g_file + HWGX_TRACE_HDR + 20) == 1, "first FRAM is frame 0, index 0, first flag", 0);

    /* count 0 = off, no I/O seam = off */
    fresh(&g_io);
    HWGX_SetTrace(0, 0, "a");
    send_frame(0);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.trace_state == 0 && g_creates == 0, "count 0 is off", st.trace_state);
    fresh(NULL);
    HWGX_SetTrace(0, 5, "a");
    HWGX_Stats(&st);
    check(st.trace_state == 0, "no file seam: trace stays off", st.trace_state);

    /* the run ends mid-window: the file ends after the open frame's stream, no FEND */
    fresh(&g_io);
    HWGX_SetTrace(1, 100, "a");
    HWGX_FrameEnd(); HWGX_Seal();
    send_frame(1);
    HWGX_FrameEnd(); HWGX_Seal();
    send_frame(2);
    HWGX_TraceStop();
    HWGX_Stats(&st);
    check(st.trace_state == 3 && g_closes == 1, "TraceStop closes a recording window", st.trace_state);
    HWGX_TraceStop();
    check(g_closes == 1, "TraceStop twice closes once", g_closes);

    /* more entries than the buffer holds inside one frame: STRM chunks of 16384 entries, nothing lost */
    fresh(&g_io);
    HWGX_SetTrace(0, 1, "a");
    for (i = 0; i < HWGX_TRACE_BUF * 2 + 123; i++) {
        HWGX_FifoWord(i, HWGX_PATH_DMA);
    }
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    w.off = HWGX_TRACE_HDR;
    while (walk_next(&w)) {
        if (strcmp(w.tag, "STRM") == 0) {
            strm++;
            ents += w.len / 8u;
            check(w.len / 8u <= HWGX_TRACE_BUF, "a STRM chunk holds at most 16384 entries", w.len / 8u);
        }
    }
    check(st.trace_entries == HWGX_TRACE_BUF * 2 + 123 && strm == 3 && ents == st.trace_entries, "buffer overflow flushes in chunks, nothing lost", strm);
    check(rd32(g_file + HWGX_TRACE_HDR + 8 + 32 + 8 + 8 + 8 * 30000) == 30000, "entry 30000 intact after flushes", 0);

    /* no memory for the page hashes: the trace fails before it opens anything, the counters go on */
    fresh(&g_io);
    g_alloc_fail = 1;
    HWGX_SetTrace(0, 3, "a");
    HWGX_Stats(&st);
    g_alloc_fail = 0;
    send_frame(0);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.trace_state == 4 && g_creates == 0 && st.total[0] == 1, "alloc failure: trace failed, no file, counters go on", st.trace_state);

    /* a failing file: the trace fails, the counters keep going */
    fresh(&g_io);
    g_fail_after = 40;
    HWGX_SetTrace(0, 3, "a");
    HWGX_Stats(&st);
    check(st.trace_state == 4, "a short header write fails the trace", st.trace_state);
    send_frame(0);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.trace_state == 4 && st.total[0] == 1 && g_closes == 1, "counters survive a failed trace, file closed", st.total[0]);

    /* a failure in the middle of a snapshot: fails, closes, counters go on */
    fresh(&g_io);
    g_fail_after = 64 + 40 + 8 + 21 * 8 + 100;
    HWGX_SetTrace(0, 3, "a");
    send_frame(0);
    HWGX_FrameEnd(); HWGX_Seal();
    HWGX_Stats(&st);
    check(st.trace_state == 4 && g_closes == 1 && st.total[0] == 1, "a write failing inside the snapshot fails the trace and closes the file", st.trace_state);
}


/* Bet 2.2c, B3: the sealed state is read at HWGX_Seal() (the game's snapshot point), not at HWGX_FrameEnd(); a missing Seal is sealed by the next frame end. */
static void test_seal(void)
{
    Walk w;
    FrameFacts ff;
    HwGxStats st;
    const uint8_t *sp = NULL;

    fresh(&g_io);
    g_vbl = 10;
    g_tex_mapped = 1;
    memset(g_tex0, 0, sizeof g_tex0);
    HWGX_SetTrace(0, 3, "seal");
    send_frame(0);
    HWGX_FrameEnd();                                   /* the vblank boundary: the stream is closed, the state not yet read */
    memset(g_tex0 + 3u * 1024u, 0x44, 1024);           /* the game's vblank callback uploads a texture page */
    HWGX_Seal();                                       /* the snapshot point: the page is in THIS frame */
    send_frame(1);
    HWGX_FrameEnd();                                   /* frame 1: no Seal this time */
    memset(g_tex0 + 5u * 1024u, 0x55, 1024);
    send_frame(2);
    HWGX_FrameEnd();                                   /* frame 1 is sealed by this call (the page above lands in frame 1), frame 2 stays pending */
    HWG3_Port(0x48c, 0x77u);                           /* the game's vblank callback queues a word for the NEXT (unrecorded) frame */
    HWGX_Seal();                                       /* seals frame 2 and closes the window */
    HWGX_Stats(&st);
    check(st.trace_expect[3] == st.trace_got[3] && st.trace_entries == 3 * PER_FRAME, "a word queued after the last frame end is dropped from the counters", st.trace_got[3]);
    check(st.trace_state == 3 && st.trace_frames == 3, "three frames recorded", st.trace_frames);
    check(st.seals_snapshot == 2 && st.seals_next_vblank == 1, "two frames sealed at the snapshot point, one by the next frame end", st.seals_next_vblank);
    check(rd32(g_file + 16) == 11, "header flags: frame clock + seal-after-vblank-callback + RDBK", rd32(g_file + 16));
    w.off = HWGX_TRACE_HDR;
    check(walk_frame(&w, &ff, &sp) == 1 && ff.fend_pages == 1u && ff.fend_seal == HWGX_SEAL_SNAPSHOT, "frame 0: the page uploaded after FrameEnd is in the frame, seal kind = snapshot", ff.fend_pages);
    check(walk_frame(&w, &ff, &sp) == 1 && ff.fend_pages == 1u && ff.fend_seal == HWGX_SEAL_NEXT_VBLANK, "frame 1: sealed by the next frame end, seal kind = next vblank", ff.fend_pages);
    check(walk_frame(&w, &ff, &sp) == 1 && ff.fend_pages == 0u && ff.fend_seal == HWGX_SEAL_SNAPSHOT, "frame 2: sealed by the explicit call, nothing new to dump", ff.fend_pages);
    check(w.off == g_flen, "no STRM chunk after the last FEND", (long)w.off);
}

static void test_seal_frame_end(void)
{
    Walk w;
    FrameFacts ff;
    const uint8_t *sp = NULL;

    fresh(&g_io);
    g_vbl = 10;
    g_tex_mapped = 1;
    memset(g_tex0, 0, sizeof g_tex0);
    HWGX_SetSealMode(1);
    HWGX_SetTrace(0, 2, "sealfe");
    send_frame(0);
    HWGX_FrameEnd();
    memset(g_tex0 + 3u * 1024u, 0x44, 1024);           /* the callback's upload: in frame-end mode it belongs to the NEXT frame */
    HWGX_Seal();                                       /* no-op in this mode */
    send_frame(1);
    HWGX_FrameEnd();
    HWGX_Seal();
    check(rd32(g_file + 16) == 9, "frame-end mode: header flags = frame clock + RDBK (seal bit clear)", rd32(g_file + 16));
    w.off = HWGX_TRACE_HDR;
    check(walk_frame(&w, &ff, &sp) == 1 && ff.fend_pages == 0u && ff.fend_seal == 0u, "frame-end mode, frame 0: the page is not in it, seal kind 0", ff.fend_pages);
    check(walk_frame(&w, &ff, &sp) == 1 && ff.fend_pages == 1u && ff.fend_seal == 0u, "frame-end mode, frame 1: the page lands one frame later", ff.fend_pages);
}

/* Bet 2.3a: the per-line 2D chunks L2A / L2B (header flags bit 2): after R2B, form word then the images; uniform = 0x70 bytes, lines = 192 x 0x70, none = nothing (a frame sealed
   at the next vblank hook, whose own lines never ran); absent entirely when the snapshot has no line source or the seal mode is frame-end. */
static uint32_t g_l2_form;
static uint8_t g_l2_img[2][192u * HWGX_REGS2D_LEN];
static const uint8_t *snap_lines(uint32_t engine, uint32_t *form)
{
    *form = g_l2_form;
    return g_l2_form == HWGX_L2_NONE ? NULL : g_l2_img[engine];
}

static void with_lines(void)
{
    HwGxSnap sn;

    memset(&sn, 0, sizeof sn);
    sn.pltt = g_pltt; sn.oam = g_oam; sn.region = snap_region; sn.vblanks = snap_vbl; sn.clock_frame = 1u; sn.lines = snap_lines;
    HWGX_SetSnapshot(&sn);
}

/* walk to the chunk with tag `tag` inside the current frame; 1 when found */
static int walk_to(Walk *w, const char *tag)
{
    while (walk_next(w)) {
        if (strcmp(w->tag, tag) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_lines2d(void)
{
    Walk w;
    uint32_t i;
    int ok;

    for (i = 0; i < sizeof g_l2_img[0]; i++) {
        g_l2_img[0][i] = (uint8_t)(i * 3u + 1u);
        g_l2_img[1][i] = (uint8_t)(i * 5u + 2u);
    }
    /* three frames: uniform, lines, and one sealed by the next vblank hook (none) */
    fresh(&g_io);
    with_lines();
    g_vbl = 10;
    HWGX_SetTrace(0, 3, "l2");
    g_l2_form = HWGX_L2_UNIFORM;
    send_frame(0); HWGX_FrameEnd(); HWGX_Seal();
    g_l2_form = HWGX_L2_LINES;
    send_frame(1); HWGX_FrameEnd(); HWGX_Seal();
    g_l2_form = HWGX_L2_UNIFORM;   /* a stale record: must NOT be dumped for a frame the snapshot point never sealed */
    send_frame(2); HWGX_FrameEnd();
    HWGX_TraceStop();
    check(rd32(g_file + 16) == 15u, "L2: header flags = frame clock | seal at snapshot | per-line 2D (bit 2) | RDBK", rd32(g_file + 16));
    w.off = HWGX_TRACE_HDR;
    ok = walk_to(&w, "R2B ") && walk_next(&w) && strcmp(w.tag, "L2A ") == 0 && w.len == 4u + HWGX_REGS2D_LEN && rd32(w.pay) == HWGX_L2_UNIFORM && memcmp(w.pay + 4, g_l2_img[0], HWGX_REGS2D_LEN) == 0;
    check(ok, "L2: frame 0 L2A = uniform form + the 0x70-byte image, right after R2B", w.len);
    ok = walk_next(&w) && strcmp(w.tag, "L2B ") == 0 && w.len == 4u + HWGX_REGS2D_LEN && rd32(w.pay) == HWGX_L2_UNIFORM && memcmp(w.pay + 4, g_l2_img[1], HWGX_REGS2D_LEN) == 0;
    check(ok, "L2: frame 0 L2B = uniform form + engine B's image", w.len);
    ok = walk_next(&w) && strcmp(w.tag, "PLTT") == 0;
    check(ok, "L2: PLTT follows L2B", 0);
    ok = walk_to(&w, "R2B ") && walk_next(&w) && strcmp(w.tag, "L2A ") == 0 && w.len == 4u + 192u * HWGX_REGS2D_LEN && rd32(w.pay) == HWGX_L2_LINES && memcmp(w.pay + 4, g_l2_img[0], 192u * HWGX_REGS2D_LEN) == 0;
    check(ok, "L2: frame 1 L2A = lines form + 192 images", w.len);
    ok = walk_next(&w) && strcmp(w.tag, "L2B ") == 0 && w.len == 4u + 192u * HWGX_REGS2D_LEN && rd32(w.pay) == HWGX_L2_LINES && memcmp(w.pay + 4, g_l2_img[1], 192u * HWGX_REGS2D_LEN) == 0;
    check(ok, "L2: frame 1 L2B = lines form + 192 images", w.len);
    ok = walk_to(&w, "R2B ") && walk_next(&w) && strcmp(w.tag, "L2A ") == 0 && w.len == 4u && rd32(w.pay) == HWGX_L2_NONE && walk_next(&w) && strcmp(w.tag, "L2B ") == 0 && w.len == 4u && rd32(w.pay) == HWGX_L2_NONE;
    check(ok, "L2: a frame sealed by the next vblank hook / trace stop carries form NONE, no payload", w.len);
    check(w.off <= g_flen, "L2: file walks cleanly", (long)g_flen);
    /* no line source: no chunks, bit 2 clear (the pre-2.3a layout, byte for byte) */
    fresh(&g_io);
    g_vbl = 10;
    HWGX_SetTrace(0, 1, "nol2");
    send_frame(0); HWGX_FrameEnd(); HWGX_Seal();
    w.off = HWGX_TRACE_HDR;
    check(rd32(g_file + 16) == 11u && walk_to(&w, "R2B ") && walk_next(&w) && strcmp(w.tag, "PLTT") == 0, "L2: no line source -> flags 3, R2B is followed by PLTT as before", rd32(g_file + 16));
    /* frame-end seal mode: the lines would belong to the previous frame, so they are not dumped and bit 2 stays clear */
    fresh(&g_io);
    with_lines();
    g_vbl = 10;
    g_l2_form = HWGX_L2_UNIFORM;
    HWGX_SetSealMode(1);
    HWGX_SetTrace(0, 1, "fe2");
    send_frame(0); HWGX_FrameEnd(); HWGX_Seal();
    w.off = HWGX_TRACE_HDR;
    check(rd32(g_file + 16) == 9u && walk_to(&w, "R2B ") && walk_next(&w) && strcmp(w.tag, "PLTT") == 0, "L2: frame-end seal mode -> no L2 chunks, flags 1", rd32(g_file + 16));
}

static void test_cfg(void)
{
    HwCfg c;
    uint32_t skipped = 0;
    const char *ok = "gxtrace = 900:10\n";
    const char *bad[] = { "gxtrace = 10\n", "gxtrace = 10:0\n", "gxtrace = 10:601\n", "gxtrace = x:5\n", "gxtrace = :5\n", "gxtrace = 99999999:5\n", "gxtrace = 5:\n" };
    unsigned i;

    hwcfg_defaults(&c);
    check(c.gx_trace_count == 0, "default: trace off", c.gx_trace_count);
    check(hwcfg_parse(&c, ok, strlen(ok), &skipped) == 1 && c.gx_trace_from == 900 && c.gx_trace_count == 10 && skipped == 0, "gxtrace = FROM:COUNT parsed", c.gx_trace_from);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        hwcfg_defaults(&c);
        check(hwcfg_parse(&c, bad[i], strlen(bad[i]), &skipped) == 0 && skipped == 1 && c.gx_trace_count == 0, "bad gxtrace value rejected", i);
    }
    hwcfg_defaults(&c);
    check(c.census == 0, "default: census off", c.census);
    check(hwcfg_parse(&c, "census = 1\n", 11, &skipped) == 1 && c.census == 1 && skipped == 0, "census = 1 parsed", c.census);
    check(hwcfg_parse(&c, "census = 0\n", 11, &skipped) == 1 && c.census == 0, "census = 0 parsed", c.census);
    check(hwcfg_parse(&c, "census = 2\n", 11, &skipped) == 0 && skipped == 1 && c.census == 0, "census = 2 rejected", c.census);
    hwcfg_defaults(&c);
    check(c.gx_seal_frame_end == 0, "default: seal at the snapshot point", c.gx_seal_frame_end);
    check(hwcfg_parse(&c, "gxseal = frame-end\n", 19, &skipped) == 1 && c.gx_seal_frame_end == 1, "gxseal = frame-end parsed", c.gx_seal_frame_end);
    check(hwcfg_parse(&c, "gxseal = snapshot\n", 18, &skipped) == 1 && c.gx_seal_frame_end == 0, "gxseal = snapshot parsed", c.gx_seal_frame_end);
    check(hwcfg_parse(&c, "gxseal = later\n", 15, &skipped) == 0 && c.gx_seal_frame_end == 0, "gxseal = later rejected", c.gx_seal_frame_end);
}

int main(void)
{
    test_counts();
    test_block();
    test_canary();
    test_trace();
    test_rdbk();
    test_trace_content();
    test_trace_edges();
    test_seal();
    test_seal_frame_end();
    test_lines2d();
    test_cfg();
    printf("hw_gx_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
