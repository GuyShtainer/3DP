/* hw_gx.c -- the geometry engine's input layer: counts and orders every word the game sends to the GX FIFO / command ports, keeps a bounded
 * trace, and proves nothing slipped past the hooks (the canary). Format and rationale: hw_gx.h and the private design notes. Pure C. */
#include <assert.h>
#include <string.h>

#include "hw_gx.h"

#define CANARY_BASE   0xc4a70000u   /* word i of 0x400..0x5FF holds CANARY_BASE | i */
#define WORDS_MAX     0x100000u     /* one bulk send: a hard cap (the largest display list is a few thousand words) */
#define TS_OFF        0u
#define TS_WAIT       1u
#define TS_REC        2u
#define TS_DONE       3u
#define TS_FAIL       4u
#define NO_OFF        0xffffffffu

typedef struct Ent {
    uint32_t word;
    uint16_t addr;
    uint8_t path;
    uint8_t pad;
} Ent;
_Static_assert(sizeof(Ent) == HWGX_TRACE_ENTRY, "trace entry is 8 bytes");

typedef struct GxState {
    uint8_t *regs;
    const uint8_t *vram;
    uint32_t vram_bytes;
    HwGxIo io;
    int have_io;
    HwGxStats st;
    uint32_t cur[HWGX_PATHS];      /* words in the frame being collected */
    uint32_t cur_rdbk, sp_rdbk;    /* RDBK entries (path 4) of the open frame / of the frame pending its seal (bet 2.4a) */
    uint32_t shadow[HWGX_CANARY_WORDS];
    uint32_t base[HWGX_CANARY_WORDS];   /* what each canary word must hold between slices: the pattern */
    uint32_t spu[HWGX_CANARY_WORDS];    /* the ARM7's own view of the same bytes (its SPU registers), swapped in for the length of a sound slice */
    uint32_t t_from, t_count, t_start;   /* window as configured; the frame the recording really began at */
    char t_path[96];
    void *t_file;
    uint32_t cur_entries_mark;           /* trace_entries when the open frame began */
    uint32_t sp_pending;                 /* 1: the stream of a frame was closed at the vblank hook, its state chunks await the snapshot point (bet 2.2c, B3) */
    uint32_t seal_fe;                    /* B3 A/B control: seal at the vblank hook (pre-2.2c) */
    uint32_t sp_close;                   /* the pending frame is the window's last: close the file after sealing it */
    uint32_t sp_vb, sp_next, sp_entries, sp_cnt[HWGX_PATHS];   /* saved at the hook: the NEXT frame's event key and number, the pending frame's entry counts */
    uint32_t region_base[HWGX_REGIONS];  /* first index of each page region in page_hash */
    uint64_t *page_hash;                 /* HWGX_TOTAL_PAGES hashes of every page as the reader last received it; io.alloc'd when a window is armed (never bss) */
    uint32_t nbuf;
    Ent buf[HWGX_TRACE_BUF];
} GxState;

static GxState s_gx;
static HwGxBlockObserver s_block_observer;
static HwGxObserver s_observer;   /* outside s_gx: HWGX_Init clears s_gx */
static HwGxObserver s_feed;
static HwGxBlockObserver s_feed_block;
static HwGxSnap s_snap;           /* ditto: set once by hw_sys.c */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t load32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof v);
    return v;
}

/* ---- trace (version 2: hw_gx.h has the byte layout) ---------------------------------------------------------------------------- */
#define HASH_SEED 0xcbf29ce484222325ull
#define HASH_PRIME 0x100000001b3ull
#define PAGE_WORDS (HWGX_PAGE_BYTES / 4u)
#define STAGE_PAGES 8u   /* pages gathered into one append */

static const uint16_t k_region_pages[HWGX_REGIONS] = {
    128, 128, 128, 128,                 /* 0..3   TEX slots 0-3 */
    16, 16, 16, 16, 16, 16,             /* 4..9   TEXPLTT slots 0-5 */
    0, 0, 0, 0, 0, 0,                   /* 10..15 unused */
    512, 256, 128, 128,                 /* 16..19 main BG, main OBJ, sub BG, sub OBJ windows */
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8        /* 20..29 main BG ext palette slots 0-3, sub BG slots 0-3, main OBJ ext, sub OBJ ext */
};

static int t_write(const void *buf, uint32_t len)
{
    int n;

    assert(s_gx.have_io && s_gx.t_file != NULL);
    n = s_gx.io.append(s_gx.t_file, buf, len);
    if (n < 0 || (uint32_t)n != len) {
        s_gx.st.trace_state = TS_FAIL;
        return -1;
    }
    s_gx.st.trace_bytes += len;
    return 0;
}

static int t_chunk(const char *tag, uint32_t len)
{
    uint8_t h[8];

    memcpy(h, tag, 4);
    put_le32(h + 4, len);
    return t_write(h, sizeof h);
}

/* a chunk with a payload that is already in memory */
static int t_chunk_data(const char *tag, const void *p, uint32_t len)
{
    return t_chunk(tag, len) == 0 && (len == 0u || t_write(p, len) == 0) ? 0 : -1;
}

/* the buffered stream entries as one STRM chunk */
static void t_flush(void)
{
    if (s_gx.st.trace_state == TS_REC && s_gx.nbuf > 0u) {
        (void)t_chunk_data("STRM", s_gx.buf, s_gx.nbuf * HWGX_TRACE_ENTRY);
    }
    s_gx.nbuf = 0u;
}

static void t_push_pad(uint32_t word, uint32_t addr, uint32_t path, uint32_t pad)
{
    Ent *e;

    if (s_gx.sp_pending && s_gx.nbuf + 1u >= HWGX_TRACE_BUF) {
        /* between the vblank hook and the snapshot point the buffer cannot be flushed (a STRM chunk there would sit inside the pending frame's state chunks): a full buffer
           in that gap fails the trace loudly instead of misordering it. The gap holds the IRQ handler and the game's vblank callback: a few words at most. */
        s_gx.st.trace_state = TS_FAIL;
        return;
    }
    assert(s_gx.nbuf < HWGX_TRACE_BUF);
    e = &s_gx.buf[s_gx.nbuf++];
    e->word = word;
    e->addr = (uint16_t)addr;
    e->path = (uint8_t)path;
    e->pad = (uint8_t)pad;
    if (s_gx.nbuf == HWGX_TRACE_BUF) {
        t_flush();
    }
}

static void t_push(uint32_t word, uint32_t addr, uint32_t path)
{
    t_push_pad(word, addr, path, 0u);
}

/* Words buffered when the window closes belong to a frame that is not recorded (the game's vblank callback and the rest of the loop after the last sealed frame): drop them
   from the file AND from the trace counters, so the counters keep matching the file. */
static void t_discard(void)
{
    uint32_t i;

    for (i = 0u; i < s_gx.nbuf; i++) {
        if (s_gx.buf[i].path == HWGX_PATH_RDBK) {
            if (s_gx.cur_rdbk > 0u) {
                s_gx.cur_rdbk--;
            }
            continue;
        }
        if (s_gx.st.trace_entries > 0u) {
            s_gx.st.trace_entries--;
        }
        if (s_gx.st.trace_got[s_gx.buf[i].path & 3u] > 0u) {
            s_gx.st.trace_got[s_gx.buf[i].path & 3u]--;
        }
    }
    s_gx.nbuf = 0u;
}

static void t_close(uint32_t state)
{
    t_discard();
    if (s_gx.t_file != NULL) {
        s_gx.io.close(s_gx.t_file);
        s_gx.t_file = NULL;
    }
    if (s_gx.st.trace_state != TS_FAIL) {
        s_gx.st.trace_state = state;
    }
}

static int t_header(uint32_t start)
{
    uint8_t h[HWGX_TRACE_HDR];

    memset(h, 0, sizeof h);
    memcpy(h, "GXTR", 4);
    put_le32(h + 4, HWGX_TRACE_VERSION);
    put_le32(h + 8, start);
    put_le32(h + 12, s_gx.t_count);
    put_le32(h + 16, (s_snap.clock_frame != 0u ? HWGX_FLAG_CLOCK_FRAME : 0u) | (s_gx.seal_fe != 0u ? 0u : HWGX_FLAG_SEAL_SNAPSHOT) |
                         (s_gx.seal_fe == 0u && s_snap.lines != NULL ? HWGX_FLAG_LINES2D : 0u) | HWGX_FLAG_RDBK);
    put_le32(h + 20, HWGX_TRACE_ENTRY);
    put_le32(h + 24, HWGX_TRACE_REGS_OFF);
    put_le32(h + 28, HWGX_TRACE_REGS_LEN);
    put_le32(h + 32, s_gx.vram != NULL ? s_gx.vram_bytes : 0u);
    put_le32(h + 36, HWGX_PAGE_BYTES);
    put_le32(h + 40, s_snap.vblanks != NULL ? s_snap.vblanks() : 0u);
    return t_write(h, sizeof h);
}

/* the frame's opening chunk: its event key (absolute frame, DS vblank count) and its place in the window */
static int t_frame_begin(uint32_t frame, uint32_t vblanks)
{
    uint8_t b[32];

    memset(b, 0, sizeof b);
    put_le32(b + 0, frame);
    put_le32(b + 4, vblanks);
    put_le32(b + 8, s_gx.st.trace_frames);   /* frames of the window closed so far = this frame's index */
    put_le32(b + 12, s_gx.st.trace_frames == 0u ? 1u : 0u);
    return t_chunk_data("FRAM", b, sizeof b);
}

static uint64_t page_hash(const uint8_t *p)
{
    uint64_t h = HASH_SEED;
    uint32_t i;

    for (i = 0u; i < PAGE_WORDS; i++) {
        h = (h ^ load32(p + 4u * i)) * HASH_PRIME;
    }
    return h;
}

/* Dirty = content differs from what the reader holds (initially all zero: an unmapped region reads as zeros, so the first frame dumps exactly the non-zero pages
   of everything mapped). The hash of every page is kept; the 1 KB dirty bitmap of hw_vram.c is not used because plain CPU stores are not hooked (hw_vram.h). */
static int t_pages(uint32_t first_region, uint32_t last_region, uint32_t *pages_out)
{
    static const uint8_t zero[HWGX_PAGE_BYTES];
    uint16_t dirty[512];
    uint8_t stage[STAGE_PAGES * (4u + HWGX_PAGE_BYTES)];
    uint32_t r;

    for (r = first_region; r <= last_region; r++) {
        uint32_t np = k_region_pages[r], i, nd = 0u, ns = 0u, base = s_gx.region_base[r];
        const uint8_t *src;
        uint8_t h8[8];

        if (np == 0u) {
            continue;
        }
        src = s_snap.region != NULL ? s_snap.region(r) : NULL;
        for (i = 0u; i < np; i++) {
            uint64_t h = page_hash(src != NULL ? src + (size_t)i * HWGX_PAGE_BYTES : zero);

            if (h != s_gx.page_hash[base + i]) {
                s_gx.page_hash[base + i] = h;
                dirty[nd++] = (uint16_t)i;
            }
        }
        if (nd == 0u) {
            continue;
        }
        put_le32(h8, r);
        put_le32(h8 + 4, nd);
        if (t_chunk("PAGE", 8u + nd * (4u + HWGX_PAGE_BYTES)) != 0 || t_write(h8, sizeof h8) != 0) {
            return -1;
        }
        for (i = 0u; i < nd; i++) {
            put_le32(stage + ns * (4u + HWGX_PAGE_BYTES), dirty[i]);
            memcpy(stage + ns * (4u + HWGX_PAGE_BYTES) + 4u, src != NULL ? src + (size_t)dirty[i] * HWGX_PAGE_BYTES : zero, HWGX_PAGE_BYTES);
            ns++;
            if (ns == STAGE_PAGES || i + 1u == nd) {
                if (t_write(stage, ns * (4u + HWGX_PAGE_BYTES)) != 0) {
                    return -1;
                }
                ns = 0u;
            }
        }
        *pages_out += nd;
        s_gx.st.trace_pages += nd;
    }
    return 0;
}

/* The frame's state (Rule 1(a) state and Rule 1(b) alignment data), written at the SNAPSHOT POINT (bet 2.2c, B3): 3D registers, VRAMCNT, 3D page regions, both 2D engines'
   registers, the standard palettes, OAM, the 2D page regions, then FEND with the entry counts the frame had at its vblank hook (sp_*) for the parser to check. `kind` goes
   into FEND word 6: HWGX_SEAL_SNAPSHOT (the game passed its snapshot point, after the vblank callback) or HWGX_SEAL_NEXT_VBLANK (it waited again first). */
/* One L2A / L2B chunk: the form word, then the engine's per-line register images when this frame's lines ran (sealed at the snapshot point). */
static int t_lines(const char *tag, uint32_t engine, uint32_t kind)
{
    uint8_t w[4];
    const uint8_t *src = NULL;
    uint32_t form = HWGX_L2_NONE, len = 0u;

    if (kind == HWGX_SEAL_SNAPSHOT && s_snap.lines != NULL) {
        src = s_snap.lines(engine, &form);
        if (src == NULL) {
            form = HWGX_L2_NONE;
        }
    }
    if (form == HWGX_L2_UNIFORM) {
        len = HWGX_REGS2D_LEN;
    } else if (form == HWGX_L2_LINES) {
        len = 192u * HWGX_REGS2D_LEN;
    }
    put_le32(w, form);
    return t_chunk(tag, 4u + len) == 0 && t_write(w, sizeof w) == 0 && (len == 0u || t_write(src, len) == 0) ? 0 : -1;
}

static void t_snapshot(uint32_t kind)
{
    uint8_t vc[16], fe[32];
    static const uint8_t zeros[HWGX_TRACE_REGS_LEN];
    const uint8_t *io = s_gx.regs;
    uint32_t pages = 0u, p;

    memset(vc, 0, sizeof vc);
    if (io != NULL) {
        memcpy(vc, io + 0x240u, 10u);   /* VRAMCNT_A..G, WRAMCNT, VRAMCNT_H, VRAMCNT_I */
    }
    if (t_chunk_data("R3D ", io != NULL ? io + HWGX_TRACE_REGS_OFF : zeros, HWGX_TRACE_REGS_LEN) != 0 || t_chunk_data("VCNT", vc, sizeof vc) != 0 ||
        t_pages(0u, 9u, &pages) != 0 ||
        t_chunk_data("R2A ", io != NULL ? io : zeros, HWGX_REGS2D_LEN) != 0 || t_chunk_data("R2B ", io != NULL ? io + 0x1000u : zeros, HWGX_REGS2D_LEN) != 0 ||
        (s_gx.seal_fe == 0u && s_snap.lines != NULL && (t_lines("L2A ", 0u, kind) != 0 || t_lines("L2B ", 1u, kind) != 0)) ||
        t_chunk_data("PLTT", s_snap.pltt != NULL ? s_snap.pltt : zeros, HWGX_PLTT_LEN) != 0 || t_chunk_data("OAM ", s_snap.oam != NULL ? s_snap.oam : zeros, HWGX_OAM_LEN) != 0 ||
        t_pages(16u, 29u, &pages) != 0) {
        return;
    }
    memset(fe, 0, sizeof fe);
    put_le32(fe, s_gx.sp_entries);
    for (p = 0u; p < HWGX_PATHS; p++) {
        put_le32(fe + 4u + 4u * p, s_gx.sp_cnt[p]);
    }
    put_le32(fe + 20, pages);
    put_le32(fe + 24, kind);
    put_le32(fe + 28, s_gx.sp_rdbk);
    (void)t_chunk_data("FEND", fe, sizeof fe);
}

static void (*s_open_hook)(void);

static void t_open(uint32_t start)
{
    assert(s_gx.st.trace_state == TS_WAIT);
    s_gx.t_file = s_gx.have_io ? s_gx.io.create(s_gx.t_path) : NULL;
    if (s_gx.t_file == NULL) {
        s_gx.st.trace_state = TS_FAIL;
        return;
    }
    s_gx.t_start = start;
    s_gx.st.trace_state = TS_REC;
    s_gx.cur_entries_mark = s_gx.st.trace_entries;
    if (t_header(start) != 0 || t_frame_begin(start, s_snap.vblanks != NULL ? s_snap.vblanks() : 0u) != 0) {
        t_close(TS_FAIL);
        return;
    }
    if (s_open_hook != NULL) {
        s_open_hook();
    }
}

void HWGX_SetTrace(uint32_t from_frame, uint32_t count, const char *path)
{
    size_t n;
    uint32_t r, base = 0u;

    assert(path != NULL);
    n = strlen(path);
    if (count == 0u || !s_gx.have_io || n >= sizeof s_gx.t_path) {
        s_gx.st.trace_state = TS_OFF;
        return;
    }
    if (s_gx.page_hash == NULL && s_gx.io.alloc != NULL) {
        s_gx.page_hash = (uint64_t *)s_gx.io.alloc(HWGX_TOTAL_PAGES * (uint32_t)sizeof(uint64_t));
    }
    if (s_gx.page_hash == NULL) {
        s_gx.st.trace_state = TS_FAIL;   /* no memory for the dirty-page hashes: no trace, the counters go on */
        return;
    }
    memcpy(s_gx.t_path, path, n + 1u);
    s_gx.t_from = from_frame;
    s_gx.t_count = count;
    for (r = 0u; r < HWGX_REGIONS; r++) {   /* every page starts as "the reader holds zeros" */
        s_gx.region_base[r] = base;
        base += k_region_pages[r];
    }
    {
        static const uint8_t zero[HWGX_PAGE_BYTES];
        uint64_t hz = page_hash(zero);
        uint32_t i;

        for (i = 0u; i < base; i++) {
            s_gx.page_hash[i] = hz;
        }
    }
    s_gx.st.trace_state = TS_WAIT;
    if (from_frame <= s_gx.st.frames) {
        t_open(s_gx.st.frames);
    }
}

/* ---- recording --------------------------------------------------------------------------------------------------------------- */
static void count_word(uint32_t path, uint32_t addr, uint32_t word)
{
    s_gx.cur[path]++;
    if (s_feed != NULL) {
        s_feed(addr, word);
    }
    if (s_observer != NULL) {
        s_observer(addr, word);
    }
    if (s_gx.st.trace_state == TS_REC) {
        t_push(word, addr, path);
        s_gx.st.trace_entries++;
        s_gx.st.trace_got[path]++;
    }
}

void HWGX_FifoWord(uint32_t word, uint32_t path)
{
    if (path >= HWGX_PATHS) {
        s_gx.st.stray++;
        return;
    }
    count_word(path, 0u, word);
}

static void port_store(uint32_t off, uint32_t val, uint32_t path)
{
    assert(off >= HWGX_FIFO_END && off < HWGX_PORT_END && path < HWGX_PATHS);
    s_gx.shadow[(off - HWGX_FIFO_OFF) / 4u] = val;
    count_word(path, off, val);
}

static void any_store(uint32_t off, uint32_t val, uint32_t path)
{
    off &= ~3u;
    if (off >= HWGX_FIFO_OFF && off < HWGX_FIFO_END) {
        count_word(path, 0u, val);
    } else if (off >= HWGX_FIFO_END && off < HWGX_PORT_END) {
        port_store(off, val, path);
    } else {
        s_gx.st.stray++;
    }
}

void HWGX_Port(uint32_t off, uint32_t val)
{
    any_store(off, val, HWGX_PATH_PORT);
}

uint32_t HWGX_PortLast(uint32_t off)
{
    off &= ~3u;
    if (off < HWGX_FIFO_END || off >= HWGX_PORT_END) {
        return 0u;
    }
    return s_gx.shadow[(off - HWGX_FIFO_OFF) / 4u];
}

/* ---- the SDK-facing hooks (hw_dev_hooks.h) ------------------------------------------------------------------------------------ */
void HWG3_Port(uint32_t off, uint32_t val)
{
    HWGX_Port(off, val);
}

/* Bet 2.4a: the game just READ `words` registers starting at I/O offset `off` (GXSTAT, POS/VEC_RESULT, CLIPMTX, VECMTX) and got `vals`. Trace-only: one head entry
   { word = count, addr = off, path = RDBK, pad = 1 } and then, per value, { word = value, addr = off + 4 * i, path = RDBK, pad = 2 }, at the exact stream position. Nothing is
   stored into the I/O block and no counter the port runs on changes. */
void HWG3_Readback(uint32_t off, uint32_t words, const void *vals)
{
    const uint8_t *v = (const uint8_t *)vals;
    uint32_t i;

    assert(words >= 1u && words <= HWGX_RDBK_MAX);
    if (s_gx.st.trace_state != TS_REC || vals == NULL || words == 0u || words > HWGX_RDBK_MAX || off >= 0x1000u) {
        return;
    }
    t_push_pad(words, off, HWGX_PATH_RDBK, 1u);
    for (i = 0u; i < words; i++) {
        t_push_pad(load32(v + 4u * i), off + 4u * i, HWGX_PATH_RDBK, 2u);
    }
    s_gx.cur_rdbk += words + 1u;
}

/* Slice 2.4c: the game WROTE GXSTAT (off 0x600) or DISP3DCNT (off 0x060); `value` is what the port's engine applied (hw_devs.c HWG3_StatWrite). Trace-only, one entry
   { word = value, addr = off, path = RDBK, pad = 3 } at the exact stream position, so a replay applies the same write (the FIFO-interrupt bits 30..31 of GXSTAT, the
   write-1-to-clear flags). Counted as an RDBK entry (FEND word 7); no counter the port runs on changes. */
void HWGX_StatTrace(uint32_t off, uint32_t value)
{
    if (s_gx.st.trace_state != TS_REC) {
        return;
    }
    t_push_pad(value, off, HWGX_PATH_RDBK, 3u);
    s_gx.cur_rdbk += 1u;
}

/* called right after a trace window opens (the engine seeds the replay with the GXSTAT bits the game set before the window) */
void HWGX_SetTraceOpenHook(void (*fn)(void))
{
    s_open_hook = fn;
}

void HWG3_Words(uint32_t off, const void *src, uint32_t words, uint32_t mode)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t path = mode == HWGX_MODE_FIFO ? HWGX_PATH_SENDFIFO : HWGX_PATH_CPUSEND;
    uint32_t i;

    assert(src != NULL || words == 0u);
    assert(mode <= HWGX_MODE_FIFO);
    if (words > WORDS_MAX) {
        s_gx.st.stray += words;
        return;
    }
    for (i = 0u; i < words; i++) {
        any_store(mode == HWGX_MODE_COPY ? off + 4u * i : off, load32(s + 4u * i), path);
    }
}

/* ---- frame end, canary, stats ------------------------------------------------------------------------------------------------ */
static uint32_t canary_word(uint32_t i)
{
    return CANARY_BASE | i;
}

void HWGX_ArmCanary(void)
{
    uint32_t i;

    if (s_gx.regs == NULL) {
        return;
    }
    for (i = 0u; i < HWGX_CANARY_WORDS; i++) {
        s_gx.base[i] = canary_word(i);
        s_gx.spu[i] = 0u;   /* a reset clears the SPU registers too */
        put_le32(s_gx.regs + HWGX_FIFO_OFF + 4u * i, s_gx.base[i]);
    }
}

static void canary_check(void)
{
    uint32_t i, bad = 0u;

    if (s_gx.regs == NULL) {
        return;
    }
    for (i = 0u; i < HWGX_CANARY_WORDS; i++) {
        uint32_t v = load32(s_gx.regs + HWGX_FIFO_OFF + 4u * i);

        if (v != s_gx.base[i]) {
            if (s_gx.st.canary_words == 0u && bad == 0u) {
                s_gx.st.canary_first_off = HWGX_FIFO_OFF + 4u * i;
                s_gx.st.canary_first_val = v;
            }
            bad++;
            {
                uint32_t k, off = HWGX_FIFO_OFF + 4u * i;

                for (k = 0u; k < s_gx.st.canary_noffs && s_gx.st.canary_offs[k] != off; k++) {
                }
                if (k == s_gx.st.canary_noffs && k < 8u) {
                    s_gx.st.canary_offs[k] = off;
                    s_gx.st.canary_vals[k] = v;
                    s_gx.st.canary_noffs++;
                }
            }
            put_le32(s_gx.regs + HWGX_FIFO_OFF + 4u * i, s_gx.base[i]);   /* re-arm: the next frame counts only NEW stores */
        }
    }
    if (bad != 0u) {
        s_gx.st.canary_frames++;
        s_gx.st.canary_words += bad;
    }
}

/* The ARM7 sound driver runs on this CPU and shares the I/O block, but a real ARM7 is another bus: its SPU registers (0x400..0x51F there) are not the
   ARM9's GX FIFO and command ports at the same addresses. So its slice gets a view of its own: the canary is checked as the slice begins (an unhooked
   ARM9 store before it is still a hit), the SPU words are swapped in (they read 0 until the driver writes them, exactly as on the DS, not the pattern),
   and swapped out again when it ends. */
void HWGX_SliceBegin(void)
{
    uint32_t i, before = s_gx.st.canary_words;

    canary_check();
    s_gx.st.canary_at_slice += s_gx.st.canary_words - before;
    if (s_gx.regs == NULL) {
        return;
    }
    for (i = 0u; i < HWGX_CANARY_WORDS; i++) {
        put_le32(s_gx.regs + HWGX_FIFO_OFF + 4u * i, s_gx.spu[i]);
    }
}

void HWGX_SliceEnd(void)
{
    uint32_t i;

    if (s_gx.regs == NULL) {
        return;
    }
    for (i = 0u; i < HWGX_CANARY_WORDS; i++) {
        s_gx.spu[i] = load32(s_gx.regs + HWGX_FIFO_OFF + 4u * i);
        put_le32(s_gx.regs + HWGX_FIFO_OFF + 4u * i, s_gx.base[i]);
    }
}

static void account_frame(void)
{
    uint32_t p;

    for (p = 0u; p < HWGX_PATHS; p++) {
        s_gx.st.total[p] += s_gx.cur[p];
        s_gx.st.last[p] = s_gx.cur[p];
        if (s_gx.cur[p] > s_gx.st.peak[p]) {
            s_gx.st.peak[p] = s_gx.cur[p];
        }
        if (s_gx.st.trace_state == TS_REC) {
            s_gx.st.trace_expect[p] += s_gx.cur[p];
        }
        s_gx.cur[p] = 0u;
    }
    if (s_gx.st.trace_state == TS_REC) {
        s_gx.st.trace_frames++;
    }
}

/* Seal the pending frame: its state chunks + FEND, then either close the window or open the next frame (FRAM with the key saved at the hook). Idempotent. */
static void seal(uint32_t kind)
{
    if (!s_gx.sp_pending) {
        return;
    }
    s_gx.sp_pending = 0u;
    if (s_gx.st.trace_state != TS_REC) {
        return;
    }
    t_snapshot(kind);
    if (kind == HWGX_SEAL_SNAPSHOT) {
        s_gx.st.seals_snapshot++;
    } else {
        s_gx.st.seals_next_vblank++;
    }
    if (s_gx.st.trace_state == TS_REC && s_gx.sp_close) {
        t_close(TS_DONE);
    } else if (s_gx.st.trace_state == TS_REC && t_frame_begin(s_gx.sp_next, s_gx.sp_vb) != 0) {
        t_close(TS_FAIL);
    }
    s_gx.sp_close = 0u;
    if (s_gx.st.trace_state == TS_FAIL && s_gx.t_file != NULL) {
        t_close(TS_FAIL);
    }
}

void HWGX_Seal(void)
{
    if (s_gx.seal_fe == 0u) {
        seal(HWGX_SEAL_SNAPSHOT);
    }
}

void HWGX_SetSealMode(int frame_end)
{
    s_gx.seal_fe = frame_end != 0 ? 1u : 0u;
}

/* The vblank hook (the host wait, before the pump raises the DS vblank). The recorded frame's STREAM ends here. Its STATE is sealed later, at the game's snapshot point
   (HWGX_Seal, after the vblank callback: what the DS renders at line 215 sees the vblank handler's and callback's uploads), or at the next hook if the game never gets there. */
void HWGX_FrameEnd(void)
{
    uint32_t next, p;

    canary_check();
    seal(HWGX_SEAL_NEXT_VBLANK);   /* the previous frame is still pending: the game waited again before its snapshot point */
    if (s_gx.st.trace_state == TS_REC) {
        t_flush();
        s_gx.sp_entries = s_gx.st.trace_entries - s_gx.cur_entries_mark;
        s_gx.sp_rdbk = s_gx.cur_rdbk;
        s_gx.cur_rdbk = 0u;
        for (p = 0u; p < HWGX_PATHS; p++) {
            s_gx.sp_cnt[p] = s_gx.cur[p];
        }
        s_gx.sp_vb = s_snap.vblanks != NULL ? s_snap.vblanks() : 0u;
        s_gx.cur_entries_mark = s_gx.st.trace_entries;   /* words from here on are the next frame's */
        s_gx.sp_pending = 1u;
    }
    account_frame();    /* closes the frame's counters; trace_frames++ while recording */
    s_gx.st.frames++;
    next = s_gx.st.frames;
    if (s_gx.sp_pending) {
        s_gx.sp_next = next;
        s_gx.sp_close = next >= s_gx.t_start + s_gx.t_count ? 1u : 0u;
    } else if (s_gx.st.trace_state == TS_WAIT && next >= s_gx.t_from) {
        t_open(next);
    }
    if (s_gx.seal_fe != 0u) {
        seal(0u);   /* frame-end mode: the state is sealed right here (kind 0) */
    }
    if (s_gx.st.trace_state == TS_FAIL && s_gx.t_file != NULL) {
        t_close(TS_FAIL);
    }
}

void HWGX_TraceStop(void)
{
    if (s_gx.st.trace_state == TS_REC) {
        seal(HWGX_SEAL_NEXT_VBLANK);
    }
    if (s_gx.st.trace_state == TS_REC) {
        t_close(TS_DONE);
    }
}

void HWGX_SetSnapshot(const HwGxSnap *s)
{
    s_snap = s != NULL ? *s : (HwGxSnap){ 0 };
}

void HWGX_SetObserver(HwGxObserver fn)
{
    s_observer = fn;
}

void HWGX_SetFeed(HwGxObserver word, HwGxBlockObserver block)
{
    s_feed = word;
    s_feed_block = block;
}

void HWGX_SetBlockObserver(HwGxBlockObserver fn)
{
    s_block_observer = fn;
}

int HWGX_FifoBlock(const uint32_t *w, uint32_t n, uint32_t path)
{
    if ((s_block_observer == NULL && s_feed_block == NULL) || path >= HWGX_PATHS || s_gx.st.trace_state == TS_REC || w == NULL) {
        return 0;
    }
    s_gx.cur[path] += n;
    if (s_feed_block != NULL) {
        s_feed_block(w, n);
    }
    if (s_block_observer != NULL) {
        s_block_observer(w, n);
    }
    return 1;
}

void HWGX_Stats(HwGxStats *out)
{
    uint32_t p;

    assert(out != NULL);
    *out = s_gx.st;
    for (p = 0u; p < HWGX_PATHS; p++) {
        out->total[p] += s_gx.cur[p];
    }
}

void HWGX_Init(uint8_t *regs, const uint8_t *vram, uint32_t vram_bytes, const HwGxIo *io)
{
    if (s_gx.t_file != NULL && s_gx.have_io) {
        s_gx.io.close(s_gx.t_file);
    }
    memset(&s_gx, 0, sizeof s_gx);   /* HWGX_Init runs once per process (hw_sys_init); a second call would drop the io.alloc'd hash table (host tests only) */
    s_gx.regs = regs;
    s_gx.vram = vram;
    s_gx.vram_bytes = vram_bytes;
    if (io != NULL) {
        assert(io->create != NULL && io->append != NULL && io->close != NULL);
        s_gx.io = *io;
        s_gx.have_io = 1;
    }
    s_gx.st.canary_first_off = NO_OFF;
    HWGX_ArmCanary();
}
