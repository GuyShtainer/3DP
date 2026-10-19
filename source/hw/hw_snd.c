/* hw_snd.c -- the mailbox and the cadence of the ARM7 sound driver (bet 1.5). Pure C; the driver itself is reached through HwSndOps
 * (source/hw/hw_snd7.c binds them to the prefixed ARM7 objects), so tools/host-tests/hw_snd_test.c runs this file with fakes.
 *
 * Mailbox: a word the ARM9 sends on the SOUND tag (hw_pxi_arm7.c -> hwsnd_send) is queued, not run: the real ARM7 takes it in its FIFO
 * interrupt, which here is the next slice, i.e. the next pump (a yield point). The slice hands each word to the ARM7's own PXI callback,
 * exactly as the FIFO IRQ would: a pointer word queues a command list, word 0 wakes the driver thread.
 * Cadence: the driver's loop body (SndThread) runs once per wake-up message and once per SND_PROC_INTERVAL (HWSND_PERIOD_SYS of virtual
 * time). The SDK's own alarm would fire at most once per wake however late the pump is, so the slice counts periods itself
 * (event-model.md, "Periodic alarms"): elapsed / period passes, at most HWSND_CATCHUP_MAX, the rest of a longer backlog dropped and counted.
 * Registers: the ARM7 code writes the SPU window of the I/O block as plain memory (nitrosdk-snd-arm7.patch); the slice diffs the window
 * to count changed words. No mixer. Design and mapping: the private design notes. */
#include <assert.h>
#include <string.h>

#include "hw_log.h"
#include "hw_pure.h"

#define SPU_BYTES (HWSND_SPU_WORDS * 4u)

_Static_assert(HWSND_SPU_OFF + HWSND_SPU_WORDS * 4u == 0x520u, "the SPU window is 0x400..0x51f");
_Static_assert(HWSND_PERIOD_SYS == 1396736u, "SND_PROC_INTERVAL 0xAA8 OS ticks x 512 sys ticks");
/* Between two slices the ARM9 queues at most 8 command-list pointers (SND_FlushCommand waits for a free list otherwise, which pumps) and, because wake
   words coalesce below, at most one wake-up word. (The earlier "8 + 1" bound counted a wake per flush; the real bound relies on the coalescing.) */
_Static_assert(HWSND_CATCHUP_MAX >= 1u && HWSND_MAIL_DEPTH >= 9u, "the ARM9 queues at most 8 command lists plus one coalesced wake-up");
_Static_assert(HWSND_REPLY_DEPTH >= 8u, "one alarm notification per SND alarm can be pending");

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof v);
    return v;
}

static void spu_snapshot(HwSnd *s)
{
    uint32_t i;

    assert(s != NULL && s->io != NULL);
    for (i = 0; i < HWSND_SPU_WORDS; i++) {
        s->shadow[i] = rd32(s->io + HWSND_SPU_OFF + 4u * i);
    }
}

static void spu_diff(HwSnd *s)
{
    uint32_t i, changed = 0;

    assert(s != NULL && s->io != NULL);
    for (i = 0; i < HWSND_SPU_WORDS; i++) {
        uint32_t v = rd32(s->io + HWSND_SPU_OFF + 4u * i);

        if (v != s->shadow[i]) {
            changed++;
            s->shadow[i] = v;
        }
    }
    s->spu_words_changed += changed;
    if (changed != 0u) {
        s->spu_slices_changed++;
    }
}

/* ---- the channel busy bits ------------------------------------------------------------------------------------------------------------
 * The ARM7 driver (SND_UpdateSharedWork) reads SOUNDxCNT bit 31 to learn which channels still play, and the game's wave-out / cry code waits on that
 * (Sound_IsPokemonCryPlaying); real hardware clears the bit when a one-shot sample has played to its end. Registers are plain memory here, so the
 * slice does it: a key-on seen at the end of a slice is timed from that slice's virtual time and the bit is cleared at the start of the first slice
 * at or after its end. Loop-mode sounds, PSG and noise never end (the driver stops them with SOUNDxCNT = 0). Nothing is mixed. */
#define SND_CNT(ch)  (HWSND_SPU_OFF + 0x10u * (ch))
#define SND_CAP0CNT  0x108u   /* offset inside the SPU window: 0x508 */
#define SYS_PER_TIMER_TICK 16u     /* the sound timer ticks at bus/2 */
#define SYS_PER_CAP_SAMPLE 8192u   /* the capture unit takes one sample per 1024 bus clocks */
#define CNT_BUSY 0x80000000u

uint64_t hwsnd_chan_ticks(uint32_t cnt, uint32_t tmr, uint32_t pnt, uint32_t len)
{
    uint32_t repeat = (cnt >> 27) & 3u, fmt = (cnt >> 29) & 3u;
    uint64_t words = (uint64_t)(pnt & 0xffffu) + (len & 0x3fffffu), samples, period;

    assert(fmt <= 3u && repeat <= 3u);
    if (repeat != 2u || fmt == 3u || words == 0u) {
        return 0u;   /* manual/loop modes and PSG/noise run until stopped */
    }
    if (fmt == 0u) {
        samples = words * 4u;
    } else if (fmt == 1u) {
        samples = words * 2u;
    } else {
        samples = words * 8u > 8u ? words * 8u - 8u : 1u;   /* ADPCM: the first word is the decoder header */
    }
    period = (uint64_t)(0x10000u - (tmr & 0xffffu)) * SYS_PER_TIMER_TICK;
    return samples * period;
}

uint64_t hwsnd_cap_ticks(uint32_t cnt, uint32_t len)
{
    uint64_t words = (len & 0xffffu) == 0u ? 1u : (len & 0xffffu);

    assert((cnt & 0xffu) == cnt);
    if (!(cnt & 4u)) {
        return 0u;   /* bit 2 clear: loop capture, runs until stopped */
    }
    return words * ((cnt & 8u) ? 4u : 2u) * SYS_PER_CAP_SAMPLE;   /* PCM8: 4 samples a word, PCM16: 2 */
}

static void wr32m(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof v);
}

/* start of a slice: whatever has finished playing stops being busy */
static void busy_expire(HwSnd *s, uint64_t now)
{
    unsigned ch;

    assert(s != NULL && s->io != NULL);
    for (ch = 0; ch < HWSND_CHANNELS; ch++) {
        if (s->chan_live[ch] && s->chan_end[ch] != 0u && now >= s->chan_end[ch]) {
            uint8_t *r = s->io + SND_CNT(ch);

            wr32m(r, rd32(r) & ~CNT_BUSY);
            s->chan_live[ch] = 0u;
            s->chan_end[ch] = 0u;
            s->chan_expired++;
        }
    }
    for (ch = 0; ch < 2u; ch++) {
        if (s->cap_live[ch] && s->cap_end[ch] != 0u && now >= s->cap_end[ch]) {
            s->io[HWSND_SPU_OFF + SND_CAP0CNT + ch] &= (uint8_t)~0x80u;
            s->cap_live[ch] = 0u;
            s->cap_end[ch] = 0u;
            s->cap_expired++;
        }
    }
}

/* end of a slice: a channel the driver keyed on (bit 31 newly set, or its parameters rewritten while set) starts its clock */
static void busy_scan(HwSnd *s, uint64_t now)
{
    unsigned ch;

    assert(s != NULL && s->io != NULL);
    for (ch = 0; ch < HWSND_CHANNELS; ch++) {
        const uint8_t *r = s->io + SND_CNT(ch);
        uint32_t cur[4];

        cur[0] = rd32(r);
        cur[1] = rd32(r + 4);
        cur[2] = rd32(r + 8);
        cur[3] = rd32(r + 12);
        if (!(cur[0] & CNT_BUSY)) {
            s->chan_live[ch] = 0u;
            s->chan_end[ch] = 0u;
            continue;
        }
        if (s->chan_live[ch] && memcmp(cur, s->chan_snap[ch], sizeof cur) == 0) {
            continue;
        }
        {
            uint64_t t = hwsnd_chan_ticks(cur[0], cur[2] & 0xffffu, cur[2] >> 16, cur[3]);

            memcpy(s->chan_snap[ch], cur, sizeof cur);
            s->chan_live[ch] = 1u;
            s->chan_end[ch] = t != 0u ? now + t : 0u;
            s->chan_keyons++;
            if (t == 0u) {
                s->chan_unbounded++;
            }
        }
    }
    for (ch = 0; ch < 2u; ch++) {
        uint32_t cnt = s->io[HWSND_SPU_OFF + SND_CAP0CNT + ch], len = (uint32_t)s->io[HWSND_SPU_OFF + 0x114u + 8u * ch]
                                                                       | ((uint32_t)s->io[HWSND_SPU_OFF + 0x115u + 8u * ch] << 8);
        uint32_t cur[2];

        cur[0] = cnt;
        cur[1] = len;
        if (!(cnt & 0x80u)) {
            s->cap_live[ch] = 0u;
            s->cap_end[ch] = 0u;
            continue;
        }
        if (s->cap_live[ch] && memcmp(cur, s->cap_snap[ch], sizeof cur) == 0) {
            continue;
        }
        {
            uint64_t t = hwsnd_cap_ticks(cnt, len);

            memcpy(s->cap_snap[ch], cur, sizeof cur);
            s->cap_live[ch] = 1u;
            s->cap_end[ch] = t != 0u ? now + t : 0u;
            s->cap_keyons++;
        }
    }
}

void hwsnd_init(HwSnd *s, const HwSndOps *ops, uint8_t *ioreg, struct HwPxi *pxi)
{
    assert(s != NULL && ops != NULL && ioreg != NULL);
    assert(ops->boot != NULL && ops->recv != NULL && ops->run != NULL);
    memset(s, 0, sizeof *s);
    s->ops = *ops;
    s->io = ioreg;
    s->pxi = pxi;
}

void hwsnd_send(HwSnd *s, uint32_t data, unsigned err)
{
    HWCOUNT("sound: hwsnd_send (command word to the ARM7 driver)");
    assert(s != NULL);
    assert(err <= 1u);
    if (err) {
        return;   /* the SDK never sends the error flag on this tag; a stray one is not a command */
    }
    if (data == 0u) {
        uint32_t i;

        for (i = 0; i < s->mail_count; i++) {
            if (s->mail[(s->mail_head + i) % HWSND_MAIL_DEPTH] == 0u) {
                s->wake_coalesced++;   /* a wake-up is a flag (sndMesgQueue): one queued already wakes the driver once, as many sends would */
                return;
            }
        }
    }
    if (s->mail_count >= HWSND_MAIL_DEPTH) {
        s->mail_forced++;   /* cannot happen with the SDK's 8-list limit; if it does, run the slice now rather than lose a word */
        hwsnd_pump(s, s->last_sys);
    }
    /* still full: the call came from inside a slice (nothing to drain into). A command-list pointer must never be dropped, so this is fatal */
    assert(s->mail_count < HWSND_MAIL_DEPTH);
    s->mail[(s->mail_head + s->mail_count) % HWSND_MAIL_DEPTH] = data;
    s->mail_count++;
    if (data == 0u) {
        s->wake_words++;
    } else {
        s->cmd_words++;
    }
}

/* send what the ARM9 FIFO has room for, oldest first; what does not fit stays in the ring */
static void reply_drain(HwSnd *s)
{
    while (s->reply_count > 0u) {
        if (hwpxi_reply(s->pxi, HWPXI_TAG_SOUND, s->reply_ring[s->reply_head], 0u) != 0) {
            return;
        }
        s->reply_head = (s->reply_head + 1u) % HWSND_REPLY_DEPTH;
        s->reply_count--;
        s->replies++;
    }
}

/* The ARM7's reply path. snd_alarm.c:120 spins `while (PXI_SendWordByFifo(...) < 0)` inside the ARM9 timer IRQ on the DS, where the ARM9 drains the FIFO on
   its own CPU; here it is one CPU, so a full FIFO would hang. A word that does not fit therefore waits in an ARM7-side ring (drained at the start of the next
   slice, and here when there is room), and this function never reports "full". The ring overflowing (more than HWSND_REPLY_DEPTH alarm fires while the ARM9
   drained nothing) is fatal. */
int hwsnd_reply(HwSnd *s, uint32_t data)
{
    assert(s != NULL);
    if (s->pxi == NULL) {
        return -1;
    }
    reply_drain(s);   /* keep the order: older deferred words go first */
    if (s->reply_count == 0u && hwpxi_reply(s->pxi, HWPXI_TAG_SOUND, data, 0u) == 0) {
        s->replies++;
        return 0;
    }
    if (s->reply_count >= HWSND_REPLY_DEPTH) {
        s->reply_overflow++;   /* the word is lost: counted for builds without assert */
        assert(!"hwsnd_reply: the ARM7 reply ring overflowed");
        return 0;
    }
    s->reply_ring[(s->reply_head + s->reply_count) % HWSND_REPLY_DEPTH] = data;
    s->reply_count++;
    s->reply_deferred++;
    return 0;
}

/* ---- the ARM7's private alarms ------------------------------------------------------------------------------------------------------ */
uint64_t hwsnd_now_tick(const HwSnd *s)
{
    assert(s != NULL);
    return s->now_sys / HWT_SYS_PER_TICK;
}

void hwsnd_alarm_cancel(HwSnd *s, const void *key)
{
    uint32_t i;

    assert(s != NULL && key != NULL);
    for (i = 0; i < HWSND_ALARMS; i++) {
        if (s->alarm[i].key == key) {
            memset(&s->alarm[i], 0, sizeof s->alarm[i]);
        }
    }
}

void hwsnd_alarm_set(HwSnd *s, const void *key, uint64_t due_tick, uint64_t period_tick, void (*fn)(void *), void *arg)
{
    uint32_t i;
    HwSndAlarm *slot = NULL;

    assert(s != NULL && key != NULL && fn != NULL);
    for (i = 0; i < HWSND_ALARMS; i++) {
        if (s->alarm[i].key == key) {
            slot = &s->alarm[i];
            break;
        }
        if (slot == NULL && s->alarm[i].key == NULL) {
            slot = &s->alarm[i];
        }
    }
    assert(slot != NULL);   /* SND_ALARM_NUM (8) + the interval alarm fit; a ninth ARM7 alarm is a new use that needs a bigger table */
    slot->key = key;
    slot->due = due_tick;
    slot->period = period_tick;
    slot->fn = fn;
    slot->arg = arg;
    s->alarms_set++;
}

/* fire what is due at `now` (virtual time of the slice). A periodic alarm that fell behind fires at most HWSND_CATCHUP_MAX times, the rest of the
   backlog is dropped and counted, like the sequencer's periods. A handler may set or cancel alarms; each slot is copied out before its handler runs. */
static void alarms_fire(HwSnd *s)
{
    uint64_t now = hwsnd_now_tick(s);
    uint32_t i;

    for (i = 0; i < HWSND_ALARMS; i++) {
        uint32_t n = 0;

        while (s->alarm[i].key != NULL && s->alarm[i].due <= now && n < HWSND_CATCHUP_MAX) {
            HwSndAlarm a = s->alarm[i];

            if (a.period == 0u) {
                memset(&s->alarm[i], 0, sizeof s->alarm[i]);
            } else {
                s->alarm[i].due = a.due + a.period;
            }
            s->alarms_fired++;
            n++;
            a.fn(a.arg);
            if (a.period == 0u) {
                break;
            }
        }
        if (s->alarm[i].key != NULL && s->alarm[i].period != 0u && s->alarm[i].due <= now) {
            uint64_t late = now - s->alarm[i].due;

            s->alarm_backlog_dropped++;   /* re-anchor to the first boundary after now */
            s->alarm[i].due += (late / s->alarm[i].period + 1u) * s->alarm[i].period;
        }
    }
}

/* deliver the queued words in order; a word delivered may queue more only through hwsnd_send, which the driver never calls */
static void deliver(HwSnd *s)
{
    uint32_t n = s->mail_count, i;

    assert(n <= HWSND_MAIL_DEPTH);
    for (i = 0; i < n && s->mail_count > 0u; i++) {
        uint32_t w = s->mail[s->mail_head];

        s->mail_head = (s->mail_head + 1u) % HWSND_MAIL_DEPTH;
        s->mail_count--;
        s->ops.recv(s->ops.user, w, 0u);
    }
}

static void wake_passes(HwSnd *s)
{
    uint32_t n;

    for (n = 0; n < HWSND_CATCHUP_MAX; n++) {
        if (!s->ops.run(s->ops.user, 0)) {
            break;
        }
        s->steps++;
        s->wake_steps++;
    }
}

static void periodic_passes(HwSnd *s, uint64_t now)
{
    uint64_t due;
    uint32_t n, i;

    assert(s->last_sys <= now);
    due = (now - s->last_sys) / HWSND_PERIOD_SYS;
    if (due == 0u) {
        return;
    }
    if (due > HWSND_CATCHUP_MAX) {
        n = HWSND_CATCHUP_MAX;
        s->overruns++;
        s->last_sys = now;   /* the backlog beyond the cap is dropped: the sequencer runs slow under overrun, it does not burst */
    } else {
        n = (uint32_t)due;
        s->last_sys += due * HWSND_PERIOD_SYS;
    }
    for (i = 0; i < n; i++) {
        (void)s->ops.run(s->ops.user, 1);
        s->steps++;
        s->periodic_steps++;
        if (i > 0u) {
            s->catchup_steps++;
        }
    }
}

void hwsnd_pump(HwSnd *s, uint64_t now_sys)
{
    HWCOUNT("sound: hwsnd_pump (one ARM7 sound slice)");
    assert(s != NULL && s->io != NULL);
    if (s->in_pump) {
        s->reentry++;   /* the driver's own OS calls re-entered a pump: the outer slice carries on */
        return;
    }
    s->in_pump = 1u;
    s->slices++;
    s->now_sys = now_sys;
    if (s->ops.enter != NULL) {
        s->ops.enter(s->ops.user);   /* ARM9 IRQs stay pending for the whole slice (HWOS_Arm7Enter): a real ARM7 is another CPU */
    }
    reply_drain(s);
    if (!s->booted) {
        s->booted = 1u;
        s->last_sys = now_sys;
        s->boot_sys = now_sys;
        spu_snapshot(s);
        s->ops.boot(s->ops.user);
        spu_diff(s);   /* the driver's start-up writes (SOUNDCNT enable, master volume, output selector, channel clears) are counted too */
    }
    if (now_sys < s->last_sys) {
        s->last_sys = now_sys;   /* time went back: a soft reset restarted the pump's clock. Re-anchor; no period is owed */
        s->rebases++;
    }
    busy_expire(s, now_sys);
    deliver(s);
    alarms_fire(s);
    wake_passes(s);
    periodic_passes(s, now_sys);
    busy_scan(s, now_sys);
    spu_diff(s);
    reply_drain(s);
    if (s->ops.leave != NULL) {
        s->ops.leave(s->ops.user);
    }
    s->in_pump = 0u;
}
