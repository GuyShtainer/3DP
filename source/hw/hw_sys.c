/* hw_sys.c -- defines the host-side arrays that stand in for the DS memory map (declared in nitro/hw/ARM9/mmap_global.h, patched by
 * port-patches/nitrosdk-memmap.patch), the linker-supplied SDK symbols, and fills the system page at boot. SDK-world file: port flags,
 * no libctru (file I/O goes through hw_ctr.h). Design notes are kept privately. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <nitro.h>

#include "gpu2d.h"
#include "hw_benchio.h"
#include "hw_card.h"
#include "hw_census.h"
#include "hw_ctr.h"
#include "hw_devs.h"
#include "hw_gx.h"
#include "hw_log.h"
#include "hw_saveid.h"
#include "hw_cfgio.h"
#include "hw_setup.h"
#include "hw_os.h"
#include "hw_sys.h"
#include "hw_vram.h"

#define ALIGN4K __attribute__((aligned(4096)))
#define CFG_FILE_MAX 4096u   /* the flat keys (~450 B) + one ~500 B identity section per save seen (bet 2.6a) */

HwRegs g_hwregs;
u8 g_hwmainmem[0x400000] ALIGN4K;
u8 g_hwsys[0x1000] ALIGN4K;
u8 g_hwitcm[0x8000] ALIGN4K;
u8 g_hwwram[0x8000] ALIGN4K;
u8 g_hwpltt[0x800] ALIGN4K;
u8 g_hwvram_bg[0x80000] ALIGN4K;
u8 g_hwvram_dbbg[0x20000] ALIGN4K;
u8 g_hwvram_obj[0x40000] ALIGN4K;
u8 g_hwvram_dbobj[0x20000] ALIGN4K;
u8 g_hwvram_lcdc[0xA4000] ALIGN4K;
u8 g_hwoam[0x800] ALIGN4K;
u8 g_hwctrdg_rom[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
u8 g_hwbios[0x8000] ALIGN4K;

/* the DS DTCM: a plain array, the stock HW_DTCM redirect (SDK_AUTOLOAD_DTCM_START) already points here */
u32 SDK_AUTOLOAD_DTCM_START[HW_DTCM_SIZE / sizeof(u32)] ALIGN4K;

/* No overlay digests: the loader (a later bet) verifies nothing, and an empty digest table makes FS_ overlay checks pass. */
u8 SDK_OVERLAYTABLE_DIGEST[20];
u8 SDK_OVERLAY_DIGEST[20];
__asm__(".global SDK_OVERLAY_DIGEST_END\n"
        ".set SDK_OVERLAY_DIGEST_END, SDK_OVERLAY_DIGEST\n"
        ".global SDK_MAIN_ARENA_LO\n"
        ".set SDK_MAIN_ARENA_LO, g_hwmainmem + 0x4000\n");   /* the arena starts above the first 16 KB, the DS static data slot */

/* A linker-script value (the SDK reads its ADDRESS as a number). An absolute symbol is fine for the link trial but 3dsxtool rejects a relocation
 * against one ("Relocation to invalid address"), so a real 3dsx link (bet 1.6) must not take this line; card_fs.3dsx defines HW_SYS_NO_ABS_SYMS. */
#ifndef HW_SYS_NO_ABS_SYMS
__asm__(".global SDK_IRQ_STACKSIZE\n"
        ".set SDK_IRQ_STACKSIZE, 0x400\n");

/* The section-arena starts the linker script supplied on the DS (0x027e0080 / 0x01ff8680 / 0x023e0000). os_thread.c writes the launcher thread's
 * stack-limit words AT SDK_SECTION_ARENA_DTCM_START, so each must be an address inside the matching host array (same offset as on the DS). */
__asm__(".global SDK_SECTION_ARENA_DTCM_START\n"
        ".set SDK_SECTION_ARENA_DTCM_START, SDK_AUTOLOAD_DTCM_START + 0x80\n"
        ".global SDK_SECTION_ARENA_ITCM_START\n"
        ".set SDK_SECTION_ARENA_ITCM_START, g_hwitcm + 0x680\n"
        ".global SDK_SECTION_ARENA_EX_START\n"
        ".set SDK_SECTION_ARENA_EX_START, g_hwmainmem + 0x3e0000\n");
#endif

static void vram_init(void)
{
    const HwVramMem mem = { g_hwvram_lcdc, { g_hwvram_bg, g_hwvram_obj, g_hwvram_dbbg, g_hwvram_dbobj } };   /* HWVRAM_WIN_MAIN_BG, MAIN_OBJ, SUB_BG, SUB_OBJ */

    _Static_assert(sizeof g_hwvram_lcdc == HWVRAM_STORE_SIZE && sizeof g_hwvram_bg == HWVRAM_WIN_MAIN_BG_SIZE && sizeof g_hwvram_obj == HWVRAM_WIN_MAIN_OBJ_SIZE &&
                   sizeof g_hwvram_dbbg == HWVRAM_WIN_SUB_BG_SIZE && sizeof g_hwvram_dbobj == HWVRAM_WIN_SUB_OBJ_SIZE, "hw_vram.h sizes match the arrays");
    HWVRAM_Init(&mem);
}

/* The gxtrace window's SD writes are instrument time, not game time (bet 2.1e, review A-S2): each call is timed with the system tick and taken out of the
   virtual clock with the seam the census uses (hw_os_time_steal), so a traced run feels the same time as an untraced one with the same frame numbers. */
static uint64_t s_io_stolen;

static void io_steal(uint64_t t0)
{
    uint64_t dt = hwctr_sys_tick() - t0;

    s_io_stolen += dt;
    hw_os_time_steal(dt);
}

static void *gx_io_create(const char *path)
{
    uint64_t t0 = hwctr_sys_tick();
    void *f = hwctr_file_create(path);

    io_steal(t0);
    return f;
}

static int gx_io_append(void *file, const void *buf, uint32_t len)
{
    uint64_t t0 = hwctr_sys_tick();
    int n = hwctr_file_append(file, buf, len);

    io_steal(t0);
    return n;
}

static void gx_io_close(void *file)
{
    uint64_t t0 = hwctr_sys_tick();

    hwctr_file_close(file);
    io_steal(t0);
}

uint64_t hw_sys_trace_io_ticks(void)
{
    return s_io_stolen;
}

static void *gx_io_alloc(uint32_t bytes)
{
    return malloc(bytes);
}

static const HwGxIo s_gx_io = { gx_io_create, gx_io_append, gx_io_close, gx_io_alloc };

/* Bet 2.2a: the gxtrace v2 per-frame snapshot sources. Region ids: 0..3 texture slots, 4..9 texture palette slots, 16..19 the BG/OBJ windows,
   20..23 main BG ext palettes, 24..27 sub BG ext palettes, 28/29 OBJ ext palettes (main / sub). Others are not regions (NULL = all zero). */
static const uint8_t *snap_region(uint32_t id)
{
    if (id < 4u)  { return HWVRAM_TexSlot(id); }
    if (id < 10u) { return HWVRAM_TexPltt(id - 4u); }
    if (id >= 16u && id < 20u) { return HWVRAM_Window(id - 16u); }
    if (id >= 20u && id < 24u) { return HWVRAM_BgExtPltt(0u, id - 20u); }
    if (id >= 24u && id < 28u) { return HWVRAM_BgExtPltt(1u, id - 24u); }
    if (id == 28u) { return HWVRAM_ObjExtPltt(0u); }
    if (id == 29u) { return HWVRAM_ObjExtPltt(1u); }
    return NULL;
}
/* Bet 2.3a: the per-line 2D record for the gxtrace L2A / L2B chunks (hw_gx.h): the pump's short-cut frames carry one image, the others 192. */
static const uint8_t *snap_lines(uint32_t engine, uint32_t *form)
{
    const Gpu2dFrame *f = hw_devs_gpu2d();

    if (f == NULL || f->lines != GPU2D_LINES || engine >= GPU2D_ENGINES) {
        *form = HWGX_L2_NONE;
        return NULL;
    }
    *form = f->uniform ? HWGX_L2_UNIFORM : HWGX_L2_LINES;
    return f->regs[engine][0];
}
static uint32_t snap_vblanks(void) { return (uint32_t)hw_os_vblanks_total(); }

static HwCfg s_cfg;
static int s_done;
static char s_text[CFG_FILE_MAX];
static size_t s_text_len;      /* bytes of config.ini text in s_text (read or freshly formatted) */
static int s_text_whole;       /* 1 when s_text holds the WHOLE file (read < capacity), so it may be rewritten with a section appended */

static char s_rom_path[96] = HWCTR_ROM_FILE;   /* release P4b: `rom =` from config.ini when that file exists, else the fixed name */

const char *hw_sys_rom_path(void)
{
    return s_rom_path;
}

const HwCfg *hw_sys_cfg(void)
{
    return &s_cfg;
}

int hw_main_ram_contains(const void *p, unsigned n)
{
    uintptr_t a = (uintptr_t)p, lo = (uintptr_t)g_hwmainmem;

    return p != NULL && a >= lo && n <= sizeof g_hwmainmem && a - lo <= sizeof g_hwmainmem - n;
}

static void check_alignment(void)
{
    assert(((uintptr_t)&g_hwregs & 31u) == 0u);
    assert(((uintptr_t)g_hwvram_lcdc & 4095u) == 0u);          /* gx_load3d.c TEXBLK/TEXBASE need 4 KB blocks */
    assert(((uintptr_t)g_hwsys & 3u) == 0u);
    assert((uintptr_t)g_hwmainmem >= HW_SND_PTR_MIN);          /* snd_bank.c / snd_command.c pointer-vs-offset split: every host address is above it */
    assert((uintptr_t)g_hwsys >= HW_SND_PTR_MIN && (uintptr_t)g_hwvram_bg >= HW_SND_PTR_MIN);
    assert(sizeof g_hwmainmem == HW_MAIN_MEM_SIZE);            /* hw-memory-map.md 3.4: the array IS the 4 MB main RAM */
    assert(HW_MAIN_MEM_SHARED == (u32)(uintptr_t)g_hwsys);
    assert(HW_MAIN_MEM_SYSTEM == (u32)(uintptr_t)g_hwsys + 0xc00u);
    assert(HW_DTCM == (u32)(uintptr_t)SDK_AUTOLOAD_DTCM_START);
}

static HwBenchRun s_bench;   /* bet 2.7a */

static unsigned load_config(void)
{
    int n;
    unsigned applied = 0;
    uint32_t skipped = 0;

    hwcfg_defaults(&s_cfg);
    (void)hwcfgio_recover();   /* RF2: a swap that was cut short leaves no config.ini but a complete .tmp / .bak: put one back BEFORE the file is read */
    n = hwctr_file_read(HWCFG_FILE, s_text, CFG_FILE_MAX - 1u);
    if (n > 0) {
        s_text_len = (size_t)n;
        s_text_whole = (size_t)n < CFG_FILE_MAX - 1u;
        applied = hwcfg_parse(&s_cfg, s_text, (size_t)n, &skipped);
        HWLOG("config.ini read: %u keys applied, %u lines ignored", applied, (unsigned)skipped);
        return applied;
    }
    n = (int)hwcfg_format(&s_cfg, s_text, CFG_FILE_MAX);
    s_text_len = n > 0 ? (size_t)n : 0u;
    s_text_whole = 1;
    hwctr_mkdirs(HWCTR_APP_DIR);
    if (n > 0 && hwcfgio_write(s_text, (size_t)n) == 0) {
        HWLOG("config.ini missing: defaults written to %s", HWCFG_FILE);
    } else {
        HWLOG("config.ini missing and could not be written (sdmc read-only?): defaults in use");
    }
    return 0;
}

/* The touch-panel calibration the game reads with TP_GetUserInfo: the points of hwin_tp_cal(), so the SDK's own calibration maps the 12-bit readings
   the TOUCH responder writes (pixel * 16 + 8) back to the DS pixel. */
static void fill_touch_cal(NVRAMConfig *nv)
{
    HwTpCal c = hwin_tp_cal();

    assert(nv != NULL);
    nv->ncd.tp.raw_x1 = c.raw_x1;
    nv->ncd.tp.raw_y1 = c.raw_y1;
    nv->ncd.tp.dx1 = c.dx1;
    nv->ncd.tp.dy1 = c.dy1;
    nv->ncd.tp.raw_x2 = c.raw_x2;
    nv->ncd.tp.raw_y2 = c.raw_y2;
    nv->ncd.tp.dx2 = c.dx2;
    nv->ncd.tp.dy2 = c.dy2;
}

static void fill_owner(NVRAMConfig *nv)
{
    uint32_t i;

    assert(nv != NULL);
    assert(sizeof nv->ncd.owner.nickname.str / sizeof nv->ncd.owner.nickname.str[0] == HWCFG_NAME_MAX);
    nv->ncd.version = NVRAM_CONFIG_DATA_VERSION;
    nv->ncd.owner.favoriteColor = s_cfg.color & 0xfu;
    nv->ncd.owner.birthday.month = s_cfg.birth_month;
    nv->ncd.owner.birthday.day = s_cfg.birth_day;
    for (i = 0; i < HWCFG_NAME_MAX; i++) {
        nv->ncd.owner.nickname.str[i] = s_cfg.name[i];
    }
    nv->ncd.owner.nickname.length = s_cfg.name_len;
    fill_touch_cal(nv);
    nv->ncd.option.language = s_cfg.language & 7u;
    nv->ncd.option.rtcOffset = s_cfg.id_rtc_set ? s_cfg.id_rtc_offset : s_cfg.rtc_offset;   /* identity only (bet 2.6a); the clock uses rtc_offset / rtc_fixed */
}

/* Bet 2.6a: the console identity the game compares on Continue (system_data.c:41-58: the MAC, then OS_GetOwnerRtcOffset()) comes from the SAVE, so Continue is not
   "played on another DS". Order: this runs inside hw_sys_init, after config.ini is read and BEFORE fill_page() writes the system page (NVRAM user info: the MAC behind
   the NVRAMConfig, ncd.option.rtcOffset), i.e. before the game's first read; the game reads both only in GameStartLoadSave_Main, after TryLoadingSave. The save is
   the file the card reads later (platinum.sav). Keyed per save by trainer id + secret id in config.ini; adopted_from_save = 0 keeps the flat keys. */
static void adopt_identity(void)
{
    size_t len = 0, add;
    const uint8_t *img = hw_devs_save_image(&len);
    HwSaveId id;
    char sec[640];
    int r;

    if (img == NULL || !hwsaveid_read(img, len, &id)) {
        HWLOG("identity: config (no valid save in %s: mac / rtc_offset from the config)", HWCTR_SAVE_FILE);
        return;
    }
    r = hwcfg_adopt_identity(&s_cfg, s_text, s_text_len, id.tid, id.sid, id.mac, id.rtc_offset);
    if (r == HWCFG_ID_OPTOUT) {
        HWLOG("identity: config (save TID=%u SID=%u has adopted_from_save = 0: mac / rtc_offset from the config)", (unsigned)id.tid, (unsigned)id.sid);
        return;
    }
    HWLOG("identity: adopted from save TID=%u SID=%u mac=%02x:%02x:%02x:%02x:%02x:%02x rtc_offset=%lld (slot %u, saveCounter %u; %s)", (unsigned)id.tid, (unsigned)id.sid,
          s_cfg.mac[0], s_cfg.mac[1], s_cfg.mac[2], s_cfg.mac[3], s_cfg.mac[4], s_cfg.mac[5], (long long)s_cfg.id_rtc_offset, (unsigned)id.slot, (unsigned)id.save_counter,
          r == HWCFG_ID_NEW ? "first sight: section written to config.ini" : "values from the config.ini section");
    if (r != HWCFG_ID_NEW) {
        return;
    }
    add = hwcfg_format_save(id.tid, id.sid, id.mac, id.rtc_offset, sec, sizeof sec);
    if (add == 0u || !s_text_whole || s_text_len + add >= CFG_FILE_MAX - 1u) {
        HWLOG("identity: section for TID=%u SID=%u not written (config.ini full, too long, or unreadable); adopted for this run only", (unsigned)id.tid, (unsigned)id.sid);
        return;
    }
    memcpy(s_text + s_text_len, sec, add);
    s_text_len += add;
    hwctr_mkdirs(HWCTR_APP_DIR);
    if (hwcfgio_write(s_text, s_text_len) != 0) {
        HWLOG("identity: config.ini not writable: the section for TID=%u SID=%u is adopted for this run only", (unsigned)id.tid, (unsigned)id.sid);
    }
}

/* The card header the IPL leaves in main RAM: the first 0x200 bytes of the ROM file (game code, maker, FNT/FAT/overlay-table offsets: boot.c
   CheckForMemoryTampering and CARD_GetRomRegion* read them), plus the chip ID the card driver's pulled-out check compares every read against. */
static void fill_rom_header(void)
{
    u8 hdr[HWCARD_HEADER_BYTES];

    assert(HWCARD_HEADER_BYTES == 0x200u);
    *(u32 *)HW_RED_RESERVED = HWCARD_CHIP_ID;
    if (hw_devs_rom_header(hdr) != 0) {
        HWLOG("ROM header unreadable: copy %s to the SD card (first 0x200 bytes needed); the card header stays zero", s_rom_path);
        return;
    }
    memcpy((u8 *)HW_ROM_HEADER_BUF, hdr, 0x160);
    memcpy((u8 *)HW_CARD_ROM_HEADER, hdr, 0x160);
    HWLOG("ROM header: game code %.4s maker %.2s", (const char *)hdr + 0x0c, (const char *)hdr + 0x10);
}

static void fill_page(void)
{
    NVRAMConfig *nv = (NVRAMConfig *)HW_NVRAM_USER_INFO;
    u8 *mac = (u8 *)(HW_NVRAM_USER_INFO + ((sizeof(NVRAMConfig) + 3u) & ~3u));
    CTRDGModuleInfo *mi = (CTRDGModuleInfo *)HW_CTRDG_MODULE_INFO_BUF;

    assert(((uintptr_t)mac + 8u) <= HW_NVRAM_USER_INFO_END);
    assert(sizeof(NVRAMConfig) + 8u < HW_NVRAM_USER_INFO_END - HW_NVRAM_USER_INFO);
    fill_owner(nv);
    memcpy(mac, s_cfg.mac, 6);
    /* The firmware's allowed-channel mask (DS firmware header 0x3C, bit n-1 = channel n) sits right behind the MAC in the IPL's copy of the user
       settings: the ARM9 WM library is built with WM_PRECALC_ALLOWEDCHANNEL and WM_GetAllowedChannel() reads exactly this u16 (wm_sync.c:199-210,
       NVRAMConfig rounded up to 4 bytes, then MAC, then +6). Zero here made WirelessManager_StartScan raise COMM_ERROR_RESET_SAVEPOINT (review 1.6, B1). */
    *(u16 *)(mac + 6) = s_cfg.allowed_channels;
    mi->moduleID.raw = 0xffffu;                  /* CTRDG_IsExisting() == FALSE: no GBA cartridge */
    *(u8 *)HW_IS_CTRDG_EXIST = 0;
    *(u8 *)HW_SET_CTRDG_MODULE_INFO_ONCE = TRUE;
    fill_rom_header();
    *(vu16 *)REG_KEYINPUT_ADDR = 0x03ffu;        /* no key down (active low); zero would read as every key pressed */
    *(vu16 *)HW_BUTTON_XY_BUF = 0x2c00u;         /* X, Y, DEBUG released (active low as PAD_Read inverts them), lid open (bit 15 clear) */
}

const HwBenchRun *hw_sys_bench(void)
{
    return &s_bench;
}

unsigned hw_sys_init(void)
{
    unsigned applied;

    if (s_done) {
        return 0;
    }
    s_done = 1;
    check_alignment();
    applied = load_config();
    (void)hw_setup_resolve_rom(&s_cfg, s_rom_path, sizeof s_rom_path);   /* release P4b: every reader of the ROM file asks hw_sys_rom_path() from here on */
    hwbench_start(&s_bench, &s_cfg);   /* bet 2.7a: the hardware-session bench picks this launch's entry and applies its overrides, before the save is read */
    if (s_bench.note[0] != '\0') {
        HWLOG("%s", s_bench.note);
    }
    if (s_bench.refused) {
        hwctr_fatal(s_bench.note);
    }
    if (s_bench.active) {
        hw_devs_hw27_bench(s_bench.tag);
    }
    adopt_identity();
    hw_os_set_clock(s_cfg.clock_frame);   /* bet 2.2a: before the first yield, so FRAME virtual time starts at 0 */
    fill_page();
    HWGX_Init((uint8_t *)&g_hwregs, g_hwvram_lcdc, (uint32_t)sizeof g_hwvram_lcdc, &s_gx_io);   /* bet 2.1a: canary + optional trace window */
    {
        HwGxSnap snap;

        memset(&snap, 0, sizeof snap);
        snap.pltt = g_hwpltt; snap.oam = g_hwoam; snap.region = snap_region; snap.vblanks = snap_vblanks; snap.lines = snap_lines;
        snap.clock_frame = s_cfg.clock_frame ? 1u : 0u;
        HWGX_SetSnapshot(&snap);
    }
    vram_init();   /* bet 2.1b: all banks disabled, store and windows zero */
    hw_devs_census_enable(s_cfg.census);   /* bet 2.1c/2.1c2: opt-in (config.ini `census = 1`); off = no observer, no per-frame sampling, no file */
    HWGX_SetSealMode(s_cfg.gx_seal_frame_end);
    HWGX_SetTrace(s_cfg.gx_trace_from, s_cfg.gx_trace_count, HWCTR_APP_DIR "/gxtrace.bin");
    hw_devs_rendersync(s_cfg.render_sync);
    hw_devs_hw27_cfg(s_cfg.render_core, s_cfg.hwcheck, s_cfg.clock_frame);   /* bet 2.7a */
    hw_devs_wide_cfg(s_cfg.wide, s_cfg.stereo, s_cfg.stereo_slider);   /* bet 2.8 */
    hw_devs_fbdump(s_cfg.fbdump_from, s_cfg.fbdump_count);   /* bet 2.3c */
    hw_devs_polydump(s_cfg.polydump_from, s_cfg.polydump_count);   /* bet 2.5b */
    return applied;
}

/* Soft reset: every DS memory array and the register file back to zero, then the system page filled again (owner, MAC, header copies).
   The reset parameter word is the caller's to keep (hw_boot.c saves it around the reseed). The ROM and the backup flash are not memory arrays: they survive. */
void hw_sys_reseed(void)
{
    assert(s_done);
    assert(sizeof g_hwsys == 0x1000u);
    memset(&g_hwregs, 0, sizeof g_hwregs);
    HWGX_ArmCanary();   /* the zeroed I/O block lost the GX canary pattern */
    memset(g_hwmainmem, 0, sizeof g_hwmainmem);
    memset(g_hwsys, 0, sizeof g_hwsys);
    memset(g_hwitcm, 0, sizeof g_hwitcm);
    memset(g_hwwram, 0, sizeof g_hwwram);
    memset(g_hwpltt, 0, sizeof g_hwpltt);
    memset(g_hwvram_bg, 0, sizeof g_hwvram_bg);
    memset(g_hwvram_dbbg, 0, sizeof g_hwvram_dbbg);
    memset(g_hwvram_obj, 0, sizeof g_hwvram_obj);
    memset(g_hwvram_dbobj, 0, sizeof g_hwvram_dbobj);
    memset(g_hwvram_lcdc, 0, sizeof g_hwvram_lcdc);
    vram_init();   /* the registers are zero again: every bank disabled */
    memset(g_hwoam, 0, sizeof g_hwoam);
    memset(SDK_AUTOLOAD_DTCM_START, 0, HW_DTCM_SIZE);
    fill_page();
}
