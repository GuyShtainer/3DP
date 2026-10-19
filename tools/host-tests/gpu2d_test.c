/* gpu2d_test.c -- bet 2.3a: the per-line 2D record (gpu2d.c) and the pump's HBlank dispatch (hw_pump.c, hwpump_hblank_frame) over a fake host. Native, one thread, no SDK.
 * The synthetic scenario of the bet row: DISPSTAT bit 4 + IE bit 1 enabled, a registered HBlank callback that writes BG1HOFS = line; the snapshot of line n holds n for all 192
 * lines (both engines), the lines ran after the previous vblank callback (an ordered event log), DISPSTAT bit 1 reads set inside the callback and clear outside, VCOUNT reads n
 * inside, the short-cut is taken (and counted) when no HBlank IRQ is enabled, and the record's VRAM views follow a VRAMCNT change made by the vblank callback (bank H sub BG ->
 * EXTPLTT, the 2.1(b) case). Prints "gpu2d_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu2d.h"
#include "hw_dev_hooks.h"
#include "hw_log.h"
#include "hw_pure.h"
#include "hw_vram.h"

static long g_checks, g_fails;

static void check(int ok, const char *what, long v)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL: %s (%ld)\n", what, v);
    }
}

void hwlog_emit(HwLogSite *site, const char *fmt, ...)
{
    (void)fmt;
    site->count++;
}

#define IO_BYTES 0x2000u
#define BG1HOFS  0x14u   /* BG1HOFS of an engine's image */
#define EV_MAX   2048u

typedef enum { EV_VBLANK_IRQ = 1, EV_VB_CALLBACK, EV_SNAPPOINT, EV_HBLANK, EV_SNAP } EvKind;

typedef struct Ev {
    uint8_t kind;
    uint16_t arg;
} Ev;

typedef struct Host {
    uint8_t *io;
    Gpu2dFrame *rec;
    Ev ev[EV_MAX];
    uint32_t nev;
    uint32_t hb_calls, hb_bit1_set_inside, hb_vcount_ok, hb_bit0_clear_inside, vblank_irqs, hb_handler;
    uint32_t snap_calls, snap_lines, snap_bit1_after_clear_ok;
    int masked;
} Host;

static uint16_t r16(const uint8_t *io, uint32_t off) { uint16_t v; memcpy(&v, io + off, 2); return v; }
static uint32_t r32(const uint8_t *io, uint32_t off) { uint32_t v; memcpy(&v, io + off, 4); return v; }
static void w16(uint8_t *io, uint32_t off, uint16_t v) { memcpy(io + off, &v, 2); }
static void w32(uint8_t *io, uint32_t off, uint32_t v) { memcpy(io + off, &v, 4); }

static void log_ev(Host *h, EvKind k, uint32_t arg)
{
    assert(h->nev < EV_MAX);
    h->ev[h->nev].kind = (uint8_t)k;
    h->ev[h->nev].arg = (uint16_t)arg;
    h->nev++;
}

static uint64_t h_tick(void *u) { (void)u; return 0; }
static void h_wait(void *u) { (void)u; }
static void h_sleep(void *u, uint64_t sys) { (void)u; (void)sys; }
static int h_masked(void *u) { return ((Host *)u)->masked; }

/* OS_IRQTable[irq]: irq 0 = the vblank handler, irq 1 = the registered HBlank callback (writes BG1HOFS = line, in both engines) */
static void h_disp(void *u, int irq)
{
    Host *h = (Host *)u;

    if (irq == (int)HWIRQ_VBLANK) {
        h->vblank_irqs++;
        log_ev(h, EV_VBLANK_IRQ, 0);
        return;
    }
    if (irq == (int)HWIRQ_HBLANK) {
        uint16_t line = r16(h->io, HWR_VCOUNT), ds = r16(h->io, HWR_DISPSTAT);

        h->hb_calls++;
        h->hb_bit1_set_inside += (ds & 2u) ? 1u : 0u;
        h->hb_bit0_clear_inside += (ds & 1u) ? 0u : 1u;
        h->hb_vcount_ok += (line == h->hb_calls - 1u) ? 1u : 0u;
        w16(h->io, BG1HOFS, line);                    /* engine A */
        w16(h->io, GPU2D_B_BASE + BG1HOFS, (uint16_t)(line + 1000u));   /* engine B */
        log_ev(h, EV_HBLANK, line);
    }
}

static void l_snap(void *u, uint32_t first, uint32_t count)
{
    Host *h = (Host *)u;

    h->snap_calls++;
    h->snap_lines += count;
    log_ev(h, EV_SNAP, count == 1u ? first : 0xffffu);
    gpu2d_frame_lines(h->rec, h->io, first, count);
}

static void setup(HwPump *p, Host *h, uint8_t *io, uint32_t *vb, uint16_t dispstat, uint32_t ie)
{
    HwPumpOps ops = { h_tick, h_wait, h_sleep, NULL, h_disp, h_masked, h, NULL, NULL };

    memset(io, 0, IO_BYTES);
    *vb = 0;
    memset(h->ev, 0, sizeof h->ev);
    h->nev = h->hb_calls = h->hb_bit1_set_inside = h->hb_vcount_ok = h->hb_bit0_clear_inside = h->vblank_irqs = h->snap_calls = h->snap_lines = 0;
    h->io = io;
    h->masked = 0;
    hwpump_init(p, &ops, io, vb);
    hwpump_set_clock(p, HWCLOCK_FRAME);
    w32(io, HWR_IME, 1);
    w32(io, HWR_IE, ie);
    w16(io, HWR_DISPSTAT, dispstat);
}

/* ---- the VRAM model behind the views ---- */
static uint8_t g_store[HWVRAM_STORE_SIZE];
static uint8_t g_bg[HWVRAM_WIN_MAIN_BG_SIZE], g_obj[HWVRAM_WIN_MAIN_OBJ_SIZE], g_sbg[HWVRAM_WIN_SUB_BG_SIZE], g_sobj[HWVRAM_WIN_SUB_OBJ_SIZE];
static uint8_t g_pltt[2048], g_oam[2048];

static void vram_fresh(void)
{
    HwVramMem mem;

    memset(g_store, 0, sizeof g_store);
    memset(g_bg, 0, sizeof g_bg);
    memset(g_obj, 0, sizeof g_obj);
    memset(g_sbg, 0, sizeof g_sbg);
    memset(g_sobj, 0, sizeof g_sobj);
    mem.store = g_store;
    mem.win[HWVRAM_WIN_MAIN_BG] = g_bg;
    mem.win[HWVRAM_WIN_MAIN_OBJ] = g_obj;
    mem.win[HWVRAM_WIN_SUB_BG] = g_sbg;
    mem.win[HWVRAM_WIN_SUB_OBJ] = g_sobj;
    HWVRAM_Init(&mem);
}

/* what the SDK's HWVRAM_STORE macro does: the register byte, then the hook */
static void set_cnt(uint8_t *io, uint32_t bank, uint8_t value)
{
    static const uint32_t off[HWVRAM_BANKS] = { 0x240, 0x241, 0x242, 0x243, 0x244, 0x245, 0x246, 0x248, 0x249 };

    io[off[bank]] = value;
    HWVRAM_SetCnt(bank, value);
}

static void bind(Gpu2dFrame *f, const uint8_t *io)
{
    Gpu2dSrc src;

    src.io = io;
    src.pltt = g_pltt;
    src.oam = g_oam;
    src.window = HWVRAM_Window;
    src.bg_ext = HWVRAM_BgExtPltt;
    src.obj_ext = HWVRAM_ObjExtPltt;
    src.window_chunks = HWVRAM_WindowChunks;
    gpu2d_frame_begin(f, &src);
}

/* One game frame as the main loop runs it: the waiting yield raises the vblank (the handler runs), the game's vblank callback, then the snapshot point = bind the record +
   the 192 HBlank lines (hw_devs.c hblank_frame). `callback` is the vblank callback body. */
static uint32_t game_frame(HwPump *p, Host *h, void (*callback)(Host *), const HwPumpLineOps *lo)
{
    uint32_t ran;

    hwpump_run(p, HWP_WAIT);
    log_ev(h, EV_VB_CALLBACK, 0);
    if (callback != NULL) {
        callback(h);
    }
    log_ev(h, EV_SNAPPOINT, 0);
    bind(h->rec, h->io);
    ran = hwpump_hblank_frame(p, lo);
    return ran;
}

static void cb_poke(Host *h)
{
    w16(h->io, BG1HOFS, 0x7777u);                   /* a stale value the lines must overwrite */
    w16(h->io, GPU2D_B_BASE + BG1HOFS, 0x7777u);
    w16(h->io, 0x4u, 0x0010u | 0x0008u);            /* DISPSTAT is also written by the game; the lines must keep their own bits */
}

static Gpu2dFrame g_rec;
static Host g_host;
static uint8_t g_io[IO_BYTES];

static void test_full_dispatch(void)
{
    HwPump p;
    uint32_t vb, ran, n, ev_vbcb = 0, ev_sp = 0, first_hb = 0, last_hb = 0, ok_all = 1u, ok_b = 1u, inter = 1u;
    const HwPumpLineOps lo = { l_snap, &g_host };

    memset(&g_rec, 0, sizeof g_rec);
    g_host.rec = &g_rec;
    setup(&p, &g_host, g_io, &vb, 0x0018u, 0x3u);   /* DISPSTAT bit 3 (vblank IRQ) + bit 4 (HBlank IRQ), IE bits 0 and 1 */
    vram_fresh();
    ran = game_frame(&p, &g_host, cb_poke, &lo);
    check(ran == 192u && g_host.hb_calls == 192u, "full: 192 HBlank callbacks", g_host.hb_calls);
    check(g_host.vblank_irqs == 1u, "full: the vblank handler ran once (existing dispatch)", g_host.vblank_irqs);
    check(g_host.hb_bit1_set_inside == 192u, "full: DISPSTAT bit 1 reads set inside every callback", g_host.hb_bit1_set_inside);
    check(g_host.hb_bit0_clear_inside == 192u, "full: DISPSTAT bit 0 (vblank flag) reads clear inside every callback", g_host.hb_bit0_clear_inside);
    check(g_host.hb_vcount_ok == 192u, "full: VCOUNT reads n inside callback n", g_host.hb_vcount_ok);
    check((r16(g_io, HWR_DISPSTAT) & 2u) == 0u, "full: DISPSTAT bit 1 clear outside", r16(g_io, HWR_DISPSTAT));
    check((r16(g_io, HWR_DISPSTAT) & 0x18u) == 0x18u, "full: the game's DISPSTAT enable bits (3, 4) survive the lines", r16(g_io, HWR_DISPSTAT));
    check((r32(g_io, HWR_IF) & 2u) == 0u, "full: no HBlank IF bit left behind", r32(g_io, HWR_IF));
    check(p.hb.frames == 1u && p.hb.full == 1u && p.hb.shortcut == 0u && p.hb.lines == 192u && p.hb.irqs == 192u && p.hb.ds_frames == 1u && p.hb.ie_frames == 1u,
          "full: counters (frames/full/shortcut/lines/irqs/ds/ie)", p.hb.irqs);
    check(g_host.snap_calls == 192u && g_host.snap_lines == 192u && g_rec.uniform == 0u && g_rec.lines == 192u, "full: 192 single-line snaps, record not uniform", g_host.snap_calls);
    for (n = 0; n < 192u; n++) {
        uint16_t a, b;

        memcpy(&a, gpu2d_line_regs(&g_rec, 0, n) + BG1HOFS, 2);
        memcpy(&b, gpu2d_line_regs(&g_rec, 1, n) + BG1HOFS, 2);
        ok_all &= (a == n);
        ok_b &= (b == n + 1000u);
    }
    check(ok_all == 1u, "full: engine A snapshot of line n holds BG1HOFS == n for all 192 lines (line 0 included: not the stale 0x7777)", 0);
    check(ok_b == 1u, "full: engine B snapshot of line n holds n + 1000 for all 192 lines", 0);
    check(r16(gpu2d_line_regs(&g_rec, 0, 5) + 4, 0) == 0u && r16(gpu2d_line_regs(&g_rec, 0, 5) + 6, 0) == 0u, "full: DISPSTAT / VCOUNT bytes of the image are zero", 0);
    /* the order proof */
    for (n = 0; n < g_host.nev; n++) {
        if (g_host.ev[n].kind == EV_VB_CALLBACK) { ev_vbcb = n + 1u; }
        if (g_host.ev[n].kind == EV_SNAPPOINT) { ev_sp = n + 1u; }
        if (g_host.ev[n].kind == EV_HBLANK) {
            if (first_hb == 0u) { first_hb = n + 1u; }
            last_hb = n + 1u;
        }
    }
    check(ev_vbcb != 0u && ev_sp > ev_vbcb && first_hb > ev_sp, "order: vblank handler < vblank callback < snapshot point < first HBlank callback", (long)first_hb);
    check(g_host.nev == 3u + 2u * 192u && last_hb == g_host.nev - 1u, "order: log = VBLANK_IRQ, VB_CALLBACK, SNAPPOINT, then 192 x (HBLANK n, SNAP n)", g_host.nev);
    for (n = 0; n < 192u; n++) {   /* HBLANK n is immediately followed by SNAP n: the image is read AFTER the callback */
        const Ev *hb = &g_host.ev[3u + 2u * n], *sn = &g_host.ev[4u + 2u * n];

        inter &= (hb->kind == EV_HBLANK && hb->arg == n && sn->kind == EV_SNAP && sn->arg == n);
    }
    check(inter == 1u, "order: line n's snapshot is taken after line n's callback, before line n+1's", 0);
    check(r16(g_io, HWR_VCOUNT) < 263u, "full: VCOUNT back on the frame's own clock after the lines", r16(g_io, HWR_VCOUNT));
    check(p.vblanks == 1u && p.next_vblank == 2u * HWT_FRAME_SYS && p.time.virt == HWT_FRAME_SYS, "full: no virtual time charged by the 192 lines, the vblank did not move", (long)p.time.virt);
}

static void test_shortcut(void)
{
    HwPump p;
    uint32_t vb, ran;
    const HwPumpLineOps lo = { l_snap, &g_host };

    /* no HBlank IRQ enabled anywhere */
    memset(&g_rec, 0, sizeof g_rec);
    setup(&p, &g_host, g_io, &vb, 0x0008u, 0x1u);
    vram_fresh();
    w16(g_io, BG1HOFS, 0x1234u);
    ran = game_frame(&p, &g_host, NULL, &lo);
    check(ran == 0u && g_host.hb_calls == 0u && g_host.snap_calls == 1u && g_host.snap_lines == 192u, "shortcut: no callback, ONE snap of 192 lines", g_host.snap_calls);
    check(g_rec.uniform == 1u && g_rec.lines == 192u && p.hb.shortcut == 1u && p.hb.full == 0u && p.hb.lines == 0u && p.hb.irqs == 0u, "shortcut: uniform record, counted", p.hb.shortcut);
    check(r16(gpu2d_line_regs(&g_rec, 0, 0) + BG1HOFS, 0) == 0x1234u && gpu2d_line_regs(&g_rec, 0, 0) == gpu2d_line_regs(&g_rec, 0, 191), "shortcut: every line reads the one image", 0);
    check(g_host.nev == 4u, "shortcut: log = VBLANK_IRQ, VB_CALLBACK, SNAPPOINT, SNAP(all)", g_host.nev);
    /* DISPSTAT bit 4 without IE bit 1, and IE bit 1 without DISPSTAT bit 4: both are short-cuts (no IRQ can be taken), both show in the enable counters */
    setup(&p, &g_host, g_io, &vb, 0x0018u, 0x1u);
    ran = game_frame(&p, &g_host, NULL, &lo);
    check(ran == 0u && p.hb.shortcut == 1u && p.hb.ds_frames == 1u && p.hb.ie_frames == 0u, "shortcut: DISPSTAT bit 4 alone", p.hb.ds_frames);
    setup(&p, &g_host, g_io, &vb, 0x0008u, 0x3u);
    ran = game_frame(&p, &g_host, NULL, &lo);
    check(ran == 0u && p.hb.shortcut == 1u && p.hb.ds_frames == 0u && p.hb.ie_frames == 1u, "shortcut: IE bit 1 alone", p.hb.ie_frames);
}

static void test_masked(void)
{
    HwPump p;
    uint32_t vb, ran;
    const HwPumpLineOps lo = { l_snap, &g_host };

    setup(&p, &g_host, g_io, &vb, 0x0018u, 0x3u);
    vram_fresh();
    g_host.rec = &g_rec;
    hwpump_run(&p, HWP_WAIT);
    g_host.masked = 1;   /* CPSR.I set at the snapshot point: the IRQs stay pending like on the DS, the 192 images are still taken */
    bind(&g_rec, g_io);
    ran = hwpump_hblank_frame(&p, &lo);
    check(ran == 192u && g_host.hb_calls == 0u && g_host.snap_lines == 192u && g_rec.lines == 192u && (r32(g_io, HWR_IF) & 2u) != 0u, "masked: no callback runs, IF bit 1 pends, all lines recorded", g_host.hb_calls);
}

/* ---- the views follow a VRAMCNT change made by the vblank callback ---- */
static uint8_t *g_viewio;
static void cb_vramcnt(Host *h)
{
    (void)h;
    set_cnt(g_viewio, HWVRAM_BANK_H, 0x82u);   /* bank H: sub BG -> sub BG extended palette (MST 2) */
}

static void test_views(void)
{
    HwPump p;
    uint32_t vb, i, same = 1u;
    const HwPumpLineOps lo = { l_snap, &g_host };

    setup(&p, &g_host, g_io, &vb, 0x0008u, 0x1u);
    vram_fresh();
    g_viewio = g_io;
    g_host.rec = &g_rec;
    for (i = 0; i < 2048u; i++) {
        g_pltt[i] = (uint8_t)(i * 3u);
        g_oam[i] = (uint8_t)(i * 5u + 1u);
    }
    set_cnt(g_io, HWVRAM_BANK_H, 0x81u);   /* sub BG (MST 1) */
    for (i = 0; i < 0x8000u; i++) {
        g_sbg[i] = (uint8_t)(i * 7u + 1u);
    }
    bind(&g_rec, g_io);
    check(g_rec.bg_ext[1][0] == NULL && g_rec.win[HWVRAM_WIN_SUB_BG] != g_sbg && memcmp(g_rec.win[HWVRAM_WIN_SUB_BG], g_sbg, 0x8000u) == 0 && g_rec.vramcnt[8] == 0x81u, "views: before the callback, H is sub BG (no ext palette view)", g_rec.vramcnt[8]);
    game_frame(&p, &g_host, cb_vramcnt, &lo);   /* the vblank callback maps H to the ext palette; the snapshot point binds afterwards */
    check(g_rec.vramcnt[8] == 0x82u, "views: the record's VRAMCNT_H byte is the callback's value", g_rec.vramcnt[8]);
    check(g_rec.bg_ext[1][0] != NULL && g_rec.bg_ext[1][0] != g_store + 0x98000u && g_rec.bg_ext[1][3] == g_rec.bg_ext[1][0] + 0x6000u && g_rec.bg_ext[1][0] >= g_rec.vram && g_rec.bg_ext[1][3] < g_rec.vram + sizeof g_rec.vram, "views: sub BG ext palette slots 0 / 3 are in the RECORD (a copy of bank H), not the store", 0);
    for (i = 0; i < 0x8000u; i++) {
        same &= g_rec.bg_ext[1][i / 0x2000u][i % 0x2000u] == (uint8_t)(i * 7u + 1u);
    }
    check(same == 1u, "views: bank H's bytes followed it into the ext palette view", 0);
    check(g_sbg[0] == 0u && g_rec.vchunks[HWVRAM_WIN_SUB_BG] == 0u && g_rec.win[HWVRAM_WIN_SUB_BG][5] == 0u, "views: the sub BG window no longer holds H's bytes, and the record's chunk that left the mask is zeroed", 0);
    check(g_rec.bg_ext[0][0] == NULL && g_rec.obj_ext[0] == NULL && g_rec.obj_ext[1] == NULL && g_rec.layer3d == NULL, "views: unmapped views are NULL, the 3D slot is empty", 0);
    /* back to sub BG */
    set_cnt(g_io, HWVRAM_BANK_H, 0x81u);
    bind(&g_rec, g_io);
    check(g_rec.bg_ext[1][0] == NULL && g_rec.vramcnt[8] == 0x81u && g_rec.win[HWVRAM_WIN_SUB_BG][5] == (uint8_t)(5u * 7u + 1u), "views: H back to sub BG, bytes back in the window", 0);
    /* palette / OAM are copies of the snapshot point */
    g_pltt[100] ^= 0xffu;
    g_oam[1500] ^= 0xffu;
    check(g_rec.pltt[0][100] == (uint8_t)(100u * 3u) && g_rec.oam[1][1500 - 1024] == (uint8_t)(1500u * 5u + 1u) && g_rec.pltt[1][5] == g_pltt[1024 + 5], "views: palette and OAM are copies, split per engine", 0);
    check(g_rec.seq >= 3u, "views: seq counts the frames begun", g_rec.seq);
}

/* ---- review B1: the record owns its VRAM; the copy is exactly what the VRAMCNT bindings address ---- */
static void test_vram_copy(void)
{
    uint8_t io[IO_BYTES];
    uint32_t i, same = 1u;

    memset(io, 0, sizeof io);
    vram_fresh();
    memset(&g_rec, 0, sizeof g_rec);
    /* A: main BG at 0x20000 (OFS 1, 128 KB); E: main BG at 0 (64 KB); G: main OBJ 16 KB; H: sub BG 32 KB; I: sub OBJ ext palette (8 KB used) */
    set_cnt(io, HWVRAM_BANK_A, 0x89u);
    set_cnt(io, HWVRAM_BANK_E, 0x81u);
    set_cnt(io, HWVRAM_BANK_G, 0x82u);
    set_cnt(io, HWVRAM_BANK_H, 0x81u);
    set_cnt(io, HWVRAM_BANK_I, 0x83u);
    for (i = 0; i < 0x80000u; i++) {
        g_bg[i] = (uint8_t)(i * 11u + 3u);
    }
    for (i = 0; i < 0x4000u; i++) {
        g_obj[i] = (uint8_t)(i * 13u + 5u);
    }
    for (i = 0; i < 0x8000u; i++) {
        g_sbg[i] = (uint8_t)(i * 7u + 1u);
    }
    memset(g_store + 0xA0000u, 0x5Au, 0x2000u);   /* bank I in the store */
    bind(&g_rec, io);
    check(g_rec.vram_bytes == 131072u + 65536u + 16384u + 32768u + 8192u, "vram copy: bytes = A 128K + E 64K (main BG) + G 16K (main OBJ) + H 32K (sub BG) + I ext 8K", g_rec.vram_bytes);
    check(g_rec.vchunks[HWVRAM_WIN_MAIN_BG] == 0x0000FF0Fu && g_rec.vchunks[HWVRAM_WIN_MAIN_OBJ] == 0x1u && g_rec.vchunks[HWVRAM_WIN_SUB_BG] == 0x3u &&
          g_rec.vchunks[HWVRAM_WIN_SUB_OBJ] == 0u, "vram copy: the chunk masks come from VRAMCNT (main BG 0..3 + 8..15, main OBJ 0, sub BG 0..1)", g_rec.vchunks[0]);
    check(g_rec.obj_ext[1] != NULL && g_rec.obj_ext[1][0] == 0x5Au && g_rec.obj_ext[0] == NULL, "vram copy: the sub OBJ ext slot is copied, the main one is not bound", 0);
    /* the unbound main BG chunks (4..7: 0x10000..0x20000) read as zero even though the live window has bytes there */
    check(g_rec.win[0][0x10000u] == 0u && g_rec.win[0][0x1FFFFu] == 0u && g_rec.win[0][0x20000u] == g_bg[0x20000u], "vram copy: a window range no bank covers reads zero, a covered one equals the live byte", 0);
    /* the live store and windows are overwritten after the snapshot: the record is unchanged */
    memset(g_bg, 0xEE, sizeof g_bg);
    memset(g_obj, 0xEE, sizeof g_obj);
    memset(g_sbg, 0xEE, sizeof g_sbg);
    memset(g_store, 0xEE, sizeof g_store);
    for (i = 0; i < 0x20000u; i++) {
        same &= g_rec.win[0][0x20000u + i] == (uint8_t)((0x20000u + i) * 11u + 3u);
    }
    for (i = 0; i < 0x4000u; i++) {
        same &= g_rec.win[1][i] == (uint8_t)(i * 13u + 5u);
    }
    for (i = 0; i < 0x8000u; i++) {
        same &= g_rec.win[2][i] == (uint8_t)(i * 7u + 1u);
    }
    same &= g_rec.obj_ext[1][0] == 0x5Au && g_rec.obj_ext[1][0x1FFFu] == 0x5Au;
    check(same == 1u, "vram copy: overwriting the live store and windows after the snapshot leaves the record unchanged", 0);
    /* remap: H -> sub BG ext (4 slots), E -> main BG ext (4 slots): main BG shrinks to A, the sub BG window is empty */
    vram_fresh();
    memset(&g_rec, 0, sizeof g_rec);
    set_cnt(io, HWVRAM_BANK_A, 0x89u);
    set_cnt(io, HWVRAM_BANK_E, 0x81u);
    set_cnt(io, HWVRAM_BANK_G, 0x82u);
    set_cnt(io, HWVRAM_BANK_H, 0x81u);
    set_cnt(io, HWVRAM_BANK_I, 0x83u);
    bind(&g_rec, io);
    check(g_rec.vram_bytes == 253952u, "vram copy: bytes before the remap", g_rec.vram_bytes);
    set_cnt(io, HWVRAM_BANK_H, 0x82u);
    set_cnt(io, HWVRAM_BANK_E, 0x84u);
    bind(&g_rec, io);
    check(g_rec.vram_bytes == 131072u + 16384u + 32768u + 32768u + 8192u, "vram copy: bytes after H -> sub BG ext and E -> main BG ext (A 128K + G 16K + H ext 32K + E ext 32K + I ext 8K)", g_rec.vram_bytes);
    check(g_rec.vchunks[HWVRAM_WIN_MAIN_BG] == 0x0000FF00u && g_rec.vchunks[HWVRAM_WIN_SUB_BG] == 0u, "vram copy: the chunk masks follow the remap", g_rec.vchunks[0]);
    check(g_rec.bg_ext[0][3] != NULL && g_rec.bg_ext[1][3] != NULL && g_rec.bg_ext[0][0] != NULL, "vram copy: both BG ext palettes are bound", 0);
}

int main(void)
{
    test_full_dispatch();
    test_shortcut();
    test_masked();
    test_views();
    test_vram_copy();
    printf("gpu2d_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
