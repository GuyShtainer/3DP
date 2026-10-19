/* hw_pxi_arm7.c -- the loopback ARM7's per-tag responders. Pure C.
 *  PM     the SPI command protocol of ARM7/pm/pm.c: START/END/index words assembled into a command, executed against a 5-register PMIC,
 *         answered by SPIi_ReturnResult's word. PM_Init completes (handle bit), PM_*Register* and PMi_SyncFlag work.
 *  FS     the CARD request protocol (card_request.c): INIT is two words (request, command-block address), every request is acknowledged by
 *         CARD_REQ_ACK with ERR set, and the command block's `result` is written. INIT succeeds; every other request goes to ops.card_req
 *         (hw_devs.c -> hw_card.c: the 512 KB backup flash; CARD_RESULT_UNSUPPORTED when no hook is installed). ROM reads do not come
 *         through here: they go over the card-bus registers (hw_card.c).
 *  SOUND  the ARM7 sound driver's mailbox (hw_snd.c) when ops.snd is set (bet 1.5); otherwise the words are counted and dropped.
 *  TOUCH  the SPI touch-panel protocol (ARM7/spi/tp): SAMPLING writes the panel (system work touch_panel[]) from the 3DS touch screen and answers
 *         success; AUTO_ON/OFF switch a per-frame push of AUTO_SAMPLING words (hwarm7_input_frame).
 *  RTC    the DS RTC chip over the 3DS clock (hw_rtc.c); OS, MIC: swallow-only placeholders (their handle bit is seeded so the SDK's init waits
 *         complete). */
#include <assert.h>
#include <string.h>

#include "hw_log.h"
#include "hw_pure.h"

#define SPI_START_BIT   0x02000000u
#define SPI_END_BIT     0x01000000u
#define SPI_INDEX_MASK  0x000f0000u
#define SPI_INDEX_SHIFT 16u

#define PM_CMD_SYNC        0x60u
#define PM_CMD_SLEEP_START 0x61u
#define PM_CMD_SLEEP_END   0x62u
#define PM_CMD_UTILITY     0x63u
#define PM_CMD_REG_WRITE   0x64u
#define PM_CMD_REG_READ    0x65u
#define PM_CMD_SELF_BLINK  0x66u
#define PM_CMD_GET_BLINK   0x67u
#define PM_CMD_REG0VALUE   0x70u
#define PM_RESULT_SUCCESS  0u
#define PM_RESULT_INVALID  1u

#define TP_CMD_SAMPLING      0x00u
#define TP_CMD_AUTO_ON       0x01u
#define TP_CMD_AUTO_OFF      0x02u
#define TP_CMD_AUTO_SAMPLING 0x10u
#define RTC_PXI_COMMAND_SHIFT 8u
#define RTC_PXI_COMMAND_MASK  0x7f00u
#define RTC_PXI_RESULT_BIT    0x8000u

#define CARD_REQ_INIT        0u
#define CARD_REQ_ACK         1u
#define CARD_RESULT_UNSUPPORTED 3

static void spi_return(HwPxi *p, unsigned tag, unsigned command, unsigned result)
{
    /* SPIi_ReturnResult: START | END | index 0 | ((command & 0xff) | 0x80) << 8 | (result & 0xff), on the given tag (PM, touch panel) */
    uint32_t data = SPI_START_BIT | SPI_END_BIT | (((command & 0xffu) | 0x80u) << 8) | (result & 0xffu);

    (void)hwpxi_reply(p, tag, data, 0u);
}

static void pm_return(HwPxi *p, unsigned command, unsigned result)
{
    spi_return(p, HWPXI_TAG_PM, command, result);
}

static void pm_execute(HwPxi *p, HwArm7 *a)
{
    unsigned command = (a->pm_cmd[0] & 0xff00u) >> 8;
    unsigned reg;

    a->pm_commands++;
    switch (command) {
    case PM_CMD_SYNC:
        pm_return(p, PM_CMD_SYNC, PM_RESULT_SUCCESS);
        break;
    case PM_CMD_SLEEP_START:   /* the 3DS sleeps itself: report the wake-up at once */
        pm_return(p, PM_CMD_SLEEP_END, PM_RESULT_SUCCESS);
        break;
    case PM_CMD_REG_WRITE:
        reg = a->pm_cmd[0] & 0xffu;
        if (reg < sizeof a->pmic) {
            a->pmic[reg] = (uint8_t)a->pm_cmd[1];
        }
        pm_return(p, PM_CMD_REG_WRITE, PM_RESULT_SUCCESS);
        break;
    case PM_CMD_REG_READ:
        reg = a->pm_cmd[0] & 0xffu;   /* the register number is the low byte; bits 8-15 are the command */
        if (reg < sizeof a->pmic) {
            pm_return(p, PM_CMD_REG0VALUE + reg, a->pmic[reg]);
        } else {
            pm_return(p, PM_CMD_REG_READ, PM_RESULT_INVALID);
        }
        break;
    case PM_CMD_UTILITY:
    case PM_CMD_SELF_BLINK:
        pm_return(p, command, PM_RESULT_SUCCESS);
        break;
    case PM_CMD_GET_BLINK:
        pm_return(p, PM_CMD_GET_BLINK, 0u);
        break;
    default:
        pm_return(p, command, PM_RESULT_INVALID);
        break;
    }
}

static void pm_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag PM (ARM7 power/SPI responder)");
    HwArm7 *a = (HwArm7 *)user;

    assert(a != NULL);
    if (err) {
        return;
    }
    if (data & SPI_START_BIT) {
        memset(a->pm_cmd, 0, sizeof a->pm_cmd);
    }
    a->pm_cmd[(data & SPI_INDEX_MASK) >> SPI_INDEX_SHIFT] = (uint16_t)(data & 0xffffu);
    if (data & SPI_END_BIT) {
        pm_execute(p, a);
    }
}

static void card_ack(HwPxi *p)
{
    (void)hwpxi_reply(p, HWPXI_TAG_FS, CARD_REQ_ACK, 1u);
}

static void card_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag FS (ARM7 card responder)");
    HwArm7 *a = (HwArm7 *)user;
    int32_t *result;

    assert(a != NULL);
    if (!err) {
        return;
    }
    if (a->card_state == 1u) {   /* the second INIT word: the command block the ARM7 writes results into */
        a->card_cmd = data;
        a->card_state = 0u;
        a->card_requests++;
        card_ack(p);
        return;
    }
    if (data == CARD_REQ_INIT) {
        a->card_state = 1u;
        return;
    }
    a->card_requests++;
    result = (a->ops.xlat != NULL && a->card_cmd != 0u) ? (int32_t *)a->ops.xlat(a->ops.user, a->card_cmd) : NULL;
    if (result != NULL) {   /* CARDiCommandArg.result is the first member */
        *result = (a->ops.card_req != NULL) ? a->ops.card_req(a->ops.user, (unsigned)data, result) : CARD_RESULT_UNSUPPORTED;
    }
    card_ack(p);
}

static void snd_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag SOUND (forwarded to the ARM7 sound driver)");
    HwArm7 *a = (HwArm7 *)user;

    (void)p;
    assert(a != NULL);
    if (a->ops.snd != NULL) {
        hwsnd_send(a->ops.snd, data, err);   /* queued; the ARM7 takes it at the next slice (hw_snd.c) */
        return;
    }
    a->snd_swallowed++;   /* no ARM7 sound driver linked (tools such as card_fs.3dsx): count and drop */
}

static void other_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag OS/MIC (swallowed, no ARM7 model)");
    HwArm7 *a = (HwArm7 *)user;

    (void)p;
    (void)data;
    (void)err;
    assert(a != NULL);
    a->other_swallowed++;   /* OS / mic: nothing behind them yet (MIC is off the boot path) */
}

/* TOUCH: the SPI touch-panel protocol (ARM7/spi/tp). Every command is answered with its own success word: START | END | (cmd | 0x80) << 8 | SUCCESS.
   SAMPLING and every auto sample first write the panel into system work (SPITpData: x:12 y:12 touch:1 validity:2, the raw 12-bit ADC value the SDK's
   TP_GetCalibratedPoint expects: hwin_raw_from_px, calibrated by the points hwin_tp_cal puts in NVRAM). AUTO_ON's two-word command is answered once, at
   its END word, and starts the per-frame AUTO_SAMPLING pushes of hwarm7_input_frame. */
static void touch_write_panel(HwArm7 *a)
{
    uint8_t *w = a->ops.syswork != NULL ? a->ops.syswork(a->ops.user, 1u) : NULL;
    uint32_t v;

    assert(a != NULL);
    if (w == NULL) {
        return;
    }
    v = 0u;   /* validity 0 = both axes valid, touch 0: x = y = 0 */
    if (a->pen.touch) {
        v = (uint32_t)hwin_raw_from_px(a->pen.x) | ((uint32_t)hwin_raw_from_px(a->pen.y) << 12) | (1u << 24);
    }
    w[0] = (uint8_t)v;
    w[1] = (uint8_t)(v >> 8);
    w[2] = (uint8_t)(v >> 16);
    w[3] = (uint8_t)(v >> 24);
    a->touch_samples++;
}

/* WM: the wireless manager (hw_wm7.c answers; this only moves the words). The request word is a buffer address (< 2^26: a DS main-memory address). */
static void wm_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag WM (radio-less wireless manager)");
    HwArm7 *a = (HwArm7 *)user;
    uint32_t reply;

    assert(a != NULL);
    if (err) {
        return;
    }
    if (a->ops.wm_req == NULL) {
        a->other_swallowed++;
        return;
    }
    a->wm_requests++;
    reply = a->ops.wm_req(a->ops.user, data);
    if (reply != 0u) {
        a->wm_replies++;
        (void)hwpxi_reply(p, HWPXI_TAG_WM, reply, 0u);
    }
}

static void touch_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag TOUCH (touch panel)");
    HwArm7 *a = (HwArm7 *)user;

    assert(a != NULL);
    if (err) {
        return;
    }
    if (data & SPI_START_BIT) {
        a->touch_cmd = (data & 0x7f00u) >> 8;
        if (a->touch_cmd == TP_CMD_AUTO_ON) {
            a->auto_freq = (uint8_t)(data & 0xffu);
        }
    }
    if (data & SPI_END_BIT) {
        a->touch_commands++;
        if (a->touch_cmd == TP_CMD_SAMPLING) {
            touch_write_panel(a);
        } else if (a->touch_cmd == TP_CMD_AUTO_ON) {
            a->auto_on = a->auto_freq != 0u;
        } else if (a->touch_cmd == TP_CMD_AUTO_OFF) {
            a->auto_on = 0u;
        }
        spi_return(p, HWPXI_TAG_TOUCH, a->touch_cmd, PM_RESULT_SUCCESS);
    }
}

void hwarm7_input_frame(HwArm7 *a, HwPxi *p, const HwInState *st)
{
    HWCOUNT("input: hwarm7_input_frame (KEYINPUT/XY/pen per frame)");
    unsigned i;

    assert(a != NULL && p != NULL && st != NULL);
    a->pen = *st;
    if (!a->auto_on) {
        return;
    }
    assert(a->auto_freq <= 16u);
    for (i = 0; i < a->auto_freq && i < 16u; i++) {   /* one push per sampling interval; the SDK's ring keeps the last `bufSize` of them */
        touch_write_panel(a);
        spi_return(p, HWPXI_TAG_TOUCH, TP_CMD_AUTO_SAMPLING, i);
        a->auto_pushes++;
    }
}

/* RTC: the SDK's command word is command << 8 (RtcSendPxiCommand); the answer is RESULT_BIT | command << 8 | result. */
static void rtc_responder(HwPxi *p, void *user, uint32_t data, unsigned err)
{
    HWCOUNT("PXI tag RTC (clock chip)");
    HwArm7 *a = (HwArm7 *)user;
    uint8_t *buf;
    unsigned cmd, result;

    assert(a != NULL);
    if (err) {
        return;
    }
    cmd = (data & RTC_PXI_COMMAND_MASK) >> RTC_PXI_COMMAND_SHIFT;
    buf = a->ops.syswork != NULL ? a->ops.syswork(a->ops.user, 0u) : NULL;
    if (buf == NULL || a->ops.rtc_secs == NULL) {
        a->other_swallowed++;   /* no clock wired (the tools that exercise other units) */
        return;
    }
    a->rtc_commands++;
    result = hwrtc_command(&a->rtc, cmd, buf, a->ops.rtc_secs(a->ops.user));
    (void)hwpxi_reply(p, HWPXI_TAG_RTC, RTC_PXI_RESULT_BIT | (cmd << RTC_PXI_COMMAND_SHIFT) | result, 0u);
}

void hwarm7_init(HwArm7 *a, HwPxi *p, const HwArm7Ops *ops)
{
    assert(a != NULL && p != NULL && ops != NULL);
    memset(a, 0, sizeof *a);
    a->ops = *ops;
    hwrtc_init(&a->rtc, ops->rtc_offset);
    a->pmic[0] = 0x0fu;   /* PMIC control: sound amplifier on, both backlights on (the post-boot state) */
    hwpxi_set_responder(p, HWPXI_TAG_PM, pm_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_FS, card_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_SOUND, snd_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_OS, other_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_RTC, rtc_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_TOUCH, touch_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_MIC, other_responder, a);
    hwpxi_set_responder(p, HWPXI_TAG_WM, wm_responder, a);
}
