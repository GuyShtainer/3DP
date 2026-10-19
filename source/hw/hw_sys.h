/* hw_sys.h -- the DS memory arrays and the boot-time fill of the system page (memory map notes are kept privately). SDK-world header. */
#ifndef HW_SYS_H
#define HW_SYS_H

#include "hw_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. Checks the array alignments, reads sdmc:/3ds/3DP/config.ini (writes the defaults on first run), and fills the
   system page: owner info, MAC, RTC offset, "no cartridge", ROM-header copies. Call before OS_Init. Returns the number of config keys
   applied from the file (0 on first run). */
unsigned hw_sys_init(void);
struct HwBenchRun;
const struct HwBenchRun *hw_sys_bench(void);   /* bet 2.7a: this launch's bench entry (active 0 = a normal launch) */
/* Soft reset: zero every DS memory array, the register file and DTCM, then fill the system page again. Keeps the ROM and the flash. */
void hw_sys_reseed(void);
/* The ROM file the card bus reads (release P4b): config.ini `rom =` when that file exists, else sdmc:/3ds/3DP/platinum.nds. Valid after hw_sys_init. */
const char *hw_sys_rom_path(void);
/* The configuration in force (after hw_sys_init). */
const HwCfg *hw_sys_cfg(void);
/* Bet 2.2c (B1): 1 when [p, p + n) lies inside the 4 MB main RAM array (a game pointer the trace-only key line may dereference safely), else 0. */
int hw_main_ram_contains(const void *p, unsigned n);
/* System ticks the gxtrace window's SD writes took (all taken out of the game's virtual clock, bet 2.1e). */
uint64_t hw_sys_trace_io_ticks(void);

#ifdef __cplusplus
}
#endif

#endif
