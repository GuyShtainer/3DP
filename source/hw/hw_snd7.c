/* hw_snd7.c -- binds hw_snd.c (mailbox + cadence) to the real ARM7 sound driver: the NitroSDK snd sources built with SDK_ARM7, partially linked
 * and prefixed arm7_ by tools/arm7_snd.py (`make port-arm7-snd` -> port/obj/arm7_snd.o). SDK-world file. Mapping, the list of bypassed ARM7
 * entry points and the redirected references: the private design notes.
 *
 * What runs where (no second scheduler, no ARM7 thread):
 *   boot  = arm7_SND_CommandInit() (message queue, PXI callback registration -> captured below) + arm7_SNDi_PortDriverInit() (the part of SndThread
 *           before its loop minus the interval alarm; patched into snd_main.c).
 *   recv  = the ARM7's PxiFifoCallback (static in snd_command.c, reached through the pointer it registered): a pointer word is OS_SendMessage'd
 *           to the command queue, word 0 posts the wake-up message.
 *   run   = arm7_SNDi_PortDriverRun(): one pass of SndThread's loop (SND_UpdateExChannel, SND_CommandProc, SND_SeqMain, SND_ExChannelMain,
 *           SND_UpdateSharedWork, SND_CalcRandom).
 * References the ARM7 code makes outward, redirected by the symbol rename (tools/arm7_snd.py): PXI_SetFifoRecvCallback -> capture, PXI_SendWordByFifo
 * -> the reply into the ARM9 receive FIFO, SVC_*SoundBias and PMi_*Control (ARM7-only services; sleep paths) -> counters, OS_*Alarm / OS_GetTick
 * -> the ARM7's private alarm list (hw_snd.c), driven by the slice's virtual time: the ARM7 must not ride the ARM9 alarm queue, which the game never
 * initialises (system.c) and wfc_settings re-initialises.
 * The whole slice runs between HWOS_Arm7Enter and HWOS_Arm7Leave (ops.enter / ops.leave): no ARM9 IRQ and no thread switch happens inside it (review 1.5 B1). */
#include <assert.h>

#include <nitro.h>

#include "hw_ctr.h"
#include "hw_devs.h"
#include "hw_gx.h"
#include "hw_log.h"
#include "hw_os.h"
#include "hw_pure.h"
#include "hw_snd7.h"
#include <hw_os_hooks.h>

extern void arm7_SND_CommandInit(void);
extern void arm7_SNDi_PortDriverInit(void);
extern BOOL arm7_SNDi_PortDriverRun(BOOL periodic);

static HwSnd s_snd;
static PXIFifoCallback s_cb;
static uint64_t s_slice_ticks, s_slice_t0;   /* bet 2.1e (Rule 7): wall ticks (svcGetSystemTick) spent inside ARM7 slices, and the open slice's start */
static uint32_t s_slice_calls, s_slice_depth;
static uint32_t s_cb_set, s_cb_calls, s_no_cb, s_foreign_tag, s_sleep_calls;

/* ---- the ARM7's outward references (arm7_snd.o is renamed to call these) ---- */
void hwsnd_Arm7SetFifoRecvCallback(int fifotag, PXIFifoCallback cb)
{
    if (fifotag == PXI_FIFO_TAG_SOUND) {
        s_cb = cb;
        s_cb_set++;
    } else {
        s_foreign_tag++;
    }
}

int hwsnd_Arm7SendWord(int fifotag, u32 data, BOOL err)
{
    if (fifotag != PXI_FIFO_TAG_SOUND || err) {
        s_foreign_tag++;
        return -1;
    }
    return hwsnd_reply(&s_snd, data);
}

void hwsnd_SvcSetSoundBias(s32 stepLoops)
{
    (void)stepLoops;
    s_sleep_calls++;
}

void hwsnd_SvcResetSoundBias(s32 stepLoops)
{
    (void)stepLoops;
    s_sleep_calls++;
}

void hwsnd_PmSetControl(u8 ctrl)
{
    (void)ctrl;
    s_sleep_calls++;
}

void hwsnd_PmResetControl(u8 ctrl)
{
    (void)ctrl;
    s_sleep_calls++;
}

/* the ARM7's alarms: OS_SetAlarm's tick is relative, OS_SetPeriodicAlarm's start is absolute (OS_GetTick() + n at every SDK call site) */
void hwsnd_Arm7CreateAlarm(OSAlarm *alarm)
{
    alarm->handler = NULL;
    alarm->tag = 0;
}

void hwsnd_Arm7SetAlarm(OSAlarm *alarm, OSTick tick, OSAlarmHandler handler, void *arg)
{
    hwsnd_alarm_set(&s_snd, alarm, hwsnd_now_tick(&s_snd) + tick, 0u, handler, arg);
}

void hwsnd_Arm7SetPeriodicAlarm(OSAlarm *alarm, OSTick start, OSTick period, OSAlarmHandler handler, void *arg)
{
    hwsnd_alarm_set(&s_snd, alarm, start, period, handler, arg);
}

void hwsnd_Arm7CancelAlarm(OSAlarm *alarm)
{
    hwsnd_alarm_cancel(&s_snd, alarm);
}

OSTick hwsnd_Arm7GetTick(void)
{
    return (OSTick)hwsnd_now_tick(&s_snd);
}

/* The ARM7's thread entry points are compiled with the ARM7 OSThread/OSContext layout (164/72 bytes; the ARM9's are 192/100): handing them to the ARM9
   OS_CreateThread would corrupt memory. snd_main.c's SND_CreateThread / SND_SetThreadPriority are compiled out under SDK_GCC (nitrosdk-snd-arm7.patch)
   and tools/arm7_snd.py fails if the object still references them; nothing else here may create an ARM7 thread. */

/* ---- HwSndOps ---- */
static void ops_enter(void *u)
{
    (void)u;
    HWGX_SliceBegin();
    HWOS_Arm7Enter();
    if (s_slice_depth++ == 0u) {
        s_slice_t0 = hwctr_sys_tick();
    }
}

static void ops_leave(void *u)
{
    (void)u;
    if (s_slice_depth != 0u && --s_slice_depth == 0u) {
        s_slice_ticks += hwctr_sys_tick() - s_slice_t0;
        s_slice_calls++;
    }
    HWOS_Arm7Leave();
    HWGX_SliceEnd();
}

static void ops_boot(void *u)
{
    (void)u;
    s_cb = NULL;
    arm7_SND_CommandInit();
    arm7_SNDi_PortDriverInit();
    HWLOG("ARM7 sound driver booted (PXI callback %s)", s_cb != NULL ? "registered" : "MISSING");
}

static void ops_recv(void *u, uint32_t data, unsigned err)
{
    (void)u;
    if (s_cb == NULL) {
        s_no_cb++;
        return;
    }
    s_cb_calls++;
    s_cb(PXI_FIFO_TAG_SOUND, data, err ? TRUE : FALSE);
}

static int ops_run(void *u, int periodic)
{
    (void)u;
    return arm7_SNDi_PortDriverRun(periodic ? TRUE : FALSE) ? 1 : 0;
}

static void pump(void)
{
    if (s_snd.in_pump) {
        /* the slice's own code reached a pump (an OS_SpinWait on the ARM7 side: SND_BeginSleep/EndSleep, never reached in this build). Refusing it would
           hide a stranded slice, so it is fatal. */
        HWLOG("ARM7 slice re-entered a pump (in_pump=1)");
        assert(!"hw_snd7: re-entrant ARM7 slice");
    }
    hwsnd_pump(&s_snd, hw_os_pump()->time.virt);
}

HwSnd *hw_snd7_attach(HwPxi *pxi, uint8_t *io)
{
    static const HwSndOps ops = { ops_enter, ops_leave, ops_boot, ops_recv, ops_run, NULL };

    hwsnd_init(&s_snd, &ops, io, pxi);
    s_cb = NULL;
    hw_os_set_arm7_pump(pump);
    return &s_snd;
}

void hw_snd7_timing(uint64_t *ticks, uint32_t *calls)
{
    if (ticks != NULL) {
        *ticks = s_slice_ticks;
    }
    if (calls != NULL) {
        *calls = s_slice_calls;
    }
}

const HwSnd *hw_snd7_state(void)
{
    return &s_snd;
}

void hw_snd7_extra(uint32_t *cb_set, uint32_t *cb_calls, uint32_t *no_cb, uint32_t *foreign_tag, uint32_t *sleep_calls)
{
    if (cb_set != NULL) {
        *cb_set = s_cb_set;
    }
    if (cb_calls != NULL) {
        *cb_calls = s_cb_calls;
    }
    if (no_cb != NULL) {
        *no_cb = s_no_cb;
    }
    if (foreign_tag != NULL) {
        *foreign_tag = s_foreign_tag;
    }
    if (sleep_calls != NULL) {
        *sleep_calls = s_sleep_calls;
    }
}
