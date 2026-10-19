/* pxi_sdk_test.c -- the REAL SDK code (pxi_fifo.c, spi/pm.c, os_interrupt.c) against the loopback ARM7 (source/hw/hw_devs.c over hw_pxi*.c),
 * built by tools/host-tests/build.sh. Review 1.3 BLOCKER 2: before the fix PXI_Init spun forever on the ARM7 handshake and PM_Init on the
 * ARM7's handle bits. Here: PXI_Init returns, the handle bits of every ARM7 service are published, PM_Init completes, a PM sync word
 * sent through the SDK's PXI_SendWordByFifo comes back through the SDK's receive IRQ handler into a callback registered with
 * PXI_SetFifoRecvCallback, and an unregistered tag is echoed with err = TRUE (as the SDK's ARM7 handler does).
 * Prints "N checks, 0 failures"; exits non-zero on a failure. Rules: ~/.claude/c-golden-rules.md. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <nitro/types.h>
#include <nitro/os.h>
#include <nitro/pxi.h>
#include <nitro/spi/ARM9/pm.h>
#include <nitro/spi/common/type.h>
#include <nitro/spi/ARM9/tp.h>
#include <nitro/rtc.h>
#include <hw_os_hooks.h>
#include "hw_cfg.h"
#include "hw_devs.h"
#include "hw_pure.h"
#include "hw_sys.h"

HwRegs g_hwregs;
u8 g_hwmainmem[0x800000] __attribute__((aligned(4096)));
u8 g_hwsys[0x1000] __attribute__((aligned(4096)));
u8 g_hwitcm[0x8000], g_hwwram[0x8000], g_hwpltt[0x800], g_hwvram_bg[0x80000], g_hwvram_dbbg[0x20000], g_hwvram_obj[0x40000];
u8 g_hwvram_dbobj[0x20000], g_hwvram_lcdc[0xA4000] __attribute__((aligned(4096))), g_hwoam[0x800], g_hwctrdg_rom[16], g_hwbios[0x8000];
u32 SDK_AUTOLOAD_DTCM_START[0x1000];

static uint32_t g_cpsr = 0x1f;
static uint32_t g_cp15[HWOS_CP15_COUNT];
uint32_t HWOS_CpsrRead(void) { return g_cpsr; }
void HWOS_CpsrWriteC(uint32_t value) { g_cpsr = (g_cpsr & ~0xffu) | (value & 0xffu); }
uint32_t HWOS_Cp15Read(HWOSCp15Reg reg) { return g_cp15[reg]; }
void HWOS_Cp15Write(HWOSCp15Reg reg, uint32_t value) { g_cp15[reg] = value; }
void HWOS_SpinWait(uint32_t cycle) { (void)cycle; }
/* hw_log.c wants these three */
int hwctr_file_write(const char *p, const void *b, uint32_t n) { (void)p; (void)b; return (int)n; }
void hwctr_mkdirs(const char *d) { (void)d; }
void hwctr_console_write(const char *t) { (void)t; }
/* hw_devs.c boots the card bus: no save, no ROM here */
int hwctr_file_read(const char *p, void *b, uint32_t n) { (void)p; (void)b; (void)n; return -1; }
void *hwctr_file_open(const char *p) { (void)p; return NULL; }
int hwctr_file_pread(void *f, uint32_t o, void *d, uint32_t n) { (void)f; (void)o; (void)d; (void)n; return -1; }

/* bet 1.6b: the 3DS clock and the config, as the hardware layer's seams give them */
static int64_t g_now;
int64_t hwctr_rtc_secs(void) { return g_now; }
uint64_t hwctr_sys_tick(void) { static uint64_t t; return t += 4472u; }   /* bet 2.1c2: hw_devs.c reads the real tick for its frame-wall instrument */
#include "hw_ctr.h"
/* bet 2.7a: the run header's probes (hw_ctr.c on the 3DS); the host answers with fixed values */
void hwctr_info(HwCtrInfo *o) { *o = (HwCtrInfo){0}; o->is_new3ds = 1; }
uint32_t hwctr_linear_free(void) { return 0x1000000u; }
int hwctr_slider_pct(void) { return 0; }
int hwctr_proc_id(void) { return 0; }
uint64_t hwctr_idle_ticks(void) { return 0u; }
int hwctr_thread_ran_on(void *h) { (void)h; return -1; }
uint32_t hwctr_measure_mhz100(void) { return 80400u; }
static HwCfg g_cfg;
const HwCfg *hw_sys_cfg(void) { return &g_cfg; }

/* the radio-less WM is its own SDK-world unit (hw_wm7.c, covered by the emulator gate); this test has no WM traffic */
uint32_t hw_wm7_request(uint32_t cmd_addr) { (void)cmd_addr; return 0u; }
void hw_wm7_reset(void) {}
uint32_t hw_wm7_tick(void) { return 0u; }
void hw_wm7_report(unsigned a, unsigned b, unsigned c) { (void)a; (void)b; (void)c; }

static long g_checks, g_fails;
static const char *volatile g_phase = "start";

/* A hung SDK spin must fail the gate, not stall it: the alarm prints where it stood and leaves with a non-zero status. */
static void on_alarm(int sig)
{
    static const char msg[] = "pxi_sdk_test: TIMEOUT or CRASH (a spin never finished, or a fault: signal in the status), phase: ";

    (void)sig;
    (void)!write(2, msg, sizeof msg - 1u);
    (void)!write(2, g_phase, strlen(g_phase));
    (void)!write(2, "\n", 1);
    _exit(100 + sig);   /* a handled fault never reaches the system crash reporter, which can leave the process unkillable */
}

static void check(int ok, const char *what, long detail)
{
    assert(what != NULL);
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    }
}

static int g_cb_n;
static u32 g_cb_data;
static BOOL g_cb_err;
static PXIFifoTag g_cb_tag;

static void cb(PXIFifoTag tag, u32 data, BOOL err)
{
    g_cb_n++;
    g_cb_tag = tag;
    g_cb_data = data;
    g_cb_err = err;
}

/* what the pump does at the receive-FIFO IRQ: take IF bit 18, run OS_IRQTable[18] */
static void deliver_irq(void)
{
    u32 ifl = *(u32 *)((u8 *)&g_hwregs + 0x214);

    if (ifl & (1u << 18)) {
        *(u32 *)((u8 *)&g_hwregs + 0x214) = ifl & ~(1u << 18);
        assert(OS_IRQTable[18] != NULL);
        OS_IRQTable[18]();
    }
}


/* ---- bet 1.6b: the real RTC code (rtc/src/external.c) over the RTC responder, and the real TP code (spi/src/tp.c) over the touch responder ---- */
static int g_rtc_done;
static RTCResult g_rtc_result;
static void rtc_cb(RTCResult r, void *arg) { (void)arg; g_rtc_done++; g_rtc_result = r; }

static void drain_irq(void)
{
    int i;

    for (i = 0; i < 64; i++) {
        deliver_irq();
    }
}

static void test_rtc(void)
{
    RTCDate d;
    RTCTime t;
    RTCDate nd = { 27, 1, 2, 0 };
    RTCTime nt = { 8, 5, 9 };
    RTCDate d2;
    RTCTime t2;

    g_now = hwrtc_days_from_civil(2026, 10, 12) * 86400 + 13 * 3600 + 45 * 60 + 30;   /* Monday 2026-10-12 13:45:30 */
    g_phase = "rtc: RTC_Init";
    RTC_Init();
    check(1, "RTC_Init completes (the RTC handle bit is seeded)", 0);
    check(RTC_GetDateTimeAsync(&d, &t, rtc_cb, NULL) == RTC_RESULT_SUCCESS, "RTC_GetDateTimeAsync is accepted", 0);
    drain_irq();
    check(g_rtc_done == 1 && g_rtc_result == RTC_RESULT_SUCCESS, "its callback fires with SUCCESS (the reply words came back)", g_rtc_done);
    check(d.year == 26 && d.month == 10 && d.day == 12 && d.week == RTC_WEEK_MONDAY, "the date is 2026-10-12, a Monday", d.year * 10000 + d.month * 100 + d.day);
    check(t.hour == 13 && t.minute == 45 && t.second == 30, "the time is 13:45:30 (24-hour format)", t.hour * 10000 + t.minute * 100 + t.second);
    /* the clock keeps moving between reads */
    g_now += 61;
    g_rtc_done = 0;
    (void)RTC_GetDateTimeAsync(&d, &t, rtc_cb, NULL);
    drain_irq();
    check(g_rtc_done == 1 && t.hour == 13 && t.minute == 46 && t.second == 31, "61 s later the time is 13:46:31", t.minute * 100 + t.second);
    /* the game's "set clock": a write moves the offset, later reads follow it */
    g_rtc_done = 0;
    check(RTC_SetDateTimeAsync(&nd, &nt, rtc_cb, NULL) == RTC_RESULT_SUCCESS, "RTC_SetDateTimeAsync is accepted", 0);
    drain_irq();
    check(g_rtc_done == 1 && g_rtc_result == RTC_RESULT_SUCCESS, "the set completes with SUCCESS", g_rtc_result);
    g_rtc_done = 0;
    (void)RTC_GetDateTimeAsync(&d2, &t2, rtc_cb, NULL);
    drain_irq();
    check(g_rtc_done == 1 && d2.year == 27 && d2.month == 1 && d2.day == 2 && t2.hour == 8 && t2.minute == 5 && t2.second == 9,
          "a read after the set returns the new date and time", d2.year * 10000 + d2.month * 100 + d2.day);
    g_now += 3600;
    g_rtc_done = 0;
    (void)RTC_GetDateTimeAsync(&d2, &t2, rtc_cb, NULL);
    drain_irq();
    check(t2.hour == 9 && t2.minute == 5 && t2.second == 9, "an hour later the set clock reads 09:05:09 (it keeps running)", t2.hour);
}

static void test_touch(void)
{
    static const uint16_t xs[] = { 0, 1, 31, 32, 100, 127, 223, 224, 255 };
    static const uint16_t ys[] = { 0, 23, 24, 50, 96, 167, 168, 191 };
    NVRAMConfig *nv = (NVRAMConfig *)OS_GetSystemWork()->nvramUserInfo;
    HwTpCal c = hwin_tp_cal();
    TPCalibrateParam cal;
    TPData raw, disp, bufs[8];
    HwInState st;
    unsigned i, j, bad = 0;

    nv->ncd.tp.raw_x1 = c.raw_x1; nv->ncd.tp.raw_y1 = c.raw_y1; nv->ncd.tp.dx1 = c.dx1; nv->ncd.tp.dy1 = c.dy1;
    nv->ncd.tp.raw_x2 = c.raw_x2; nv->ncd.tp.raw_y2 = c.raw_y2; nv->ncd.tp.dx2 = c.dx2; nv->ncd.tp.dy2 = c.dy2;
    g_phase = "touch: TP_Init";
    TP_Init();
    check(TP_GetUserInfo(&cal) == TRUE && cal.xDotSize != 0 && cal.yDotSize != 0, "TP_GetUserInfo derives a non-zero calibration from the NVRAM points", cal.xDotSize);
    g_phase = "touch: TP_SetCalibrateParam";
    TP_SetCalibrateParam(&cal);
    g_phase = "touch: pen-up sample";
    memset(&st, 0, sizeof st);
    hw_devs_input_frame(&st);
    TP_RequestSamplingAsync();
    drain_irq();
    check(TP_WaitRawResult(&raw) == 0 && raw.touch == TP_TOUCH_OFF, "a sample with the pen up reads touch = off", raw.touch);
    g_phase = "touch: pen positions";
    for (i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        for (j = 0; j < sizeof ys / sizeof ys[0]; j++) {
            st.touch = 1;
            st.x = xs[i];
            st.y = ys[j];
            hw_devs_input_frame(&st);
            TP_RequestSamplingAsync();
            drain_irq();
            if (TP_WaitRawResult(&raw) != 0 || raw.touch != TP_TOUCH_ON || raw.validity != TP_VALIDITY_VALID) {
                bad++;
                continue;
            }
            TP_GetCalibratedPoint(&disp, &raw);
            if (disp.x != xs[i] || disp.y != ys[j]) {
                bad++;
                fprintf(stderr, "touch %u,%u came back %u,%u (raw %u,%u)\n", xs[i], ys[j], disp.x, disp.y, raw.x, raw.y);
            }
        }
    }
    check(bad == 0, "72 pen positions: the SDK's calibrated point equals the DS pixel exactly", bad);
    g_phase = "touch: auto sampling";
    /* auto sampling: AUTO_ON arms the push, each input frame pushes `frequency` samples, the SDK ring holds the latest */
    st.touch = 1; st.x = 77; st.y = 133;
    hw_devs_input_frame(&st);
    TP_RequestAutoSamplingStartAsync(0, 4, bufs, 8);
    drain_irq();
    check(TP_CheckBusy(TP_REQUEST_COMMAND_FLAG_AUTO_ON) == 0, "AUTO_ON is acknowledged (state = auto sampling)", 0);
    hw_devs_input_frame(&st);
    drain_irq();
    TP_GetLatestCalibratedPointInAuto(&disp);
    check(disp.touch == TP_TOUCH_ON && disp.x == 77 && disp.y == 133, "an input frame pushes samples; the latest calibrated point is the pen", disp.x * 1000 + disp.y);
    st.touch = 0;
    hw_devs_input_frame(&st);
    drain_irq();
    TP_GetLatestCalibratedPointInAuto(&disp);
    check(disp.touch == TP_TOUCH_OFF, "pen up: the next pushes read touch = off", disp.touch);
    TP_RequestAutoSamplingStopAsync();
    drain_irq();
    check(TP_CheckBusy(TP_REQUEST_COMMAND_FLAG_AUTO_OFF) == 0, "AUTO_OFF is acknowledged", 0);
}

int main(void)
{
    u32 word;
    int i;

    (void)signal(SIGALRM, on_alarm);
    (void)signal(SIGSEGV, on_alarm);
    (void)signal(SIGBUS, on_alarm);
    (void)signal(SIGABRT, on_alarm);
    (void)signal(SIGILL, on_alarm);
    (void)signal(SIGFPE, on_alarm);
    (void)alarm(30);
    PXI_Init();   /* before the fix: loops for ever on the ARM7 INTF handshake */
    check(1, "PXI_Init returns (the emulator branch is taken under SDK_GCC)", 0);
    check(PXI_IsCallbackReady(PXI_FIFO_TAG_PM, PXI_PROC_ARM7) && PXI_IsCallbackReady(PXI_FIFO_TAG_FS, PXI_PROC_ARM7) &&
          PXI_IsCallbackReady(PXI_FIFO_TAG_SOUND, PXI_PROC_ARM7) && PXI_IsCallbackReady(PXI_FIFO_TAG_OS, PXI_PROC_ARM7) &&
          PXI_IsCallbackReady(PXI_FIFO_TAG_RTC, PXI_PROC_ARM7) && PXI_IsCallbackReady(PXI_FIFO_TAG_TOUCHPANEL, PXI_PROC_ARM7) &&
          PXI_IsCallbackReady(PXI_FIFO_TAG_MIC, PXI_PROC_ARM7), "ARM7 handle bits published for PM, FS, SND, OS, RTC, TOUCH, MIC", 0);
    check(!PXI_IsCallbackReady(PXI_FIFO_TAG_WVR, PXI_PROC_ARM7), "no wifi: the WVR tag has no ARM7 handler", 0);
    PM_Init();   /* the empty-loop wait on pxiHandleChecker */
    check(1, "PM_Init completes", 0);
    /* a callback on the PM tag: the SDK's PM_Init registered PMi_CommonCallback; replace it with ours to read the raw word */
    PXI_SetFifoRecvCallback(PXI_FIFO_TAG_PM, cb);
    /* SPI sync: START | END | command 0x60 */
    word = 0x02000000u | 0x01000000u | (0x60u << 8);
    check(PXI_SendWordByFifo(PXI_FIFO_TAG_PM, word, FALSE) == 0, "PXI_SendWordByFifo(PM) is accepted (send FIFO never full)", 0);
    deliver_irq();
    check(g_cb_n == 1 && g_cb_tag == PXI_FIFO_TAG_PM && !g_cb_err && g_cb_data == (0x02000000u | 0x01000000u | 0xe000u),
          "the SDK's receive IRQ handler hands the PM sync echo to the registered callback", (long)g_cb_data);
    /* register 0 read: 0x0f after boot */
    word = 0x02000000u | 0x01000000u | (0x65u << 8) | 0u;
    (void)PXI_SendWordByFifo(PXI_FIFO_TAG_PM, word, FALSE);
    deliver_irq();
    check(g_cb_n == 2 && (g_cb_data & 0xffu) == 0x0fu, "PM register 0 reads back 0x0f", (long)g_cb_data);
    /* a tag with no responder: echoed with err */
    PXI_SetFifoRecvCallback((PXIFifoTag)20, cb);
    (void)PXI_SendWordByFifo((PXIFifoTag)20, 0x1234u, FALSE);
    deliver_irq();
    check(g_cb_n == 3 && g_cb_tag == 20 && g_cb_err && g_cb_data == 0x1234u, "an unregistered tag is echoed back with err = TRUE", (long)g_cb_data);
    /* the send-retry spins: 40 words in a row never find the send FIFO full */
    for (i = 0; i < 40; i++) {
        if (PXI_SendWordByFifo(PXI_FIFO_TAG_SOUND, (u32)i, FALSE) != 0) {
            break;
        }
    }
    check(i == 40, "40 SND words are accepted (swallowed by the placeholder) without a full send FIFO", i);
    g_phase = "rtc";
    test_rtc();
    g_phase = "touch";
    test_touch();
    printf("pxi_sdk_test: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
