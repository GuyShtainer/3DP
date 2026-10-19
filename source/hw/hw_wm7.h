/* hw_wm7.h -- public face of source/hw/hw_wm7.c (the radio-less ARM7 wireless manager). SDK-world header. */
#ifndef HW_WM7_H
#define HW_WM7_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One WM request: `cmd_addr` is the address of the command buffer the ARM9 sent on the WM tag. Returns the address of the callback record to send back
   (0 = nothing to answer). */
uint32_t hw_wm7_request(uint32_t cmd_addr);
/* One frame: the queued reply that is due (a scan's, after its channel time), as the address to send on the WM tag; 0 = none. */
uint32_t hw_wm7_tick(void);
void hw_wm7_reset(void);                   /* the ARM7 restarts (soft reset): forget the buffers */
uint32_t hw_wm7_count(unsigned apiid);     /* requests seen per api id */
uint32_t hw_wm7_requests(void);
uint32_t hw_wm7_unmodelled(void);
void hw_wm7_report(unsigned words_sent, unsigned requests, unsigned answered);   /* one HWLOG line: per-api counts */

#ifdef __cplusplus
}
#endif

#endif
