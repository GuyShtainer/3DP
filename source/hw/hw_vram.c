/* hw_vram.c -- the DS VRAM bank model: the remap copy between the physical store and the CPU-visible window arrays, the read-only views, the dirty
 * bitmap. Design and the known LCDC-read divergence: hw_vram.h. Pure C; rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <string.h>

#include "hw_log.h"
#include "hw_vram.h"

/* mapping domains. 0..3 are the CPU windows (HWVRAM_WIN_*), 4..9 the views; NONE / LCDC / ARM7 have no window and no view. */
enum {
    DOM_TEX = HWVRAM_WINDOWS, DOM_TEXPLTT, DOM_BGEXT, DOM_OBJEXT, DOM_SUBBGEXT, DOM_SUBOBJEXT,
    DOM_OVERLAP_END,          /* domains below this one are checked for overlaps */
    DOM_NONE = DOM_OVERLAP_END, DOM_LCDC, DOM_ARM7
};

typedef struct Map {
    uint32_t dom;
    uint32_t start, size;     /* byte range inside the domain (window offset, slot * slot size, ...) */
} Map;

#define CHUNK 0x4000u         /* every window range is a multiple of 16 KB at a 16 KB boundary, so zeroing works in these */
#define DIRTY_PAGES_MAX (HWVRAM_PAGE_WORDS * 32u)

static const uint32_t k_lcdc_off[HWVRAM_BANKS] = { 0x00000u, 0x20000u, 0x40000u, 0x60000u, 0x80000u, 0x90000u, 0x94000u, 0x98000u, 0xA0000u };
static const uint32_t k_size[HWVRAM_BANKS] = { 0x20000u, 0x20000u, 0x20000u, 0x20000u, 0x10000u, 0x4000u, 0x4000u, 0x8000u, 0x4000u };
static const uint32_t k_win_size[HWVRAM_WINDOWS] = { HWVRAM_WIN_MAIN_BG_SIZE, HWVRAM_WIN_MAIN_OBJ_SIZE, HWVRAM_WIN_SUB_BG_SIZE, HWVRAM_WIN_SUB_OBJ_SIZE };

static HwVramMem s_mem;
static int s_ready;
static uint8_t s_cnt[HWVRAM_BANKS];
static Map s_map[HWVRAM_BANKS];
static uint32_t s_dirty[HWVRAM_BANKS][HWVRAM_PAGE_WORDS];
static HwVramStats s_st;
static uint32_t s_frame_calls, s_frame_pages_base;

static Map mk(uint32_t dom, uint32_t start, uint32_t size)
{
    Map m;

    m.dom = dom;
    m.start = start;
    m.size = size;
    return m;
}

/* ---- the decode: GBATEK "DS Memory Control - VRAM" ---------------------------------------------------------------------------------- */

/* A and B: MST 0 LCDC, 1 main BG at 0x20000 * OFS, 2 main OBJ at 0x20000 * OFS.0, 3 texture slot OFS. */
static int dec_ab(uint32_t v, Map *m)
{
    uint32_t mst = v & 3u, ofs = (v >> 3) & 3u;

    switch (mst) {
    case 0: *m = mk(DOM_LCDC, 0, 0); return 0;
    case 1: *m = mk(HWVRAM_WIN_MAIN_BG, 0x20000u * ofs, 0x20000u); return 0;
    case 2: *m = mk(HWVRAM_WIN_MAIN_OBJ, 0x20000u * (ofs & 1u), 0x20000u); return 0;
    default: *m = mk(DOM_TEX, 0x20000u * ofs, 0x20000u); return 0;
    }
}

/* C and D: MST 0 LCDC, 1 main BG at 0x20000 * OFS, 2 ARM7 at 0x20000 * OFS.0, 3 texture slot OFS, 4 sub BG (C) / sub OBJ (D). */
static int dec_cd(uint32_t bank, uint32_t v, Map *m)
{
    uint32_t mst = v & 7u, ofs = (v >> 3) & 3u;

    switch (mst) {
    case 0: *m = mk(DOM_LCDC, 0, 0); return 0;
    case 1: *m = mk(HWVRAM_WIN_MAIN_BG, 0x20000u * ofs, 0x20000u); return 0;
    case 2: *m = mk(DOM_ARM7, 0, 0); return 0;
    case 3: *m = mk(DOM_TEX, 0x20000u * ofs, 0x20000u); return 0;
    case 4: *m = mk(bank == HWVRAM_BANK_C ? HWVRAM_WIN_SUB_BG : HWVRAM_WIN_SUB_OBJ, 0, 0x20000u); return 0;
    default: return -1;
    }
}

/* E (64 KB): MST 0 LCDC, 1 main BG, 2 main OBJ, 3 texture palette slots 0-3, 4 main BG extended palette (32 KB used). */
static int dec_e(uint32_t v, Map *m)
{
    switch (v & 7u) {
    case 0: *m = mk(DOM_LCDC, 0, 0); return 0;
    case 1: *m = mk(HWVRAM_WIN_MAIN_BG, 0, 0x10000u); return 0;
    case 2: *m = mk(HWVRAM_WIN_MAIN_OBJ, 0, 0x10000u); return 0;
    case 3: *m = mk(DOM_TEXPLTT, 0, 0x10000u); return 0;
    case 4: *m = mk(DOM_BGEXT, 0, 0x8000u); return 0;
    default: return -1;
    }
}

/* F and G (16 KB): MST 0 LCDC, 1 main BG, 2 main OBJ (both at 0x4000 * OFS.0 + 0x10000 * OFS.1), 3 texture palette slot OFS.0 + 4 * OFS.1 (0, 1, 4, 5),
   4 main BG extended palette slots (OFS.0 * 2 .. +1), 5 main OBJ extended palette (8 KB used). */
static int dec_fg(uint32_t v, Map *m)
{
    uint32_t ofs = (v >> 3) & 3u;
    uint32_t woff = 0x4000u * (ofs & 1u) + 0x10000u * (ofs >> 1);

    switch (v & 7u) {
    case 0: *m = mk(DOM_LCDC, 0, 0); return 0;
    case 1: *m = mk(HWVRAM_WIN_MAIN_BG, woff, 0x4000u); return 0;
    case 2: *m = mk(HWVRAM_WIN_MAIN_OBJ, woff, 0x4000u); return 0;
    case 3: *m = mk(DOM_TEXPLTT, 0x4000u * ((ofs & 1u) + 4u * (ofs >> 1)), 0x4000u); return 0;
    case 4: *m = mk(DOM_BGEXT, 0x4000u * (ofs & 1u), 0x4000u); return 0;
    case 5: *m = mk(DOM_OBJEXT, 0, 0x2000u); return 0;
    default: return -1;
    }
}

/* H (32 KB): MST 0 LCDC, 1 sub BG at 0x06200000, 2 sub BG extended palette. I (16 KB): 1 sub BG at 0x06208000, 2 sub OBJ, 3 sub OBJ extended palette. */
static int dec_hi(uint32_t bank, uint32_t v, Map *m)
{
    uint32_t mst = v & 3u;

    if (mst == 0u) {
        *m = mk(DOM_LCDC, 0, 0);
        return 0;
    }
    if (bank == HWVRAM_BANK_H) {
        *m = mst == 1u ? mk(HWVRAM_WIN_SUB_BG, 0, 0x8000u) : mk(DOM_SUBBGEXT, 0, 0x8000u);
        return mst == 3u ? -1 : 0;
    }
    switch (mst) {
    case 1: *m = mk(HWVRAM_WIN_SUB_BG, 0x8000u, 0x4000u); return 0;
    case 2: *m = mk(HWVRAM_WIN_SUB_OBJ, 0, 0x4000u); return 0;
    default: *m = mk(DOM_SUBOBJEXT, 0, 0x2000u); return 0;
    }
}

/* value -> mapping. Bit 7 clear = disabled. An MST the hardware does not define leaves the bank unmapped (counted in bad_mst). */
static void decode(uint32_t bank, uint32_t v, Map *m)
{
    int bad;

    assert(bank < HWVRAM_BANKS && m != NULL);
    *m = mk(DOM_NONE, 0, 0);
    if ((v & 0x80u) == 0u) {
        return;
    }
    if (bank <= HWVRAM_BANK_B) {
        bad = dec_ab(v, m);
    } else if (bank <= HWVRAM_BANK_D) {
        bad = dec_cd(bank, v, m);
    } else if (bank == HWVRAM_BANK_E) {
        bad = dec_e(v, m);
    } else if (bank <= HWVRAM_BANK_G) {
        bad = dec_fg(v, m);
    } else {
        bad = dec_hi(bank, v, m);
    }
    if (bad != 0) {
        *m = mk(DOM_NONE, 0, 0);
        s_st.bad_mst++;
    }
    assert(m->dom >= HWVRAM_WINDOWS || m->start + m->size <= k_win_size[m->dom]);
}

/* ---- dirty bitmap ------------------------------------------------------------------------------------------------------------------- */

static void mark_pages(uint32_t bank, uint32_t off, uint32_t len)
{
    uint32_t p, first, last;

    assert(bank < HWVRAM_BANKS);
    if (len == 0u || off >= k_size[bank]) {
        return;
    }
    if (len > k_size[bank] - off) {
        len = k_size[bank] - off;
    }
    first = off >> HWVRAM_PAGE_SHIFT;
    last = (off + len - 1u) >> HWVRAM_PAGE_SHIFT;
    assert(last < DIRTY_PAGES_MAX);
    for (p = first; p <= last; p++) {
        uint32_t bit = 1u << (p & 31u);

        if ((s_dirty[bank][p >> 5] & bit) == 0u) {
            s_dirty[bank][p >> 5] |= bit;
            s_st.pages_dirtied++;
        }
    }
}

/* a write to [off, off + len) of domain `dom` (a window): every enabled bank whose mapping covers part of it gets those pages */
static void mark_domain(uint32_t dom, uint32_t off, uint32_t len)
{
    uint32_t b;

    for (b = 0; b < HWVRAM_BANKS; b++) {
        const Map *m = &s_map[b];

        if (m->dom == dom && off < m->start + m->size && off + len > m->start) {
            uint32_t lo = off > m->start ? off : m->start;
            uint32_t hi = off + len < m->start + m->size ? off + len : m->start + m->size;

            mark_pages(b, lo - m->start, hi - lo);
        }
    }
}

/* a write to [off, off + len) of the store: the banks whose LCDC range it touches */
static void mark_store(uint32_t off, uint32_t len)
{
    uint32_t b;

    for (b = 0; b < HWVRAM_BANKS; b++) {
        if (off < k_lcdc_off[b] + k_size[b] && off + len > k_lcdc_off[b]) {
            uint32_t lo = off > k_lcdc_off[b] ? off : k_lcdc_off[b];
            uint32_t hi = off + len < k_lcdc_off[b] + k_size[b] ? off + len : k_lcdc_off[b] + k_size[b];

            mark_pages(b, lo - k_lcdc_off[b], hi - lo);
        }
    }
}

void HWVRAM_Touch(const void *dst, uint32_t bytes)
{
    uintptr_t p = (uintptr_t)dst;
    uint32_t w;

    if (!s_ready || bytes == 0u) {
        return;
    }
    if (p - (uintptr_t)s_mem.store < HWVRAM_STORE_SIZE) {
        uint32_t off = (uint32_t)(p - (uintptr_t)s_mem.store);

        s_st.touches++;
        mark_store(off, bytes > HWVRAM_STORE_SIZE - off ? HWVRAM_STORE_SIZE - off : bytes);
        return;
    }
    for (w = 0; w < HWVRAM_WINDOWS; w++) {
        if (p - (uintptr_t)s_mem.win[w] < k_win_size[w]) {
            uint32_t off = (uint32_t)(p - (uintptr_t)s_mem.win[w]);

            s_st.touches++;
            mark_domain(w, off, bytes > k_win_size[w] - off ? k_win_size[w] - off : bytes);
            return;
        }
    }
}

uint32_t HWVRAM_DirtyPages(uint32_t bank, uint32_t *bits)
{
    uint32_t i, n = 0u;

    assert(bank < HWVRAM_BANKS);
    if (bank >= HWVRAM_BANKS) {
        return 0u;
    }
    for (i = 0; i < HWVRAM_PAGE_WORDS; i++) {
        uint32_t w = s_dirty[bank][i];

        if (bits != NULL) {
            bits[i] = w;
        }
        for (; w != 0u; w &= w - 1u) {
            n++;
        }
        s_dirty[bank][i] = 0u;
    }
    return n;
}

/* ---- the remap copy ----------------------------------------------------------------------------------------------------------------- */

static int is_window(const Map *m)
{
    return m->dom < HWVRAM_WINDOWS;
}

/* is the 16 KB chunk at `start` of window `dom` covered by any enabled bank? */
static int covered(uint32_t dom, uint32_t start)
{
    uint32_t b;

    for (b = 0; b < HWVRAM_BANKS; b++) {
        if (s_map[b].dom == dom && start >= s_map[b].start && start < s_map[b].start + s_map[b].size) {
            return 1;
        }
    }
    return 0;
}

static void overlap_check(uint32_t bank)
{
    uint32_t b;
    const Map *n = &s_map[bank];

    if (n->dom >= DOM_OVERLAP_END) {
        return;
    }
    for (b = 0; b < HWVRAM_BANKS; b++) {
        const Map *o = &s_map[b];

        if (b != bank && o->dom == n->dom && n->start < o->start + o->size && o->start < n->start + n->size) {
            s_st.overlaps++;
            HWLOG("VRAM: banks %c and %c overlap in mapping domain %u at 0x%x (VRAMCNT %02x / %02x): the DS ORs the reads, the model keeps the later load (census finding, Phase 3)",
                  (int)('A' + (int)b), (int)('A' + (int)bank), (unsigned)n->dom, (unsigned)(n->start > o->start ? n->start : o->start), (unsigned)s_cnt[b],
                  (unsigned)s_cnt[bank]);
        }
    }
}

static HwVramObserver s_observer;

void HWVRAM_SetObserver(HwVramObserver fn)
{
    s_observer = fn;
}

void HWVRAM_SetCnt(uint32_t bank, uint32_t value)
{
    Map nm, om;
    uint8_t v = (uint8_t)value;
    uint32_t off;

    assert(bank < HWVRAM_BANKS);
    if (bank >= HWVRAM_BANKS) {
        return;
    }
    s_st.calls++;
    s_frame_calls++;
    if (s_observer != NULL) {
        s_observer(bank, value & 0xffu);
    }
    if (!s_ready) {
        return;
    }
    om = s_map[bank];
    decode(bank, v, &nm);
    if (v == s_cnt[bank] && om.dom == nm.dom && om.start == nm.start && om.size == nm.size) {
        return;   /* a rewrite of the same value: counted above, nothing moves */
    }
    s_st.changed++;
    off = k_lcdc_off[bank];
    if (is_window(&om)) {                                                      /* FLUSH: the CPU may have written through the old window */
        memcpy(s_mem.store + off, s_mem.win[om.dom] + om.start, om.size);
        s_st.flush_bytes += om.size;
    }
    s_cnt[bank] = v;
    s_map[bank] = nm;
    if (is_window(&om)) {                                                      /* ZERO what no enabled bank covers any more */
        uint32_t c;

        for (c = om.start; c < om.start + om.size; c += CHUNK) {
            if (!covered(om.dom, c)) {
                memset(s_mem.win[om.dom] + c, 0, CHUNK);
                s_st.zero_bytes += CHUNK;
            }
        }
    }
    overlap_check(bank);
    if (is_window(&nm)) {                                                      /* LOAD: the store into the new window range */
        memcpy(s_mem.win[nm.dom] + nm.start, s_mem.store + off, nm.size);
        s_st.load_bytes += nm.size;
    }
    mark_pages(bank, 0u, k_size[bank]);                                        /* what a view of this bank shows changed */
}

uint8_t HWVRAM_Cnt(uint32_t bank)
{
    assert(bank < HWVRAM_BANKS);
    return bank < HWVRAM_BANKS ? s_cnt[bank] : 0u;
}

/* ---- views -------------------------------------------------------------------------------------------------------------------------- */

/* the bytes [off, off + len) of domain `dom`, if one enabled bank covers all of them */
static const uint8_t *view(uint32_t dom, uint32_t off, uint32_t len)
{
    uint32_t b;

    if (!s_ready) {
        return NULL;
    }
    for (b = 0; b < HWVRAM_BANKS; b++) {
        const Map *m = &s_map[b];

        if (m->dom == dom && off >= m->start && off + len <= m->start + m->size) {
            return s_mem.store + k_lcdc_off[b] + (off - m->start);
        }
    }
    return NULL;
}

const uint8_t *HWVRAM_TexSlot(uint32_t slot)
{
    return slot < 4u ? view(DOM_TEX, slot * HWVRAM_TEX_SLOT_SIZE, HWVRAM_TEX_SLOT_SIZE) : NULL;
}

const uint8_t *HWVRAM_TexPltt(uint32_t slot)
{
    return slot < 6u ? view(DOM_TEXPLTT, slot * HWVRAM_TEXPLTT_SIZE, HWVRAM_TEXPLTT_SIZE) : NULL;
}

const uint8_t *HWVRAM_BgExtPltt(uint32_t engine, uint32_t slot)
{
    if (engine > 1u || slot > 3u) {
        return NULL;
    }
    return view(engine == 0u ? DOM_BGEXT : DOM_SUBBGEXT, slot * HWVRAM_EXTPLTT_SLOT, HWVRAM_EXTPLTT_SLOT);
}

const uint8_t *HWVRAM_ObjExtPltt(uint32_t engine)
{
    return engine > 1u ? NULL : view(engine == 0u ? DOM_OBJEXT : DOM_SUBOBJEXT, 0u, HWVRAM_EXTPLTT_SLOT);
}

const uint8_t *HWVRAM_Window(uint32_t id)
{
    return (s_ready && id < HWVRAM_WINDOWS) ? s_mem.win[id] : NULL;
}

uint32_t HWVRAM_WindowChunks(uint32_t id)
{
    uint32_t c, m = 0u;

    if (!s_ready || id >= HWVRAM_WINDOWS) {
        return 0u;
    }
    for (c = 0u; c < k_win_size[id] / CHUNK; c++) {
        if (covered(id, c * CHUNK)) {
            m |= 1u << c;
        }
    }
    return m;
}

/* ---- lifecycle and counters --------------------------------------------------------------------------------------------------------- */

void HWVRAM_Init(const HwVramMem *mem)
{
    uint32_t b;

    assert(mem != NULL && mem->store != NULL);
    memset(&s_st, 0, sizeof s_st);
    memset(s_cnt, 0, sizeof s_cnt);
    memset(s_dirty, 0, sizeof s_dirty);
    s_frame_calls = 0u;
    s_frame_pages_base = 0u;
    s_mem = *mem;
    for (b = 0; b < HWVRAM_BANKS; b++) {
        s_map[b] = mk(DOM_NONE, 0, 0);
    }
    memset(s_mem.store, 0, HWVRAM_STORE_SIZE);
    for (b = 0; b < HWVRAM_WINDOWS; b++) {
        assert(s_mem.win[b] != NULL);
        memset(s_mem.win[b], 0, k_win_size[b]);
    }
    s_ready = 1;
}

void HWVRAM_FrameEnd(void)
{
    uint32_t pages = s_st.pages_dirtied - s_frame_pages_base;

    s_st.frames++;
    s_st.calls_last = s_frame_calls;
    if (s_frame_calls > s_st.calls_peak) {
        s_st.calls_peak = s_frame_calls;
    }
    if (s_frame_calls != 0u) {
        s_st.frames_with_calls++;
    }
    s_st.dirty_last = pages;
    if (pages > s_st.dirty_peak) {
        s_st.dirty_peak = pages;
    }
    s_frame_calls = 0u;
    s_frame_pages_base = s_st.pages_dirtied;
}

void HWVRAM_Stats(HwVramStats *out)
{
    assert(out != NULL);
    *out = s_st;
}
