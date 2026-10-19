/* hw_os_hooks.h -- the HWOS_* hooks behind the NitroSDK ARM9 routines that touched the DS CPU directly.
 *
 * Slice 1.2b of the 3DS port (19 hooks since slice 1.3 added HWOS_SpinWait) replaces every mwcc `asm` function body in the SDK census set. Pure
 * computation was translated to C in place. Anything that read or wrote CPSR, cp15, banked registers,
 * the IRQ vector or a saved context was NOT translated: the SDK function became a thin C stub that
 * calls one of the hooks below, so slice 1.3 (the event/thread model on the ARM11) implements each hook
 * once, in one place. Until then the hooks are declared only (a link of the real game would stop at the
 * first unresolved HWOS_ symbol, which is the intended tripwire).
 *
 * Each comment says what the DS code behind the hook did. Types are plain C so host tests can include
 * this header without the SDK.
 */
#ifndef HW_OS_HOOKS_H
#define HW_OS_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ARM9 CPU status register. The DS code read it with `mrs cpsr` and wrote only the control byte (mode,
   T, F, I bits) with `msr cpsr_c`. HWOS_CpsrWriteC must apply only the low 8 bits of `value`. The
   OS_{Enable,Disable,Restore}Interrupts[_IrqAndFiq], OS_GetCpsrIrq and OS_GetProcMode functions are plain
   C on top of these two hooks. */
uint32_t HWOS_CpsrRead(void);
void HWOS_CpsrWriteC(uint32_t value);

/* The cp15 registers the SDK touches, as a shadow register file. The DS code used mrc/mcr; every TCM,
   protection-unit, protection-region and exception-vector-base function in os/src is read-modify-write C
   over HWOS_Cp15Read/Write. HWOS_CP15_REGION0 + n is protection region n (n = 0..7). */
typedef enum HWOSCp15Reg {
    HWOS_CP15_CONTROL = 0,   /* c1,c0,0 : protection unit / caches / TCM enables, exception vector base */
    HWOS_CP15_DCACHE_CFG,    /* c2,c0,0 : per-region data cache enable bits */
    HWOS_CP15_ICACHE_CFG,    /* c2,c0,1 : per-region instruction cache enable bits */
    HWOS_CP15_WRITEBUF,      /* c3,c0,0 : per-region write buffer enable bits */
    HWOS_CP15_DPERM,         /* c5,c0,2 : data access permissions */
    HWOS_CP15_IPERM,         /* c5,c0,3 : instruction access permissions */
    HWOS_CP15_DTCM,          /* c9,c1,0 : DTCM base and size */
    HWOS_CP15_ITCM,          /* c9,c1,1 : ITCM size */
    HWOS_CP15_REGION0,       /* c6,c0,0 : protection region 0; REGION0 + n for region n */
    HWOS_CP15_REGION1,
    HWOS_CP15_REGION2,
    HWOS_CP15_REGION3,
    HWOS_CP15_REGION4,
    HWOS_CP15_REGION5,
    HWOS_CP15_REGION6,
    HWOS_CP15_REGION7,
    HWOS_CP15_COUNT
} HWOSCp15Reg;

uint32_t HWOS_Cp15Read(HWOSCp15Reg reg);
void HWOS_Cp15Write(HWOSCp15Reg reg, uint32_t value);

/* OS_LoadContext. DS: restore the divider/sqrt unit state from the context, switch to SVC mode with IRQ
   and FIQ off, load SPSR from the saved CPSR, load SP_svc, load r0-r14 of the user bank and resume at
   pc_plus4 - 4 (`subs pc, lr, #4`). Never returns. The saved layout is OSContext. */
void HWOS_LoadContext(void *osContext);
/* OS_SaveContext has no hook: it is setjmp-like (returns 0 now, 1 when the saved context is loaded), so
   it cannot be a returning wrapper. os_context.c leaves it extern and the hw_os layer defines
   `int OS_SaveContext(OSContext *)` (BOOL). DS: CP_SaveContext, then store CPSR, r0-r14 (r0 = 1 in the
   image), SP_svc and pc_plus4 = the instruction after the call. */

/* OS_IrqHandler. DS: the ARM9 IRQ vector. Return if IME is 0; pick the lowest set bit of IE & IF,
   acknowledge it in IF, then jump to OS_IRQTable[n] with lr = OS_IrqHandler_ThreadSwitch. */
void HWOS_IrqHandler(void);
/* OS_IrqHandler_ThreadSwitch. DS: after the IRQ handler body, wake the threads queued on
   OSi_IrqThreadQueue, and if a reschedule is pending save the interrupted thread's context (including
   the CP context) and load the highest-priority READY thread's, returning into it. */
void HWOS_IrqHandlerThreadSwitch(void);

/* The ARM7 sound slice (bet 1.5, source/hw/hw_snd.c). Not a DS hook: the ARM7 was another CPU. Between Enter and Leave (nestable) the interrupt
   pump treats the ARM9 as masked, so no ARM9 IRQ handler or thread switch can run inside the slice even though the ARM7 code calls the ARM9's
   OS_DisableInterrupts / OS_RestoreInterrupts. Pending IRQs are taken after the slice returns. */
void HWOS_Arm7Enter(void);
void HWOS_Arm7Leave(void);

/* OS_Halt (os_terminate_proc.c). DS: `mcr p15,0,r0,c7,c0,4`, wait for interrupt: stop the core until an IRQ arrives. */
void HWOS_Halt(void);

/* OS_SpinWait (slice 1.3; review 1.2 §2). DS: a busy loop of floor(cycle / 4) + 1 iterations at 67.03 MHz, used both as a short
   delay and as the body of every "wait for the ARM7 to answer" spin. On the single-thread event model the ARM7 only advances at a
   yield point, so the loop cannot be a loop: the hook is a yield point that advances the virtual clock by `cycle` ARM9 cycles
   and runs whatever is due. SVC_WaitByLoop, the BIOS twin, is defined directly in source/hw/hw_os.c. */
void HWOS_SpinWait(uint32_t cycle);

/* OS_SetTimerControl (patched, port-patches/nitrosdk-devices.patch). DS: write the timer's control register. The port also brings the
   timers up to date first and, when the write carries the enable bit, marks the timer as re-armed, so a stop-less re-arm between two
   pumps still reloads the counter. `id` is 0-3. */
void HWOS_TimerControlWrite(unsigned id, uint16_t control);

/* OSi_WaitVCount0. DS: with IME forced on, spin until REG_VCOUNT reads 0, then restore IME. */
void HWOS_WaitVCount0(void);

/* DS: the user/system-bank SP (`stmia r0,{sp}^`) and the current SP. */
uint32_t HWOS_GetSystemStackPointer(void);
uint32_t HWOS_GetCurrentStackPointer(void);

/* Exceptions (os_exception.c). `getAndDisplay` is OSi_GetAndDisplayContext. */
/* DS: the vector entry. Call the debugger handler if one is installed, switch to a stack at the end of
   ITCM, and for data-abort/undefined modes call getAndDisplay with r12 -> the saved register frame. */
void HWOS_ExceptionHandler(void (*getAndDisplay)(void));
/* DS: hook placed in the debugger's vector slot; filters out the debugger's own breakpoint encodings
   (undefined instruction 0xE7FFFFFF/0xE7FDDEFE/0xE6000010, thumb 0xEFFF/0xDEFE/0xBE00), otherwise runs
   getAndDisplay, then chains to the debugger handler. */
void HWOS_DebuggerExceptionHook(void (*getAndDisplay)(void));
/* DS: fill the OSiExContext (OSContext, cp15 control, spsr, exinfo, debug[4]) from the exception frame:
   r0-r3 and the frame pointer saved by the handler, r4-r11 live, the faulting CPSR/PC from the frame,
   and the banked SP/LR/SPSR of the faulting mode. */
void HWOS_FillExContext(void *osiExContext);
/* DS: before the user exception handler runs, switch to system mode on a copy of the current stack and
   enable the protection unit; Leave undoes it. */
void HWOS_UserExceptionEnter(void);
void HWOS_UserExceptionLeave(void);

/* Reset (os_reset.c). DS: set SP to the top of the DTCM IRQ stack minus SDK_IRQ_STACKSIZE and branch to
   doReset (OSi_DoResetSystem), which never returns. */
void HWOS_ResetSystem(void (*doReset)(void));
/* DS: OSi_DoBoot, run from ITCM/WRAM at reset: IME off, clear the IRQ vector, handshake with the other
   CPU over the SUBP interrupt FIFO, wipe the PXI/WM/component parameter areas, then jump to the
   entry point stored in the ROM header copy. Never returns. */
void HWOS_DoBoot(void);

#ifdef __cplusplus
}
#endif

#endif
