/* hw_vram_test.c -- bet 2.1b: the VRAM bank model of source/hw/hw_vram.c alone. Native, one thread, no SDK, no libctru.
 * The VRAMCNT values are the SDK's own: tools/host-tests/build.sh extracts the GX_VRAMCNT_* enums of gx_vramcnt.c and the REG_GX_VRAMCNT_*_SHIFT macros of
 * ioreg_GX.h into vramcnt_sdk.h. Checks: the bank -> window / view decode for every mapping class, the field, title and menu plans, bank H moving
 * sub BG -> EXTPLTT -> sub BG with its bytes, a CPU-written BG bank over an LCDC round trip, a GX_BeginLoadTex / GX_EndLoadTex pair, a vacated window reading
 * zero, the dirty bitmap against a real DMA (hw_dma.c), overlaps and the counters. Prints "hw_vram_test: N checks, F failures". Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_dev_hooks.h"
#include "hw_log.h"
#include "hw_pure.h"
#include "hw_vram.h"
#include "vramcnt_sdk.h"

static long g_checks, g_fails;
static uint32_t g_log_emits;

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

/* hw_log.c needs libctru's file layer; the test only counts the overlap messages */
void hwlog_emit(HwLogSite *site, const char *fmt, ...)
{
    (void)fmt;
    if (site->count++ == 0u) {
        g_log_emits++;
    }
}

static uint8_t g_store[HWVRAM_STORE_SIZE];
static uint8_t g_bg[HWVRAM_WIN_MAIN_BG_SIZE], g_obj[HWVRAM_WIN_MAIN_OBJ_SIZE], g_sbg[HWVRAM_WIN_SUB_BG_SIZE], g_sobj[HWVRAM_WIN_SUB_OBJ_SIZE];
static const uint32_t k_lcdc[HWVRAM_BANKS] = { 0x00000u, 0x20000u, 0x40000u, 0x60000u, 0x80000u, 0x90000u, 0x94000u, 0x98000u, 0xA0000u };
static const uint32_t k_size[HWVRAM_BANKS] = { 0x20000u, 0x20000u, 0x20000u, 0x20000u, 0x10000u, 0x4000u, 0x4000u, 0x8000u, 0x4000u };

static uint8_t *win_ptr(uint32_t id)
{
    uint8_t *w[HWVRAM_WINDOWS] = { g_bg, g_obj, g_sbg, g_sobj };

    assert(id < HWVRAM_WINDOWS);
    return w[id];
}

static const uint32_t k_wsize[HWVRAM_WINDOWS] = { HWVRAM_WIN_MAIN_BG_SIZE, HWVRAM_WIN_MAIN_OBJ_SIZE, HWVRAM_WIN_SUB_BG_SIZE, HWVRAM_WIN_SUB_OBJ_SIZE };

static void fresh(void)
{
    HwVramMem mem;

    mem.store = g_store;
    mem.win[HWVRAM_WIN_MAIN_BG] = g_bg;
    mem.win[HWVRAM_WIN_MAIN_OBJ] = g_obj;
    mem.win[HWVRAM_WIN_SUB_BG] = g_sbg;
    mem.win[HWVRAM_WIN_SUB_OBJ] = g_sobj;
    HWVRAM_Init(&mem);
}

/* a distinct non-zero first and last byte in the bank's place in the store */
static void mark_bank(uint32_t bank)
{
    g_store[k_lcdc[bank]] = (uint8_t)(0xA0u + bank);
    g_store[k_lcdc[bank] + k_size[bank] - 1u] = (uint8_t)(0xB0u + bank);
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int windows_zero(void)
{
    uint32_t w;

    for (w = 0; w < HWVRAM_WINDOWS; w++) {
        if (!all_zero(win_ptr(w), k_wsize[w])) {
            return 0;
        }
    }
    return 1;
}

/* ---- the decode table ---------------------------------------------------------------------------------------------------------------- */
enum { K_WIN, K_TEX, K_TP, K_BGX, K_OBJX, K_NONE };

typedef struct Row {
    uint32_t bank;
    uint32_t val;
    int kind;
    uint32_t a, b;   /* K_WIN: window id, start; K_TEX/K_TP: slot; K_BGX: engine, slot; K_OBJX: engine */
    uint32_t delta;  /* view: byte offset from the bank's start in the store */
} Row;

#define A HWVRAM_BANK_A
#define B HWVRAM_BANK_B
#define C HWVRAM_BANK_C
#define D HWVRAM_BANK_D
#define E HWVRAM_BANK_E
#define F HWVRAM_BANK_F
#define G HWVRAM_BANK_G
#define H HWVRAM_BANK_H
#define I HWVRAM_BANK_I
#define MBG HWVRAM_WIN_MAIN_BG
#define MOBJ HWVRAM_WIN_MAIN_OBJ
#define SBG HWVRAM_WIN_SUB_BG
#define SOBJ HWVRAM_WIN_SUB_OBJ

static const Row k_rows[] = {
    { A, GX_VRAMCNT_A_BG_0x06000000, K_WIN, MBG, 0x00000, 0 }, { A, GX_VRAMCNT_A_BG_0x06020000, K_WIN, MBG, 0x20000, 0 },
    { A, GX_VRAMCNT_A_BG_0x06040000, K_WIN, MBG, 0x40000, 0 }, { A, GX_VRAMCNT_A_BG_0x06060000, K_WIN, MBG, 0x60000, 0 },
    { A, GX_VRAMCNT_A_OBJ_0x06400000, K_WIN, MOBJ, 0x00000, 0 }, { A, GX_VRAMCNT_A_OBJ_0x06420000, K_WIN, MOBJ, 0x20000, 0 },
    { B, GX_VRAMCNT_B_BG_0x06000000, K_WIN, MBG, 0x00000, 0 }, { B, GX_VRAMCNT_B_BG_0x06060000, K_WIN, MBG, 0x60000, 0 },
    { B, GX_VRAMCNT_B_OBJ_0x06420000, K_WIN, MOBJ, 0x20000, 0 },
    { C, GX_VRAMCNT_C_BG_0x06020000, K_WIN, MBG, 0x20000, 0 }, { C, GX_VRAMCNT_C_SUBBG_0x06200000, K_WIN, SBG, 0, 0 },
    { D, GX_VRAMCNT_D_BG_0x06000000, K_WIN, MBG, 0x00000, 0 }, { D, GX_VRAMCNT_D_BG_0x06060000, K_WIN, MBG, 0x60000, 0 },
    { D, GX_VRAMCNT_D_SUBOBJ_0x06600000, K_WIN, SOBJ, 0, 0 },
    { E, GX_VRAMCNT_E_BG_0x06000000, K_WIN, MBG, 0, 0 }, { E, GX_VRAMCNT_E_OBJ_0x06400000, K_WIN, MOBJ, 0, 0 },
    { F, GX_VRAMCNT_F_BG_0x06000000, K_WIN, MBG, 0x00000, 0 }, { F, GX_VRAMCNT_F_BG_0x06004000, K_WIN, MBG, 0x04000, 0 },
    { F, GX_VRAMCNT_F_BG_0x06010000, K_WIN, MBG, 0x10000, 0 }, { F, GX_VRAMCNT_F_BG_0x06014000, K_WIN, MBG, 0x14000, 0 },
    { F, GX_VRAMCNT_F_OBJ_0x06400000, K_WIN, MOBJ, 0x00000, 0 }, { F, GX_VRAMCNT_F_OBJ_0x06414000, K_WIN, MOBJ, 0x14000, 0 },
    { G, GX_VRAMCNT_G_BG_0x06004000, K_WIN, MBG, 0x04000, 0 }, { G, GX_VRAMCNT_G_BG_0x06010000, K_WIN, MBG, 0x10000, 0 },
    { G, GX_VRAMCNT_G_OBJ_0x06404000, K_WIN, MOBJ, 0x04000, 0 }, { G, GX_VRAMCNT_G_OBJ_0x06410000, K_WIN, MOBJ, 0x10000, 0 },
    { H, GX_VRAMCNT_H_SUBBG_0x06200000, K_WIN, SBG, 0, 0 },
    { I, GX_VRAMCNT_I_SUBBG_0x06208000, K_WIN, SBG, 0x8000, 0 }, { I, GX_VRAMCNT_I_SUBOBJ_0x06600000, K_WIN, SOBJ, 0, 0 },
    { A, GX_VRAMCNT_A_TEX_0, K_TEX, 0, 0, 0 }, { A, GX_VRAMCNT_A_TEX_3, K_TEX, 3, 0, 0 },
    { B, GX_VRAMCNT_B_TEX_1, K_TEX, 1, 0, 0 }, { C, GX_VRAMCNT_C_TEX_2, K_TEX, 2, 0, 0 }, { D, GX_VRAMCNT_D_TEX_3, K_TEX, 3, 0, 0 },
    { E, GX_VRAMCNT_E_TEXPLTT_0123, K_TP, 0, 0, 0 }, { E, GX_VRAMCNT_E_TEXPLTT_0123, K_TP, 3, 0, 0xC000 },
    { F, GX_VRAMCNT_F_TEXPLTT_0, K_TP, 0, 0, 0 }, { F, GX_VRAMCNT_F_TEXPLTT_1, K_TP, 1, 0, 0 },
    { F, GX_VRAMCNT_F_TEXPLTT_4, K_TP, 4, 0, 0 }, { F, GX_VRAMCNT_F_TEXPLTT_5, K_TP, 5, 0, 0 },
    { G, GX_VRAMCNT_G_TEXPLTT_0, K_TP, 0, 0, 0 }, { G, GX_VRAMCNT_G_TEXPLTT_1, K_TP, 1, 0, 0 },
    { G, GX_VRAMCNT_G_TEXPLTT_4, K_TP, 4, 0, 0 }, { G, GX_VRAMCNT_G_TEXPLTT_5, K_TP, 5, 0, 0 },
    { E, GX_VRAMCNT_E_BGEXTPLTT_0123, K_BGX, 0, 0, 0 }, { E, GX_VRAMCNT_E_BGEXTPLTT_0123, K_BGX, 0, 3, 0x6000 },
    { F, GX_VRAMCNT_F_BGEXTPLTT_01, K_BGX, 0, 1, 0x2000 }, { F, GX_VRAMCNT_F_BGEXTPLTT_23, K_BGX, 0, 2, 0 },
    { G, GX_VRAMCNT_G_BGEXTPLTT_01, K_BGX, 0, 0, 0 }, { G, GX_VRAMCNT_G_BGEXTPLTT_23, K_BGX, 0, 3, 0x2000 },
    { F, GX_VRAMCNT_F_OBJEXTPLTT, K_OBJX, 0, 0, 0 }, { G, GX_VRAMCNT_G_OBJEXTPLTT, K_OBJX, 0, 0, 0 },
    { H, GX_VRAMCNT_H_SUBBGEXTPLTT_0123, K_BGX, 1, 0, 0 }, { H, GX_VRAMCNT_H_SUBBGEXTPLTT_0123, K_BGX, 1, 3, 0x6000 },
    { I, GX_VRAMCNT_I_SUBOBJEXTPLTT, K_OBJX, 1, 0, 0 },
    { A, GX_VRAMCNT_A_LCDC_0x06800000, K_NONE, 0, 0, 0 }, { B, GX_VRAMCNT_B_LCDC_0x06820000, K_NONE, 0, 0, 0 },
    { C, GX_VRAMCNT_C_ARM7_0x06000000, K_NONE, 0, 0, 0 }, { D, GX_VRAMCNT_D_ARM7_0x06020000, K_NONE, 0, 0, 0 },
    { E, GX_VRAMCNT_E_LCDC_0x06880000, K_NONE, 0, 0, 0 }, { H, GX_VRAMCNT_H_LCDC_0x06898000, K_NONE, 0, 0, 0 },
    { I, GX_VRAMCNT_I_LCDC_0x068A0000, K_NONE, 0, 0, 0 },
    { A, GX_VRAMCNT_A_DISABLE, K_NONE, 0, 0, 0 }, { G, GX_VRAMCNT_G_DISABLE, K_NONE, 0, 0, 0 }, { I, GX_VRAMCNT_I_DISABLE, K_NONE, 0, 0, 0 },
    { C, 0x87u, K_NONE, 0, 0, 0 }, { E, 0x87u, K_NONE, 0, 0, 0 }, { H, 0x83u, K_NONE, 0, 0, 0 },   /* an MST the hardware does not define */
};

static const uint8_t *row_view(const Row *r)
{
    switch (r->kind) {
    case K_TEX: return HWVRAM_TexSlot(r->a);
    case K_TP: return HWVRAM_TexPltt(r->a);
    case K_BGX: return HWVRAM_BgExtPltt(r->a, r->b);
    default: return HWVRAM_ObjExtPltt(r->a);
    }
}

static void test_row(const Row *r, unsigned idx)
{
    uint32_t sz = k_size[r->bank];

    fresh();
    mark_bank(r->bank);
    HWVRAM_SetCnt(r->bank, r->val);
    if (r->kind == K_WIN) {
        const uint8_t *w = win_ptr(r->a);
        uint32_t span = sz;

        check(w[r->b] == 0xA0u + r->bank && w[r->b + span - 1u] == 0xB0u + r->bank, "row: bank bytes appear in the window", idx);
        check((r->b == 0u || w[r->b - 1u] == 0u) && (r->b + span >= k_wsize[r->a] || w[r->b + span] == 0u), "row: window neighbours stay zero", idx);
        check(HWVRAM_Window(r->a) == w, "row: HWVRAM_Window", idx);
    } else if (r->kind == K_NONE) {
        check(windows_zero(), "row: unmapped bank appears nowhere", idx);
        check(row_view(&(Row){ 0, 0, K_TEX, 0, 0, 0 }) == NULL && HWVRAM_BgExtPltt(0, 0) == NULL && HWVRAM_ObjExtPltt(1) == NULL, "row: no views", idx);
    } else {
        const uint8_t *v = row_view(r);

        check(v != NULL && v == g_store + k_lcdc[r->bank] + r->delta, "row: view points into the store", idx);
        check(windows_zero(), "row: a view bank has no window copy", idx);
    }
    HWVRAM_SetCnt(r->bank, 0u);
    check(windows_zero(), "row: disabling vacates (zero)", idx);
}

static void test_decode(void)
{
    unsigned i;

    for (i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        test_row(&k_rows[i], i);
    }
    fresh();
    HWVRAM_SetCnt(C, 0x87u);
    HWVRAM_SetCnt(H, 0x83u);
    {
        HwVramStats st;

        HWVRAM_Stats(&st);
        check(st.bad_mst == 2u, "undefined MST counted", st.bad_mst);
    }
    check(HWVRAM_TexSlot(4) == NULL && HWVRAM_TexPltt(6) == NULL && HWVRAM_BgExtPltt(2, 0) == NULL && HWVRAM_BgExtPltt(0, 4) == NULL &&
              HWVRAM_ObjExtPltt(2) == NULL && HWVRAM_Window(4) == NULL, "out-of-range view ids", 0);
}

/* ---- the three bank plans -------------------------------------------------------------------------------------------------------------- */
static void set_all(const uint8_t v[HWVRAM_BANKS])
{
    uint32_t b;

    for (b = 0; b < HWVRAM_BANKS; b++) {
        HWVRAM_SetCnt(b, v[b]);
    }
}

static void disable_all(void)
{
    uint32_t b;

    for (b = 0; b < HWVRAM_BANKS; b++) {
        HWVRAM_SetCnt(b, 0u);
    }
}

static void test_plans(void)
{
    /* order: A B C D E F G H I */
    const uint8_t field[HWVRAM_BANKS] = { GX_VRAMCNT_A_TEX_0, GX_VRAMCNT_B_TEX_1, GX_VRAMCNT_C_BG_0x06000000, 0, GX_VRAMCNT_E_TEXPLTT_0123,
                                          GX_VRAMCNT_F_OBJ_0x06400000, GX_VRAMCNT_G_OBJ_0x06404000, GX_VRAMCNT_H_SUBBG_0x06200000, GX_VRAMCNT_I_SUBOBJ_0x06600000 };
    const uint8_t title[HWVRAM_BANKS] = { GX_VRAMCNT_A_TEX_0, GX_VRAMCNT_B_BG_0x06000000, GX_VRAMCNT_C_SUBBG_0x06200000, 0, 0, 0, GX_VRAMCNT_G_TEXPLTT_0,
                                          GX_VRAMCNT_H_SUBBGEXTPLTT_0123, 0 };
    const uint8_t menu[HWVRAM_BANKS] = { GX_VRAMCNT_A_BG_0x06000000, GX_VRAMCNT_B_TEX_0, GX_VRAMCNT_C_SUBBG_0x06200000, 0, GX_VRAMCNT_E_OBJ_0x06400000,
                                         GX_VRAMCNT_F_TEXPLTT_0, GX_VRAMCNT_G_TEXPLTT_1, 0, GX_VRAMCNT_I_SUBOBJ_0x06600000 };
    const uint8_t menu256[HWVRAM_BANKS] = { GX_VRAMCNT_A_BG_0x06000000, GX_VRAMCNT_B_BG_0x06020000, GX_VRAMCNT_C_SUBBG_0x06200000, 0, 0, 0, 0, 0, 0 };
    HwVramStats st;
    uint32_t i, ext_before;

    fresh();
    /* (i) the field */
    set_all(field);
    check(HWVRAM_TexSlot(0) == g_store + k_lcdc[A] && HWVRAM_TexSlot(1) == g_store + k_lcdc[B] && HWVRAM_TexSlot(2) == NULL && HWVRAM_TexSlot(3) == NULL,
          "field: texture slots 0,1 = A,B; 2,3 unmapped", 0);
    check(HWVRAM_TexPltt(0) == g_store + k_lcdc[E] && HWVRAM_TexPltt(3) == g_store + k_lcdc[E] + 0xC000u && HWVRAM_TexPltt(4) == NULL, "field: palette slots 0-3 = E", 0);
    /* window addresses of the plan: F at 0x06400000 and G at 0x06404000 of main OBJ, C main BG, H sub BG, I sub OBJ: CPU writes through each */
    g_bg[0x1234] = 0x11u;
    g_obj[0x3FFF] = 0x22u;
    g_obj[0x4000] = 0x33u;
    g_sobj[0x3FFF] = 0x55u;
    /* title: bank H moves sub BG -> EXTPLTT with its bytes; the game resets every bank first */
    for (i = 0; i < 0x8000u; i++) {
        g_sbg[i] = (uint8_t)(i * 7u + 1u);
    }
    disable_all();
    check(windows_zero(), "field -> reset: every window vacated", 0);
    check(g_store[k_lcdc[C] + 0x1234] == 0x11u && g_store[k_lcdc[F] + 0x3FFF] == 0x22u && g_store[k_lcdc[G]] == 0x33u && g_store[k_lcdc[H] + 5u] == (uint8_t)(5u * 7u + 1u) &&
              g_store[k_lcdc[I] + 0x3FFF] == 0x55u, "field -> reset: CPU-written bytes flushed to the store", 0);
    set_all(title);
    check(HWVRAM_BgExtPltt(1, 0) == g_store + k_lcdc[H] && HWVRAM_BgExtPltt(1, 3) == g_store + k_lcdc[H] + 0x6000u, "title: sub BG ext palette = H", 0);
    {
        int same = 1;

        for (i = 0; i < 0x8000u; i++) {
            same &= HWVRAM_BgExtPltt(1, i / 0x2000u)[i % 0x2000u] == (uint8_t)(i * 7u + 1u);
        }
        check(same, "title: bank H bytes follow it into the sub BG ext palette", 0);
    }
    check(HWVRAM_TexSlot(0) == g_store + k_lcdc[A] && HWVRAM_TexSlot(1) == NULL && HWVRAM_TexPltt(0) == g_store + k_lcdc[G] && HWVRAM_TexPltt(1) == NULL,
          "title: TEX_0_A + TEXPLTT_0_G", 0);
    check(g_sbg[0] == 0u && g_sbg[0x20000 - 1u] == 0u && g_bg[0] == 0u, "title: sub BG window holds C (zero), H's old bytes are gone from it", 0);
    ext_before = (uint32_t)(g_sbg[1] + g_sbg[0x7FFF]);
    /* ... and back to sub BG for the next field visit */
    disable_all();
    HWVRAM_SetCnt(H, GX_VRAMCNT_H_SUBBG_0x06200000);
    {
        int same = 1;

        for (i = 0; i < 0x8000u; i++) {
            same &= g_sbg[i] == (uint8_t)(i * 7u + 1u);
        }
        check(same && ext_before == 0u, "title -> field: bank H bytes are back in the sub BG window", 0);
    }
    /* (iii) menu / journal */
    disable_all();
    set_all(menu);
    check(HWVRAM_TexSlot(0) == g_store + k_lcdc[B] && HWVRAM_TexPltt(0) == g_store + k_lcdc[F] && HWVRAM_TexPltt(1) == g_store + k_lcdc[G] && HWVRAM_TexSlot(1) == NULL,
          "menu: TEX_0_B + TEXPLTT_01_FG", 0);
    g_obj[0xFFFF] = 0x66u;
    g_bg[0x1FFFF] = 0x77u;
    disable_all();
    set_all(menu256);
    check(g_store[k_lcdc[E] + 0xFFFF] == 0x66u && g_bg[0x1FFFF] == 0x77u && g_store[k_lcdc[A] + 0x1FFFF] == 0x77u, "menu -> 256 KB BG: E's OBJ byte flushed, A's BG byte reloaded", 0);
    g_bg[0x20000] = 0x88u;
    HWVRAM_SetCnt(A, 0u);
    check(g_bg[0x1FFFF] == 0u && g_bg[0x20000] == 0x88u, "BG_256_AB: dropping A zeroes only A's range, B's bytes stay", 0);
    HWVRAM_Stats(&st);
    check(st.overlaps == 0u, "the three plans: no overlap", st.overlaps);
}

/* ---- remap round trips ---------------------------------------------------------------------------------------------------------------- */
static void test_round_trips(void)
{
    uint8_t texels[512];
    const uint8_t *t;
    uint32_t i;

    /* a CPU-written BG bank survives an LCDC round trip */
    fresh();
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_BG_0x06000000);
    for (i = 0; i < 0x20000u; i += 0x1001u) {
        g_bg[i] = (uint8_t)(i >> 4 | 1u);
    }
    g_bg[0x1FFFF] = 0xEEu;
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_LCDC_0x06840000);
    check(g_store[k_lcdc[C] + 0x1FFFF] == 0xEEu && g_store[k_lcdc[C] + 0x1001] == (uint8_t)(0x1001u >> 4 | 1u), "LCDC round trip: bytes in the store", 0);
    check(all_zero(g_bg, 0x20000u), "LCDC round trip: the BG window is vacated", 0);
    g_store[k_lcdc[C] + 0x40] = 0x5Au;   /* an LCDC write while remapped */
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_BG_0x06000000);
    check(g_bg[0x1FFFF] == 0xEEu && g_bg[0x40] == 0x5Au && g_bg[0x1001] == (uint8_t)(0x1001u >> 4 | 1u), "LCDC round trip: bytes back in the BG window", 0);
    g_bg[0x40] = 0x6Bu;
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_BG_0x06000000);
    check(g_bg[0x40] == 0x6Bu && g_store[k_lcdc[C] + 0x40] == 0x5Au, "rewriting the same value moves nothing (no flush, no reload)", 0);

    /* GX_BeginLoadTex / GX_EndLoadTex: TEX_0 -> LCDC -> write -> TEX_0 inside one frame */
    fresh();
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_LCDC_0x06800000);
    for (i = 0; i < sizeof texels; i++) {
        texels[i] = (uint8_t)(i * 13u + 5u);
    }
    memcpy(g_store + k_lcdc[A], texels, sizeof texels);
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_TEX_0);
    t = HWVRAM_TexSlot(0);
    check(t != NULL && memcmp(t, texels, sizeof texels) == 0, "Begin/EndLoadTex: texels visible through the TEX view, byte-identical", 0);
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_LCDC_0x06800000);   /* the next upload: the old texels are still there to patch */
    check(HWVRAM_TexSlot(0) == NULL && memcmp(g_store + k_lcdc[A], texels, sizeof texels) == 0, "BeginLoadTex: no TEX view while mapped to LCDC, old texels still there", 0);
    g_store[k_lcdc[A] + 7] ^= 0xFFu;
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_TEX_0);
    check(HWVRAM_TexSlot(0)[7] == (uint8_t)(texels[7] ^ 0xFFu) && HWVRAM_TexSlot(0)[8] == texels[8], "second upload patched one texel", 0);
    check(windows_zero(), "Begin/EndLoadTex touches no window", 0);

    /* partial coverage: a vacated range another bank still covers is not zeroed */
    fresh();
    HWVRAM_SetCnt(F, GX_VRAMCNT_F_BG_0x06000000);
    HWVRAM_SetCnt(E, GX_VRAMCNT_E_BG_0x06000000);
    {
        HwVramStats st;

        HWVRAM_Stats(&st);
        check(st.overlaps == 1u, "F and E on the same BG start overlap", st.overlaps);
    }
}

/* ---- the dirty bitmap against a real DMA ---------------------------------------------------------------------------------------------- */
static uint8_t g_regs[0x1000], g_main[0x10000];
#define FAKE_BG 0x06000000u
#define FAKE_LCDC 0x06800000u
#define FAKE_MAIN 0x02000000u

static void *fk_xlat(void *u, uint32_t addr, uint32_t bytes, HwDmaRegion *region)
{
    (void)u;
    if (addr >= FAKE_BG && addr - FAKE_BG + bytes <= HWVRAM_WIN_MAIN_BG_SIZE) {
        *region = HWDMA_VRAM;
        return g_bg + (addr - FAKE_BG);
    }
    if (addr >= FAKE_LCDC && addr - FAKE_LCDC + bytes <= HWVRAM_STORE_SIZE) {
        *region = HWDMA_VRAM;
        return g_store + (addr - FAKE_LCDC);
    }
    if (addr >= FAKE_MAIN && addr - FAKE_MAIN + bytes <= sizeof g_main) {
        *region = HWDMA_MAIN;
        return g_main + (addr - FAKE_MAIN);
    }
    *region = HWDMA_UNMAPPED;
    return NULL;
}

static void fk_wrote(void *u, HwDmaRegion region, const void *dst, uint32_t bytes)
{
    (void)u;
    if (region == HWDMA_VRAM) {   /* the same rule as hw_devs.c dma_wrote */
        HWVRAM_Touch(dst, bytes);
    }
}

static void put32(uint32_t off, uint32_t v)
{
    memcpy(g_regs + off, &v, sizeof v);
}

static void dma_run(uint32_t src, uint32_t dst, uint32_t words)
{
    HwDma d;
    HwDmaOps ops;

    memset(&ops, 0, sizeof ops);
    ops.xlat = fk_xlat;
    ops.wrote = fk_wrote;
    hwdma_init(&d, &ops, g_regs);
    put32(HWR_DMA0SAD, src);
    put32(HWR_DMA0SAD + 4u, dst);
    put32(HWR_DMA0SAD + 8u, HWDMA_CNT_ENABLE | HWDMA_CNT_32BIT | words);
    (void)hwdma_start(&d, 0);
}

static void test_dirty(void)
{
    uint32_t bits[HWVRAM_PAGE_WORDS], b, n;
    uint32_t total = 0u;

    fresh();
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_BG_0x06000000);
    HWVRAM_SetCnt(H, GX_VRAMCNT_H_SUBBG_0x06200000);
    n = HWVRAM_DirtyPages(C, bits);
    check(n == 128u && bits[0] == 0xFFFFFFFFu && bits[3] == 0xFFFFFFFFu, "a remap marks the whole bank (128 pages of C)", n);
    check(HWVRAM_DirtyPages(H, NULL) == 32u, "a remap marks the whole bank (32 pages of H)", 0);
    check(HWVRAM_DirtyPages(C, bits) == 0u && bits[0] == 0u, "DirtyPages cleared the bank", 0);
    /* DMA 3000 bytes from main RAM to BG window offset 5000: bytes 5000..7999 = pages 4..7 */
    dma_run(FAKE_MAIN, FAKE_BG + 5000u, 750u);
    n = HWVRAM_DirtyPages(C, bits);
    check(n == 4u && bits[0] == 0xF0u && bits[1] == 0u && bits[2] == 0u && bits[3] == 0u, "DMA marks exactly pages 4-7", (long)bits[0]);
    for (b = 0; b < HWVRAM_BANKS; b++) {
        total += HWVRAM_DirtyPages(b, NULL);
    }
    check(total == 0u, "no other bank was marked", total);
    /* a DMA that stops exactly on a page edge, and one byte past it */
    dma_run(FAKE_MAIN, FAKE_BG + 1024u, 256u);
    n = HWVRAM_DirtyPages(C, bits);
    check(n == 1u && bits[0] == 0x2u, "DMA of exactly one page marks one page", (long)bits[0]);
    dma_run(FAKE_MAIN, FAKE_BG + 1024u, 257u);
    check(HWVRAM_DirtyPages(C, NULL) == 2u, "DMA one word past a page edge marks two", 0);
    /* a DMA into the LCDC window marks the bank by its LCDC offset (bank H starts at 0x98000) */
    dma_run(FAKE_MAIN, FAKE_LCDC + 0x98000u + 2048u, 512u);
    n = HWVRAM_DirtyPages(H, bits);
    check(n == 2u && bits[0] == 0xCu, "DMA through LCDC marks pages 2-3 of H", (long)bits[0]);
    check(HWVRAM_DirtyPages(C, NULL) == 0u, "... and nothing in C", 0);
    /* a DMA into a window nothing covers marks nothing; a DMA that is not into VRAM marks nothing */
    dma_run(FAKE_MAIN, FAKE_BG + 0x30000u, 64u);
    dma_run(FAKE_BG, FAKE_MAIN + 0x100u, 64u);
    total = 0u;
    for (b = 0; b < HWVRAM_BANKS; b++) {
        total += HWVRAM_DirtyPages(b, NULL);
    }
    check(total == 0u, "an uncovered window and a read from VRAM mark nothing", total);
    /* the MIi_Cpu* path: HWVRAM_Touch on a window address / a store address / a foreign address */
    HWVRAM_Touch(g_bg + 0x20000u + 100u, 10u);
    check(HWVRAM_DirtyPages(C, NULL) == 0u, "Touch beyond C's range marks nothing", 0);
    HWVRAM_Touch(g_bg + 0x1FC00u, 0x400u);
    check(HWVRAM_DirtyPages(C, bits) == 1u && bits[3] == 0x80000000u, "Touch of the last page of C", (long)bits[3]);
    HWVRAM_Touch(g_main, 4096u);
    HWVRAM_Touch(g_bg + 0x1F000u, 0u);
    HWVRAM_Touch(NULL, 4u);
    check(HWVRAM_DirtyPages(C, NULL) == 0u, "foreign and empty touches are ignored", 0);
    HWVRAM_Touch(g_bg + 0x1FFF0u, 0x100000u);   /* runs past the window: clipped, not an overrun */
    check(HWVRAM_DirtyPages(C, NULL) == 1u, "a touch running past the window is clipped", 0);
    /* a store write by LCDC-mapped bank A */
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_LCDC_0x06800000);
    (void)HWVRAM_DirtyPages(A, NULL);
    HWVRAM_Touch(g_store + 0x1FC00u, 0x800u);   /* the last page of A and the first of B (B is disabled but the store write is physical) */
    check(HWVRAM_DirtyPages(A, NULL) == 1u && HWVRAM_DirtyPages(B, NULL) == 1u, "a store write spanning A and B marks one page in each", 0);
}

/* ---- overlaps and counters ---------------------------------------------------------------------------------------------------------- */
static void test_overlap_and_counters(void)
{
    HwVramStats st;

    fresh();
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_BG_0x06000000);
    HWVRAM_SetCnt(B, GX_VRAMCNT_B_BG_0x06020000);
    HWVRAM_Stats(&st);
    check(st.overlaps == 0u, "adjacent BG banks do not overlap", st.overlaps);
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_BG_0x06020000);
    HWVRAM_Stats(&st);
    check(st.overlaps == 1u, "B and C on 0x06020000: one overlap", st.overlaps);
    HWVRAM_SetCnt(D, GX_VRAMCNT_D_BG_0x06020000);
    HWVRAM_Stats(&st);
    check(st.overlaps == 3u, "a third bank on the same range: two more", st.overlaps);
    fresh();
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_TEX_0);
    HWVRAM_SetCnt(B, GX_VRAMCNT_B_TEX_0);
    HWVRAM_Stats(&st);
    check(st.overlaps == 1u, "two banks on one texture slot overlap", st.overlaps);
    fresh();
    HWVRAM_SetCnt(C, GX_VRAMCNT_C_ARM7_0x06000000);
    HWVRAM_SetCnt(D, GX_VRAMCNT_D_ARM7_0x06000000);
    HWVRAM_SetCnt(F, GX_VRAMCNT_F_BGEXTPLTT_01);
    HWVRAM_SetCnt(G, GX_VRAMCNT_G_BGEXTPLTT_23);
    HWVRAM_SetCnt(F, GX_VRAMCNT_F_TEXPLTT_0);
    HWVRAM_SetCnt(G, GX_VRAMCNT_G_TEXPLTT_1);
    HWVRAM_Stats(&st);
    check(st.overlaps == 0u, "ARM7 mappings, F/G ext palette halves, F/G palette slots: no overlap", st.overlaps);

    /* counters: a rewrite counts as a call, not a change; per-frame totals and the peak */
    fresh();
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_LCDC_0x06800000);
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_TEX_0);
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_TEX_0);
    HWVRAM_FrameEnd();
    HWVRAM_SetCnt(A, GX_VRAMCNT_A_LCDC_0x06800000);
    HWVRAM_FrameEnd();
    HWVRAM_FrameEnd();
    HWVRAM_Stats(&st);
    check(st.calls == 4u && st.changed == 3u && st.frames == 3u && st.calls_peak == 3u && st.calls_last == 0u && st.frames_with_calls == 2u,
          "counters: calls 4, changed 3, 3 frames, peak 3, last 0, 2 frames with calls", (long)st.calls);
    check(HWVRAM_Cnt(A) == GX_VRAMCNT_A_LCDC_0x06800000 && HWVRAM_Cnt(B) == 0u, "HWVRAM_Cnt reports what the hook saw", 0);
    check(st.pages_dirtied >= 128u && st.dirty_peak >= 128u, "per-frame dirty pages recorded", (long)st.dirty_peak);
}

int main(void)
{
    test_decode();
    test_plans();
    test_round_trips();
    test_dirty();
    test_overlap_and_counters();
    check(g_log_emits == 1u, "every overlap went through the one HWLOG site, which recorded once", g_log_emits);
    printf("hw_vram_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails != 0 ? 1 : 0;
}
