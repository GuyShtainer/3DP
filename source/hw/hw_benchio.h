/* hw_benchio.h -- bet 2.7a: the file side of the hardware-session bench (hw_bench.h): which entry this launch runs, the save check, the overrides, and the copy of the run's logs
 * into bench/<tag>/ when it ends. Plain C over hw_ctr.h file calls only (host-tested over a temp directory). */
#ifndef HW_BENCHIO_H
#define HW_BENCHIO_H

#include <stdint.h>

#include "hw_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HWBENCH_DIR       HWCTR_APP_DIR "/bench"
#define HWBENCH_LIST_FILE HWCTR_APP_DIR "/bench.txt"
#define HWBENCH_CNT_FILE  HWCTR_APP_DIR "/bench.cnt"
#define HWBENCH_SHA_FILE  HWCTR_APP_DIR "/platinum.sav.sha256"

typedef struct HwBenchRun {
    int active;               /* this launch runs a bench entry */
    int refused;              /* bench.txt exists but the launch must not go on (the save is not the kit's, or bench.txt is malformed): note says why */
    int index, total;         /* entry index (0-based) and the number of entries */
    int salvaged;             /* the previous entry had no DONE marker: its leftover logs were copied to bench/<tag>/ with a CRASHED marker */
    char tag[16];
    char note[200];           /* one line for the hwlog / console */
    uint8_t sav_start[32];    /* SHA-256 of platinum.sav at the start */
} HwBenchRun;

/* Called once, after config.ini was parsed into *cfg and BEFORE anything reads the save: decides the entry, advances bench.cnt (at START: a crash does not loop), checks the save
   against platinum.sav.sha256, applies the entry's overrides on top of *cfg. No bench.txt = a normal launch (run->active 0, refused 0). */
void hwbench_start(HwBenchRun *run, HwCfg *cfg);
/* Called when the run ends: copies hwlog.txt, hwcount.txt, boot.out, console.txt and keys.txt into bench/<tag>/ and writes bench/<tag>/DONE (tag, index, why, whether platinum.sav
   is byte-identical to the start). No-op when run->active is 0. */
void hwbench_finish(const HwBenchRun *run, const char *why);

#ifdef __cplusplus
}
#endif

#endif
