/* hw_setup.h -- the first-launch setup screen (release P4b): pick the player's own ROM, import a save or start a new one, write config.ini. It runs on the main thread BEFORE hw_sys_init,
 * through the text console only (hwctr_console_write) and the hw_ctr.h seam, so it needs no libctru and is host-tested over a fake seam. The pure rules (SHA-1, the ROM header and file system
 * check, save kinds, the config edit, backup names) are hw_setup_core.h's. */
#ifndef HW_SETUP_H
#define HW_SETUP_H

#include <stddef.h>

#include "hw_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HWSETUP_NOT_NEEDED 0   /* a usable ROM, SELECT not held (or setup = 0, or a bench launch): nothing was shown or logged */
#define HWSETUP_DONE       1   /* the screens ran and ended normally: go on and boot */
#define HWSETUP_QUIT       2   /* HOME, or a scripted run whose script ended: the caller dumps the log and exits */

/* The ROM the card bus reads: `rom =` from the config when that file exists, else the fixed name HWCTR_ROM_FILE. Returns 1 when the file at *out exists, else 0 (out then holds the fixed name). */
int hw_setup_resolve_rom(const HwCfg *cfg, char *out, size_t cap);

/* `rom = <rompath>` into config.ini (the verified write: .tmp, re-read, swap, re-read through the real parser). 0, or -1 with hw_setup_last_why() saying why. Public for the host test. */
int hw_setup_write_rom(const char *rompath);
const char *hw_setup_last_why(void);

/* Decide and, when needed, run the setup screen. Reads config.ini and input.txt itself. */
int hw_setup_run(void);

#ifdef __cplusplus
}
#endif

#endif
