/* hw_pxi.c -- the ARM9<->ARM7 IPC FIFO as a loopback. The "ARM7" is a table of per-tag responders (hw_pxi_arm7.c); a word the ARM9
 * sends reaches its responder at once and the replies are queued for the ARM9's receive FIFO, whose not-empty IRQ (IF bit 18) is raised
 * if the SDK enabled it. IPCFIFOCNT is plain memory, so hwpxi_cnt_sync recomputes its status bits whenever the SDK is about to read it.
 * Pure C, host-tested. Design: the private design notes ("PXI"). */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

#define PXI_RECV_RI 0x0400u   /* IPCFIFOCNT bit 10: IRQ when the receive FIFO becomes not empty */

static uint16_t rd16(const uint8_t *io, uint32_t off)
{
    uint16_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static void wr16(uint8_t *io, uint32_t off, uint16_t v)
{
    memcpy(io + off, &v, sizeof v);
}

static uint32_t rd32(const uint8_t *io, uint32_t off)
{
    uint32_t v;
    memcpy(&v, io + off, sizeof v);
    return v;
}

static void wr32(uint8_t *io, uint32_t off, uint32_t v)
{
    memcpy(io + off, &v, sizeof v);
}

void hwpxi_init(HwPxi *p, uint8_t *ioreg)
{
    assert(p != NULL && ioreg != NULL);
    memset(p, 0, sizeof *p);
    p->io = ioreg;
    hwpxi_cnt_sync(p);
}

void hwpxi_set_responder(HwPxi *p, unsigned tag, HwPxiResponder fn, void *user)
{
    assert(p != NULL && fn != NULL);
    assert(tag > 0u && tag < HWPXI_TAGS);   /* tag 0 is the "extension" tag; the SDK ignores words with it */
    if (tag > 0u && tag < HWPXI_TAGS) {
        p->resp[tag] = fn;
        p->resp_user[tag] = user;
        p->handle_bits |= 1u << tag;
    }
}

/* ERR is write-1-to-clear. A plain-memory register cannot tell "the SDK wrote 1 to acknowledge" from "still latched", so: a latched
   error is shown once (err_state 1 -> 2), and whatever the SDK did next (every path that sees ERR writes it back) acknowledges it. */
void hwpxi_cnt_sync(HwPxi *p)
{
    uint16_t cnt;

    assert(p != NULL && p->io != NULL);
    cnt = rd16(p->io, HWR_PXI_CNT);
    cnt = (uint16_t)(cnt & ~(HWPXI_CNT_ERR | HWPXI_CNT_SEND_CLEAR | HWPXI_CNT_SEND_EMPTY | HWPXI_CNT_SEND_FULL | HWPXI_CNT_RECV_EMPTY |
                             HWPXI_CNT_RECV_FULL));
    cnt = (uint16_t)(cnt | HWPXI_CNT_SEND_EMPTY);   /* the loopback ARM7 consumes a sent word at once: the send FIFO is always empty */
    if (p->count == 0u) {
        cnt = (uint16_t)(cnt | HWPXI_CNT_RECV_EMPTY);
    }
    if (p->count >= HWPXI_FIFO_DEPTH) {
        cnt = (uint16_t)(cnt | HWPXI_CNT_RECV_FULL);
    }
    if (p->err_state == 1u) {
        cnt = (uint16_t)(cnt | HWPXI_CNT_ERR);
        p->err_state = 2u;
    } else {
        p->err_state = 0u;
    }
    wr16(p->io, HWR_PXI_CNT, cnt);
}

int hwpxi_reply(HwPxi *p, unsigned tag, uint32_t data, unsigned err)
{
    uint32_t word;
    uint16_t cnt;

    assert(p != NULL && tag < HWPXI_TAGS);
    assert(data < (1u << 26));
    if (p->count >= HWPXI_FIFO_DEPTH) {
        p->recv_overflow++;
        p->err_state = 1u;
        hwpxi_cnt_sync(p);
        return -1;
    }
    word = (tag & 31u) | ((err & 1u) << 5) | (data << 6);
    p->recv[(p->head + p->count) % HWPXI_FIFO_DEPTH] = word;
    p->count++;
    p->replies++;
    hwpxi_cnt_sync(p);
    cnt = rd16(p->io, HWR_PXI_CNT);
    if (p->count == 1u && (cnt & PXI_RECV_RI)) {   /* the empty -> not-empty edge */
        wr32(p->io, HWR_IF, rd32(p->io, HWR_IF) | (1u << HWIRQ_FIFO_RECV));
    }
    return 0;
}

void hwpxi_send(HwPxi *p, uint32_t word)
{
    unsigned tag = word & 31u;
    unsigned err = (word >> 5) & 1u;
    uint32_t data = word >> 6;

    assert(p != NULL);
    p->sent[tag]++;
    if (tag != 0u && p->resp[tag] != NULL) {
        p->resp[tag](p, p->resp_user[tag], data, err);
    } else if (tag != 0u && !err) {
        p->unanswered[tag]++;
        (void)hwpxi_reply(p, tag, data, 1u);   /* the SDK's own ARM7 handler echoes a word nobody registered for, with ERR set */
    } else {
        p->unanswered[tag]++;
    }
    hwpxi_cnt_sync(p);
}

uint32_t hwpxi_recv(HwPxi *p)
{
    uint32_t word;

    assert(p != NULL);
    if (p->count == 0u) {
        p->err_state = 1u;   /* reading an empty FIFO latches ERR */
        hwpxi_cnt_sync(p);
        return 0u;
    }
    word = p->recv[p->head];
    p->head = (p->head + 1u) % HWPXI_FIFO_DEPTH;
    p->count--;
    hwpxi_cnt_sync(p);
    return word;
}
