/* hw_savewb_stub.c -- host-test stand-in for source/hw/hw_savewb.c (bet 2.6b). pxi_sdk_test links the real hw_devs.c, whose card_boot / input frame / exit call the write-back glue;
 * the glue's worker needs libctru threads and the SD card, so the host links these no-ops (the decision logic itself is tested by hw_save_test.c, the worker by gate B in Azahar). */
#include "hw_savewb.h"

void hwwb_init(const uint8_t *flash, uint32_t bytes, const char *path, uint32_t counter) { (void)flash; (void)bytes; (void)path; (void)counter; }
const char *hwwb_boot_path(const char *path, uint32_t bytes) { (void)bytes; return path; }
void hwwb_lock(void) {}
void hwwb_unlock(uint32_t counter, uint32_t vb) { (void)counter; (void)vb; }
void hwwb_frame(uint32_t counter, uint32_t vb) { (void)counter; (void)vb; }
void hwwb_stop(uint32_t counter, uint32_t vb) { (void)counter; (void)vb; }
void hwwb_stats(HwWbStats *out) { *out = (HwWbStats){0}; out->worker_core = -1; out->worker_ran_on = -1; }

/* hw_devs.c's input frame now reads the vblank count for the write-back's debounce; hw_os.c is not part of this link, so the host test has no vblanks (the debounce is in hw_save_test.c). */
uint64_t hw_os_vblanks_total(void) { return 0; }
