/* hw_os.c -- the HWOS_* hooks of port-shim/include/hw_os_hooks.h (19 of them), OS_SaveContext, SVC_WaitByLoop and the tick that
 * replaces os_tick.c, over the pure modules of hw_pure.h. SDK-world file: compiled with the port flags, no libctru (hw_ctr.h is the seam).
 * Design: the private design notes. The DS CPU state it stands in for is a shadow: one CPSR word, a cp15 register file.
 */
#include <assert.h>
#include <string.h>

#include <nitro/os.h>
#include <nitro/gx.h>
#include <nitro/mi/uncompress.h>
#include <nitro/cp/context.h>
#include <hw_os_hooks.h>

#include "hw_ctr.h"
#include "hw_devs.h"
#include "hw_log.h"
#include "hw_os.h"
#include "hw_pure.h"

#define PSR_I           0x80u
#define PSR_F           0x40u
#define PSR_MODE_IRQ    0x12u
#define PSR_CTRL_MASK   0xffu
#define MIN_HOST_STACK  0x10000u
#define MAX_DISPATCH    64u        /* per pump: a handler that re-raises its own IRQ forever is a bug, not a loop */

extern void OSi_SetTimerReserved(int timerNum);
extern OSThreadQueue OSi_IrqThreadQueue;   /* os_irqHandler.c; the SDK_THREAD_INFINITY form { head, tail } */
extern vu64 OSi_TickCounter;
vu64 OSi_TickCounter;
BOOL OSi_NeedResetTimer = FALSE;

static uint32_t s_cpsr = HWOS_BOOT_CPSR;        /* SYS mode, IRQ and FIQ ON: crt0.c:69-70 enables them before NitroMain; the SDK masks with IME/IE, not the I bit */
static uint32_t s_irq_saved;                    /* the CPSR the running IRQ interrupted */
static uint32_t s_cp15[HWOS_CP15_COUNT] = { 0x00012078u };
static HwPump s_pump;
static uint32_t s_forced_carry;   /* forced vblanks (FRAME clock) of the pump instances before the last soft reset */
static unsigned s_clock_frame;    /* bet 2.2a: config.ini `clock`; survives the pump re-init of a soft reset */
static uint64_t s_vblank_carry;   /* DS vblanks of the pump instances before the last soft reset (bet 2.1e: the rtc_fixed clock) */
static HwCtx s_ctx;
static int s_ready;
static int s_tick_on;
static void (*s_arm7_pump)(void);
static void (*s_vblank_hook)(void);   /* bet 2.3c: runs before the VBlank handler (hw_devs.c: the renderer-overlap counter) */
static uint32_t s_arm7_depth;                   /* >0 while the ARM7 sound slice runs: ARM9 IRQs stay pending (see HWOS_Arm7Enter) */

/* ---- pump environment -------------------------------------------------------------------------------------------------- */
static uint64_t env_real_tick(void *u)
{
    (void)u;
    return hwctr_sys_tick();
}

static void env_wait_vblank(void *u)
{
    (void)u;
    hwctr_wait_vblank();
}

static void env_sleep(void *u, uint64_t sys)
{
    (void)u;
    hwctr_sleep_sys(sys);
}

static void env_arm7(void *u)
{
    (void)u;
    if (s_arm7_pump) {
        s_arm7_pump();
    }
}

static int env_irq_masked(void *u)
{
    (void)u;
    return (s_cpsr & PSR_I) != 0 || s_arm7_depth != 0u;   /* inside the ARM7 slice no ARM9 IRQ runs, whatever the ARM7 code's OS_RestoreInterrupts did to the shadow CPSR */
}

/* run OS_IRQTable[irq] the way the vector did: IRQ mode, interrupts off, then the thread switch (lr = OS_IrqHandler_ThreadSwitch) */
static void env_dispatch(void *u, int irq)
{
    uint32_t saved = s_cpsr, prev = s_irq_saved;
    OSIrqFunction f = NULL;

    (void)u;
    assert(irq >= 0 && irq < 32);
    if (irq >= 0 && irq < OS_IRQ_TABLE_MAX) {
        f = OS_IRQTable[irq];
    }
    s_irq_saved = saved;
    s_cpsr = (saved & ~PSR_CTRL_MASK) | PSR_MODE_IRQ | PSR_I | PSR_F;
    if (irq == (int)HWIRQ_VBLANK && s_vblank_hook != NULL) {
        s_vblank_hook();
    }
    if (f) {
        f();
    } else {
        HWLOG("IRQ %d raised with no handler in OS_IRQTable", irq);
    }
    HWOS_IrqHandlerThreadSwitch();
    s_irq_saved = prev;
    s_cpsr = saved;
}

static void *be_create(void (*entry)(void *), void *arg, uint32_t stack, void *u)
{
    (void)u;
    return hwctr_thread_create(entry, arg, stack);
}

static void be_wake(void *h, void *u)
{
    (void)u;
    hwctr_thread_wake(h);
}

static void be_park(void *h, void *u)
{
    (void)u;
    hwctr_thread_park(h);
}

static void be_exit(void *h, void *u)
{
    (void)u;
    hwctr_thread_exit_current(h);
}

static void be_kill(void *h, void *u)
{
    (void)u;
    hwctr_thread_kill(h);
}

/* The GX command FIFO is drained instantly here (gx_burst), so it is always below half and empty: the GXFIFO IRQ (IE bit 21) is a level line that
   is asserted whenever the game enabled it and chose a condition (GXSTAT bits 30-31 != 0). MI_SendGXCommandAsync's multi-chunk DMA chain depends on it. */
static void env_level(void *u)
{
    (void)u;
    if (s_ready && (reg_OS_IE & (1u << 21)) && ((reg_G3X_GXSTAT >> 30) & 3u) != 0u) {
        reg_OS_IF |= (1u << 21);
    }
}

static void env_vblank(void *u)
{
    (void)u;
    hw_devs_ds_vblank();
}

static const HwPumpOps k_pump_ops = { env_real_tick, env_wait_vblank, env_sleep, env_arm7, env_dispatch, env_irq_masked, NULL, env_level, env_vblank };

void hw_os_init(void)
{
    static const HwCtxBackend be = { be_create, be_wake, be_park, be_exit, be_kill, NULL };

    if (s_ready) {
        return;
    }
    s_ready = 1;
    hwctx_init(&s_ctx, &be, hwctr_main_handle());
    hwpump_init(&s_pump, &k_pump_ops, (uint8_t *)HW_IOREG, (uint32_t *)HW_VBLANK_COUNT_BUF);
    hwpump_set_clock(&s_pump, s_clock_frame ? HWCLOCK_FRAME : HWCLOCK_WALL);
}

/* Soft reset: the CPU shadow, the tick and the pump (timers, vblank phase) back to their power-on values. The contexts are NOT touched here
   (hwctx_restart ends them); the I/O register file and the IRQ table are zeroed by the caller (hw_world.c) before this runs. */
void hw_os_reset(void)
{
    uint32_t i;
    HwHblank hb_saved;

    hw_os_init();
    s_forced_carry += s_pump.forced_vblanks;
    s_cpsr = HWOS_BOOT_CPSR;
    s_irq_saved = 0u;
    for (i = 0; i < HWOS_CP15_COUNT; i++) {
        s_cp15[i] = 0u;
    }
    s_cp15[0] = 0x00012078u;
    s_tick_on = 0;
    s_arm7_depth = 0u;
    OSi_TickCounter = 0;
    OSi_NeedResetTimer = FALSE;
    s_vblank_carry += s_pump.vblanks;   /* the RTC chip keeps counting through a soft reset */
    hb_saved = s_pump.hb;               /* bet 2.3a: the HBlank counters are run totals (hwcount), not per power-on */
    hwpump_init(&s_pump, &k_pump_ops, (uint8_t *)HW_IOREG, (uint32_t *)HW_VBLANK_COUNT_BUF);
    hwpump_set_clock(&s_pump, s_clock_frame ? HWCLOCK_FRAME : HWCLOCK_WALL);   /* a reset restarts virtual time at 0 in FRAME mode (vblanks == 0 again) */
    hb_saved.in_line = 0u;
    s_pump.hb = hb_saved;
}

uint32_t hw_os_hblank_frame(const HwPumpLineOps *lo)
{
    hw_os_init();
    return hwpump_hblank_frame(&s_pump, lo);
}

void hw_os_set_clock(unsigned frame)
{
    hw_os_init();
    s_clock_frame = frame != 0u;
    hwpump_set_clock(&s_pump, s_clock_frame ? HWCLOCK_FRAME : HWCLOCK_WALL);
}

unsigned hw_os_clock_is_frame(void)
{
    return s_clock_frame;
}

uint32_t hw_os_forced_total(void)
{
    return s_forced_carry + s_pump.forced_vblanks;
}

HwCtx *hw_os_ctx(void)
{
    hw_os_init();
    return &s_ctx;
}

void hw_os_set_arm7_pump(void (*fn)(void))
{
    s_arm7_pump = fn;
}

const HwPump *hw_os_pump(void)
{
    return &s_pump;
}

uint64_t hw_os_vblanks_total(void)
{
    return s_vblank_carry + s_pump.vblanks;
}

void hw_os_set_vblank_hook(void (*fn)(void))
{
    s_vblank_hook = fn;
}

void hw_os_time_steal(uint64_t sys_ticks)
{
    hwtime_steal(&s_pump.time, sys_ticks);
}

/* ---- the CPU state shadow -------------------------------------------------------------------------------------------- */
uint32_t HWOS_CpsrRead(void)
{
    HWCOUNT("OS: HWOS_CpsrRead");
    return s_cpsr;
}

/* The ARM7 sound driver runs on the same CPU as the ARM9 code, but a real ARM7 is another processor: it can never be interrupted by an ARM9 IRQ, and
   the ARM9 never loses its thread to it. The ARM7 code calls the ARM9's own OS_DisableInterrupts / OS_RestoreInterrupts (PxiFifoCallback, StartTimer,
   every OS_ReceiveMessage), and a restore that clears the I bit would otherwise take pending ARM9 IRQs INSIDE the slice, whose thread switch strands the
   slice (in_pump stays 1, every later sound wait deadlocks; review 1.5 B1). While the depth is > 0 the pump sees the CPU as masked; what became pending
   is taken by the dispatch_loop that follows the slice, on the thread that pumped. */
void HWOS_Arm7Enter(void)
{
    HWCOUNT("OS: HWOS_Arm7Enter");
    s_arm7_depth++;
}

void HWOS_Arm7Leave(void)
{
    HWCOUNT("OS: HWOS_Arm7Leave");
    assert(s_arm7_depth > 0u);
    s_arm7_depth--;
}

void HWOS_CpsrWriteC(uint32_t value)
{
    HWCOUNT("OS: HWOS_CpsrWriteC");
    uint32_t old = s_cpsr;

    assert((s_cpsr & 0x1fu) != 0u);   /* the shadow always holds a mode */
    s_cpsr = (s_cpsr & ~PSR_CTRL_MASK) | (value & PSR_CTRL_MASK);
    if ((old & PSR_I) && !(s_cpsr & PSR_I) && s_ready) {
        hwpump_run(&s_pump, HWP_DISPATCH_ONLY);   /* Y5: a pending IRQ is taken the instant interrupts come back on */
    }
}

void hw_os_dispatch_now(void)
{
    /* a DMA is instantaneous here: its completion IRQ is delivered at the start call when interrupts are on (otherwise Y5 delivers it on restore) */
    if (s_ready && !(s_cpsr & PSR_I)) {
        hwpump_run(&s_pump, HWP_DISPATCH_ONLY);
    }
}

uint32_t HWOS_Cp15Read(HWOSCp15Reg reg)
{
    HWCOUNT("OS: HWOS_Cp15Read");
    return (reg >= 0 && reg < HWOS_CP15_COUNT) ? s_cp15[reg] : 0u;
}

void HWOS_Cp15Write(HWOSCp15Reg reg, uint32_t value)
{
    HWCOUNT("OS: HWOS_Cp15Write");
    if (reg >= 0 && reg < HWOS_CP15_COUNT) {
        s_cp15[reg] = value;
    }
}

uint32_t HWOS_GetSystemStackPointer(void)
{
    HWCOUNT("OS: HWOS_GetSystemStackPointer");
    return (uint32_t)(uintptr_t)__builtin_frame_address(0);
}

uint32_t HWOS_GetCurrentStackPointer(void)
{
    HWCOUNT("OS: HWOS_GetCurrentStackPointer");
    return (uint32_t)(uintptr_t)__builtin_frame_address(0);
}

/* ---- contexts ------------------------------------------------------------------------------------------------------- */
BOOL OS_SaveContext(OSContext *context)
{
    assert(s_arm7_depth == 0u);   /* a thread switch from inside the ARM7 slice must be impossible, not merely counted */
    hw_os_init();
    CP_SaveContext(&context->cp_context);
    context->cpsr = s_cpsr;
    context->r[0] = 1;   /* the DS image has r0 = 1 (the "resumed" return value) */
    return (BOOL)hwctx_save(&s_ctx, context, &context->r[11], &context->r[12]);
}

void HWOS_LoadContext(void *osContext)
{
    HWCOUNT("OS: HWOS_LoadContext");
    OSContext *ctx = (OSContext *)osContext;
    OSThread *th = (OSThread *)osContext;                  /* OSContext is the first member of OSThread */
    OSThread *me = (OSThread *)hwctx_cur_owner(&s_ctx);
    uint32_t stack = th->stackBottom > th->stackTop ? th->stackBottom - th->stackTop : 0u;
    int retire = me != NULL && me != th && me->state == OS_THREAD_STATE_TERMINATED;

    assert(osContext != NULL);
    hw_os_init();
    CPi_RestoreContext(&ctx->cp_context);
    s_cpsr = (s_cpsr & ~PSR_CTRL_MASK) | (ctx->cpsr & PSR_CTRL_MASK);
    if (hwctx_load(&s_ctx, ctx, ctx->r[11], ctx->r[12], (uintptr_t)(ctx->pc_plus4 - 4u), (uintptr_t)ctx->r[0], (uintptr_t)ctx->lr,
                   stack < MIN_HOST_STACK ? MIN_HOST_STACK : stack, retire) < 0) {
        hwctr_fatal("out of host threads (HWCTX_MAX)");
    }
    /* returning here means a later load targeted THIS thread's saved context: the DS equivalent of OS_SaveContext returning 1 */
}

/* OS_IrqHandler: take at most one pending IRQ (the pump's own loop is hwpump_run). */
void HWOS_IrqHandler(void)
{
    HWCOUNT("OS: HWOS_IrqHandler");
    uint8_t *io = (uint8_t *)HW_IOREG;
    uint32_t ime, ie, ifl, n;

    hw_os_init();
    memcpy(&ime, io + HWR_IME, sizeof ime);
    memcpy(&ie, io + HWR_IE, sizeof ie);
    memcpy(&ifl, io + HWR_IF, sizeof ifl);
    if (!(ime & 1u) || !(ie & ifl)) {
        return;
    }
    for (n = 0; !((ie & ifl) & (1u << n)); n++) {
    }
    ifl &= ~(1u << n);
    memcpy(io + HWR_IF, &ifl, sizeof ifl);
    env_dispatch(NULL, (int)n);
}

/* OS_IrqHandler_ThreadSwitch: wake what waits on OSi_IrqThreadQueue; if a reschedule is pending run the best READY thread. */
void HWOS_IrqHandlerThreadSwitch(void)
{
    HWCOUNT("OS: HWOS_IrqHandlerThreadSwitch");
    assert(s_arm7_depth == 0u);
    OSThreadInfo *ti = &OSi_ThreadInfo;
    OSThread *t, *cur;
    void (*cb)(OSThread *, OSThread *);

    if (OSi_IrqThreadQueue.head) {
        for (t = OSi_IrqThreadQueue.head; t;) {
            OSThread *next = t->link.next;
            t->state = OS_THREAD_STATE_READY;
            t->queue = NULL;
            t->link.prev = NULL;
            t->link.next = NULL;
            t = next;
        }
        OSi_IrqThreadQueue.head = NULL;
        OSi_IrqThreadQueue.tail = NULL;
        ti->isNeedRescheduling = 1;
    }
    if (!ti->isNeedRescheduling) {
        return;
    }
    ti->isNeedRescheduling = 0;
    s_cpsr = (s_cpsr & ~PSR_CTRL_MASK) | PSR_MODE_IRQ | PSR_I | PSR_F;
    for (t = ti->list; t && t->state != OS_THREAD_STATE_READY; t = t->next) {
    }
    cur = ti->current;
    if (!t || t == cur) {
        return;
    }
    cb = (void (*)(OSThread *, OSThread *))ti->switchCallback;
    if (cb) {
        cb(cur, t);
    }
    ti->current = t;
    if (OS_SaveContext(&cur->context) == 0) {
        cur->context.cpsr = s_irq_saved;   /* the interrupted state, not the IRQ-mode one */
        OS_LoadContext(&t->context);
    }
}

/* ---- yield points -------------------------------------------------------------------------------------------------- */
void HWOS_Halt(void)
{
    HWCOUNT("OS: HWOS_Halt");
    hw_os_init();
    hwpump_run(&s_pump, HWP_WAIT);
    hwlog_flush_if_due(s_pump.vblanks);
}

void HWOS_SpinWait(uint32_t cycle)
{
    HWCOUNT("OS: HWOS_SpinWait");
    hw_os_init();
    hwpump_spin(&s_pump, cycle ? cycle : 1u);
}

void SVC_WaitByLoop(s32 count)
{
    if (count > 0) {
        HWOS_SpinWait((uint32_t)count * 4u);   /* the BIOS loop is 4 cycles per iteration */
    }
}

void HWOS_WaitVCount0(void)
{
    HWCOUNT("OS: HWOS_WaitVCount0");
    uint16_t zero = 0;

    hw_os_init();
    hwpump_run(&s_pump, HWP_WAIT);
    memcpy((uint8_t *)HW_IOREG + HWR_VCOUNT, &zero, sizeof zero);
}

/* OS_SetTimerControl (patched, nitrosdk-devices.patch): bring the timers up to date, write the register, and when the write carries the
   enable bit tell the timer unit it is a re-arm (so the reload is latched even if the pump never saw the register at 0). */
void HWOS_TimerControlWrite(unsigned id, uint16_t control)
{
    HWCOUNT("OS: HWOS_TimerControlWrite");
    uint8_t *io = (uint8_t *)HW_IOREG;

    assert(id < 4u);
    hw_os_init();
    hwpump_sync(&s_pump);
    memcpy(io + HWR_TM0CNT_L + 4u * id + 2u, &control, sizeof control);
    if (control & 0x80u) {
        hwtimer_ctl_write(&s_pump.tm, id);
    }
}

/* ---- the BIOS calls and SDK leftovers the link trial found orphaned (crt0.c / syscall / utest are not compiled) ----------------- */
#define CPUSET_FIXED  0x01000000u
#define CPUSET_32BIT  0x04000000u
#define CPUSET_COUNT  0x001fffffu

void SVC_CpuSet(const void *srcp, void *destp, u32 dmaCntData)
{
    uint32_t n = dmaCntData & CPUSET_COUNT, w = (dmaCntData & CPUSET_32BIT) ? 4u : 2u, i;
    const uint8_t *s = (const uint8_t *)srcp;
    uint8_t *d = (uint8_t *)destp;

    assert(srcp != NULL && destp != NULL);
    for (i = 0; i < n; i++) {
        memmove(d + (size_t)i * w, (dmaCntData & CPUSET_FIXED) ? s : s + (size_t)i * w, w);
    }
}

void SVC_CpuSetFast(const void *srcp, void *destp, u32 dmaCntData)
{
    assert(srcp != NULL && destp != NULL);
    SVC_CpuSet(srcp, destp, (dmaCntData & (CPUSET_COUNT | CPUSET_FIXED)) | CPUSET_32BIT);
}

/* the BIOS CRC-16: reflected polynomial 0xa001 over `size` bytes (the BIOS works in halfwords, so an odd last byte is ignored) */
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size)
{
    const uint8_t *p = (const uint8_t *)datap;
    uint32_t crc = start & 0xffffu, i, b;

    assert(datap != NULL || size == 0u);
    for (i = 0; i < (size & ~1u); i++) {
        crc ^= p[i];
        for (b = 0; b < 8u; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xa001u : crc >> 1;
        }
    }
    return (u16)crc;
}

u16 SVC_Sqrt(u32 src)
{
    return (u16)hwcp_isqrt(src);
}

/* unpack bit-packed source units into wider destination units (GBATEK BIOS UnpackBits) */
void SVC_UnpackBits(const void *srcp, void *destp, const MIUnpackBitsParam *paramp)
{
    const uint8_t *s = (const uint8_t *)srcp;
    uint8_t *d = (uint8_t *)destp;
    uint32_t in_bits, out_bits, offset, acc = 0u, nacc = 0u, i, pos, mask, v;

    assert(srcp != NULL && destp != NULL && paramp != NULL);
    in_bits = paramp->srcBitNum;
    out_bits = paramp->destBitNum;
    offset = paramp->destOffset;
    if (in_bits == 0u || in_bits > 8u || out_bits == 0u || out_bits > 32u) {
        return;
    }
    mask = (1u << in_bits) - 1u;
    for (i = 0; i < paramp->srcNum; i++) {
        for (pos = 0; pos < 8u; pos += in_bits) {
            v = ((uint32_t)s[i] >> pos) & mask;
            if (v != 0u || paramp->destOffset0_on) {
                v += offset;
            }
            if (out_bits < 32u) {
                v &= (1u << out_bits) - 1u;
            }
            acc |= v << nacc;
            nacc += out_bits;
            if (nacc >= 32u) {
                memcpy(d, &acc, sizeof acc);
                d += sizeof acc;
                acc = 0u;
                nacc = 0u;
            }
        }
    }
}

/* the overlay loader's in-place backward LZ (hw_blz.c is the decoder, host-tested) */
void MIi_UncompressBackward(void *bottom)
{
    (void)hwblz_uncompress((uint8_t *)bottom);
}

/* linker-section anchor (crt0.c): nothing to do */
void OSi_ReferSymbol(void *symbol)
{
    (void)symbol;
}

/* utest_assert.c's equality check, used by os_thread.c's debug asserts: log the mismatch once per site */
void UTi_AssertEq(const char *file, int line, const char *exp_str1, int exp_val1, const char *exp_str2, int exp_val2)
{
    if (exp_val1 != exp_val2) {
        HWLOG("UT_AssertEq %s:%d: %s[=%d] != %s[=%d]", file, line, exp_str1, exp_val1, exp_str2, exp_val2);
    }
}

/* ---- tick (replaces os_tick.c) ------------------------------------------------------------------------------------------- */
void OS_InitTick(void)
{
    hw_os_init();
    if (!s_tick_on) {
        s_tick_on = 1;
        OSi_SetTimerReserved(OS_TIMER_0);
        hwtime_set_os_tick(&s_pump.time, 0);
        OSi_TickCounter = 0;
        OSi_NeedResetTimer = FALSE;
    }
}

BOOL OS_IsTickAvailable(void)
{
    return s_tick_on ? TRUE : FALSE;
}

OSTick OS_GetTick(void)
{
    OSTick t;

    hw_os_init();
    (void)hwpump_poll(&s_pump);   /* WALL: hwtime_update with the host tick; FRAME: a charged poll */
    t = hwtime_os_tick(&s_pump.time);
    OSi_TickCounter = t >> 16;
    return t;
}

u32 OSi_GetTick_noProfile(void)
{
    return (u32)OS_GetTick();
}

u16 OS_GetTickLo(void)
{
    return (u16)OS_GetTick();
}

void OS_SetTick(OSTick count)
{
    hw_os_init();
    hwtime_set_os_tick(&s_pump.time, count);
}

/* MSL's assert failure (the DWC GameSpy base's assertWithLine, gsAssert.h): the condition is logged, then the game stops */
void __msl_assertion_failed(const char *condition, const char *file, const char *func, int line)
{
    HWLOG("assertion failed: %s (%s:%d in %s)", condition, file, line, func);
    hwlog_dump();
    hwctr_fatal("assertion failed");
}

/* ---- exceptions, reset and boot: nothing on the 3DS can reach them; reaching one is logged and fatal ----------------------------- */
void HWOS_ExceptionHandler(void (*getAndDisplay)(void))
{
    HWCOUNT("OS: HWOS_ExceptionHandler");
    (void)getAndDisplay;
    HWLOG("HWOS_ExceptionHandler reached");
    hwlog_dump();
    hwctr_fatal("DS exception handler reached");
}

void HWOS_DebuggerExceptionHook(void (*getAndDisplay)(void))
{
    HWCOUNT("OS: HWOS_DebuggerExceptionHook");
    (void)getAndDisplay;
    HWLOG("HWOS_DebuggerExceptionHook reached");
}

void HWOS_FillExContext(void *osiExContext)
{
    HWCOUNT("OS: HWOS_FillExContext");
    (void)osiExContext;
    HWLOG("HWOS_FillExContext reached");
}

void HWOS_UserExceptionEnter(void)
{
    HWCOUNT("OS: HWOS_UserExceptionEnter");
    HWLOG("HWOS_UserExceptionEnter reached");
}

void HWOS_UserExceptionLeave(void)
{
    HWCOUNT("OS: HWOS_UserExceptionLeave");
    HWLOG("HWOS_UserExceptionLeave reached");
}

void HWOS_DoBoot(void)
{
    HWCOUNT("OS: HWOS_DoBoot");
    HWLOG("HWOS_DoBoot reached");
    hwlog_dump();
    hwctr_fatal("OSi_DoBoot reached");
}
