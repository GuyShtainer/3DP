/* hw_snd7.h -- public face of source/hw/hw_snd7.c (the ARM7 sound driver glue). SDK-world header. */
#ifndef HW_SND7_H
#define HW_SND7_H

#include "hw_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by hw_devs.c each time the loopback ARM7 is (re)built: restarts the driver's mailbox state and registers the slice with the pump.
   Returns the instance the SOUND responder feeds. */
HwSnd *hw_snd7_attach(HwPxi *pxi, uint8_t *io);
/* Counters (read-only). */
const HwSnd *hw_snd7_state(void);
/* Rule 7 (bet 2.1e): wall ticks (svcGetSystemTick, 268 111 856 Hz) spent in ARM7 sound slices since boot, and how many slices (any pointer may be NULL). */
void hw_snd7_timing(uint64_t *ticks, uint32_t *calls);
/* The glue's own counters (any pointer may be NULL): PXI callback registrations seen, words delivered to it, words dropped for want of one,
   outward PXI references to a tag other than SOUND, sleep-path calls (SVC sound bias / PMIC control). */
void hw_snd7_extra(uint32_t *cb_set, uint32_t *cb_calls, uint32_t *no_cb, uint32_t *foreign_tag, uint32_t *sleep_calls);

#ifdef __cplusplus
}
#endif

#endif
