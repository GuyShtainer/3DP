/* hw_wmq.h -- the radio-less ARM7 wireless manager's reply queue (review 1.6, B2). Pure C (no SDK): the fifo7to9 buffer and the callback-control
 * word are handed in, so the unit is host-tested (tools/host-tests/hw_os_test.c).
 *
 * A real ARM7 request handler gets its reply buffer with WMSP_GetBuffer4Callback2Wm9() (it waits until the ARM9 has cleared bit 0 of
 * OSSystemWork.wm_callback_control, then sets it) and returns the callback with WMSP_ReturnResult2Wm9(). The ARM9's WmReceiveFifo clears the bit
 * when it has processed the callback. Scans take real time (maxChannelTime per channel), so their reply is queued and delivered by a later frame. */
#ifndef HW_WMQ_H
#define HW_WMQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWWMQ_REPLY_BYTES 256u   /* WM_FIFO_BUF_SIZE: the whole fifo7to9 buffer, which the ARM9 invalidates (the ARM7 clears 160 of it per reply) */
#define HWWMQ_DEPTH       8u
#define HWWMQ_BUSY_BIT    1u     /* WM_EXCEPTION_CB_MASK */

typedef struct HwWmQ {
    struct {
        uint8_t data[HWWMQ_REPLY_BYTES];
        uint32_t due;            /* the frame count at which the reply may be delivered */
    } e[HWWMQ_DEPTH];
    unsigned head, n;
    uint32_t frame;              /* frames ticked so far */
    uint32_t posted, delivered, deferred, dropped, max_wait;   /* statistics */
} HwWmQ;

void hwwmq_init(HwWmQ *q);
/* A reply to send `delay` frames from now. Immediate (delay 0, queue empty, ARM9 buffer free): copied to `dst`, the busy bit set, 1 returned (the
   caller sends the PXI word now). Otherwise queued (0 returned); -1 when the queue is full (the reply is dropped, counted). */
int hwwmq_post(HwWmQ *q, volatile uint16_t *control, void *dst, const void *reply, uint32_t delay);
/* One frame: advances the clock and delivers the oldest due reply if the ARM9 has released the buffer. Returns 1 when `dst` was written. */
int hwwmq_tick(HwWmQ *q, volatile uint16_t *control, void *dst);
unsigned hwwmq_pending(const HwWmQ *q);
/* A scan's time in frames: `channels` x `max_channel_time` ms (0 = the SDK default 30 ms), rounded up to whole 60 Hz frames, at least 1. */
uint32_t hwwmq_scan_frames(unsigned channels, unsigned max_channel_time);

#ifdef __cplusplus
}
#endif

#endif
