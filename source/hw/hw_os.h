/* hw_os.h -- public face of source/hw/hw_os.c (the HWOS_* hooks, tick, timers, IRQ dispatch, contexts). SDK-world header. */
#ifndef HW_OS_H
#define HW_OS_H

#include "hw_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent; call before OS_Init (it is also called lazily by every hook). */
void hw_os_init(void);
void hw_os_dispatch_now(void);   /* deliver pending IRQs now if CPSR.I is clear (synchronous DMA completion) */
/* Soft reset of the CPU shadow, tick and pump (hw_world.c, hw_boot.c). */
void hw_os_reset(void);
/* The cooperative-context table (hw_world.c restarts it on a soft reset). */
HwCtx *hw_os_ctx(void);
/* The ARM7 driver slice, run at every pump (bet 1.5). NULL removes it. */
void hw_os_set_arm7_pump(void (*fn)(void));
/* Read-only view of the pump (counters: vblanks, overruns, dispatched). */
const HwPump *hw_os_pump(void);
/* DS vblanks raised since power-on, across soft resets (the pump's own counter restarts at a reset). */
uint64_t hw_os_vblanks_total(void);
/* Bet 2.3a: the frame's 192 HBlank lines (hwpump_hblank_frame); the game's snapshot point calls it through hw_devs_snapshot_point. */
uint32_t hw_os_hblank_frame(const HwPumpLineOps *lo);
/* Bet 2.3c: called on the game thread right before the vblank interrupt handler (the game's VBlank callback, its VRAM writer) runs. NULL removes it. */
void hw_os_set_vblank_hook(void (*fn)(void));
/* Bet 2.1c2: exclude `sys_ticks` real sys ticks (268 111 856 Hz) from virtual time: the feature census's own work, which the game must not feel. */
void hw_os_time_steal(uint64_t sys_ticks);   /* a no-op in the FRAME clock (the host clock does not feed virtual time) */
/* Bet 2.2a: `clock = frame|wall`. Call before the first yield (hw_sys_init); survives soft resets. */
void hw_os_set_clock(unsigned frame);
unsigned hw_os_clock_is_frame(void);
uint32_t hw_os_forced_total(void);   /* vblanks forced by the FRAME clock's spin budget, across soft resets */

#ifdef __cplusplus
}
#endif

#endif
