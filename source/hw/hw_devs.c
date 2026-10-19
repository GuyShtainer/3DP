/* hw_devs.c -- the SDK-world seam of the device units: the HWDEV_* hooks that the patched NitroSDK files call, over the pure modules
 * hw_pxi / hw_pxi_arm7 / hw_dma / hw_cp. Owns the one instance of each and the address classification of the DS arrays.
 * Design: the private design notes ("devices"). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nitro.h>
#include <nitro/fx/fx_cp.h>      /* bet 2.8: FX_Div, MTX_PerspectiveW / MTX_OrthoW (the camera hook builds the DS matrix with the SDK's own code) */
#include <nitro/fx/fx_mtx44.h>
#include <hw_dev_hooks.h>

#include "../../port/subprojects/NitroSDK-4.2.30001/libraries/card/include/card_common.h"
#include "hw_budget.h"
#include "hw_census.h"
#include "hw_ctr.h"
#include "hw_devs.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "gpu3d_pica.h"
#include "gpu3d_rec.h"
#include "gpu3d_selfcheck.h"
#include "hw_gx.h"
#include "hw_vram.h"
#include "hw_log.h"
#include "hw_os.h"
#include "hw_pure.h"
#include "hw_savewb.h"
#include "hw_screen.h"
#include "hw_sys.h"
#include "hw_wm7.h"
#include "hw_wide.h"
#include "version.h"

_Static_assert(HWG3_MODE_SEND == HWGX_MODE_SEND && HWG3_MODE_COPY == HWGX_MODE_COPY && HWG3_MODE_FIFO == HWGX_MODE_FIFO, "GX bulk modes");

#define HW_HOST_PTR_MIN 0x00100000u   /* the 3DS process image starts here: nothing below it is memory */
#define HW_HOST_PTR_END 0x40000000u   /* the 3DS application address space ends here (image, heap, linear heap, stack) */
#define HW_GXFIFO_BYTES 0x40u
#define HWGX_TRACE_FILE HWCTR_APP_DIR "/gxtrace.bin"
#define HWCENSUS_FILE   HWCTR_APP_DIR "/hwcensus.txt"   /* bet 2.1c: written every HWCEN_DUMP_FRAMES frames and at exit */

static HwDma s_dma;
static HwPxi s_pxi;
static HwArm7 s_arm7;
static int s_inited;

/* the card bus: a ROM file behind a block cache, and the 512 KB backup flash */
#define HW_FLASH_BYTES  0x80000u
#define HW_FLASH_SECTOR 0x10000u
static HwCard s_card;
static uint8_t s_flash[HW_FLASH_BYTES];
static void *s_rom_file;
static int s_rom_tried;
static HwCardOps s_rom_ops;
static int s_rom_ops_set;

_Static_assert(offsetof(CARDiCommandArg, src) == offsetof(HwCardCmd, src), "HwCardCmd.src");
_Static_assert(offsetof(CARDiCommandArg, dst) == offsetof(HwCardCmd, dst), "HwCardCmd.dst");
_Static_assert(offsetof(CARDiCommandArg, len) == offsetof(HwCardCmd, len), "HwCardCmd.len");
_Static_assert(offsetof(CARDiCommandArg, spec.sect_size) == offsetof(HwCardCmd, sect_size), "HwCardCmd.sect_size");
_Static_assert(CARD_REQ_IDENTIFY == HWCARD_REQ_IDENTIFY && CARD_REQ_READ_BACKUP == HWCARD_REQ_READ_BACKUP &&
               CARD_REQ_WRITE_BACKUP == HWCARD_REQ_WRITE_BACKUP && CARD_REQ_PROGRAM_BACKUP == HWCARD_REQ_PROGRAM_BACKUP &&
               CARD_REQ_VERIFY_BACKUP == HWCARD_REQ_VERIFY_BACKUP && CARD_REQ_ERASE_PAGE_BACKUP == HWCARD_REQ_ERASE_PAGE &&
               CARD_REQ_ERASE_SECTOR_BACKUP == HWCARD_REQ_ERASE_SECTOR && CARD_REQ_ERASE_CHIP_BACKUP == HWCARD_REQ_ERASE_CHIP &&
               CARD_REQ_READ_STATUS == HWCARD_REQ_READ_STATUS && CARD_REQ_WRITE_STATUS == HWCARD_REQ_WRITE_STATUS &&
               CARD_REQ_ERASE_SUBSECTOR_BACKUP == HWCARD_REQ_ERASE_SUBSECTOR, "CARD request numbers");
_Static_assert(CARD_RESULT_SUCCESS == HWCARD_RESULT_SUCCESS && CARD_RESULT_FAILURE == HWCARD_RESULT_FAILURE &&
               CARD_RESULT_INVALID_PARAM == HWCARD_RESULT_INVALID_PARAM && CARD_RESULT_UNSUPPORTED == HWCARD_RESULT_UNSUPPORTED,
               "CARD result numbers");

static uint8_t *io_regs(void)
{
    return (uint8_t *)HW_IOREG;
}

static int within(uint32_t addr, const void *base, uint32_t size)
{
    uint32_t b = (uint32_t)(uintptr_t)base;

    return addr >= b && addr - b < size;
}

/* Plain host memory: [addr, addr+len) lies inside the application address space (overflow-safe: no wrap past 2^32). */
static void *host_range(uint32_t addr, uint32_t len)
{
    assert(HW_HOST_PTR_MIN < HW_HOST_PTR_END);
    if (addr < HW_HOST_PTR_MIN || addr >= HW_HOST_PTR_END || len > HW_HOST_PTR_END - addr) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

/* DS address -> host memory + the region it belongs to. Every DS range is a host array, so a DS address is the host address of its array;
   anything else above the process image base is plain host memory (the game's .bss and heaps: "main RAM"). */
static void *dma_xlat(void *u, uint32_t addr, uint32_t bytes, HwDmaRegion *region)
{
    /* the DS arrays a transfer may touch, in classification order; the whole [addr, addr+bytes) must lie inside the array its start address is in */
    const struct { const void *base; uint32_t size; HwDmaRegion reg; } arr[] = {
        { io_regs(), HW_REGS_SIZE, HWDMA_IO },
        { g_hwoam, HW_OAM_SIZE + HW_DB_OAM_SIZE, HWDMA_OAM },
        { g_hwpltt, HW_PLTT_SIZE + HW_DB_PLTT_SIZE, HWDMA_PLTT },
        { g_hwvram_bg, HW_BG_VRAM_SIZE, HWDMA_VRAM },
        { g_hwvram_dbbg, HW_DB_BG_VRAM_SIZE, HWDMA_VRAM },
        { g_hwvram_obj, HW_OBJ_VRAM_SIZE, HWDMA_VRAM },
        { g_hwvram_dbobj, HW_DB_OBJ_VRAM_SIZE, HWDMA_VRAM },
        { g_hwvram_lcdc, HW_LCDC_VRAM_SIZE, HWDMA_VRAM },
    };
    unsigned i;

    (void)u;
    assert(region != NULL);
    *region = HWDMA_MAIN;
    if (addr < HW_HOST_PTR_MIN) {
        *region = HWDMA_UNMAPPED;
        return NULL;
    }
    for (i = 0; i < sizeof arr / sizeof arr[0]; i++) {
        if (within(addr, arr[i].base, arr[i].size)) {
            uint32_t off = addr - (uint32_t)(uintptr_t)arr[i].base;

            if (i == 0u && within(addr, io_regs() + REG_GXFIFO_OFFSET, HW_GXFIFO_BYTES)) {
                *region = HWDMA_GXFIFO;
                return NULL;
            }
            if (bytes > arr[i].size - off) {   /* runs off the end of its array */
                *region = HWDMA_UNMAPPED;
                return NULL;
            }
            *region = arr[i].reg;
            break;
        }
    }
    /* anything else must lie inside the application address space (the game's image, heaps, stacks = "main RAM"): an address one past an object, or a
       DS address that happens to be large, is UNMAPPED and counted, never copied (review 1.6, F2) */
    if (host_range(addr, bytes) == NULL) {
        *region = HWDMA_UNMAPPED;
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

/* Path 1 of the four GX input paths (bet 2.1a, hw_gx.h): a word DMA channel 3 moved into the GX FIFO. The other three paths reach hw_gx.c directly
   through the HWG3_* hooks. Nothing consumes the stream yet (bet 2.4 decodes it). */
static void gx_sink(void *u, uint32_t word)
{
    (void)u;
    HWGX_FifoWord(word, HWGX_PATH_DMA);
}

static int gx_block(void *u, const uint32_t *src, uint32_t n)
{
    (void)u;
    return HWGX_FifoBlock(src, n, HWGX_PATH_DMA);
}

/* A DMA copy into VRAM finished (bet 2.1b): the dirty bitmap of the banks that map the span. Other regions are not VRAM. */
static void dma_wrote(void *u, HwDmaRegion region, const void *dst, uint32_t bytes)
{
    (void)u;
    if (region == HWDMA_VRAM) {
        HWVRAM_Touch(dst, bytes);
    }
}

static void *arm7_xlat(void *u, uint32_t addr)
{
    (void)u;
    return host_range(addr, (uint32_t)sizeof(CARDiCommandArg));
}

static int s_rom_err_logged;

static int rom_pread(void *u, uint32_t off, void *dst, uint32_t len)
{
    int n;

    (void)u;
    assert(dst != NULL);
    if (!s_rom_tried) {
        s_rom_tried = 1;
        s_rom_file = hwctr_file_open(hw_sys_rom_path());
        if (s_rom_file == NULL) {
            HWLOG("ROM missing: %s (the card bus reads 0xFF)", hw_sys_rom_path());
        }
    }
    if (s_rom_file == NULL) {
        return -1;
    }
    n = hwctr_file_pread(s_rom_file, off, dst, len);
    if (n < 0 && !s_rom_err_logged) {
        s_rom_err_logged = 1;
        HWLOG("ROM read failed at 0x%08x (%u bytes): served as 0xFF, retried on the next access", (unsigned)off, (unsigned)len);
    }
    return n;
}

static void *flash_mem(void *u, uint32_t addr, uint32_t len)
{
    (void)u;
    return host_range(addr, len);
}

/* A backup request from the ARM9's CARD driver: the command block is plain host memory (a DS address of the host array). */
static int32_t card_request(void *u, unsigned req, void *cmd)
{
    (void)u;
    assert(cmd != NULL);
    return hwcard_flash_request(&s_card, req, (HwCardCmd *)cmd, flash_mem, NULL);
}

/* Bet 1.5: the ARM7 sound driver's glue (hw_snd7.c, with the prefixed ARM7 objects) is optional. Linked: the SOUND tag feeds its mailbox;
   not linked (tools that exercise other units, e.g. card_fs.3dsx): the weak reference is NULL and the tag swallows its words. */
extern HwSnd *hw_snd7_attach(HwPxi *pxi, uint8_t *io) __attribute__((weak));
extern void hw_snd7_timing(uint64_t *ticks, uint32_t *calls) __attribute__((weak));

/* a fresh loopback ARM7: every responder registered on s_pxi, the sound mailbox attached when present */
static int64_t rtc_secs(void *u)
{
    const HwCfg *c = hw_sys_cfg();

    (void)u;
    if (c->rtc_fixed_set) {   /* bet 2.1e, Rule 8: the fixed clock runs on DS vblanks (60/s), never on the host clock */
        return hwrtc_fixed_secs(c->rtc_fixed, hw_os_vblanks_total());
    }
    return hwctr_rtc_secs();
}

/* "fixed <epoch> -> DS y-m-d h:m:s (weekday n)": what the chip answers now. Empty (nothing to log) without rtc_fixed. Each HWLOG site records its first call only,
   so frame 0 and exit are two sites. */
static int rtc_fixed_text(char *out, size_t cap)
{
    const HwCfg *c = hw_sys_cfg();
    HwRtcFields f;

    if (!c->rtc_fixed_set) {
        return 0;
    }
    hwrtc_fields_from_secs(hwrtc_fixed_secs(c->rtc_fixed, hw_os_vblanks_total()), &f);
    snprintf(out, cap, "fixed %lld -> DS %04lld-%02u-%02u %02u:%02u:%02u (weekday %u, 0 = Sunday), %llu DS vblanks since power-on", (long long)c->rtc_fixed,
             (long long)f.year, f.mon, f.day, f.hour, f.min, f.sec, f.wday, (unsigned long long)hw_os_vblanks_total());
    return 1;
}

static uint8_t *syswork(void *u, unsigned which)
{
    OSSystemWork *w = OS_GetSystemWork();

    (void)u;
    assert(which <= 1u);
    return which == 0u ? (uint8_t *)w->real_time_clock : (uint8_t *)w->touch_panel;
}

static uint32_t wm_request(void *u, uint32_t cmd_addr)
{
    (void)u;
    return hw_wm7_request(cmd_addr);
}

/* the radio-less WM's queued replies (a scan answers after its channel time): sent on the WM tag from the frame hook */
static void wm_frame(void)
{
    uint32_t reply = hw_wm7_tick();

    if (reply != 0u) {
        s_arm7.wm_replies++;
        if (hwpxi_reply(&s_pxi, HWPXI_TAG_WM, reply, 0u) != 0) {
            HWLOG("WM queued reply %08x could not be sent (PXI FIFO full)", (unsigned)reply);
        }
    }
}

static void arm7_attach(void)
{
    HwArm7Ops aops = { .xlat = arm7_xlat, .card_req = card_request, .rtc_secs = rtc_secs,
                       .rtc_offset = hw_sys_cfg()->rtc_fixed_set ? 0 : hw_sys_cfg()->rtc_offset,
                       .syswork = syswork, .wm_req = wm_request };

    hw_wm7_reset();

    if (hw_snd7_attach != NULL) {
        aops.snd = hw_snd7_attach(&s_pxi, io_regs());
    }
    hwarm7_init(&s_arm7, &s_pxi, &aops);
    {
        char t[128];

        if (rtc_fixed_text(t, sizeof t)) {
            HWLOG("rtc: %s at frame 0", t);
        }
    }
}

/* Reads sdmc:/3ds/3DP/platinum.sav into the flash image (erased 0xFF when the file is missing). Returns the byte count, -1 when the file is missing. */
static int flash_load(void)
{
    int n;
    const char *from = hwwb_boot_path(HWCTR_SAVE_FILE, (uint32_t)sizeof s_flash);   /* bet 2.6c: adopts a stranded .tmp first (review 2.6 B2) */

    memset(s_flash, 0xff, sizeof s_flash);
    n = hwctr_file_read(from, s_flash, sizeof s_flash);
    if (n < 0) {
        HWLOG("no save file %s: backup flash starts erased (0xFF)", HWCTR_SAVE_FILE);
    } else {
        HWLOG("save %s loaded: %d bytes into the 512 KB flash image", HWCTR_SAVE_FILE, n);
    }
    return n;
}

/* the write counter the save write-back watches: every flash write and erase the CARD driver made (hw_card.c) */
static uint32_t flash_ops(void)
{
    return (uint32_t)(s_card.flash_writes + s_card.flash_erases);
}

static void card_boot(void)
{
    static const HwCardOps rops = { rom_pread, NULL };

    hwcard_init(&s_card, s_rom_ops_set ? &s_rom_ops : &rops, (const uint8_t *)io_regs());
    (void)flash_load();
    hwcard_flash_attach(&s_card, s_flash, sizeof s_flash, HW_FLASH_SECTOR);
    hwwb_init(s_flash, (uint32_t)sizeof s_flash, HWCTR_SAVE_FILE, flash_ops());   /* bet 2.6b: the write-back watches the same image the card reads */
}

/* Bet 2.6a: the save image as hw_sys_init needs it (BEFORE hw_devs_init, to adopt the save's console identity into the system page). Loads the file; the
   card's own load later reads the same file again. NULL when there is no save file. */
const uint8_t *hw_devs_save_image(size_t *len)
{
    if (flash_load() < 0) {
        return NULL;
    }
    if (len != NULL) {
        *len = sizeof s_flash;
    }
    return s_flash;
}

void hw_devs_set_rom_ops(const HwCardOps *ops)
{
    s_rom_ops_set = ops != NULL;
    if (ops != NULL) {
        s_rom_ops = *ops;
    }
    s_inited = 0;   /* the next hook re-creates every unit with the new reader */
}

const HwCard *hw_devs_card(void)
{
    return &s_card;
}

const uint8_t *hw_devs_flash(uint32_t *size)
{
    if (size != NULL) {
        *size = (uint32_t)sizeof s_flash;
    }
    return s_flash;
}

int hw_devs_rom_header(uint8_t *dst512)
{
    int n;

    assert(dst512 != NULL);
    hw_devs_init();
    n = s_card.ops.pread(s_card.ops.user, 0u, dst512, HWCARD_HEADER_BYTES);
    return n == (int)HWCARD_HEADER_BYTES ? 0 : -1;
}

static uint32_t hw_devs_unanswered(void)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < HWPXI_TAGS; i++) {
        n += s_pxi.unanswered[i];
    }
    return n;
}

/* DS vblanks the pump raised since power-on (bet 2.1e, review T-S1): every per-frame total in the log is "host frames" (gspWaitForVBlank returns) and the DS vblank
   count goes beside it, because the two diverge (a vblank raised at a non-waiting yield passes no host wait). */
static uint32_t ms10(double ticks);

static unsigned ds_vblanks(void)
{
    return (unsigned)hw_os_vblanks_total();
}

/* The GX input layer's summary (bet 2.1a): totals, average and peak words per frame per path, the canary, the trace window. */
static void gx_report(void)
{
    HwGxStats st;
    uint32_t fr;
    char first[24];

    HWGX_TraceStop();
    HWGX_Stats(&st);
    fr = st.frames != 0u ? st.frames : 1u;
    HWLOG("GX input: %u host frames / %u DS vblanks; words DMA %u CPUSEND %u SENDFIFO %u PORT %u (stray %u); per frame avg %u/%u/%u/%u peak %u/%u/%u/%u last %u/%u/%u/%u (DMA/CPUSEND/SENDFIFO/PORT)",
          (unsigned)st.frames, ds_vblanks(), (unsigned)st.total[0], (unsigned)st.total[1], (unsigned)st.total[2], (unsigned)st.total[3], (unsigned)st.stray,
          (unsigned)(st.total[0] / fr), (unsigned)(st.total[1] / fr), (unsigned)(st.total[2] / fr), (unsigned)(st.total[3] / fr),
          (unsigned)st.peak[0], (unsigned)st.peak[1], (unsigned)st.peak[2], (unsigned)st.peak[3],
          (unsigned)st.last[0], (unsigned)st.last[1], (unsigned)st.last[2], (unsigned)st.last[3]);
    if (st.canary_words != 0u) {
        snprintf(first, sizeof first, "0x%03x value %08x", (unsigned)st.canary_first_off, (unsigned)st.canary_first_val);
    } else {
        snprintf(first, sizeof first, "none");
    }
    HWLOG("GX canary: hits %u words in %u frames; first changed offset %s (0 hits = no store slipped past the hooks)", (unsigned)st.canary_words,
          (unsigned)st.canary_frames, first);
    if (st.canary_noffs != 0u) {
        char offs[8 * 16 + 1];
        uint32_t k, n = 0u;

        offs[0] = '\0';
        for (k = 0u; k < st.canary_noffs; k++) {
            n += (uint32_t)snprintf(offs + n, sizeof offs - n, " %03x=%x", (unsigned)st.canary_offs[k], (unsigned)st.canary_vals[k]);
        }
        HWLOG("GX canary: first distinct offsets:%s (%u of the words found at a sound-slice start)", offs, (unsigned)st.canary_at_slice);
    }
    if (st.trace_state != 0u) {
        uint32_t io = ms10((double)hw_sys_trace_io_ticks());

        HWLOG("gxtrace SD writes: %u.%u ms taken out of the game's clock (hw_os_time_steal, like the census)", (unsigned)(io / 10u), (unsigned)(io % 10u));
        HWLOG("gxtrace seal: %u frame(s) sealed at the snapshot point (after the vblank callback), %u at the next vblank hook (the game waited again first)",
              (unsigned)st.seals_snapshot, (unsigned)st.seals_next_vblank);
        HWLOG("gxtrace: state %u (3 = done); %u frames, %u entries; appended %u/%u/%u/%u; counters %u/%u/%u/%u; %u bytes",
              (unsigned)st.trace_state, (unsigned)st.trace_frames, (unsigned)st.trace_entries, (unsigned)st.trace_got[0], (unsigned)st.trace_got[1],
              (unsigned)st.trace_got[2], (unsigned)st.trace_got[3], (unsigned)st.trace_expect[0], (unsigned)st.trace_expect[1], (unsigned)st.trace_expect[2],
              (unsigned)st.trace_expect[3], (unsigned)st.trace_bytes);
    }
}

/* Once per frame, before the other per-frame work: close the GX input frame, publish the per-path counters (hwcount.txt) and watch the canary. */
#define GX_SAMPLE_FRAMES 100u
static void gx_frame(void)
{
    static uint32_t prev_canary, isum[4], imax[4];   /* the sample interval's per-path sum and max (bet 2.1e, review A-S3) */
    HwGxStats st;
    uint32_t hits, k;

    HWGX_FrameEnd();
    HWGX_Stats(&st);
    HWCOUNT_ADD("GX input: DMA words (path 1)", st.last[0]);
    HWCOUNT_ADD("GX input: CPUSEND words (MI_CpuSend32/Copy, path 2)", st.last[1]);
    HWCOUNT_ADD("GX input: SENDFIFO words (GX_SendFifo*, path 3)", st.last[2]);
    HWCOUNT_ADD("GX input: PORT words (reg_G3_* stores, path 4)", st.last[3]);
    hits = st.canary_words - prev_canary;
    prev_canary = st.canary_words;
    HWCOUNT_ADD("GX input: canary hits (words changed behind the hooks)", hits);
    if (hits != 0u && hits == st.canary_words) {
        HWLOG("GX canary: a store slipped past the hooks at I/O offset 0x%03x (value %08x)", (unsigned)st.canary_first_off, (unsigned)st.canary_first_val);
        hwtrace("gx: CANARY HIT at I/O offset 0x%03x value %08x", (unsigned)st.canary_first_off, (unsigned)st.canary_first_val);
    }
    for (k = 0u; k < 4u; k++) {   /* the game loop is 30 fps in DS time, so a single sampled frame can be the idle half: report the whole interval */
        isum[k] += st.last[k];
        if (st.last[k] > imax[k]) {
            imax[k] = st.last[k];
        }
    }
    if (st.frames % GX_SAMPLE_FRAMES == 0u) {
        hwtrace("gx: frames %u..%u sum dma=%u cpusend=%u sendfifo=%u port=%u; max/frame %u/%u/%u/%u", (unsigned)(st.frames - GX_SAMPLE_FRAMES + 1u),
                (unsigned)st.frames, (unsigned)isum[0], (unsigned)isum[1], (unsigned)isum[2], (unsigned)isum[3], (unsigned)imax[0], (unsigned)imax[1],
                (unsigned)imax[2], (unsigned)imax[3]);
        for (k = 0u; k < 4u; k++) {
            isum[k] = 0u;
            imax[k] = 0u;
        }
    }
}

/* ---- bet 2.4b: the geometry engine ----------------------------------------------------------------------------------------
   Every GX word (hw_gx.c HWGX_SetFeed) is stored in a small buffer, (address, word) in program order, and run through gpu3d.c when the buffer fills, when a DMA burst
   arrives in memory (run in place), when the game reads an engine register (HWG3_Sync), and at the DS vertical blank. A tick pair per flush, never per word, feeds the
   Rule 7 bucket "geometry engine" (the timers line). The engine is malloc'd at first use (Gpu3d is ~2.9 MB, never bss: the link layout the harness keys read stays put). */
#define G3_BUF 4096u
static Gpu3d *s_g3;
static uint32_t s_wide, s_stereo;   /* bet 2.8: config.ini wide / stereo (hw_devs_wide_cfg, below) */
static uint32_t s_slider_force;     /* bet 2.8: config.ini stereo_slider + 1 (0 = read the hardware slider) */
static struct { uint16_t addr[G3_BUF]; uint32_t word[G3_BUF]; } *s_g3buf;
static uint32_t s_g3n, s_g3dirty;
static struct { uint64_t ticks, words, flushes, blocks, syncs, vblanks, max_ticks, sync_ticks_flush, sync_ticks_publish, flush_ticks, block_ticks; } s_g3p;

static Gpu3d *g3(void)
{
    if (s_g3 == NULL) {
        s_g3 = (Gpu3d *)malloc(sizeof *s_g3);
        s_g3buf = malloc(sizeof *s_g3buf);
        if (s_g3 == NULL || s_g3buf == NULL) {
            HWLOG("geometry engine: out of memory (%u bytes)", (unsigned)sizeof *s_g3);
            abort();
        }
        gpu3d_reset(s_g3);
        gpu3d_wide_config(s_g3, s_wide, s_stereo, HWWIDE_GUARD_Q12);   /* bet 2.8: a no-op on the narrow path (wide = 0) */
#ifdef HW_G3_PROF
        s_g3->clock = hwctr_sys_tick;   /* the Rule 7 sub-buckets: `make HW_PROF=-DHW_G3_PROF` (a tick read costs ~370 ticks per command, so the default build leaves it off) */
#endif
        s_g3n = 0u;
        s_g3dirty = 1u;
    }
    return s_g3;
}

/* ---- bet 2.8: widescreen + stereo ------------------------------------------------------------------------------------------
   config.ini `wide` / `stereo` (hw_sys.c -> hw_devs_wide_cfg). The camera hooks (port-patches/port-wide-camera.patch, src/camera.c Camera_ComputeProjectionMatrix) widen the
   projection's arguments and register the matrix the game built, with the DS matrix's _00 / _11 the same camera builds from its unwidened arguments (the SDK's own
   MTX_PerspectiveW / MTX_OrthoW, the calls the game makes with wide = 0, so the values are the DS's bit for bit); with wide = 0 they return their argument and register
   nothing. A camera whose sinFovY x 5/4 would reach 1 is left narrow by all three (each decides from sinFovY alone: argument evaluation order is unspecified). Game thread. */
static uint32_t s_wide_regs, s_wide_sin_over;   /* projections registered / cameras too wide to widen */

void hw_devs_wide_cfg(uint32_t wide, uint32_t stereo, uint32_t slider_force)
{
    s_slider_force = slider_force > 101u ? 0u : slider_force;
    s_wide = wide != 0u ? 1u : 0u;
    s_stereo = (s_wide != 0u && stereo != 0u) ? 1u : 0u;
    if (stereo != 0u && s_wide == 0u) {
        HWLOG("wide: stereo = 1 needs wide = 1: stereo off");
    }
    if (s_wide != 0u) {
        HWLOG("wide: on (400x240, centre at %d,%d), stereo %u (guard %d/4096, iod %.0f world units at slider 1), slider %s%d", HWWIDE_X0, HWWIDE_Y0, (unsigned)s_stereo,
              HWWIDE_GUARD_Q12, (double)HWWIDE_IOD, s_slider_force != 0u ? "forced " : "hardware", s_slider_force != 0u ? (int)s_slider_force - 1 : 0);
    }
    if (s_g3 != NULL) {
        gpu3d_wide_config(s_g3, s_wide, s_stereo, HWWIDE_GUARD_Q12);
    }
}

uint32_t hw_devs_wide_on(void) { return s_wide; }
uint32_t hw_devs_stereo_on(void) { return s_stereo; }

int32_t HWDEV_WideSin(int32_t sin_fovy)
{
    int ok = 0;
    int32_t v;

    if (s_wide == 0u) {
        return sin_fovy;
    }
    v = hwwide_sin(sin_fovy, &ok);
    s_wide_sin_over += ok ? 0u : 1u;
    return v;
}

int32_t HWDEV_WideAspect(int32_t aspect, int32_t sin_fovy)
{
    int ok = 0;

    if (s_wide == 0u) {
        return aspect;
    }
    (void)hwwide_sin(sin_fovy, &ok);
    return ok ? hwwide_aspect(aspect) : aspect;
}

void HWDEV_WideProjection(const void *proj_mtx, int32_t ortho, int32_t sin_fovy, int32_t cos_fovy, int32_t aspect, int32_t near_clip, int32_t far_clip, int32_t distance)
{
    MtxFx44 ds;
    int ok = 0;

    if (s_wide == 0u || proj_mtx == NULL) {
        return;
    }
    (void)hwwide_sin(sin_fovy, &ok);
    if (!ok) {
        return;
    }
    if (ortho == 0) {
        MTX_PerspectiveW(sin_fovy, cos_fovy, aspect, near_clip, far_clip, FX32_ONE, &ds);
    } else {
        fx32 top = FX_Mul(FX_Div(sin_fovy, cos_fovy), distance), right = FX_Mul(top, aspect);

        MTX_OrthoW(top, -top, -right, right, near_clip, far_clip, FX32_ONE, &ds);
    }
    gpu3d_wide_register(g3(), (const int32_t *)proj_mtx, ds._00, ds._11, distance);
    s_wide_regs++;
}

static void g3_flush(void)
{
    uint64_t t0, dt;
    uint32_t i, n = s_g3n;

    if (n == 0u) {
        return;
    }
    s_g3n = 0u;
    t0 = hwctr_sys_tick();
    for (i = 0u; i < n; i++) {
        (void)gpu3d_entry(s_g3, s_g3buf->addr[i], s_g3buf->word[i]);
    }
    dt = hwctr_sys_tick() - t0;
    s_g3p.ticks += dt;
    s_g3p.flush_ticks += dt;
    s_g3p.words += n;
    s_g3p.flushes++;
    if (dt > s_g3p.max_ticks) {
        s_g3p.max_ticks = dt;
    }
    s_g3dirty = 1u;
}

static void g3_word(uint32_t addr, uint32_t word)
{
    (void)g3();
    s_g3buf->addr[s_g3n] = (uint16_t)addr;
    s_g3buf->word[s_g3n] = word;
    if (++s_g3n == G3_BUF) {
        g3_flush();
    }
}

static void g3_block(const uint32_t *w, uint32_t n)
{
    uint64_t t0, dt;
    uint32_t i;

    (void)g3();
    g3_flush();   /* program order: the words buffered before the burst run first */
    t0 = hwctr_sys_tick();
    for (i = 0u; i < n; i++) {
        (void)gpu3d_entry(s_g3, 0u, w[i]);
    }
    dt = hwctr_sys_tick() - t0;
    s_g3p.ticks += dt;
    s_g3p.block_ticks += dt;
    s_g3p.words += n;
    s_g3p.blocks++;
    if (dt > s_g3p.max_ticks) {
        s_g3p.max_ticks = dt;
    }
    s_g3dirty = 1u;
}

static void io_put32(uint32_t off, uint32_t v)
{
    memcpy(io_regs() + off, &v, sizeof v);
}

static uint32_t io_get32(uint32_t off)
{
    uint32_t v;

    memcpy(&v, io_regs() + off, sizeof v);
    return v;
}

/* The engine's registers into the I/O block, called by the SDK right before it reads one (hw_dev_hooks.h). GXSTAT keeps the FIFO-interrupt bits the game set. */
static void g3_publish(void)
{
    uint8_t *io = io_regs();
    Gpu3d *g = g3();
    uint32_t i, d3;
    uint16_t h;

    if (s_g3dirty == 0u) {
        return;
    }
    s_g3dirty = 0u;
    io_put32(GPU3D_REG_GXSTAT, gpu3d_read(g, GPU3D_REG_GXSTAT));
    for (i = 0u; i < 4u; i++) {
        io_put32(GPU3D_REG_POS_RESULT + 4u * i, gpu3d_read(g, GPU3D_REG_POS_RESULT + 4u * i));
    }
    for (i = 0u; i < 3u; i++) {
        h = (uint16_t)g->vec_res[i];
        memcpy(io + GPU3D_REG_VEC_RESULT + 2u * i, &h, sizeof h);
    }
    for (i = 0u; i < 16u; i++) {
        io_put32(GPU3D_REG_CLIPMTX + 4u * i, gpu3d_read(g, GPU3D_REG_CLIPMTX + 4u * i));
    }
    for (i = 0u; i < 9u; i++) {
        io_put32(GPU3D_REG_VECMTX + 4u * i, gpu3d_read(g, GPU3D_REG_VECMTX + 4u * i));
    }
    h = (uint16_t)g->npoly;
    memcpy(io + 0x604u, &h, sizeof h);
    h = (uint16_t)g->nvtx;
    memcpy(io + 0x606u, &h, sizeof h);
    memcpy(&h, io + 0x60u, sizeof h);
    h = (uint16_t)((h & ~(3u << 12)) | gpu3d_disp3dcnt_flags(g));   /* bits 12 (RDLINES underflow, never raised: no scanline-fetch model) and 13 (RAM overflow) are write-1-to-clear flags, never the game's stored bits */
    memcpy(io + 0x60u, &h, sizeof h);
    memcpy(&h, io + 0x540u, sizeof h);   /* DISP_1DOT_DEPTH, a plain register the engine reads when it culls */
    d3 = h;
    gpu3d_write_1dot(g, d3);
}

void HWG3_Sync(void)
{
    uint64_t t0;

    s_g3p.syncs++;
    (void)g3();
    t0 = hwctr_sys_tick();
    g3_flush();
    s_g3p.sync_ticks_flush += hwctr_sys_tick() - t0;
    t0 = hwctr_sys_tick();
    g3_publish();
    s_g3p.sync_ticks_publish += hwctr_sys_tick() - t0;
}

/* GXSTAT or DISP3DCNT was just written: the game's value (it read the published one, so a write-1-to-clear bit it did not mean to touch is still the hardware's rule). */
void HWG3_StatWrite(uint32_t off)
{
    Gpu3d *g = g3();
    uint32_t v;

    g3_flush();
    if (off == GPU3D_REG_GXSTAT) {
        v = io_get32(GPU3D_REG_GXSTAT);
        gpu3d_write_gxstat(g, v);
    } else {
        v = io_get32(0x60u) & 0xffffu;
        gpu3d_write_disp3dcnt(g, v);
    }
    HWGX_StatTrace(off, v);   /* trace-only (slice 2.4c): the replay applies the same write */
    s_g3dirty = 1u;
    g3_publish();
}

/* a trace window just opened: seed it with the FIFO-interrupt bits the game set before (the replay engine starts from reset) */
static void g3_trace_open(void)
{
    HWGX_StatTrace(GPU3D_REG_GXSTAT, gpu3d_read(g3(), GPU3D_REG_GXSTAT) & 0xc0000000u);
}

/* the DS vertical blank (the pump): the swapped list becomes the one on show and the engine's busy bit ends */
void hw_devs_ds_vblank(void)
{
    uint64_t t0;

    if (s_g3 == NULL) {
        return;
    }
    t0 = hwctr_sys_tick();
    g3_flush();
    gpu3d_vblank(s_g3);
    s_g3dirty = 1u;
    s_g3p.ticks += hwctr_sys_tick() - t0;
    s_g3p.vblanks++;
}

const struct Gpu3d *hw_devs_gpu3d(void)
{
    return s_g3;
}

/* ---- wall-time instrument and the census cost model (bet 2.1c2) -------------------------------------------------------------
   Measured on Azahar: the census decoding every GX word as it arrived cost ~385 ns per word, ~11 ms per
   field iteration, which pushed the loop body past one vblank and doubled the game's per-vblank work (main.c:154, virtual time is real-time driven). So:
   a DMA burst (87 % of the words) is decoded in place by one HWCEN_GxBlock call (hw_dma.c gx_block -> HWGX_FifoBlock -> cen_gx_block: no per-word call, no
   copy, parameter words of undecoded commands skipped per command); the other paths store the word into a buffer that HWCEN_GxBlock decodes at frame end or
   when it fills. Every tick the census spends is measured with hwctr_sys_tick (a tick pair per burst / flush / frame end / dump, never per word) and
   taken out of the game's virtual clock (hw_os_time_steal), so a census-on run feels the same time as a census-off run; the store is calibrated once at
   enable and charged per buffered word. Ticks are svcGetSystemTick (268 111 856 Hz). */
#define TICKS_PER_MS 268111.856
#define CEN_BUF      8192u   /* FIFO words buffered before a flush (32 KB) */
static struct {
    uint64_t last, sum, peak;      /* frame wall: previous input_frame tick, sum / peak of the gaps */
    uint64_t eng_prev, eng_sum;    /* slice (c) timing rule: the engine's ticks (s_g3p.ticks) at the previous input_frame, and the sum taken out of the frame wall */
    uint32_t frames;               /* gaps counted */
    uint32_t eng_clamped;          /* review N3: frames whose engine ticks exceeded the frame wall (preemption or an under-count) and were clamped to it */
    uint64_t gx_ticks, gx_words, port_calls;   /* decode passes (HWCEN_GxBlock); FIFO words decoded; command-port stores */
    uint64_t fe_ticks, dump_ticks, stolen, store_ticks;
    uint64_t vs_ticks, vs_calls, vs_bytes, vs_max;   /* bet 2.3d, Rule 7 bucket "vram snapshot": ticks inside gpu2d_frame_begin (palettes + OAM + the bound VRAM copy), calls, VRAM bytes copied (sum / largest frame) */
    uint64_t hb_ticks, hb_calls;   /* bet 2.3a, Rule 7 bucket "hblank dispatch": ticks inside hwpump_hblank_frame, and snap calls (192 per full frame, 1 per short-cut frame) */
} s_prof;
static unsigned s_census_on;
static uint32_t s_cen_buf[CEN_BUF], s_cen_n;
static uint32_t s_store_x16;       /* calibrated ticks per buffered word, x16 */
static uint32_t s_tick_cost;       /* calibrated ticks one hwctr_sys_tick read costs the caller */

/* ---- bet 2.3a: the per-line 2D record and the HBlank dispatch; bet 2.3c: the render thread; bet 2.3d: the self-contained record ------------------------
   Two Gpu2dFrame slots (malloc'd at the first snapshot point, ~1.2 MB each, never bss so the link layout the harness keys read is unchanged). The game thread records
   the frame into slot `cur` (palettes, OAM, VRAMCNT, the bound VRAM, then the pump's 192 HBlank lines), then OFFERS it (hwscr_pipe_offer): taken = the render thread
   composites it (gpu2d_render, layer3d NULL) and presents both engines on the two screens while the game thread runs the next frame; refused (the previous frame is
   still in the renderer) = an OVERRUN, counted, the frame dropped, never queued. The record owns a COPY of every VRAM view the renderer reads, taken at the snapshot
   point on the game thread (bet 2.3d, review B1): the earlier argument "the game writes VRAM only in its vblank callback, which runs before the snapshot point" was
   false. The main loop (port/src/main.c:137-196) runs RunApplication / mainTaskMgr / printTaskMgr and the post-vblank tasks outside the callback, and writes VRAM
   immediately: Window_CopyToVRAM (render_text.c:126,251; text.c:219,242; 72 files) through Bg_LoadTiles to GX_LoadBGxChar, Bg_CopyTilemapBufferToVRAM through
   LoadBgVRAMScr to GX_LoadBGxScr (bg_window.c:771-845), GX_LoadOBJ / GXS_LoadOBJ (graphics.c, palette.c), and every GX_SetBank* during an application init is a
   remap memcpy of up to 128 KB (hw_vram.c HWVRAM_SetCnt). So the render thread never reads live VRAM: gpu2d_render reads the record only, and the render thread and
   the game thread share nothing but the slot, which `busy` hands over. The render thread owns Gpu2dOut and every libctru framebuffer call (hw_ctr.c holds the lock). */
static uint32_t s_hb_irqs_seen;
static Gpu2dFrame *s_g2d;   /* the slot recorded last (read by the gxtrace seal right after the snapshot point) */
static Gpu2dFrame *s_g2d_slot[2];
typedef struct G2dMeta {
    uint32_t vb, a_on_top, dump;
} G2dMeta;
static G2dMeta s_g2d_meta[2];
static HwScrPipe s_pipe;
static Gpu2dOut *s_g2d_out;           /* the render thread's composite (0.5 MB, malloc'd with the slots) */
static void *s_rthread;
static void *s_rself;                 /* the render thread's own handle, never cleared */
static int s_rcore;
static uint8_t *s_fb_buf;             /* HWSCR_DUMP_SIZE bytes, only when fbdump is on */
static uint32_t s_fb_from, s_fb_count, s_fb_written, s_fb_failed, s_fb_waits, s_fb_gaveup;
/* ---- bet 2.8: the wide top screen on the render thread (allocated by render_start when config.ini wide = 1; NULL otherwise) ---- */
static uint32_t *s_wtop[2];           /* the composed 400 x 240 top images: mono / left, right */
static uint32_t *s_crop;              /* the DS 256 x 192 centre of the wide 3D layer: engine A's BG0 input (gpu2d reads 256-wide rows) */
static Gpu2dOut *s_g2d_out_r;         /* the right eye's engine A (stereo only) */
static uint8_t *s_wfb_buf;            /* HWSCR_WDUMP_SIZE, only with fbdump */
static struct {
    uint32_t wide, stereo, mono_slider0, narrow, eye_fail, wdump_written, wdump_failed;   /* frames presented wide (of them stereo / mono because the slider was 0), presented narrow on a wide run, eyes refused */
    int slider_min, slider_max;
    float sigma_max;
} s_wst = { 0, 0, 0, 0, 0, 0, 0, 101, -1, 0.0f };

static void wide_crop(const uint32_t *layer_w)
{
    uint32_t y;

    for (y = 0u; y < GPU2D_LINES; y++) {
        memcpy(s_crop + y * GPU2D_W, layer_w + (y + HWWIDE_Y0) * HWWIDE_W + HWWIDE_X0, GPU2D_W * sizeof *s_crop);
    }
}

static int slider_pct_now(void)
{
    return s_slider_force != 0u ? (int)s_slider_force - 1 : hwctr_slider_pct();
}

static struct {
    uint64_t render, present, dump;   /* ticks, render thread only */
    uint64_t wall, walled;            /* bet 2.5(d) N4 (report only): the render thread's wall per PRESENTED frame (3D raster + readbacks + de-tile + edge + split, gpu2d, present; the fbdump write is not in it) and the frames counted */
    uint64_t wait;                    /* ticks the game thread waited for a dump-window frame (stolen from the game clock) */
} s_rprof;

/* ---- bet 2.5b: the 3D half of the frame record --------------------------------------------------------------------------------
   Two Gpu3dRec slots next to the two Gpu2dFrame slots (same index, handed over by the same pipe offer). The game thread fills slot `cur` at the snapshot point: the engine's
   sealed list as PLY1 bytes (gpu3d_export_list), then the texture plan (gpu3d_rec_textures: keys, slots, decoded texels of the misses). The render thread rasterises it
   (pica_render_record) and points the 2D record's layer3d at the result before gpu2d_render. The game thread never calls citro3d; the render thread never reads live
   engine or VRAM state. The texture cache mirror is committed only when the offer was TAKEN (an overrun drops the record, and then nothing it planned happened). */
static Gpu3dRec *s_g3r[2];
static uint8_t *s_g3r_scratch;           /* G3R_SCRATCH_BYTES (0x98000): the referenced bytes gathered out of the slot views */
static G3rMirror s_g3r_mirror;           /* the cache as the render thread will have it once every taken record is rendered */
static int s_g3r_ready, s_g3r_failed;
static uint32_t s_pd_from, s_pd_count;   /* config.ini `polydump = FROM:COUNT` (emu-poly-export) */
static uint32_t s_pd_written, s_pd_failed, s_pd_tex;
static struct {
    uint64_t export_ticks, hash_ticks, decode_ticks;   /* game thread: gpu3d_export_list; gather + content keys; texture decodes */
    uint64_t records, list_bytes, rec_bytes, texel_bytes, hashed_bytes, lookups, hits, decodes, keys;
    uint64_t unsupported, full;                        /* compressed textures drawn untextured; tex/cache/pool full */
    uint64_t export_max;
} s_g3x;
static struct {
    uint64_t raster, readback, convert, frames;        /* render thread: pica_render_record minus the readback; the display transfer; the layer-word conversion */
    uint64_t dsrb, edge;                               /* bet 2.5c: the depth/stencil GX_TextureCopy; the Morton de-tile + the edge pass */
    uint64_t split;                                    /* bet 2.5(d): the extra work of the frame split (first frame end, second begin + rewrite quad, second depth copy) */
    int init_rc;
} s_pr = { 0, 0, 0, 0, 0, 0, 0, 1 };   /* init_rc 1 = pica_init not yet called */
/* ---- bet 2.7a: the budget distributions (hw_budget.h) ---------------------------------------------------------------------------------------------
   One HwBud per bucket, written by ONE thread. Game thread (sampled per event of its own): frame = the host frame wall minus the engine and the renderer wait
   (the TIMING gate's number), eng = the geometry engine per host frame, snd = the ARM7 sound slice per host frame, vram = the vram snapshot per snapshot point,
   hblank = the HBlank dispatch per snapshot point, hash = the texture hashing per 3D record. Render thread (sampled per job): raster .. split per 3D frame,
   gpu2d (the composite, which runs INSIDE gpu2d_render per line and cannot be timed apart), present, rwall = the whole job. */
static struct {
    HwBud frame, busy, eng, snd, vram, hblank, hash;
    HwBud raster, readback, dsrb, detile, edge, split, convert, gpu2d, present, rwall;
    HwBud eye2, wide;   /* bet 2.8 (appended, wide runs only): the right eye's whole PICA frame; the wide work (centre crops, the right eye's engine A, both wide composes) */
} s_bud;
#define FPS_WINDOW 600u
static struct {
    HwCtrInfo info;
    uint32_t mhz_start, mhz_exit, linear_start;
    uint32_t cfg_render_core, cfg_hwcheck, cfg_clock_frame;
    int have_info, slider_max, game_first, game_last, render_ran, wd_core;
    char bench[24];
    int fps_state;                       /* 0 not landed, 1 open, 2 closed at FPS_WINDOW DS vblanks, 3 closed early (the run ended first) */
    uint64_t fps_t0, fps_t1;
    uint32_t fps_h0, fps_h1, fps_vb0, fps_vb1, fps_p0, fps_p1;
    uint32_t fps_pt0, fps_pt1, fps_wo0, fps_wo1, fps_ro0, fps_ro1;   /* B2: snapshot points, wall pacing overruns, render overruns at the window's open and close */
    uint64_t snd_prev;
    uint32_t probe_ticks;   /* ticks of one per-frame probe set (3D slider read + core id + sound-slice read), measured at the start */
    int hc_state;                        /* hwcheck: 0 not run, 1 ran, -1 could not run */
    PicaSelfcheck hc;
} s_hw27 = { .cfg_render_core = 2, .game_first = -1, .game_last = -1, .render_ran = -1, .wd_core = -1 };

static uint32_t bud32(uint64_t ticks)
{
    return ticks > 0xffffffffull ? 0xffffffffu : (uint32_t)ticks;
}
static const uint8_t *g3r_tex_slot(uint32_t k) { return HWVRAM_TexSlot(k); }
static const uint8_t *g3r_tex_pltt(uint32_t k) { return HWVRAM_TexPltt(k); }

static uint32_t s_uns_frames, s_uns_sum, s_uns_mask;   /* bet 2.3d (review N5): GPU2D_UNSUPPORTED seen by the renderer; render thread only, read after hw_devs_render_stop */
static uint32_t s_rsync;
static uint64_t s_idle_seen;   /* hwctr_idle_ticks() at the previous input frame (2.7(b) N2) */
static uint64_t s_wait_seen;   /* s_rprof.wait at the previous input frame */

void hw_devs_rendersync(uint32_t on)
{
    s_rsync = on;
}

void hw_devs_fbdump(uint32_t from, uint32_t count)
{
    s_fb_from = from;
    s_fb_count = count;
}

void hw_devs_polydump(uint32_t from, uint32_t count)
{
    s_pd_from = from;
    s_pd_count = count;
    if (count != 0u) {
        hwctr_mkdirs(HWCTR_APP_DIR "/poly");
    }
}

static uint16_t io_get16(uint32_t off)
{
    uint16_t v;

    memcpy(&v, io_regs() + off, sizeof v);
    return v;
}

/* the first record: two Gpu3dRec slots (list buffer + texel pool each, ~2.8 MB), the gather scratch; malloc'd, never bss (the link layout the harness keys read stays put) */
static int g3r_start(void)
{
    uint32_t i;

    for (i = 0u; i < 2u; i++) {
        Gpu3dRec *r = (Gpu3dRec *)calloc(1u, sizeof *r);

        if (r == NULL) {
            return 0;
        }
        r->list_cap = s_wide != 0u ? G3R_WIDE_LIST_MAX : G3R_LIST_MAX;   /* bet 2.8: a wide list holds up to 4096 polygons / 40960 vertices */
        r->list = (uint8_t *)malloc(r->list_cap);
        r->texels = (uint32_t *)malloc((size_t)G3R_POOL_WORDS * 4u);
        r->texel_cap = G3R_POOL_WORDS;
        if (r->list == NULL || r->texels == NULL) {
            return 0;
        }
        s_g3r[i] = r;
    }
    s_g3r_scratch = (uint8_t *)malloc(G3R_SCRATCH_BYTES);
    if (s_g3r_scratch == NULL) {
        return 0;
    }
    gpu3d_rec_mirror_init(&s_g3r_mirror);
    return 1;
}

/* FNV-1a 64 over the flat texture memory then the flat palette memory: the name of the TEX1 file and the header hash of a polydump list (the harness writes the same) */
static uint64_t fnv64(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

static void g3r_polydump(const Gpu3dRec *r, uint32_t vb)
{
    static uint64_t seen[16];
    static uint32_t nseen;
    char path[80];
    uint64_t h;
    uint32_t k, i, off, len;
    int wrote_all = 1, have = 0;

    /* the whole flat views, gathered out of the slot views */
    for (k = 0u; k < 4u; k++) {
        const uint8_t *v = HWVRAM_TexSlot(k);

        if (v != NULL) {
            memcpy(s_g3r_scratch + (size_t)k * 0x20000u, v, 0x20000u);
        } else {
            memset(s_g3r_scratch + (size_t)k * 0x20000u, 0, 0x20000u);
        }
    }
    for (k = 0u; k < 6u; k++) {
        const uint8_t *v = HWVRAM_TexPltt(k);

        if (v != NULL) {
            memcpy(s_g3r_scratch + G3R_TEXMEM_BYTES + (size_t)k * 0x4000u, v, 0x4000u);
        } else {
            memset(s_g3r_scratch + G3R_TEXMEM_BYTES + (size_t)k * 0x4000u, 0, 0x4000u);
        }
    }
    h = fnv64(0xcbf29ce484222325ull, s_g3r_scratch, G3R_SCRATCH_BYTES);
    for (k = 0u; k < 8u; k++) {
        r->list[0x2c + k] = (uint8_t)((k < 4u ? (uint32_t)h : (uint32_t)(h >> 32)) >> (8u * (k & 3u)));
    }
    snprintf(path, sizeof path, HWCTR_APP_DIR "/poly/poly_%06u.bin", (unsigned)vb);
    if (hwctr_file_write(path, r->list, r->list_len) != (int)r->list_len) {
        wrote_all = 0;
    }
    for (i = 0u; i < nseen; i++) {
        if (seen[i] == h) {
            have = 1;
        }
    }
    if (!have && nseen < 16u) {
        uint8_t *img = (uint8_t *)malloc(16u + G3R_SCRATCH_BYTES);

        seen[nseen++] = h;
        if (img != NULL) {
            memcpy(img, "TEX1", 4);
            off = 1u; memcpy(img + 4, &off, 4);
            len = G3R_TEXMEM_BYTES; memcpy(img + 8, &len, 4);
            len = G3R_PALMEM_BYTES; memcpy(img + 12, &len, 4);
            memcpy(img + 16, s_g3r_scratch, G3R_SCRATCH_BYTES);
            snprintf(path, sizeof path, HWCTR_APP_DIR "/poly/tex_%08x%08x.bin", (unsigned)(h >> 32), (unsigned)h);
            if (hwctr_file_write(path, img, 16u + G3R_SCRATCH_BYTES) != (int)(16u + G3R_SCRATCH_BYTES)) {
                wrote_all = 0;
            }
            s_pd_tex++;
            free(img);
        } else {
            wrote_all = 0;
        }
    }
    if (wrote_all) {
        s_pd_written++;
    } else {
        s_pd_failed++;
    }
}

/* The game thread's half of the record, at the snapshot point (bet 2.5b): the engine's sealed list as PLY1, then the texture plan. Engine A's 3D layer off (DISPCNT bit 3) or no
   engine yet = no list (list_len 0) and the render thread leaves layer3d NULL. The registers the list header carries live in the I/O image the game wrote. */
static void g3r_record(Gpu3dRec *r, uint32_t vb)
{
    Gpu3dExportRegs regs;
    G3rSrc src;
    uint64_t t0, t1;
    uint32_t i;
    size_t n;

    gpu3d_rec_reset(r);
    if (s_g3 == NULL || (io_get32(0x000u) & 8u) == 0u) {
        return;
    }
    memset(&regs, 0, sizeof regs);
    regs.key = vb;
    regs.disp3dcnt = io_get16(0x060u);
    regs.alpha_ref = io_get32(0x340u) & 0x1fu;
    regs.clear_color = io_get32(0x350u);
    regs.clear_depth = io_get32(0x354u);
    regs.fog_color = io_get32(0x358u);
    regs.fog_offset = io_get32(0x35cu) & 0x7fffu;
    for (i = 0u; i < 8u; i++) {
        regs.edge_color[i] = io_get16(0x330u + 2u * i);
    }
    for (i = 0u; i < 32u; i++) {
        regs.toon[i] = io_get16(0x380u + 2u * i);
    }
    t0 = hwctr_sys_tick();
    n = gpu3d_export_list(s_g3, &regs, r->list, r->list_cap);
    t1 = hwctr_sys_tick();
    s_g3x.export_ticks += t1 - t0;
    if (t1 - t0 > s_g3x.export_max) {
        s_g3x.export_max = t1 - t0;
    }
    if (n == 0u) {
        return;   /* more than the PLY1 image the record owns (cannot happen: the engine holds 2048 polygons and 20480 expanded vertices at most, 4096 / 40960 on a wide run) */
    }
    r->list_len = (uint32_t)n;
    r->key = vb;
    memset(&src, 0, sizeof src);
    src.tex_slot = g3r_tex_slot;
    src.tex_pltt = g3r_tex_pltt;
    src.scratch = s_g3r_scratch;
    src.clock = hwctr_sys_tick;
    (void)gpu3d_rec_textures(r, &s_g3r_mirror, &src);
    s_g3x.hash_ticks += r->st.hash_ticks;
    hwbud_add(&s_bud.hash, bud32(r->st.hash_ticks));
    s_g3x.decode_ticks += r->st.decode_ticks;
    s_g3x.records++;
    s_g3x.list_bytes += r->list_len;
    s_g3x.rec_bytes += gpu3d_rec_bytes(r);
    s_g3x.texel_bytes += r->st.texel_bytes;
    s_g3x.hashed_bytes += r->st.hashed_bytes;
    s_g3x.lookups += r->st.lookups;
    s_g3x.hits += r->st.hits;
    s_g3x.decodes += r->st.decodes;
    s_g3x.keys += r->st.keys;
    s_g3x.unsupported += r->st.unsupported;
    s_g3x.full += (uint64_t)r->st.tex_full + r->st.cache_full + r->st.pool_full;
}

static const uint8_t *g2d_window(uint32_t id) { return HWVRAM_Window(id); }
static const uint8_t *g2d_bg_ext(uint32_t e, uint32_t slot) { return HWVRAM_BgExtPltt(e, slot); }
static const uint8_t *g2d_obj_ext(uint32_t e) { return HWVRAM_ObjExtPltt(e); }

static void g2d_snap(void *u, uint32_t first, uint32_t count)
{
    (void)u;
    s_prof.hb_calls++;
    gpu2d_frame_lines(s_g2d, io_regs(), first, count);
}

const Gpu2dFrame *hw_devs_gpu2d(void)
{
    return s_g2d;
}

/* fbdump: one composited frame to sdmc:/3ds/3DP/fb/<vb>.bin (render thread; the SD time is measured and shown, the game clock only loses what the game waits) */
static void fbdump_write(uint32_t vb, uint32_t a_on_top)
{
    char path[64];

    if (s_fb_buf == NULL) {
        s_fb_failed++;
        return;
    }
    hwscr_dump_pack(s_fb_buf, vb, a_on_top, s_g2d_out);
    snprintf(path, sizeof path, HWCTR_APP_DIR "/fb/%u.bin", (unsigned)vb);
    if (hwctr_file_write(path, s_fb_buf, HWSCR_DUMP_SIZE) == (int)HWSCR_DUMP_SIZE) {
        s_fb_written++;
    } else {
        s_fb_failed++;
    }
}

/* bet 2.8: the wide top image(s) of a dump-window frame to fb/<vb>w.bin (hw_screen.h FBW1) */
static void wdump_write(const Gpu2dFrame *fr, uint32_t vb, int eyes, int spct)
{
    char path[64];

    if (s_wfb_buf == NULL) {
        s_wst.wdump_failed++;
        return;
    }
    hwscr_wdump_pack(s_wfb_buf, vb, s_wtop[0], eyes == 2 ? s_wtop[1] : NULL, pica_layer_wide(0u), eyes == 2 ? pica_layer_wide(1u) : NULL, pica_stats.clear_word, fr, (uint32_t)spct, eyes == 2 ? pica_stats.last_sigma : 0.0f);
    snprintf(path, sizeof path, HWCTR_APP_DIR "/fb/%uw.bin", (unsigned)vb);
    if (hwctr_file_write(path, s_wfb_buf, HWSCR_WDUMP_SIZE) == (int)HWSCR_WDUMP_SIZE) {
        s_wst.wdump_written++;
    } else {
        s_wst.wdump_failed++;
    }
}

extern const uint8_t gpu3d_pica_shbin[];
extern const uint32_t gpu3d_pica_shbin_size;

static void render_thread(void *arg)
{
    void *self = *(void *const *)arg;   /* the thread's own handle (s_rself, set before the first wake): hw_devs_render_stop clears s_rthread while this thread may still be on its way to the park */

    for (;;) {
        Gpu2dFrame *fr;
        G2dMeta m;
        uint32_t top;
        uint64_t t0, t1, t2, t3, tw0 = hwctr_sys_tick();

        const uint32_t *wl[2] = { NULL, NULL };   /* bet 2.8: this frame's wide 3D layers (mono / left, right) */
        int eyes = 0, spct = 0;
        uint64_t tw = 0u;

        m = s_g2d_meta[s_pipe.rslot];
        fr = s_g2d_slot[s_pipe.rslot];
        fr->layer3d = NULL;
        if (s_g3r[s_pipe.rslot] != NULL && s_g3r[s_pipe.rslot]->list_len != 0u) {   /* bet 2.5b: the port's own 3D layer, on the PICA, on this thread */
            const Gpu3dRec *rec = s_g3r[s_pipe.rslot];

            if (s_pr.init_rc == 1) {   /* first 3D frame: citro3d is initialised on the thread that uses it (after this, 0 = ready, other = failed and the layer stays NULL) */
                s_pr.init_rc = pica_init(gpu3d_pica_shbin, gpu3d_pica_shbin_size);
                if (s_pr.init_rc == 0 && s_hw27.cfg_hwcheck != 0u) {   /* bet 2.7a: the self-check runs once, before the first game frame is drawn */
                    s_hw27.hc_state = pica_selfcheck(s_hw27.cfg_hwcheck == 2u ? 1 : s_hw27.cfg_hwcheck == 3u ? 2 : 0, &s_hw27.hc) == 0 ? 1 : -1;
                }
            }
            if (s_pr.init_rc == 0) {
            uint64_t p0 = hwctr_sys_tick(), p1;
            uint64_t rb0 = pica_stats.readback_ticks, cv0 = pica_stats.convert_ticks, tf0 = pica_stats.transfer_ticks, ds0 = pica_stats.ds_ticks, ed0 = pica_stats.edge_ticks + pica_stats.detile_ticks, sp0 = pica_stats.split_ticks;
            uint64_t dt0 = pica_stats.detile_ticks;

            if (s_wide != 0u && s_wtop[0] != NULL) {   /* bet 2.8: stereo draws the left eye here (the existing buckets) and the right one below; slider 0 = one mono pass */
                spct = s_stereo != 0u ? slider_pct_now() : 0;
                eyes = spct > 0 ? 2 : 1;
            }
            if ((eyes == 0 ? pica_render_record(rec) : pica_render_eye(rec, eyes == 2 ? -1 : 0, (float)spct / 100.0f)) == 0) {
                p1 = hwctr_sys_tick();
                s_pr.convert += pica_stats.convert_ticks - cv0;
                s_pr.readback += pica_stats.transfer_ticks - tf0;
                s_pr.split += pica_stats.split_ticks - sp0;
                s_pr.raster += (p1 - p0) - (pica_stats.readback_ticks - rb0) - (pica_stats.split_ticks - sp0);
                s_pr.dsrb += pica_stats.ds_ticks - ds0;
                s_pr.edge += pica_stats.edge_ticks + pica_stats.detile_ticks - ed0;
                hwbud_add(&s_bud.convert, bud32(pica_stats.convert_ticks - cv0));
                hwbud_add(&s_bud.readback, bud32(pica_stats.transfer_ticks - tf0));
                hwbud_add(&s_bud.split, bud32(pica_stats.split_ticks - sp0));
                hwbud_add(&s_bud.raster, bud32((p1 - p0) - (pica_stats.readback_ticks - rb0) - (pica_stats.split_ticks - sp0)));
                hwbud_add(&s_bud.dsrb, bud32(pica_stats.ds_ticks - ds0));
                hwbud_add(&s_bud.detile, bud32(pica_stats.detile_ticks - dt0));
                hwbud_add(&s_bud.edge, bud32(pica_stats.edge_ticks + pica_stats.detile_ticks - ed0 - (pica_stats.detile_ticks - dt0)));
                s_pr.frames++;
                fr->layer3d = pica_layer_words();
                if (eyes != 0) {
                    uint64_t e0 = hwctr_sys_tick();

                    wl[0] = pica_layer_wide(0u);
                    if (wl[0] == NULL) {
                        eyes = 0;   /* a narrow list on a wide run (cannot happen: the engine exports every list wide) */
                    } else if (eyes == 2) {
                        if (pica_render_eye(rec, 1, (float)spct / 100.0f) == 0) {
                            wl[1] = pica_layer_wide(1u);
                            if (pica_stats.last_sigma > s_wst.sigma_max) {
                                s_wst.sigma_max = pica_stats.last_sigma;
                            }
                        } else {
                            s_wst.eye_fail++;
                            eyes = 1;
                        }
                        hwbud_add(&s_bud.eye2, bud32(hwctr_sys_tick() - e0));
                    }
                }
            } else if (eyes != 0) {
                s_wst.eye_fail++;
                eyes = 0;
            }
            }
        }
        if (eyes != 0) {
            uint64_t c0 = hwctr_sys_tick();

            wide_crop(wl[0]);
            fr->layer3d = s_crop;
            tw += hwctr_sys_tick() - c0;
        }
        t0 = hwctr_sys_tick();
        gpu2d_render(fr, s_g2d_out);
        t1 = hwctr_sys_tick();
        if (eyes != 0 && m.a_on_top == 0u) {
            eyes = 0;   /* engine A on the bottom screen: the bottom screen stays 256 x 192, the frame is presented narrow */
        }
        if (eyes != 0) {   /* bet 2.8: the wide top image(s); the right eye re-composes engine A over its own 3D layer */
            uint64_t c0 = hwctr_sys_tick();

            gpu2d_compose_wide(fr, s_g2d_out, wl[0], s_wtop[0]);
            if (eyes == 2 && wl[1] != NULL && s_g2d_out_r != NULL) {
                wide_crop(wl[1]);
                gpu2d_render_a(fr, s_g2d_out_r);
                gpu2d_compose_wide(fr, s_g2d_out_r, wl[1], s_wtop[1]);
            } else {
                eyes = 1;
            }
            tw += hwctr_sys_tick() - c0;
            hwbud_add(&s_bud.wide, bud32(tw));
        }
        if (s_wide != 0u) {
            if (eyes == 0) {
                s_wst.narrow++;
            } else {
                s_wst.wide++;
                s_wst.stereo += eyes == 2 ? 1u : 0u;
                s_wst.mono_slider0 += (s_stereo != 0u && spct == 0) ? 1u : 0u;
                if (s_stereo != 0u) {
                    s_wst.slider_min = spct < s_wst.slider_min ? spct : s_wst.slider_min;
                    s_wst.slider_max = spct > s_wst.slider_max ? spct : s_wst.slider_max;
                }
            }
        }
        if (s_g2d_out->unsupported != 0u) {   /* review N5: surface it to the hwlog / hwcount (the DETERMINISM gate), not only the fbdump header */
            s_uns_frames++;
            s_uns_sum += s_g2d_out->unsupported;
            s_uns_mask |= s_g2d_out->unsupported_mask;
        }
        hwscr_pipe_rendered(&s_pipe);
        top = m.a_on_top != 0u ? 0u : 1u;
        if (eyes != 0) {
            hwctr_gfx_present_wide(s_wtop[0], eyes == 2 ? s_wtop[1] : NULL, s_g2d_out->pix[1]);
        } else {
            hwctr_gfx_present(s_g2d_out->pix[top], s_g2d_out->pix[top ^ 1u]);
        }
        t2 = hwctr_sys_tick();
        if (m.dump != 0u) {
            fbdump_write(m.vb, m.a_on_top);
            if (eyes != 0) {
                wdump_write(fr, m.vb, eyes, spct);
            }
        }
        t3 = hwctr_sys_tick();
        hwbud_add(&s_bud.gpu2d, bud32(t1 - t0));
        hwbud_add(&s_bud.present, bud32(t2 - t1));
        hwbud_add(&s_bud.rwall, bud32(t2 - tw0));
        s_rprof.render += t1 - t0;
        s_rprof.present += t2 - t1;
        s_rprof.wall += t2 - tw0;
        s_rprof.walled++;
        s_rprof.dump += t3 - t2;
        hwscr_pipe_done(&s_pipe);
        hwctr_thread_park(self);   /* idle until the next offer (a kill ends the thread inside the park) */
    }
}

/* first snapshot point: the slots, the composite, the dump buffer, the render thread (it starts parked: the FIRST offer's wake enters render_thread, which then works first and parks after each job; the thread is created on config.ini render_core (default 2, the New 3DS extra core; 2 falls back to 1, then to the game's own core), see hwctr_thread_create_core) */
static int render_start(void)
{
    uint32_t i;

    for (i = 0; i < 2u; i++) {
        s_g2d_slot[i] = (Gpu2dFrame *)calloc(1u, sizeof(Gpu2dFrame));
        if (s_g2d_slot[i] == NULL) {
            return 0;
        }
    }
    s_g2d_out = (Gpu2dOut *)calloc(1u, sizeof(Gpu2dOut));
    if (s_g2d_out == NULL) {
        return 0;
    }
    if (s_fb_count != 0u) {
        s_fb_buf = (uint8_t *)malloc(HWSCR_DUMP_SIZE);
        hwctr_mkdirs(HWCTR_APP_DIR "/fb");
    }
    if (s_wide != 0u) {   /* bet 2.8: the wide composite's buffers (a failure leaves the run narrow, logged at exit as 0 wide frames) */
        s_wtop[0] = (uint32_t *)malloc((size_t)HWWIDE_W * HWWIDE_H * 4u);
        s_wtop[1] = (uint32_t *)malloc((size_t)HWWIDE_W * HWWIDE_H * 4u);
        s_crop = (uint32_t *)malloc((size_t)GPU2D_PIXELS * 4u);
        s_g2d_out_r = s_stereo != 0u ? (Gpu2dOut *)calloc(1u, sizeof(Gpu2dOut)) : NULL;
        s_wfb_buf = s_fb_count != 0u ? (uint8_t *)malloc(HWSCR_WDUMP_SIZE) : NULL;
        if (s_wtop[0] == NULL || s_wtop[1] == NULL || s_crop == NULL || (s_stereo != 0u && s_g2d_out_r == NULL)) {
            free(s_wtop[0]);
            s_wtop[0] = NULL;
        }
    }
    hwscr_pipe_init(&s_pipe);
    s_rthread = hwctr_thread_create_core(render_thread, &s_rself, 0x10000u, (int)s_hw27.cfg_render_core);   /* bet 2.7a: config.ini render_core (default 2); falls back 2 -> 1 -> the default core */
    if (s_rthread == NULL) {
        return 0;
    }
    s_rself = s_rthread;
    s_rcore = hwctr_thread_core(s_rthread);
    return 1;
}

/* A dump-window frame must not be dropped: wait (bounded) for the previous frame, and take that time out of the game's clock like the census / gxtrace I/O. */
static void fbdump_wait_idle(void)
{
    uint64_t t0 = hwctr_sys_tick();
    uint32_t spins = 0u;

    while (hwscr_pipe_busy(&s_pipe) != 0u && spins < 20000u) {   /* <= 20 s */
        hwctr_sleep_sys(268112u);   /* 1 ms */
        spins++;
    }
    if (spins != 0u) {
        uint64_t dt = hwctr_sys_tick() - t0;

        s_fb_waits++;
        s_rprof.wait += dt;
        hw_os_time_steal(dt);
    }
    if (hwscr_pipe_busy(&s_pipe) != 0u) {
        s_fb_gaveup++;
    }
}

/* The pump's HBlank lines for the frame whose vblank callback just ran: bind the record, run the 192 lines, time the bucket, offer the
   frame to the renderer. */
static void hblank_frame(void)
{
    static const HwPumpLineOps lo = { g2d_snap, NULL };
    static int s_started, s_failed;
    Gpu2dSrc src;
    uint64_t t0;
    uint32_t ran, vb, cur, in_window;
    const HwHblank *hb;

    if (!s_started && !s_failed) {
        s_started = render_start();
        s_failed = !s_started;
    }
    if (s_failed) {
        if (s_g2d_slot[0] == NULL) {
            s_g2d_slot[0] = (Gpu2dFrame *)calloc(1u, sizeof(Gpu2dFrame));
        }
        if (s_g2d_slot[0] == NULL) {
            return;
        }
        s_g2d = s_g2d_slot[0];   /* no renderer: the record alone (the pre-2.3c behaviour) */
    } else {
        s_g2d = s_g2d_slot[s_pipe.cur];
    }
    src.io = io_regs();
    src.pltt = g_hwpltt;
    src.oam = g_hwoam;
    src.window = g2d_window;
    src.bg_ext = g2d_bg_ext;
    src.obj_ext = g2d_obj_ext;
    src.window_chunks = HWVRAM_WindowChunks;
    t0 = hwctr_sys_tick();
    gpu2d_frame_begin(s_g2d, &src);   /* the vram snapshot: palettes, OAM, VRAMCNT and the bound VRAM copied into the slot (review B1) */
    {
        uint64_t dt = hwctr_sys_tick() - t0;

        s_prof.vs_ticks += dt;
        hwbud_add(&s_bud.vram, bud32(dt));
        s_prof.vs_calls++;
        s_prof.vs_bytes += s_g2d->vram_bytes;
        if (s_g2d->vram_bytes > s_prof.vs_max) {
            s_prof.vs_max = s_g2d->vram_bytes;
        }
    }
    t0 = hwctr_sys_tick();
    ran = hw_os_hblank_frame(&lo);
    t0 = hwctr_sys_tick() - t0;
    s_prof.hb_ticks += t0;
    hwbud_add(&s_bud.hblank, bud32(t0));
    hb = &hw_os_pump()->hb;
    HWCOUNT_ADD("gpu2d: HBlank frames (snapshot points reached)", 1u);
    if (ran != 0u) {
        HWCOUNT_ADD("gpu2d: HBlank frames dispatched in full (192 lines)", 1u);
    } else {
        HWCOUNT_ADD("gpu2d: HBlank frames short-cut (no HBlank IRQ enabled: one register image)", 1u);
    }
    HWCOUNT_ADD("gpu2d: HBlank lines dispatched", ran);
    HWCOUNT_ADD("gpu2d: HBlank IRQs raised", hb->irqs - s_hb_irqs_seen);
    s_hb_irqs_seen = hb->irqs;
    if (s_failed) {
        return;
    }
    vb = (uint32_t)hw_os_vblanks_total();
    in_window = s_fb_count != 0u && vb >= s_fb_from && vb - s_fb_from < s_fb_count;
    if (in_window || s_rsync != 0u) {
        fbdump_wait_idle();
    }
    cur = s_pipe.cur;
    if (s_g3 != NULL && (io_get32(0x000u) & 8u) != 0u && !s_g3r_ready && !s_g3r_failed) {
        s_g3r_ready = g3r_start();
        s_g3r_failed = !s_g3r_ready;
    }
    if (s_g3r_ready) {
        g3r_record(s_g3r[cur], vb);
        if (s_pd_count != 0u && vb >= s_pd_from && vb - s_pd_from < s_pd_count && s_g3r[cur]->list_len != 0u) {
            uint64_t d0 = hwctr_sys_tick(), dt;

            g3r_polydump(s_g3r[cur], vb);
            dt = hwctr_sys_tick() - d0;
            hw_os_time_steal(dt);   /* the SD time is not game cost (like fbdump / gxtrace) */
        }
    }
    s_g2d_meta[cur].vb = vb;
    s_g2d_meta[cur].a_on_top = hwscr_a_on_top(io_regs());
    s_g2d_meta[cur].dump = in_window ? 1u : 0u;
    if (hwscr_pipe_offer(&s_pipe) != 0u) {
        if (s_g3r_ready && s_g3r[cur]->list_len != 0u) {
            s_g3r_mirror = s_g3r[cur]->mirror;   /* the render thread took the record: the cache plan it carries now happens */
        }
        hwctr_thread_wake(s_rthread);
    }
}

void hw_devs_render_stop(void)
{
    uint32_t spins = 0u;

    if (s_rthread == NULL) {
        return;
    }
    while (hwscr_pipe_busy(&s_pipe) != 0u && spins < 5000u) {   /* <= 5 s: the last frame finishes (and its dump is written) */
        hwctr_sleep_sys(268112u);
        spins++;
    }
    s_hw27.render_ran = hwctr_thread_ran_on(s_rthread);
    hwctr_thread_kill(s_rthread);   /* parked between jobs: it ends inside its park */
    s_rthread = NULL;
}

uint32_t hw_devs_snapshot_point(void)
{
    hblank_frame();   /* the lines of this frame run first, so the gxtrace seal below carries them (header flag bit 2) */
    HWGX_Seal();
    return (uint32_t)hw_os_vblanks_total();
}


static uint32_t ms10(double ticks)   /* ticks -> tenths of a millisecond (no float printf on the SDK side) */
{
    return (uint32_t)(ticks * 10.0 / TICKS_PER_MS + 0.5);
}

static void cen_steal(uint64_t ticks)
{
    s_prof.stolen += ticks;
    hw_os_time_steal(ticks);
}

static void cen_flush(void)
{
    uint64_t t0, dt, st;

    if (s_cen_n == 0u) {
        return;
    }
    t0 = hwctr_sys_tick();
    HWCEN_GxBlock(s_cen_buf, s_cen_n);
    dt = hwctr_sys_tick() - t0 + s_tick_cost;
    st = ((uint64_t)s_cen_n * s_store_x16) >> 4;
    s_prof.gx_ticks += dt;
    s_prof.gx_words += s_cen_n;
    s_prof.store_ticks += st;
    s_cen_n = 0u;
    cen_steal(dt + st);
}

static void cen_gx_slow(uint32_t addr, uint32_t word)
{
    if (addr != 0u) {   /* a command-port store: its decoder state is separate from the FIFO's, so the order against the buffer does not matter */
        s_prof.port_calls++;
        HWCEN_GxWord(addr, word);
        return;
    }
    cen_flush();   /* the buffer is full */
    s_cen_buf[s_cen_n++] = word;
}

/* the hot path: one word, one store */
static void cen_gx(uint32_t addr, uint32_t word)
{
    if (addr == 0u && s_cen_n < CEN_BUF) {
        s_cen_buf[s_cen_n++] = word;
        return;
    }
    cen_gx_slow(addr, word);
}

/* A DMA burst already in memory: decoded in place, no per-word call and no copy. The words buffered before it are decoded first (program order). */
static void cen_gx_block(const uint32_t *w, uint32_t n)
{
    uint64_t t0, dt;

    cen_flush();
    t0 = hwctr_sys_tick();
    HWCEN_GxBlock(w, n);
    dt = hwctr_sys_tick() - t0 + s_tick_cost;
    s_prof.gx_ticks += dt;
    s_prof.gx_words += n;
    cen_steal(dt);
}

static void cen_vram(uint32_t bank, uint32_t value)
{
    HWCEN_VramCnt(bank, value);
}

/* The store path costs a call, a compare and a store per word and cannot be timed per word: time a run of them against an empty loop of the same shape. */
static void cen_calibrate(void)
{
    enum { N = 4096 };
    uint64_t t0, t1, t2;
    uint32_t i;
    void (*volatile obs)(uint32_t, uint32_t) = cen_gx;

    t0 = hwctr_sys_tick();
    for (i = 0; i < 64u; i++) {
        (void)hwctr_sys_tick();
    }
    s_tick_cost = (uint32_t)((hwctr_sys_tick() - t0) / 64u);   /* the last read ends the window: 64 reads + 1 */
    t0 = hwctr_sys_tick();
    for (i = 0; i < N; i++) {
        obs(0u, i);
        s_cen_n = 0u;
    }
    t1 = hwctr_sys_tick();
    for (i = 0; i < N; i++) {
        __asm__ volatile("" ::: "memory");
        s_cen_n = 0u;
    }
    t2 = hwctr_sys_tick();
    s_cen_n = 0u;
    s_store_x16 = (t1 - t0) > (t2 - t1) ? (uint32_t)((((t1 - t0) - (t2 - t1)) * 16u) / N) : 0u;
}

void hw_devs_census_enable(unsigned on)
{
    s_census_on = on != 0u;
    if (s_census_on) {
        HWCEN_Reset();   /* the census observes the GX words and the VRAMCNT bytes; it never changes them */
        cen_calibrate();
        HWGX_SetObserver(cen_gx);
        HWGX_SetBlockObserver(cen_gx_block);
        HWVRAM_SetObserver(cen_vram);
    }
}

/* The feature census text (bet 2.1c) -> hwcensus.txt: one whole-file write of a few KB every 300 frames and at exit, never per frame. */
static void census_dump(void)
{
    static char text[HWCEN_TEXT_MAX];
    uint64_t t0 = hwctr_sys_tick();
    size_t n = HWCEN_Format(text, sizeof text);

    hwctr_mkdirs(HWCTR_APP_DIR);
    if (hwctr_file_write(HWCENSUS_FILE, text, (uint32_t)n) < 0) {
        HWLOG("census: could not write %s", HWCENSUS_FILE);
    }
    t0 = hwctr_sys_tick() - t0;
    s_prof.dump_ticks += t0;
    if (s_census_on) {
        cen_steal(t0);
    }
}

/* The VRAM model's summary (bet 2.1b): hook calls vs the register census, overlaps, dirty pages, VRAMCNT stores that bypassed the hook. */
static uint32_t s_vram_bypass;
static void vram_report(void)
{
    HwVramStats st;

    HWVRAM_Stats(&st);
    HWLOG("VRAM hook: %u calls in %u host frames / %u DS vblanks (%u frames had any), peak %u per frame, %u changed a mapping", (unsigned)st.calls, (unsigned)st.frames, ds_vblanks(),
          (unsigned)st.frames_with_calls, (unsigned)st.calls_peak, (unsigned)st.changed);
    HWLOG("VRAM copies: flush %u B, load %u B, zero %u B; bank overlaps %u, undefined MST %u", (unsigned)st.flush_bytes, (unsigned)st.load_bytes,
          (unsigned)st.zero_bytes, (unsigned)st.overlaps, (unsigned)st.bad_mst);
    HWLOG("VRAM dirty: bulk-write touches %u, pages dirtied %u (peak %u per frame); VRAMCNT bytes that bypassed the hook %u", (unsigned)st.touches,
          (unsigned)st.pages_dirtied, (unsigned)st.dirty_peak, (unsigned)s_vram_bypass);
}

/* Once per frame: close the VRAM frame and publish its counters; compare each VRAMCNT register byte with the value the hook saw (0x247 is WRAMCNT). */
static void vram_frame(void)
{
    HwVramStats st;
    uint32_t b, bypass = 0u;
    const uint8_t *io = io_regs();

    HWVRAM_FrameEnd();
    HWVRAM_Stats(&st);
    HWCOUNT_ADD("VRAM: VRAMCNT hook calls (HWVRAM_SetCnt)", st.calls_last);
    HWCOUNT_ADD("VRAM: pages newly dirty (DMA / MIi_Cpu* writes + remaps)", st.dirty_last);
    for (b = 0; b < HWVRAM_BANKS; b++) {
        uint32_t reg = b < 7u ? 0x240u + b : 0x241u + b;   /* A-G at 0x240-0x246, H-I at 0x248-0x249 */

        bypass += io[reg] != HWVRAM_Cnt(b);
    }
    if (bypass != 0u) {
        s_vram_bypass += bypass;
        HWLOG("VRAM: %u VRAMCNT register byte(s) differ from the value the hook saw (a store that bypassed HWVRAM_SetCnt)", (unsigned)bypass);
    }
    HWCOUNT_ADD("VRAM: VRAMCNT bytes that bypassed the hook", bypass);
}

/* ---- bet 2.7a: the run header, the budget block, the fps window ----------------------------------------------------------------------------------
   Everything the hardware session needs in one hwlog: `hdr:` lines (model, launch kind, measured clock, thread placement, slider, linear memory), `budget:` lines
   (avg / p95 / max per bucket from s_bud, the counts, the fps window). All of it is wall time or machine state, so run.py lists the prefixes in WALL_LINES (the
   DETERMINISM compare ignores them); no HWCOUNT row is added. hwlog keeps one line per call site, so every budget line has its own site (BUDLINE). The fps window
   opens at the field landing (port_main.c trace_sink -> hw_devs_field_landed) and closes after FPS_WINDOW host frames. */

void hw_devs_hw27_cfg(uint32_t render_core, uint32_t hwcheck, uint32_t clock_frame)
{
    s_hw27.cfg_render_core = render_core;
    s_hw27.cfg_hwcheck = hwcheck;
    s_hw27.cfg_clock_frame = clock_frame;
}

void hw_devs_hw27_bench(const char *tag)
{
    assert(tag != NULL);
    snprintf(s_hw27.bench, sizeof s_hw27.bench, "%s", tag);
}

void hw_devs_header_start(void)
{
    hwctr_info(&s_hw27.info);
    s_hw27.have_info = 1;
    s_hw27.mhz_start = hwctr_measure_mhz100();
    {   /* what the per-frame probes of hw27_frame cost (the only per-frame additions of bet 2.7a that are not a bucket's own existing tick pair): 256 sets timed with one tick pair */
        uint64_t t0 = hwctr_sys_tick();
        volatile int sink;
        int k;

        for (k = 0; k < 256; k++) {
            sink = hwctr_slider_pct() + hwctr_proc_id();
        }
        (void)sink;
        s_hw27.probe_ticks = (uint32_t)((hwctr_sys_tick() - t0) / 256u);
    }
    s_hw27.linear_start = hwctr_linear_free();
    {   /* review 2.8 N7: what the narrow path pays for the wide-sized engine state (no gate measures it), so the hardware session sees the wide=0 headroom. hdr: is wall class (never compared) */
        uint32_t heap_total = 0u, heap_free = hwctr_heap_free(&heap_total);

        HWLOG("hdr: memory at boot: engine state sizeof(Gpu3d) %u KB (sizeof(Gpu3dPoly) %u bytes); heap free %u KB of %u KB; linear free %u KB",
              (unsigned)(sizeof(Gpu3d) / 1024u), (unsigned)sizeof(Gpu3dPoly), (unsigned)(heap_free / 1024u), (unsigned)(heap_total / 1024u), (unsigned)(s_hw27.linear_start / 1024u));
    }
    s_hw27.slider_max = s_hw27.info.slider_pct;
}

void hw_devs_note_watchdog(void)
{
    s_hw27.wd_core = hwctr_proc_id();
}

void hw_devs_field_landed(void)
{
    if (s_hw27.fps_state == 0) {
        s_hw27.fps_state = 1;
        s_hw27.fps_t0 = hwctr_sys_tick();
        s_hw27.fps_h0 = s_prof.frames;
        s_hw27.fps_vb0 = (uint32_t)hw_os_vblanks_total();
        s_hw27.fps_p0 = s_pipe.presented;
        s_hw27.fps_pt0 = s_pipe.points;
        s_hw27.fps_wo0 = (uint32_t)hw_os_pump()->overruns;
        s_hw27.fps_ro0 = s_pipe.overruns;
    }
}

static void hw27_fps_close(int state)
{
    s_hw27.fps_state = state;
    s_hw27.fps_t1 = hwctr_sys_tick();
    s_hw27.fps_h1 = s_prof.frames;
    s_hw27.fps_vb1 = (uint32_t)hw_os_vblanks_total();
    s_hw27.fps_p1 = s_pipe.presented;
    s_hw27.fps_pt1 = s_pipe.points;
    s_hw27.fps_wo1 = (uint32_t)hw_os_pump()->overruns;
    s_hw27.fps_ro1 = s_pipe.overruns;
}

/* The per-input-frame part: the slider's maximum, the core the game thread runs on, the sound slice of this frame, the fps window. */
static void hw27_frame(void)
{
    int sl = hwctr_slider_pct();
    int pid = hwctr_proc_id();
    uint64_t sticks = 0u;
    uint32_t scalls = 0u;

    if (sl > s_hw27.slider_max) {
        s_hw27.slider_max = sl;
    }
    if (s_hw27.game_first < 0) {
        s_hw27.game_first = pid;
    }
    s_hw27.game_last = pid;
    if (hw_snd7_timing != NULL) {
        hw_snd7_timing(&sticks, &scalls);
        hwbud_add(&s_bud.snd, bud32(sticks - s_hw27.snd_prev));
        s_hw27.snd_prev = sticks;
    }
    if (s_hw27.fps_state == 1 && (uint32_t)hw_os_vblanks_total() - s_hw27.fps_vb0 >= FPS_WINDOW) {
        hw27_fps_close(2);
    }
}

static void bud_us(const HwBud *b, unsigned out[3])
{
    out[0] = (unsigned)(hwbud_avg_ms(b) * 1000.0 + 0.5);
    out[1] = (unsigned)(hwbud_p95_ms(b) * 1000.0 + 0.5);
    out[2] = (unsigned)(hwbud_max_ms(b) * 1000.0 + 0.5);
}

/* one hwlog site per bucket (hwlog keeps one line per site): a table of sites, one per bucket, instead of a statement macro (2.7(b) N9, golden rule 8) */
enum { BUD_LINES = 19 };
static HwLogSite s_bud_sites[BUD_LINES];

static void budline(unsigned idx, const char *nm, const HwBud *b)
{
    unsigned u[3];
    HwLogSite *site = &s_bud_sites[idx];

    assert(idx < BUD_LINES);
    site->file = __FILE__;
    site->line = __LINE__;
    if (site->count == 0u) {
        site->slot = -1;
    }
    bud_us(b, u);
    if (hwbud_p95_over(b)) {   /* N1: the p95 is past the histogram range: say so, do not print the max as if it were the p95 */
        hwlog_emit(site, "budget: %-8s avg %u.%03u p95 >%u max %u.%03u ms n=%u", nm, u[0] / 1000u, u[0] % 1000u, (unsigned)HWBUD_RANGE_MS, u[2] / 1000u, u[2] % 1000u, (unsigned)b->n);
    } else {
        hwlog_emit(site, "budget: %-8s avg %u.%03u p95 %u.%03u max %u.%03u ms n=%u", nm, u[0] / 1000u, u[0] % 1000u, u[1] / 1000u, u[1] % 1000u, u[2] / 1000u, u[2] % 1000u,
                   (unsigned)b->n);
    }
}

static unsigned ratio100(uint32_t num, uint64_t ticks)   /* num per second, x 100 */
{
    return ticks == 0u ? 0u : (unsigned)((double)num * 100.0 * 268111856.0 / (double)ticks + 0.5);
}

static void hw27_report_header(void)
{
    const HwCtrInfo *in = &s_hw27.info;
    unsigned lin_exit = (unsigned)(hwctr_linear_free() / 1024u), lin_start = (unsigned)(s_hw27.linear_start / 1024u);
    int save_core = -1, save_req = -2;
    HwWbStats ws;

    hwwb_stats(&ws);
    save_req = ws.worker_core;
    save_core = ws.worker_ran_on;
    s_hw27.mhz_exit = hwctr_measure_mhz100();
    HWLOG("hdr: build %s; model %s; launch %s", APP_VERSION_STR, in->is_new3ds == 1 ? "New3DS" : in->is_new3ds == 0 ? "Old3DS" : "unknown",
          in->homebrew ? "3dsx (Homebrew Launcher)" : "cia (installed title)");
    HWLOG("hdr: 3D slider start %d%% max %d%%; linear free %u KB at start, %u KB at exit (%d KB)", in->slider_pct, s_hw27.slider_max, lin_start, lin_exit,
          (int)lin_exit - (int)lin_start);
    HWLOG("hdr: emu probe svcGetSystemInfo(0x20000,0) rc 0x%08lx v %lld -> emulator %d", (unsigned long)(uint32_t)in->emu_rc, (long long)in->emu_v, in->emu);
    HWLOG("hdr: clock requested 804 MHz (osSetSpeedupEnable + exheader); measured %u.%02u MHz at start, %u.%02u at exit; APT_SetAppCpuTimeLimit(30) rc 0x%08x",
          (unsigned)(s_hw27.mhz_start / 100u), (unsigned)(s_hw27.mhz_start % 100u), (unsigned)(s_hw27.mhz_exit / 100u), (unsigned)(s_hw27.mhz_exit % 100u), (unsigned)in->cpu_limit_rc);
    HWLOG("hdr: per-frame probes (3D slider read + core id) cost %u ticks = %u ns per frame; the bucket tick pairs are the existing ones", (unsigned)s_hw27.probe_ticks,
          (unsigned)((uint64_t)s_hw27.probe_ticks * 1000u / 268u));
    HWLOG("hdr: cfg clock %s rendersync %u render_core requested %u granted %d ran_on %d hwcheck %u bench %s", s_hw27.cfg_clock_frame ? "frame" : "wall", (unsigned)s_rsync,
          (unsigned)s_hw27.cfg_render_core, s_rcore, s_hw27.render_ran, (unsigned)s_hw27.cfg_hwcheck, s_hw27.bench[0] != '\0' ? s_hw27.bench : "-");
    HWLOG("hdr: threads (svcGetProcessorID, -1 = never ran) main %d, game %d..%d (first..last frame_hook), render %d, save worker %d (requested %d), watchdog %d", in->proc_id,
          s_hw27.game_first, s_hw27.game_last, s_hw27.render_ran, save_core, save_req, s_hw27.wd_core);
}

static void hw27_report_fps(void)
{
    if (s_hw27.fps_state == 1) {
        hw27_fps_close(3);
    }
    if (s_hw27.fps_state == 0) {
        HWLOG("budget: fps window never opened (no field landing in this run)");
        return;
    }
    {
        uint64_t dt = s_hw27.fps_t1 - s_hw27.fps_t0;
        uint32_t h = s_hw27.fps_h1 - s_hw27.fps_h0, v = s_hw27.fps_vb1 - s_hw27.fps_vb0, p = s_hw27.fps_p1 - s_hw27.fps_p0;
        unsigned hf = ratio100(h, dt), vf = ratio100(v, dt), pf = ratio100(p, dt);
        unsigned ms = (unsigned)(dt / 268112u);

        uint32_t pt = s_hw27.fps_pt1 - s_hw27.fps_pt0, wo = s_hw27.fps_wo1 - s_hw27.fps_wo0, ro = s_hw27.fps_ro1 - s_hw27.fps_ro0;

        HWLOG("budget: fps window %u DS vblanks in %u ms (%s): DS vblanks %u.%02u/s, host frames %u (%u.%02u/s, headroom only)", (unsigned)v, ms,
              s_hw27.fps_state == 2 ? "full window" : "PARTIAL: the run ended first", vf / 100u, vf % 100u, (unsigned)h, hf / 100u, hf % 100u);
        HWLOG("budget: fps gates presented %u (%u.%02u/s) of %u snapshot points, wall pacing overruns %u, render overruns %u", (unsigned)p, pf / 100u, pf % 100u,
              (unsigned)pt, (unsigned)wo, (unsigned)ro);
    }
}

static void hw27_report_hwcheck(void)
{
    const PicaSelfcheck *h = &s_hw27.hc;
    uint32_t k, stale = 0u, layout = 0u, first_stale = 0u, first_layout = 0u;
    const char *mut = s_hw27.cfg_hwcheck == 2u ? " (MUTANT: planted de-tile fault, FAIL expected)" : s_hw27.cfg_hwcheck == 3u ? " (MUTANT: planted missing depth merge, FAIL expected)" : "";

    if (s_hw27.cfg_hwcheck == 0u) {
        return;
    }
    if (s_hw27.hc_state != 1) {
        HWLOG("hwcheck: NOT RUN (%s)", s_hw27.hc_state == 0 ? "no 3D frame reached the renderer, or citro3d did not start" : "the check frames could not be rendered");
        return;
    }
    for (k = 1u; k < h->frames_run; k++) {
        if (h->code[k] == G3S_STALE && stale++ == 0u) first_stale = k;
        if (h->code[k] == G3S_LAYOUT && layout++ == 0u) first_layout = k;
    }
    if (h->code[0] == G3S_OK) {
        HWLOG("hwcheck d24s8: PASS (frame 0: stencil, depth and colour at the 4 probes as a D24S8 target holds them)%s", mut);
    } else {
        HWLOG("hwcheck d24s8: FAIL (frame 0: %s at probe %s (%u,%u) read %06x wanted %06x)%s", h->bad_what != NULL ? h->bad_what : "?", h->bad_probe != NULL ? h->bad_probe : "?",
              (unsigned)h->bad_x, (unsigned)h->bad_y, (unsigned)h->bad_got, (unsigned)h->bad_want, mut);
    }
    if (h->frames_run == G3S_FRAMES && stale == 0u && layout == 0u && h->code[0] == G3S_OK) {
        HWLOG("hwcheck readback: PASS (%u frames, each read back its own polygon IDs and colours, never the previous frame's)%s", (unsigned)h->frames_run, mut);
    } else {
        HWLOG("hwcheck readback: FAIL (%u of %u frames run; %u STALE (first at frame %u), %u wrong layout (first at frame %u), frame 0 %s)%s", (unsigned)h->frames_run, (unsigned)G3S_FRAMES,
              (unsigned)stale, (unsigned)first_stale, (unsigned)layout, (unsigned)first_layout, h->code[0] == G3S_OK ? "ok" : "failed", mut);
        if (h->bad_what != NULL) {   /* F-2: the first mismatch of the first failing frame, so a red check says WHAT differed */
            HWLOG("hwcheck readback: first mismatch at frame %u: %s at probe %s (%u,%u) read %06x wanted %06x", (unsigned)h->bad_frame, h->bad_what, h->bad_probe != NULL ? h->bad_probe : "?",
                  (unsigned)h->bad_x, (unsigned)h->bad_y, (unsigned)h->bad_got, (unsigned)h->bad_want);
        }
    }
    if (h->trans_run != 0u && h->code[G3S_TRANS] == G3S_OK) {
        HWLOG("hwcheck trans: PASS (the split frame: O stencil/depth/colour, O+T blended with the merged depth, T stored with the merged depth)%s", mut);
    } else {
        HWLOG("hwcheck trans: FAIL (%s at probe %s (%u,%u) read %06x wanted %06x)%s", h->trans_run == 0u ? "frame not run" : h->t_what != NULL ? h->t_what : "?",
              h->t_probe != NULL ? h->t_probe : "?", (unsigned)h->t_x, (unsigned)h->t_y, (unsigned)h->t_got, (unsigned)h->t_want, mut);
    }
}

static void hw27_report(void)
{
    hw27_report_header();
    hw27_report_hwcheck();
    budline(0, "frame", &s_bud.frame);
    budline(1, "busy", &s_bud.busy);
    budline(2, "engine", &s_bud.eng);
    budline(3, "sound", &s_bud.snd);
    budline(4, "vram", &s_bud.vram);
    budline(5, "hblank", &s_bud.hblank);
    budline(6, "hash", &s_bud.hash);
    budline(7, "raster", &s_bud.raster);
    budline(8, "readback", &s_bud.readback);
    budline(9, "dsrb", &s_bud.dsrb);
    budline(10, "detile", &s_bud.detile);
    budline(11, "edge", &s_bud.edge);
    budline(12, "split", &s_bud.split);
    budline(13, "convert", &s_bud.convert);
    budline(14, "gpu2d", &s_bud.gpu2d);
    budline(15, "present", &s_bud.present);
    budline(16, "rwall", &s_bud.rwall);
    if (s_wide != 0u) {   /* bet 2.8: appended buckets, wide runs only */
        budline(17, "eye2", &s_bud.eye2);
        budline(18, "wide", &s_bud.wide);
    }
    HWLOG("budget: counts host frames %u, DS vblanks %u, 3D records %u, 3D rendered %u, composited %u, presented %u", (unsigned)s_prof.frames, ds_vblanks(),
          (unsigned)s_g3x.records, (unsigned)s_pr.frames, (unsigned)s_pipe.composited, (unsigned)s_pipe.presented);
    HWLOG("budget: counts2 render overruns %u, wall pacing overruns %u, engine clamped %u", (unsigned)s_pipe.overruns, (unsigned)hw_os_pump()->overruns, (unsigned)s_prof.eng_clamped);
    HWLOG("budget: notes sound = per host frame; vram/hblank = per snapshot point; hash = per 3D record; raster..split = per 3D frame");
    HWLOG("budget: notes2 gpu2d includes the composite; rwall = per render job (wall)");
    HWLOG("budget: notes3 readback includes the GPU draw (the transfer waits for it); raster = CPU submit only; p95 >64 = past the 64 ms range");
    HWLOG("budget: notes4 busy = game thread work per host frame, engine in, vblank wait out; frame = wall - engine - renderer wait (idle in)");
    hw27_report_fps();
}

void hw_devs_report(void)
{
    hw_devs_init();
    HWLOG("SysTask lists (bet 2.2c F8, trace only): vblank list high-water %u of 128 slots here (the DS holds 32), post-vblank list high-water %u of 32",
          hwtask_high(0u), hwtask_high(1u));
    HWLOG("PXI words sent: PM %u FS %u SOUND %u RTC %u TOUCH %u OS %u MIC %u; unanswered %u; touch commands answered %u, words swallowed by OS/RTC/MIC %u; RTC commands %u; touch samples %u, auto pushes %u",
          (unsigned)s_pxi.sent[HWPXI_TAG_PM], (unsigned)s_pxi.sent[HWPXI_TAG_FS], (unsigned)s_pxi.sent[HWPXI_TAG_SOUND], (unsigned)s_pxi.sent[HWPXI_TAG_RTC],
          (unsigned)s_pxi.sent[HWPXI_TAG_TOUCH], (unsigned)s_pxi.sent[HWPXI_TAG_OS], (unsigned)s_pxi.sent[HWPXI_TAG_MIC], (unsigned)hw_devs_unanswered(),
          (unsigned)s_arm7.touch_commands, (unsigned)s_arm7.other_swallowed, (unsigned)s_arm7.rtc_commands, (unsigned)s_arm7.touch_samples,
          (unsigned)s_arm7.auto_pushes);
    hw_wm7_report((unsigned)s_pxi.sent[HWPXI_TAG_WM], (unsigned)s_arm7.wm_requests, (unsigned)s_arm7.wm_replies);
    HWLOG("DMA: started ch0 %u ch1 %u ch2 %u ch3 %u; GX FIFO words %u; IRQs raised %u; timed (non-immediate) starts %u; repeat ignored %u; unmapped %u",
          (unsigned)s_dma.started[0], (unsigned)s_dma.started[1], (unsigned)s_dma.started[2], (unsigned)s_dma.started[3], (unsigned)s_dma.gx_words,
          (unsigned)s_dma.irqs, (unsigned)s_dma.timed, (unsigned)s_dma.repeat_ignored, (unsigned)s_dma.unmapped);
    {
        char t[128];

        if (rtc_fixed_text(t, sizeof t)) {
            HWLOG("rtc: %s at exit", t);
        }
    }
    gx_report();
    vram_report();
    {   /* bet 2.6b: the save write-back. The counts are reproducible (hwcount.txt rows); the milliseconds are wall time and sit on the WALL_LINES list of run.py */
        HwWbStats ws;
        uint32_t wm, gm;

        hwwb_stats(&ws);
        HWCOUNT_ADD("save: flash write+erase operations (hw_card flash_writes + flash_erases)", flash_ops());
        HWCOUNT_ADD("save: write-back flushes coalesced (rounds that extended a pending flush)", ws.coalesced);
        HWCOUNT_ADD("save: backup-lock waits for a snapshot copy", ws.lock_waits);
        wm = (uint32_t)(ws.worker_ticks * 10u / 268112u);
        gm = (uint32_t)(ws.game_ticks * 100u / 268112u);
        HWLOG("save: %u lock / %u unlock hooks, %u flash writes + %u erases, %u write-backs ok, %u failed, %u rounds coalesced; original %s",
              (unsigned)ws.locks, (unsigned)ws.unlocks, (unsigned)s_card.flash_writes, (unsigned)s_card.flash_erases, (unsigned)ws.flushes_ok, (unsigned)ws.flushes_failed,
              (unsigned)ws.coalesced, ws.orig_copied == 1u ? "copied to .orig" : ws.orig_copied == 2u ? ".orig kept from an earlier run" : ws.orig_copied == 3u ? ".orig copy FAILED (not retried)" : "not needed");
        HWLOG("timers save: worker %u.%u ms in %u flush(es) (wall, worker thread, core %d); game thread %u.%02u ms in %u hook calls (lock + unlock + frame check; Rule 7 buckets)",
              (unsigned)(wm / 10u), (unsigned)(wm % 10u), (unsigned)(ws.flushes_ok + ws.flushes_failed), ws.worker_core, (unsigned)(gm / 100u), (unsigned)(gm % 100u), (unsigned)ws.game_calls);
    }
    {   /* bet 2.3a (Rule 6 answer for the path): was any HBlank IRQ ever enabled? */
        const HwHblank *hb = &hw_os_pump()->hb;

        HWLOG("gpu2d HBlank: %u snapshot points = %u short-cut (one register image) + %u full (%u lines, %u IRQs raised); DISPSTAT bit 4 set at %u, IE bit 1 set at %u",
              (unsigned)hb->frames, (unsigned)hb->shortcut, (unsigned)hb->full, (unsigned)hb->lines, (unsigned)hb->irqs, (unsigned)hb->ds_frames, (unsigned)hb->ie_frames);
    }
    {   /* bet 2.3c: the screens. Counts only (reproducible under the frame clock): composited == vblanks dispatched when nothing overran */
        HWCOUNT_ADD("2d screens: snapshot points offered (vblanks dispatched)", s_pipe.points);
        HWCOUNT_ADD("2d screens: frames composited (gpu2d_render)", s_pipe.composited);
        HWCOUNT_ADD("2d screens: frames presented", s_pipe.presented);
        HWCOUNT_ADD("2d screens: overruns (frame dropped, the renderer still had the previous one)", s_pipe.overruns);
        HWCOUNT_ADD("2d screens: VRAM bytes copied into the record at the snapshot points (bound windows + ext palettes)", s_prof.vs_bytes);
        HWCOUNT_ADD("2d screens: frames the renderer met an unsupported feature in", s_uns_frames);
        HWCOUNT_ADD("2d screens: unsupported features met by the renderer (sum over frames)", s_uns_sum);
        HWCOUNT_ADD("2d screens: fbdump frames written", s_fb_written);
        HWLOG("2d screens: %u snapshot points (game frames), %u composited, %u presented, %u overruns; render core %d",
              (unsigned)s_pipe.points, (unsigned)s_pipe.composited, (unsigned)s_pipe.presented, (unsigned)s_pipe.overruns, s_rcore);
        HWLOG("2d vram snapshot: %u bytes copied over %u snapshot points, largest frame %u bytes; unsupported: %u frames, %u features, mask %04x",
              (unsigned)s_prof.vs_bytes, (unsigned)s_prof.vs_calls, (unsigned)s_prof.vs_max, (unsigned)s_uns_frames, (unsigned)s_uns_sum, (unsigned)s_uns_mask);
        HWLOG("fbdump: %u written, %u failed, %u waits for the renderer, %u gave up", (unsigned)s_fb_written, (unsigned)s_fb_failed, (unsigned)s_fb_waits, (unsigned)s_fb_gaveup);
    }
    if (s_census_on) {
        cen_flush();
        census_dump();
        HWLOG("census: %u host frames / %u DS vblanks, %u features in hwcensus.txt (3D active on BG0 in %u frames)", (unsigned)HWCEN_Frames(), ds_vblanks(), (unsigned)HWCEN_Features(),
              (unsigned)HWCEN_Count("3d_active_on_bg0"));
    } else {
        HWLOG("census: off (config.ini `census = 1` or run.py --census turns it on)");
    }
    {   /* bet 2.1c2: the wall-time line run.py prints after every run */
        uint32_t avg = s_prof.frames != 0u ? ms10((double)s_prof.sum / (double)s_prof.frames) : 0u;
        uint32_t gx = ms10((double)s_prof.gx_ticks + (double)s_prof.store_ticks), pk = ms10((double)s_prof.peak);
        uint32_t fe = ms10((double)s_prof.fe_ticks), dp = ms10((double)s_prof.dump_ticks), sl = ms10((double)s_prof.stolen);

        unsigned fr = s_prof.frames, vbl = ds_vblanks();
        uint64_t sl_ticks = 0u;
        uint32_t sl_calls = 0u, slm, cen;

        if (hw_snd7_timing != NULL) {
            hw_snd7_timing(&sl_ticks, &sl_calls);
        }
        slm = ms10((double)sl_ticks);
        cen = ms10((double)s_prof.gx_ticks + (double)s_prof.store_ticks + (double)s_prof.fe_ticks + (double)s_prof.dump_ticks);
        uint32_t exm = s_prof.frames != 0u ? ms10((double)s_prof.eng_sum / (double)s_prof.frames) : 0u;
        HWLOG("frame wall: engine ticks clamped to the frame wall in %u of %u frames (review N3: each is a frame where the engine bucket exceeded the wall gap)", (unsigned)s_prof.eng_clamped, fr);
        HWLOG("frame wall: avg %u.%u ms peak %u.%u ms over %u host fr / %u DS vb; engine excluded %u.%u ms; census: gxword %u.%u ms (%u calls), frameend %u.%u ms, dump %u.%u ms",
              (unsigned)(avg / 10u), (unsigned)(avg % 10u), (unsigned)(pk / 10u), (unsigned)(pk % 10u), fr, vbl, (unsigned)(exm / 10u), (unsigned)(exm % 10u), (unsigned)(gx / 10u),
              (unsigned)(gx % 10u), (unsigned)(s_prof.gx_words + s_prof.port_calls), (unsigned)(fe / 10u), (unsigned)(fe % 10u), (unsigned)(dp / 10u), (unsigned)(dp % 10u));
        /* Rule 7 bucket "hblank dispatch" (bet 2.3a): wall ms inside hwpump_hblank_frame, the snap calls (192 per field frame when not short-cut) */
        uint32_t hbm = ms10((double)s_prof.hb_ticks);
        /* Rule 7 buckets (bet 2.1e): the game frame (host wait to host wait), the ARM7 sound slice, the census; wall ms on this host, never gated on Azahar */
        uint32_t rn = s_pipe.composited != 0u ? (uint32_t)((double)s_rprof.render * 100.0 / TICKS_PER_MS / (double)s_pipe.composited + 0.5) : 0u;
        uint32_t pr = s_pipe.presented != 0u ? (uint32_t)((double)s_rprof.present * 100.0 / TICKS_PER_MS / (double)s_pipe.presented + 0.5) : 0u;
        uint32_t dpm = ms10((double)s_rprof.dump), wtm = ms10((double)s_rprof.wait);

        uint32_t g3m = ms10((double)s_g3p.ticks);
        HWLOG("geometry engine (Rule 7): %u.%u ms in %u flushes + %u DMA bursts (%u words, longest flush %u ticks), %u register syncs, %u vblank latches; %u polygons, %u vertices built, %u culled, %u clipped away, %u swaps",
              (unsigned)(g3m / 10u), (unsigned)(g3m % 10u), (unsigned)s_g3p.flushes, (unsigned)s_g3p.blocks, (unsigned)s_g3p.words, (unsigned)s_g3p.max_ticks, (unsigned)s_g3p.syncs,
              (unsigned)s_g3p.vblanks, s_g3 != NULL ? (unsigned)s_g3->st_polys : 0u, s_g3 != NULL ? (unsigned)s_g3->st_vtxs : 0u, s_g3 != NULL ? (unsigned)s_g3->st_culled : 0u,
              s_g3 != NULL ? (unsigned)s_g3->st_clipped_out : 0u, s_g3 != NULL ? (unsigned)s_g3->st_swaps : 0u);
        if (s_g3 != NULL) {   /* slice (c): the engine's sub-buckets, in ticks (268111856 per second); hwlog keeps 160 characters per line and one line per call site */
#ifdef HW_G3_PROF   /* make HW_PROF=-DHW_G3_PROF: the per-command-class tick pairs (each tick read costs ~370 ticks, so they are off in the shipped build) */
            static const char *const nm[GPU3D_PROF_N] = { "matrix", "vertex(all)", "polygon-build", "clip", "normal", "state", "swap", "tests" };
            uint64_t t0 = hwctr_sys_tick(), t1;
            uint32_t k;

            for (k = 0u; k < 64u; k++) {
                (void)hwctr_sys_tick();
            }
            t1 = hwctr_sys_tick();
            HWLOG("g3 tick read costs %u ticks; flushes %u ms, bursts %u ms, sync flush %u ms, publish %u ms", (unsigned)((t1 - t0) / 64u), (unsigned)(s_g3p.flush_ticks / 268112u),
                  (unsigned)(s_g3p.block_ticks / 268112u), (unsigned)(s_g3p.sync_ticks_flush / 268112u), (unsigned)(s_g3p.sync_ticks_publish / 268112u));
#define G3CLS(k) HWLOG("g3 %s: %u kticks, %u calls", nm[k], (unsigned)(s_g3->prof_ticks[k] / 1000u), (unsigned)s_g3->prof_n[k])
            G3CLS(0);
            G3CLS(1);
            G3CLS(2);
            G3CLS(3);
            G3CLS(4);
            G3CLS(5);
            G3CLS(6);
            G3CLS(7);
#undef G3CLS
#endif
            HWLOG("g3 counts: polys %u, clipper in %u, cut/rejected %u, lit NORMALs %u (memo %u)", (unsigned)s_g3->st_polys, (unsigned)s_g3->st_clip_calls, (unsigned)s_g3->st_clip_cut,
                  (unsigned)s_g3->st_lit, (unsigned)s_g3->st_lit_memo);
            HWLOG("g3 counts: culled %u, clipped away %u, one-dot %u, swaps %u, vertices %u, cut vertices %u, cut short at 10 vertices %u", (unsigned)s_g3->st_culled,
                  (unsigned)s_g3->st_clipped_out, (unsigned)s_g3->st_dot, (unsigned)s_g3->st_swaps, (unsigned)s_g3->st_vtxs, (unsigned)s_g3->st_cutv, (unsigned)s_g3->st_clip_over);
            /* slice 2.4d, review N7 (plan row 2.4: "the count of props drawn is logged per frame"). hwlog keeps one line per call site, so the per-frame figures are kept in the
               engine per DS vblank and reported here as totals, per-3D-frame averages and maxima; gpu3d_replay prints every frame (the private replay tool). */
            HWLOG("g3 per frame: %u vblanks, %u with 3D; %u lists sealed, %u polys (avg %u, max %u); BOX_TEST passed %u of %u (max %u of %u in a frame)", (unsigned)s_g3->fr_vblanks,
                  (unsigned)s_g3->fr_3d, (unsigned)s_g3->fr_sealed, (unsigned)s_g3->fr_sealed_polys, s_g3->fr_sealed != 0u ? (unsigned)(s_g3->fr_sealed_polys / s_g3->fr_sealed) : 0u,
                  (unsigned)s_g3->fr_sealed_polys_max, (unsigned)s_g3->fr_view, (unsigned)s_g3->fr_box, (unsigned)s_g3->fr_view_max, (unsigned)s_g3->fr_box_max);
            HWCOUNT_ADD("G3 engine: polygons cut short at 10 vertices (skew quad, review B1)", s_g3->st_clip_over);
            HWCOUNT_ADD("G3 engine: BOX_TESTs passed (in view), all frames", s_g3->fr_view);
            HWCOUNT_ADD("G3 engine: polygons sealed by SWAP_BUFFERS, all frames", s_g3->fr_sealed_polys);
        }
        HWLOG("timers: frame avg %u.%u peak %u.%u ms, %u host fr / %u DS vb (%u.%02u per fr); sound slice %u.%u ms in %u calls; census %u.%u ms; hblank dispatch %u.%u ms in %u calls; geometry engine %u.%u ms",
              (unsigned)(avg / 10u), (unsigned)(avg % 10u), (unsigned)(pk / 10u), (unsigned)(pk % 10u), fr, vbl, fr != 0u ? vbl / fr : 0u,
              fr != 0u ? (unsigned)((uint64_t)vbl * 100u / fr % 100u) : 0u, (unsigned)(slm / 10u), (unsigned)(slm % 10u), (unsigned)sl_calls,
              (unsigned)(cen / 10u), (unsigned)(cen % 10u), (unsigned)(hbm / 10u), (unsigned)(hbm % 10u), (unsigned)s_prof.hb_calls, (unsigned)(g3m / 10u), (unsigned)(g3m % 10u));
        uint32_t vsm = s_prof.vs_calls != 0u ? (uint32_t)((double)s_prof.vs_ticks * 1000.0 / TICKS_PER_MS / (double)s_prof.vs_calls + 0.5) : 0u;   /* thousandths of a ms */

        HWLOG("timers 2d: render %u.%02u ms per frame over %u frames; present %u.%02u ms per frame over %u frames; vram snapshot %u.%03u ms per frame over %u frames (Rule 7 buckets; wall ms on this host)",
              (unsigned)(rn / 100u), (unsigned)(rn % 100u), (unsigned)s_pipe.composited, (unsigned)(pr / 100u), (unsigned)(pr % 100u), (unsigned)s_pipe.presented,
              (unsigned)(vsm / 1000u), (unsigned)(vsm % 1000u), (unsigned)s_prof.vs_calls);
        {   /* bet 2.5b: the 3D half of the record. Counts first (reproducible under the frame clock: the game thread alone decides them), then the wall-ms buckets (WALL_LINES) */
            uint64_t rec = s_g3x.records != 0u ? s_g3x.records : 1u, pf = s_pr.frames != 0u ? s_pr.frames : 1u;
            #define PER(ticks, n, scale) ((uint32_t)((double)(ticks) * (scale) / TICKS_PER_MS / (double)(n) + 0.5))
            uint32_t ex = PER(s_g3x.export_ticks, rec, 100.0), exm = PER(s_g3x.export_max, 1u, 100.0), hs = PER(s_g3x.hash_ticks, rec, 1000.0), dc = PER(s_g3x.decode_ticks, rec, 1000.0);
            uint32_t rs = PER(s_pr.raster, pf, 100.0), rb = PER(s_pr.readback, pf, 100.0), cv = PER(s_pr.convert, pf, 100.0), dsr = PER(s_pr.dsrb, pf, 100.0), edg = PER(s_pr.edge, pf, 100.0), spl = PER(s_pr.split, pf, 100.0);
            uint32_t rtw = s_rprof.walled != 0u ? (uint32_t)((double)s_rprof.wall * 100.0 / TICKS_PER_MS / (double)s_rprof.walled + 0.5) : 0u;

            #undef PER
            HWCOUNT_ADD("3d record: records handed to the renderer (snapshot points with engine A BG0 3D on)", s_g3x.records);
            HWCOUNT_ADD("3d record: PLY1 list bytes", s_g3x.list_bytes);
            HWCOUNT_ADD("3d record: bytes carried (list + decoded texels + index + table)", s_g3x.rec_bytes);
            HWCOUNT_ADD("3d record: decoded texel bytes (cache misses)", s_g3x.texel_bytes);
            HWCOUNT_ADD("3d record: texture lookups (textured polygons)", s_g3x.lookups);
            HWCOUNT_ADD("3d record: texture lookups resolved without a decode", s_g3x.hits);
            HWCOUNT_ADD("3d record: content keys computed", s_g3x.keys);
            HWCOUNT_ADD("3d record: texture decodes", s_g3x.decodes);
            HWCOUNT_ADD("3d record: texture + palette bytes hashed", s_g3x.hashed_bytes);
            HWCOUNT_ADD("3d record: polygons drawn untextured (compressed format)", s_g3x.unsupported);
            HWCOUNT_ADD("3d record: textures that found no room (tex table, cache slot or texel pool)", s_g3x.full);
            HWCOUNT_ADD("3d pica: texture uploads (render thread)", pica_stats.tex_uploads);
            HWCOUNT_ADD("3d pica: frames rasterised", s_pr.frames);
            HWCOUNT_ADD("3d pica: frames with the edge pass", pica_stats.edge_frames_run);
            HWCOUNT_ADD("3d pica: pixels edge-marked", pica_stats.edge_marked);
            HWCOUNT_ADD("3d pica: translucent polygons skipped by the same-ID rule", pica_stats.same_id_polys);
            HWCOUNT_ADD("3d pica: edge frames split after the opaque polygons", pica_stats.split_frames);
            HWCOUNT_ADD("3d pica: split frames with a second depth copy", pica_stats.split_depth_copies);
            HWCOUNT_ADD("3d pica: opaque polygons after the first translucent one", pica_stats.opaque_after_split);
            HWLOG("3d record: %u records, %u list bytes (%u per record), %u bytes carried (%u per record), %u texel bytes decoded",
                  (unsigned)s_g3x.records, (unsigned)s_g3x.list_bytes, (unsigned)(s_g3x.list_bytes / rec), (unsigned)s_g3x.rec_bytes, (unsigned)(s_g3x.rec_bytes / rec), (unsigned)s_g3x.texel_bytes);
            HWLOG("3d textures: %u lookups, %u hits, %u keys, %u decodes, %u bytes hashed, %u compressed, %u without room",
                  (unsigned)s_g3x.lookups, (unsigned)s_g3x.hits, (unsigned)s_g3x.keys, (unsigned)s_g3x.decodes, (unsigned)s_g3x.hashed_bytes, (unsigned)s_g3x.unsupported, (unsigned)s_g3x.full);
            HWLOG("3d pica: %u frames rasterised, %u texture uploads, %u failed, init %d", (unsigned)s_pr.frames, (unsigned)pica_stats.tex_uploads, (unsigned)pica_stats.tex_failed, s_pr.init_rc);
            HWLOG("timers 3d game: g3 export %u.%02u ms per record (max %u.%02u) over %u records; g3 tex hash %u.%03u ms per record, decode %u.%03u ms",
                  (unsigned)(ex / 100u), (unsigned)(ex % 100u), (unsigned)(exm / 100u), (unsigned)(exm % 100u), (unsigned)s_g3x.records, (unsigned)(hs / 1000u), (unsigned)(hs % 1000u),
                  (unsigned)(dc / 1000u), (unsigned)(dc % 1000u));
            HWLOG("timers 3d render: pica raster %u.%02u ms, pica readback %u.%02u ms, layer convert %u.%02u ms per frame over %u frames",
                  (unsigned)(rs / 100u), (unsigned)(rs % 100u), (unsigned)(rb / 100u), (unsigned)(rb % 100u), (unsigned)(cv / 100u), (unsigned)(cv % 100u), (unsigned)s_pr.frames);
            HWLOG("timers 3d render: pica ds readback %u.%02u ms, edge pass %u.%02u ms (de-tile + marking) per frame over %u frames; %u edge frames, %u pixels marked, %u same-ID skips",
                  (unsigned)(dsr / 100u), (unsigned)(dsr % 100u), (unsigned)(edg / 100u), (unsigned)(edg % 100u), (unsigned)s_pr.frames, (unsigned)pica_stats.edge_frames_run,
                  (unsigned)pica_stats.edge_marked, (unsigned)pica_stats.same_id_polys);
            HWLOG("timers 3d render: frame split %u.%02u ms per frame over %u frames (%u split, %u second depth copies, %u opaque after the split)",
                  (unsigned)(spl / 100u), (unsigned)(spl % 100u), (unsigned)s_pr.frames, (unsigned)pica_stats.split_frames, (unsigned)pica_stats.split_depth_copies, (unsigned)pica_stats.opaque_after_split);
            HWLOG("timers 3d render: thread wall %u.%02u ms per presented frame over %u (raster, readbacks, de-tile, edge, split, gpu2d, present; report only)",
                  (unsigned)(rtw / 100u), (unsigned)(rtw % 100u), (unsigned)s_rprof.walled);
            if (s_pd_count != 0u) {
                HWLOG("polydump: %u lists written, %u failed, %u texture files", (unsigned)s_pd_written, (unsigned)s_pd_failed, (unsigned)s_pd_tex);
            }
            if (s_wide != 0u) {   /* bet 2.8, wide runs only */
                HWLOG("wide present: %u frames wide (%u stereo, %u mono at slider 0), %u narrow, %u eyes refused, wide buffers %s; slider %d..%d%%, sigma max %u/1000000",
                      (unsigned)s_wst.wide, (unsigned)s_wst.stereo, (unsigned)s_wst.mono_slider0, (unsigned)s_wst.narrow, (unsigned)s_wst.eye_fail, s_wtop[0] != NULL ? "ok" : "MISSING",
                      s_wst.slider_max < 0 ? 0 : s_wst.slider_min, s_wst.slider_max < 0 ? 0 : s_wst.slider_max, (unsigned)(s_wst.sigma_max * 1e6f + 0.5f));
                HWLOG("wide pica: %u wide frames (eyes), linearSpaceFree %u KB after the wide target + buffers were made, %u KB now",
                      (unsigned)pica_stats.wide_frames, (unsigned)(pica_stats.wide_linear_free / 1024u), (unsigned)(hwctr_linear_free() / 1024u));
                HWLOG("wide: guard-band bound sigma|1-c/w| <= 1.5625 g: %u of %u sheared vertices outside it (0 expected on the field camera)",
                      (unsigned)pica_stats.wide_shear_viol, (unsigned)pica_stats.wide_shear_verts);
                if (s_g3 != NULL) {
                    HWLOG("wide engine: %u projections registered, %u cameras too wide; %u loads, %u matched; polygons %u wide, %u centred, %u partial-viewport, %u guard-clipped, %u past the DS limits",
                          (unsigned)s_wide_regs, (unsigned)s_wide_sin_over, (unsigned)s_g3->st_reg_loads, (unsigned)s_g3->st_reg_hits, (unsigned)s_g3->st_wide_polys,
                          (unsigned)s_g3->st_centred_polys, (unsigned)s_g3->st_partial_vp, (unsigned)s_g3->st_guard_polys, (unsigned)s_g3->st_wide_overflow);
                }
                if (s_fb_count != 0u) {
                    HWLOG("wide fbdump: %u written, %u failed", (unsigned)s_wst.wdump_written, (unsigned)s_wst.wdump_failed);
                }
            }
        }
        HWLOG("fbdump SD writes: %u.%u ms on the render thread, %u.%u ms the game waited for a dump-window frame (taken out of the game's clock)", (unsigned)(dpm / 10u), (unsigned)(dpm % 10u),
              (unsigned)(wtm / 10u), (unsigned)(wtm % 10u));
        HWLOG("clock: %s; DS vblanks %u; forced vblanks %u (FRAME spin budget %u sys ticks per frame, bet 2.2a); wall-mode pacing overruns %u",
              hw_os_clock_is_frame() ? "frame" : "wall", vbl, (unsigned)hw_os_forced_total(), (unsigned)HWT_SPIN_BUDGET_SYS, (unsigned)hw_os_pump()->overruns);
        HWLOG("census cost: store %u/16 ticks per word, tick read %u ticks; %u.%u ms taken out of the game's clock", (unsigned)s_store_x16, (unsigned)s_tick_cost, (unsigned)(sl / 10u), (unsigned)(sl % 10u));
    }
    {
        HWG3_Sync();
        const uint32_t *clip = (const uint32_t *)(io_regs() + REG_CLIPMTX_RESULT_0_OFFSET), *vec = (const uint32_t *)(io_regs() + REG_VECMTX_RESULT_0_OFFSET);

        HWLOG("G3 readbacks: CLIPMTX diagonal %08x %08x %08x %08x, VECMTX diagonal %08x %08x %08x (identity = 00001000); GXSTAT %08x",
              (unsigned)clip[0], (unsigned)clip[5], (unsigned)clip[10], (unsigned)clip[15], (unsigned)vec[0], (unsigned)vec[4], (unsigned)vec[8],
              (unsigned)*(const vu32 *)REG_GXSTAT_ADDR);
    }
    if (s_arm7.ops.snd != NULL) {
        const HwSnd *sn = s_arm7.ops.snd;

        HWLOG("sound slice: slices=%u cmd_words=%u wake_words=%u replies=%u reply_overflow=%u overruns=%u reentry=%u rebases=%u mail_forced=%u alarms_set=%u alarms_fired=%u",
              (unsigned)sn->slices, (unsigned)sn->cmd_words, (unsigned)sn->wake_words, (unsigned)sn->replies, (unsigned)sn->reply_overflow, (unsigned)sn->overruns,
              (unsigned)sn->reentry, (unsigned)sn->rebases, (unsigned)sn->mail_forced, (unsigned)sn->alarms_set, (unsigned)sn->alarms_fired);
        HWLOG("sound busy bits: channel key-ons or rewrites of a busy channel %u (loop/PSG, never end: %u), expired %u; capture key-ons %u, expired %u",
              (unsigned)sn->chan_keyons, (unsigned)sn->chan_unbounded, (unsigned)sn->chan_expired, (unsigned)sn->cap_keyons, (unsigned)sn->cap_expired);
    }
    hw27_report();
}

/* The registers the game writes that nothing in the hardware layer sees (plain memory): once a frame, count the words that differ from the last frame.
   A word rewritten with the same value is invisible; this is the "which register blocks does the game use" census of the private design notes. */
typedef struct RegBlock {
    uint32_t off, words;
    uint32_t last[0x70 / 4];
} RegBlock;

static RegBlock s_blocks[] = {
    { 0x000u, 0x70u / 4u, { 0 } },    /* engine A: DISPCNT, BG0-3 control/scroll/affine, WIN, MOSAIC, BLDCNT/ALPHA/Y */
    { 0x1000u, 0x70u / 4u, { 0 } },   /* engine B, same layout */
    { 0x240u, 3u, { 0 } },            /* VRAMCNT_A-G, WRAMCNT, VRAMCNT_H-I */
    { 0x304u, 1u, { 0 } },            /* POWCNT1 */
    { 0x330u, 4u, { 0 } },            /* G3: CLEAR_COLOR, CLEAR_DEPTH, CLRIMAGE_OFFSET, FOG_COLOR */
    { 0x350u, 8u, { 0 } },            /* G3: FOG_OFFSET .. FOG_TABLE, EDGE_COLOR, TOON/ALPHA_TEST */
    { 0x0b0u, 12u, { 0 } },           /* DMA0-3 SAD/DAD/CNT */
    { 0x100u, 4u, { 0 } },            /* timers 0-3 */
    { 0x1a0u, 6u, { 0 } },            /* card: AUXSPICNT, ROMCTRL, command */
};

static void reg_census(void)
{
    const uint8_t *io = io_regs();
    unsigned b;

    for (b = 0; b < sizeof s_blocks / sizeof s_blocks[0]; b++) {
        RegBlock *r = &s_blocks[b];
        uint32_t i, changed = 0;

        for (i = 0; i < r->words && i < 0x70u / 4u; i++) {
            uint32_t v;

            memcpy(&v, io + r->off + 4u * i, sizeof v);
            changed += v != r->last[i];
            r->last[i] = v;
        }
        switch (b) {
        case 0: HWCOUNT_ADD("G2: engine A register words changed (0x4000000-0x400006F)", changed); break;
        case 1: HWCOUNT_ADD("G2: engine B register words changed (0x4001000-0x400106F)", changed); break;
        case 2: HWCOUNT_ADD("GX: VRAMCNT/WRAMCNT words changed (0x4000240)", changed); break;
        case 3: HWCOUNT_ADD("GX: POWCNT1 changes (0x4000304)", changed); break;
        case 4: HWCOUNT_ADD("G3: clear colour/depth/fog colour words changed (0x4000330)", changed); break;
        case 5: HWCOUNT_ADD("G3: fog/edge/toon words changed (0x4000350)", changed); break;
        case 6: HWCOUNT_ADD("DMA: DMA0-3 register words changed (0x40000B0)", changed); break;
        case 7: HWCOUNT_ADD("timers: TM0-3 register words changed (0x4000100)", changed); break;
        default: HWCOUNT_ADD("card: AUXSPICNT/ROMCTRL/command words changed (0x40001A0)", changed); break;
        }
    }
}

/* One input frame (port_main's vblank hook): the DS key registers from the held keys, the pen to the touch responder. */
void hw_devs_input_frame(const HwInState *st)
{
    uint64_t now = hwctr_sys_tick();

    assert(st != NULL);
    if (s_prof.last != 0u) {
        uint64_t dt = now - s_prof.last;
        uint64_t waited = s_rprof.wait - s_wait_seen;   /* bet 2.3c: the time the game waited for the renderer (rendersync / fbdump window) is not game cost */

        dt = dt > waited ? dt - waited : 0u;
        uint64_t eng = s_g3p.ticks - s_prof.eng_prev;   /* slice (c): the engine's share of this frame is taken out of the wall, as the renderer wait is (decision of the main loop, 2026-10-13) */

        if (eng > dt) {
            eng = dt;
            s_prof.eng_clamped++;
        }
        s_prof.eng_sum += eng;
        dt -= eng;
        hwbud_add(&s_bud.eng, bud32(eng));
        hwbud_add(&s_bud.frame, bud32(dt));
        {   /* N2: busy = the game thread's work in this host frame = the wall minus the renderer wait minus its own idle (vblank wait + timer sleep); it includes the engine */
            uint64_t idle = hwctr_idle_ticks() - s_idle_seen;
            uint64_t wall = (now - s_prof.last) > waited ? (now - s_prof.last) - waited : 0u;

            hwbud_add(&s_bud.busy, bud32(wall > idle ? wall - idle : 0u));
        }
        s_prof.sum += dt;
        s_prof.frames++;
        if (dt > s_prof.peak) {
            s_prof.peak = dt;
        }
    }
    s_prof.last = now;
    s_wait_seen = s_rprof.wait;
    s_idle_seen = hwctr_idle_ticks();
    s_prof.eng_prev = s_g3p.ticks;
    hw27_frame();
    hw_devs_init();
    *(vu16 *)REG_KEYINPUT_ADDR = hwin_keyinput(st->keys);
    *(vu16 *)HW_BUTTON_XY_BUF = hwin_xybuf(st->keys);
    hwarm7_input_frame(&s_arm7, &s_pxi, st);
    wm_frame();
    gx_frame();
    vram_frame();
    hwwb_frame(flash_ops(), (uint32_t)hw_os_vblanks_total());   /* bet 2.6b: harvest a finished save write-back, hand out a due one */
    if (s_census_on) {
        uint64_t t0;
        int dump;

        cen_flush();   /* every GX word of this frame is decoded before the frame is closed */
        t0 = hwctr_sys_tick();
        dump = HWCEN_FrameEnd(io_regs(), g_hwoam);   /* bet 2.1c: sample the registers / OAM / 3D state and close the census frame */

        t0 = hwctr_sys_tick() - t0;
        s_prof.fe_ticks += t0;
        cen_steal(t0);
        if (dump) {
            census_dump();
        }
    }
    reg_census();
    HWCOUNT("input frames");
}

void hw_devs_reset(void)
{
    static const HwDmaOps dops = { dma_xlat, gx_sink, NULL, dma_wrote, gx_block };

    hw_devs_init();
    if (s_g3 != NULL) {   /* a soft reset: the engine powers up again (registers are zeroed by the caller) */
        gpu3d_reset(s_g3);
        gpu3d_wide_config(s_g3, s_wide, s_stereo, HWWIDE_GUARD_Q12);
#ifdef HW_G3_PROF
        s_g3->clock = hwctr_sys_tick;   /* the Rule 7 sub-buckets: `make HW_PROF=-DHW_G3_PROF` (a tick read costs ~370 ticks per command, so the default build leaves it off) */
#endif
        s_g3n = 0u;
        s_g3dirty = 1u;
    }
    hwdma_init(&s_dma, &dops, io_regs());
    hwpxi_init(&s_pxi, io_regs());
    arm7_attach();
    hwcard_reset(&s_card);
}

void hw_devs_init(void)
{
    static const HwDmaOps dops = { dma_xlat, gx_sink, NULL, dma_wrote, gx_block };

    if (s_inited) {
        return;
    }
    s_inited = 1;
    HWGX_SetFeed(g3_word, g3_block);
    HWGX_SetTraceOpenHook(g3_trace_open);
    hwdma_init(&s_dma, &dops, io_regs());
    hwpxi_init(&s_pxi, io_regs());
    arm7_attach();
    card_boot();
}

const HwDma *hw_devs_dma(void)
{
    return &s_dma;
}

const HwPxi *hw_devs_pxi(void)
{
    return &s_pxi;
}

/* ---- PXI ----------------------------------------------------------------------------------------------------------------- */
void HWDEV_PxiReset(void)
{
    HWCOUNT("PXI: HWDEV_PxiReset");
    OSSystemWork *w = OS_GetSystemWork();

    hw_devs_init();
    hwpxi_init(&s_pxi, io_regs());   /* a fresh loopback: queues empty, every responder registered */
    arm7_attach();
    w->pxiHandleChecker[PXI_PROC_ARM7] = s_pxi.handle_bits;   /* what the ARM7's PXI_SetFifoRecvCallback calls would have published */
    hwpxi_cnt_sync(&s_pxi);
    HWLOG("PXI loopback up: ARM7 handle bits %08x", (unsigned)s_pxi.handle_bits);
}

void HWDEV_PxiCntSync(void)
{
    HWCOUNT("PXI: HWDEV_PxiCntSync");
    hw_devs_init();
    hwpxi_cnt_sync(&s_pxi);
}

void HWDEV_PxiCheckData(uint32_t tag, uint32_t data)
{
    char msg[96];

    HWCOUNT("PXI send: 26-bit pointer-limit check");
    if (data >= (1u << 26)) {
        snprintf(msg, sizeof msg, "PXI tag %u data 0x%08x does not fit 26 bits (pointer above 0x04000000)", (unsigned)tag, (unsigned)data);
        hwctr_fatal(msg);
    }
}

void HWDEV_PxiSend(uint32_t word)
{
    HWCOUNT("PXI: HWDEV_PxiSend");
    hw_devs_init();
    hwpxi_send(&s_pxi, word);
}

uint32_t HWDEV_PxiRecv(void)
{
    HWCOUNT("PXI: HWDEV_PxiRecv");
    hw_devs_init();
    return hwpxi_recv(&s_pxi);
}

/* ---- DMA and the divider / sqrt unit ------------------------------------------------------------------------------------- */
/* The first unmapped transfer of each (channel, source, destination), up to 16: an ordered trace line with the caller, so the next DMA bug is in
   boot.out instead of needing a debugger (review 1.6, F2; F1 took a gdb session). */
static void dma_report_unmapped(unsigned ch, uint32_t sad, uint32_t dad, uint32_t cnt, void *caller)
{
    static struct { uint32_t sad, dad; uint8_t ch; } seen[16];
    static unsigned n;
    unsigned i;

    for (i = 0; i < n; i++) {
        if (seen[i].ch == ch && seen[i].sad == sad && seen[i].dad == dad) {
            return;
        }
    }
    if (n < 16u) {
        seen[n].ch = (uint8_t)ch;
        seen[n].sad = sad;
        seen[n].dad = dad;
        n++;
        hwtrace("dma: UNMAPPED ch%u sad=%08x dad=%08x cnt=%08x caller=%p", ch, (unsigned)sad, (unsigned)dad, (unsigned)cnt, caller);
    }
}

void HWDEV_DmaStart(uint32_t dmaNo)
{
    HWCOUNT("DMA: HWDEV_DmaStart");
    const uint8_t *r = io_regs() + 0xb0u + 12u * (dmaNo & 3u);
    uint32_t sad, dad, cnt, before;

    hw_devs_init();
    memcpy(&sad, r, 4);
    memcpy(&dad, r + 4, 4);
    memcpy(&cnt, r + 8, 4);
    before = s_dma.unmapped;
    (void)hwdma_start(&s_dma, (unsigned)dmaNo);
    if (s_dma.unmapped != before) {
        dma_report_unmapped((unsigned)dmaNo, sad, dad, cnt, __builtin_return_address(0));
    }
    hw_os_dispatch_now();
}

void HWDEV_CpDivRun(void)
{
    HWCOUNT("CP: HWDEV_CpDivRun");
    hwcp_div_run(io_regs());
}

void HWDEV_CpSqrtRun(void)
{
    HWCOUNT("CP: HWDEV_CpSqrtRun");
    hwcp_sqrt_run(io_regs());
}

/* ---- card bus ------------------------------------------------------------------------------------------------------------------ */
/* bet 2.6b: the SDK's backup lock (card_common.c CARD_LockBackup / CARD_UnlockBackup): the save write-back's hook points. */
void HWDEV_BackupLock(uint32_t lock_id)
{
    (void)lock_id;
    HWCOUNT("save: HWDEV_BackupLock (CARD_LockBackup)");
    hw_devs_init();
    hwwb_lock();
}

void HWDEV_BackupUnlock(uint32_t lock_id)
{
    (void)lock_id;
    HWCOUNT("save: HWDEV_BackupUnlock (CARD_UnlockBackup)");
    hwwb_unlock(flash_ops(), (uint32_t)hw_os_vblanks_total());
}

/* the app is ending: a flush in flight finishes, a pending one (inside its debounce window) is written now */
void hw_devs_save_stop(void)
{
    hwwb_stop(flash_ops(), (uint32_t)hw_os_vblanks_total());
}

void HWDEV_CardCtrlWrite(uint32_t ctrl)
{
    HWCOUNT("card: HWDEV_CardCtrlWrite");
    hw_devs_init();
    hwcard_ctrl_write(&s_card, ctrl);
}

uint32_t HWDEV_CardCtrlRead(void)
{
    HWCOUNT("card: HWDEV_CardCtrlRead");
    hw_devs_init();
    return hwcard_ctrl_read(&s_card);
}

uint32_t HWDEV_CardDataRead(void)
{
    HWCOUNT("card: HWDEV_CardDataRead");
    hw_devs_init();
    return hwcard_data_read(&s_card);
}
