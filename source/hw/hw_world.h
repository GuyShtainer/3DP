/* hw_world.h -- boot glue of the overlays and the soft reset (hw_world.c). SDK-world header. */
#ifndef HW_WORLD_H
#define HW_WORLD_H

#include "hw_ovl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call first thing in main(), after hw_sys_init and before any game or SDK code has touched its data: takes the pristine copies of the main
   binary's and every overlay's .data. `entry` is NitroMain. Returns 0, or -1 (fatal-logged) if the linker symbols do not fit the buffer. */
int hw_world_init(void (*entry)(void));
/* Start entry on a fresh host thread; the calling (main) thread parks for good. Returns only on failure. */
int hw_world_run(void);
const HwOvl *hw_world_ovl(void);
/* Stand-in modules that model game-RAM state (heap pointers the game handed them): the soft reset reseeds them (world_reseed). */
void hw_dwc_reset(void);
void hw_ppwlobby_reset(void);

#ifdef __cplusplus
}
#endif

#endif
