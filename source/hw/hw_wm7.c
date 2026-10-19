/* hw_wm7.c -- the ARM7 wireless manager (WM) of a console with NO RADIO IN RANGE (bet 1.6b). SDK-world file.
 *
 * The game's wireless layer (wireless_manager.c over the SDK's WM) talks to the ARM7 on the WM PXI tag: the ARM9 puts a request (u16 api id + u32
 * parameters) in a command buffer and sends its address; the ARM7 marks it accepted, does the work and sends back the address of a callback record in
 * the `fifo7to9` buffer. The real ARM7 side (libraries/wm/src/ARM7, ~9 300 lines over the wl_arm7 radio driver) drives the wireless chip's registers;
 * this port has no radio, so this file answers each request the way that code answers when the air is empty: the same WMStatus.state transitions the
 * req_*.c handlers make, errcode SUCCESS for the requests that need no radio, and PARENT_NOT_FOUND for every scan. Nothing is received or sent.
 * Every api id is counted (hw_wm7_count) and the first sighting of each is logged. A request this file does not model is answered WM_ERRCODE_FAILED
 * (never silently dropped, which would leave the ARM9 callback table waiting for ever) and logged "NOT MODELLED".
 * Not modelled yet: anything that needs a peer (MP, DCF, key sharing, test modes). The game only reaches them from a connected state, which an
 * empty air never produces. */
#include <assert.h>
#include <string.h>

#include <nitro.h>
#include <nitro/wm.h>

#include "hw_log.h"
#include "hw_wm7.h"
#include "hw_wmq.h"

#define CB_CLEAR_BYTES HWWMQ_REPLY_BYTES

/* The firmware's allowed-channel mask, bit n-1 = channel n: the u16 behind the MAC in the system page's user settings (hw_sys.c fill_page, from
   config.ini). The real ARM7 hands the same value to WMStatus (enableChannel = allowed << 1); the ARM9 WM_GetAllowedChannel() reads the NVRAM copy. */
static u16 allowed_channels(void)
{
    return *(const u16 *)(HW_NVRAM_USER_INFO + ((sizeof(NVRAMConfig) + 3u) & ~3u) + 6u);
}

static WMStatus *s_status;
static u32 *s_fifo7to9;
/* Replies are built in a scratch record, then handed to the queue (hw_wmq.c): the real ARM7 may only write fifo7to9 once the ARM9 has cleared
   wm_callback_control bit 0, and a scan's reply comes after the scan's channel time, never inside the request. */
static u32 s_scratch[CB_CLEAR_BYTES / 4u];
static uint32_t s_delay;   /* frames before the reply being built may be delivered */
static HwWmQ s_q;
static uint32_t s_cb_busy_stalls;
static uint32_t s_count[WM_APIID_ASYNC_KIND_MAX];
static uint32_t s_unmodelled, s_requests;

uint32_t hw_wm7_count(unsigned apiid)
{
    return apiid < WM_APIID_ASYNC_KIND_MAX ? s_count[apiid] : 0u;
}

uint32_t hw_wm7_requests(void)
{
    return s_requests;
}

uint32_t hw_wm7_unmodelled(void)
{
    return s_unmodelled;
}

void hw_wm7_reset(void)
{
    s_status = NULL;
    s_fifo7to9 = NULL;
    hwwmq_init(&s_q);
    /* the per-api counters are statistics for the report and survive a soft reset */
}

static WMCallback *begin_reply(u16 apiid, u16 errcode)
{
    WMCallback *cb = (WMCallback *)s_scratch;

    memset(cb, 0, CB_CLEAR_BYTES);
    cb->apiid = apiid;
    cb->errcode = errcode;
    return cb;
}

static void set_state(u16 state)
{
    if (s_status != NULL) {
        s_status->state = state;
    }
}

static u16 cur_state(void)
{
    return s_status != NULL ? s_status->state : (u16)WM_STATE_READY;
}

static int in_states(const u16 *list, unsigned n)
{
    unsigned i;
    u16 st = cur_state();

    for (i = 0; i < n; i++) {
        if (list[i] == st) {
            return 1;
        }
    }
    return 0;
}

/* req_Initialize.c: records the three buffers, state IDLE, success. */
static void do_initialize(const u32 *rq)
{
    s_status = (WMStatus *)rq[2];
    s_fifo7to9 = (u32 *)rq[3];
    s_status->allowedChannel = allowed_channels();
    s_status->enableChannel = (u16)(allowed_channels() << 1);
    s_status->mp_minFreq = s_status->mp_freq = 1;
    s_status->mp_maxFreq = WM_DEFAULT_MP_FREQ_LIMIT;
    s_status->mp_current_minFreq = s_status->mp_minFreq;
    s_status->mp_current_freq = s_status->mp_freq;
    s_status->mp_current_maxFreq = s_status->mp_maxFreq;
    set_state(WM_STATE_IDLE);
    (void)begin_reply(WM_APIID_INITIALIZE, WM_ERRCODE_SUCCESS);
}

/* The scan family (req_StartScan.c). IDLE / CLASS1 / SCAN accept it; the channel checks are the real handler's (channel 0 or one the firmware does not
   allow is INVALID_PARAM; ScanEx masks its list with enableChannel, an empty list or a NULL / unaligned buffer is INVALID_PARAM). The scan finds nothing
   and leaves the state SCAN until EndScan. The reply comes after the scan's channel time -- channels x maxChannelTime ms (WL_MlmeScan passive scan),
   at least one frame -- never inside the request: the game's finish callback starts the next scan at once (review 1.6, B2: answering inside the PXI
   send recursed without time passing). Error replies are delayed too, for the same reason. */
static void do_start_scan(u16 apiid, const u32 *rq)
{
    static const u16 ok[] = { WM_STATE_IDLE, WM_STATE_CLASS1, WM_STATE_SCAN };
    u16 enable = s_status != NULL ? s_status->enableChannel : (u16)0;

    if (!in_states(ok, 3)) {
        if (apiid == WM_APIID_START_SCAN_EX) {
            WMStartScanExCallback *cb = (WMStartScanExCallback *)begin_reply(apiid, WM_ERRCODE_ILLEGAL_STATE);

            cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        } else {
            WMStartScanCallback *cb = (WMStartScanCallback *)begin_reply(apiid, WM_ERRCODE_ILLEGAL_STATE);

            cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        }
        s_delay = 1u;
        return;
    }
    if (apiid == WM_APIID_START_SCAN_EX) {
        const WMStartScanExReq *rq_ex = (const WMStartScanExReq *)rq;
        u16 list = (u16)((rq_ex->channelList << 1) & enable);
        unsigned n = (unsigned)__builtin_popcount(list);
        WMStartScanExCallback *cb;

        if (n == 0u || rq_ex->scanBuf == NULL || ((uintptr_t)rq_ex->scanBuf & 3u) != 0u) {
            cb = (WMStartScanExCallback *)begin_reply(apiid, WM_ERRCODE_INVALID_PARAM);
            cb->state = WM_STATECODE_PARENT_NOT_FOUND;
            s_delay = 1u;
            return;
        }
        set_state(WM_STATE_SCAN);
        cb = (WMStartScanExCallback *)begin_reply(apiid, WM_ERRCODE_SUCCESS);
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        cb->channelList = (u16)(list >> 1);   /* the masked list, shifted back (req_StartScan.c:417) */
        cb->bssDescCount = 0;
        s_delay = hwwmq_scan_frames(n, rq_ex->maxChannelTime);
    } else {
        const WMStartScanReq *rq_sc = (const WMStartScanReq *)rq;
        unsigned channel = rq_sc->channel;
        WMStartScanCallback *cb;

        if (channel == 0u || channel > 14u || ((1u << channel) & enable) == 0u) {
            cb = (WMStartScanCallback *)begin_reply(apiid, WM_ERRCODE_INVALID_PARAM);
            cb->state = WM_STATECODE_PARENT_NOT_FOUND;
            s_delay = 1u;
            return;
        }
        set_state(WM_STATE_SCAN);
        cb = (WMStartScanCallback *)begin_reply(apiid, WM_ERRCODE_SUCCESS);
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        cb->channel = (u16)channel;
        s_delay = hwwmq_scan_frames(1u, rq_sc->maxChannelTime);
    }
}

static void do_end_scan(void)
{
    if (cur_state() != WM_STATE_SCAN && cur_state() != WM_STATE_CLASS1) {
        (void)begin_reply(WM_APIID_END_SCAN, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    set_state(WM_STATE_IDLE);
    (void)begin_reply(WM_APIID_END_SCAN, WM_ERRCODE_SUCCESS);
}

static void do_start_parent(void)
{
    WMStartParentCallback *cb;

    if (cur_state() != WM_STATE_IDLE) {
        cb = (WMStartParentCallback *)begin_reply(WM_APIID_START_PARENT, WM_ERRCODE_ILLEGAL_STATE);
        cb->state = WM_STATECODE_PARENT_START;
        return;
    }
    set_state(WM_STATE_PARENT);
    cb = (WMStartParentCallback *)begin_reply(WM_APIID_START_PARENT, WM_ERRCODE_SUCCESS);
    cb->state = WM_STATECODE_PARENT_START;
}

static void do_end_parent(void)
{
    if (cur_state() != WM_STATE_PARENT) {
        (void)begin_reply(WM_APIID_END_PARENT, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    set_state(WM_STATE_IDLE);
    (void)begin_reply(WM_APIID_END_PARENT, WM_ERRCODE_SUCCESS);
}

/* Nobody answers a connection request on an empty air: the attempt ends at once in a failed connect and the state returns to IDLE. */
static void do_start_connect(void)
{
    WMStartConnectCallback *cb;

    cb = (WMStartConnectCallback *)begin_reply(WM_APIID_START_CONNECT, WM_ERRCODE_FAILED);
    cb->state = WM_STATECODE_DISCONNECTED;
    set_state(cur_state() == WM_STATE_CONNECT ? (u16)WM_STATE_IDLE : cur_state());
}

static void do_measure(const u32 *rq)
{
    WMMeasureChannelCallback *cb = (WMMeasureChannelCallback *)begin_reply(WM_APIID_MEASURE_CHANNEL, WM_ERRCODE_SUCCESS);

    cb->channel = (u16)(rq[1] >> 16);   /* WMMeasureChannelReq: apiid | ccaMode<<16, edThreshold | channel<<16, measureTime */
    cb->ccaBusyRatio = 0;                   /* nothing on the air */
}

static void do_reset(void)
{
    static const u16 dropped[] = { WM_STATE_CLASS1, WM_STATE_SCAN, WM_STATE_CONNECT, WM_STATE_PARENT, WM_STATE_CHILD, WM_STATE_MP_PARENT, WM_STATE_MP_CHILD };

    if (in_states(dropped, 7)) {
        set_state(WM_STATE_IDLE);
    }
    (void)begin_reply(WM_APIID_RESET, WM_ERRCODE_SUCCESS);
}

static void do_end(void)
{
    if (cur_state() != WM_STATE_IDLE) {
        (void)begin_reply(WM_APIID_END, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    set_state(WM_STATE_READY);
    (void)begin_reply(WM_APIID_END, WM_ERRCODE_SUCCESS);
}

/* Settings that act on the radio only: accepted. */
static int is_setting(u16 id)
{
    switch (id) {
    case WM_APIID_SET_P_PARAM:
    case WM_APIID_SET_GAMEINFO:
    case WM_APIID_SET_BEACON_IND:
    case WM_APIID_SET_LIFETIME:
    case WM_APIID_SET_ENTRY:
    case WM_APIID_SET_WEPKEY:
    case WM_APIID_SET_WEPKEY_EX:
    case WM_APIID_SET_MP_PARAMETER:
    case WM_APIID_SET_BEACON_PERIOD:
    case WM_APIID_AUTO_DEAUTH:
    case WM_APIID_AUTO_DISCONNECT:
    case WM_APIID_SET_PS_MODE:
    case WM_APIID_INIT_W_COUNTER:
        return 1;
    default:
        return 0;
    }
}

static void unmodelled(u16 id)
{
    s_unmodelled++;
    HWLOG("WM request api id %u NOT MODELLED (no radio): answered WM_ERRCODE_FAILED", (unsigned)id);
    (void)begin_reply(id, WM_ERRCODE_FAILED);
}

static void dispatch(u16 id, const u32 *rq)
{
    switch (id) {
    case WM_APIID_INITIALIZE:    do_initialize(rq); break;
    case WM_APIID_START_SCAN:
    case WM_APIID_START_SCAN_EX: do_start_scan(id, rq); break;
    case WM_APIID_END_SCAN:      do_end_scan(); break;
    case WM_APIID_START_PARENT:  do_start_parent(); break;
    case WM_APIID_END_PARENT:    do_end_parent(); break;
    case WM_APIID_START_CONNECT: do_start_connect(); break;
    case WM_APIID_DISCONNECT:    set_state(cur_state() == WM_STATE_CHILD ? (u16)WM_STATE_IDLE : cur_state()); (void)begin_reply(id, WM_ERRCODE_SUCCESS); break;
    case WM_APIID_MEASURE_CHANNEL: do_measure(rq); break;
    case WM_APIID_RESET:         do_reset(); break;
    case WM_APIID_END:           do_end(); break;
    default:
        if (is_setting(id)) {
            (void)begin_reply(id, WM_ERRCODE_SUCCESS);
        } else {
            unmodelled(id);
        }
        break;
    }
}

static volatile uint16_t *cb_control(void)
{
    return &OS_GetSystemWork()->wm_callback_control;
}

uint32_t hw_wm7_request(uint32_t cmd_addr)
{
    HWCOUNT("WM: hw_wm7_request (all api ids)");
    u32 *rq = (u32 *)cmd_addr;
    u16 id;
    int sent;

    if (rq == NULL) {
        return 0u;
    }
    id = (u16)(*(u16 *)rq & ~WM_API_REQUEST_ACCEPTED);
    s_requests++;
    if (id < WM_APIID_ASYNC_KIND_MAX && s_count[id]++ == 0u) {
        HWLOG("WM request api id %u first seen (radio-less ARM7 WM)", (unsigned)id);
    }
    *(u16 *)rq = (u16)(*(u16 *)rq | WM_API_REQUEST_ACCEPTED);   /* the ARM9 may reuse the command buffer (WmGetCommandBuffer4Arm7) */
    if (id != WM_APIID_INITIALIZE && s_fifo7to9 == NULL) {
        return 0u;   /* a request before INITIALIZE has nowhere to answer */
    }
    s_delay = 0u;
    dispatch(id, rq);
    sent = hwwmq_post(&s_q, cb_control(), s_fifo7to9, s_scratch, s_delay);
    if (sent < 0) {
        HWLOG("WM reply queue full: reply for api id %u dropped", (unsigned)id);
    }
    return sent == 1 ? (uint32_t)(uintptr_t)s_fifo7to9 : 0u;
}

/* One frame (hw_devs_input_frame): the due reply, if the ARM9 has released fifo7to9; returns its address for the PXI WM tag (0 = nothing). */
uint32_t hw_wm7_tick(void)
{
    HWCOUNT("WM: hw_wm7_tick (queued replies, per frame)");
    if (s_fifo7to9 == NULL) {
        return 0u;
    }
    if (hwwmq_pending(&s_q) != 0u && (*cb_control() & HWWMQ_BUSY_BIT) != 0u) {
        s_cb_busy_stalls++;
    }
    return hwwmq_tick(&s_q, cb_control(), s_fifo7to9) ? (uint32_t)(uintptr_t)s_fifo7to9 : 0u;
}

void hw_wm7_report(unsigned words_sent, unsigned requests, unsigned answered)
{
    assert(words_sent >= requests || requests > 0u);
    assert(answered <= requests + 1u || requests == 0u);
    HWLOG("WM tag: %u words sent, %u requests, %u answered; api ids INITIALIZE %u RESET %u END %u START_SCAN %u START_SCAN_EX %u END_SCAN %u MEASURE %u; NOT MODELLED %u",
          words_sent, requests, answered, (unsigned)hw_wm7_count(WM_APIID_INITIALIZE), (unsigned)hw_wm7_count(WM_APIID_RESET),
          (unsigned)hw_wm7_count(WM_APIID_END), (unsigned)hw_wm7_count(WM_APIID_START_SCAN), (unsigned)hw_wm7_count(WM_APIID_START_SCAN_EX),
          (unsigned)hw_wm7_count(WM_APIID_END_SCAN), (unsigned)hw_wm7_count(WM_APIID_MEASURE_CHANNEL), (unsigned)hw_wm7_unmodelled());
    HWLOG("WM reply queue: posted %u, delivered %u, deferred %u, dropped %u, longest wait %u frames, pending %u, ARM9-busy stalls %u; allowed-channel word 0x%04x",
          (unsigned)s_q.posted, (unsigned)s_q.delivered, (unsigned)s_q.deferred, (unsigned)s_q.dropped, (unsigned)s_q.max_wait, hwwmq_pending(&s_q),
          (unsigned)s_cb_busy_stalls, (unsigned)allowed_channels());
}
