/* hw_boot.h -- the soft reset (OS_ResetSystem) and the first entry: restore the game's data, re-seed the hardware layer, re-enter NitroMain on a
 * fresh host thread. Pure C: the SDK-world glue is hw_world.c. Design: the private design notes section "soft reset". */
#ifndef HW_BOOT_H
#define HW_BOOT_H

#include <stdint.h>

#include "hw_ovl.h"
#include "hw_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HwBootOps {
    /* Re-seed everything the hardware layer owns (I/O registers, DTCM/ITCM, PXI/DMA/card/OS state, the system page). Called with the game's
       .data/.bss already restored. The reset parameter word is saved and put back around this call by hwboot_reset. */
    void (*reseed)(void *u);
    void *user;
} HwBootOps;

typedef struct HwBoot {
    HwOvl *ovl;
    HwCtx *ctx;
    HwBootOps ops;
    void (*entry)(void);               /* NitroMain */
    volatile uint32_t *param;          /* HW_RESET_PARAMETER_BUF */
    uint32_t stack_bytes;
    uint32_t boots;                    /* entries into NitroMain: 1 at power-on, +1 per soft reset */
} HwBoot;

void hwboot_init(HwBoot *b, HwOvl *ovl, HwCtx *ctx, const HwBootOps *ops, void (*entry)(void), volatile uint32_t *param, uint32_t stack_bytes);
/* Power-on: start entry on a fresh host thread; the calling thread (the process's main thread) parks for good. Returns only on failure (-1). */
int hwboot_start(HwBoot *b);
/* Soft reset: preserve the parameter, restore the main binary's .data / zero its .bss, un-load every overlay, re-seed the hardware layer,
   put the parameter back, end every other context and re-enter entry on a fresh thread; the caller never resumes (no unwinding).
   Returns only on failure (-1, nothing re-entered). */
int hwboot_reset(HwBoot *b, uint32_t parameter);

#ifdef __cplusplus
}
#endif

#endif
