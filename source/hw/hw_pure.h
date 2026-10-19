/* hw_pure.h -- the pure (no SDK, no libctru) modules of the hardware layer: virtual time, timer 0-3 emulation, the event pump and the
 * host-thread hand-off. Plain C99 over <stdint.h> only, so tools/host-tests/hw_os_test.c compiles them on the build machine.
 * Design: the private design notes. Every function that does something observable takes its environment as a callback table, so the
 * test replaces time, the vblank wait and the IRQ dispatch with fakes.
 */
#ifndef HW_PURE_H
#define HW_PURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- time ---------------------------------------------------------------------------------------------------------------- */
#define HWT_SYS_HZ        268111856u   /* ARM11 system tick (libctru SYSCLOCK_ARM11); DS bus 33 513 982 Hz = sys/8; OS tick = sys/512 */
#define HWT_SYS_PER_BUS   8u
#define HWT_SYS_PER_TICK  512u         /* one DS OS tick (bus/64) */
#define HWT_SYS_PER_CYCLE 4u           /* one ARM9 cycle (67 027 964 Hz) */
#define HWT_FRAME_SYS     4481136u     /* one frame: 268 111 856 / 4 481 136 = 59.831 Hz, the 3DS LCD rate. The DS frame was 4 481 520 sys ticks (59.826 Hz): the port runs 86 ppm fast, locked to the real vblank */

/* Bet 2.2a: the two clocks (config.ini `clock = frame|wall`, the private design notes).
   WALL  virt = max(virt, real - base): the host clock drives virtual time; a slow host drops DS vblanks (hw_pump.c overrun rule).
   FRAME virt moves only by explicit charges (spins, OS_GetTick polls) clamped below the next vblank, and by exactly one frame per waiting yield that
         reaches the vblank; the host clock is never read for virtual time. Two runs of one binary are then bit-identical in every count. */
typedef enum HwClock { HWCLOCK_WALL = 0, HWCLOCK_FRAME = 1 } HwClock;
#define HWT_SPIN_BUDGET_SYS (2u * HWT_FRAME_SYS)   /* FRAME: charged cycles inside one frame after which a vblank is forced (forced_vblanks) */
#define HWT_POLL_CYCLES     64u                    /* FRAME: ARM9 cycles one OS_GetTick poll costs, so a tick-delta busy-wait makes progress */

typedef struct HwTime {
    uint64_t virt;       /* virtual time, sys ticks since init; monotone, never behind real time (WALL) */
    uint64_t real_base;  /* real sys tick at init */
    int64_t tick_off;    /* OS tick offset (OS_InitTick / OS_SetTick) */
    uint32_t frame;      /* 1 = HWCLOCK_FRAME: hwtime_update ignores the host clock, hwtime_steal is a no-op */
} HwTime;

void hwtime_init(HwTime *t, uint64_t real_now);
uint64_t hwtime_update(HwTime *t, uint64_t real_now);        /* WALL: virt = max(virt, real - base); FRAME: virt untouched; returns virt */
/* Pause virtual time for `ticks` real sys ticks (bet 2.1c2): the instrumentation's own cost must not reach the game's clock. real_base moves forward, so
   `real - base` is `ticks` smaller from now on; virt (monotone) stays where it is until real time catches up. FRAME: a no-op (the host clock does not
   feed virtual time, so instrument cost cannot reach the game; asserting instead would make every instrument depend on the mode). */
void hwtime_steal(HwTime *t, uint64_t ticks);
void hwtime_spin_cycles(HwTime *t, uint32_t arm9_cycles);    /* virt += 4 * cycles */
uint64_t hwtime_os_tick(const HwTime *t);                    /* virt / 512 + tick_off */
void hwtime_set_os_tick(HwTime *t, uint64_t tick);           /* OS_InitTick (0) / OS_SetTick */

/* ---- timers 0-3 ---------------------------------------------------------------------------------------------------------- */
#define HWR_DISPSTAT 0x004u   /* u16: bit0 vblank flag, bit3 vblank IRQ enable */
#define HWR_VCOUNT   0x006u   /* u16 */
#define HWR_TM0CNT_L 0x100u   /* u16 reload on write / counter on read; timer n at + 4n; CNT_H at +2 */
#define HWR_IME      0x208u   /* u32 */
#define HWR_IE       0x210u   /* u32 */
#define HWR_IF       0x214u   /* u32 */
#define HWIRQ_VBLANK 0u
#define HWIRQ_HBLANK 1u        /* IF / IE bit 1; DISPSTAT bit 1 = the HBlank flag, bit 4 = the HBlank IRQ enable (bet 2.3a) */
#define HWIRQ_TIMER0 3u
#define HWIRQ_DMA0   8u        /* DMA n raises IF bit 8 + n */
#define HWOS_BOOT_CPSR 0x1fu   /* SYS mode, I and F clear: crt0.c:69-70 enables IRQs before NitroMain, the SDK's own start-up does the rest */

typedef struct HwTimers {
    uint64_t last_bus;
    uint64_t acc[4];
    uint16_t counter[4];
    uint16_t reload[4];
    uint16_t last_written[4];
    uint8_t running[4];
} HwTimers;

void hwtimer_init(HwTimers *t, uint64_t bus_now);
/* Advance every enabled timer to `bus_now` (bus ticks) from the register block; counters are written back to CNT_L. Returns the IF bits
   (bit 3+n) of the IRQ-enabled timers that overflowed. */
uint32_t hwtimer_advance(HwTimers *t, uint8_t *ioreg, uint64_t bus_now);
/* The caller is about to write timer n's control register (OS_SetTimerControl). The timer is brought up to date by the caller first
   (hwpump_sync); a write is a re-arm when it carries the enable bit, so the next advance must latch the reload again even though the
   register was never seen with the enable bit clear in between. */
void hwtimer_ctl_write(HwTimers *t, unsigned n);
/* Bus tick of the earliest overflow of an enabled, IRQ-enabled, non-cascade timer, or UINT64_MAX. */
uint64_t hwtimer_next_irq(const HwTimers *t, const uint8_t *ioreg);

/* ---- the pump ------------------------------------------------------------------------------------------------------------ */
typedef enum HwPumpMode {
    HWP_DISPATCH_ONLY = 0, /* Y5: interrupts re-enabled; dispatch what is pending, advance nothing */
    HWP_NOWAIT,            /* Y2, Y3: advance time and run what is due, never block */
    HWP_WAIT               /* Y1, Y4, Y7: block until something is pending (the DS halt) */
} HwPumpMode;

typedef struct HwPumpOps {
    uint64_t (*real_sys_tick)(void *u);
    void (*wait_vblank)(void *u);                  /* gspWaitForVBlank: blocks to the real vblank */
    void (*sleep_sys)(void *u, uint64_t sys);      /* sleep `sys` system ticks (timer wake-up before the vblank) */
    void (*arm7_pump)(void *u);                    /* the ARM7 driver slice (bet 1.5); may be NULL */
    void (*dispatch)(void *u, int irq);            /* run OS_IRQTable[irq] and the thread switch */
    int (*irq_masked)(void *u);                    /* CPSR I bit */
    void *user;
    void (*level_irqs)(void *u);                   /* optional: level-triggered lines (the GX FIFO below-half line) re-asserted before each dispatch pass */
    void (*vblank)(void *u);                       /* optional (bet 2.4b): the DS vertical blank was raised -- the geometry engine's swap latch */
} HwPumpOps;

/* Bet 2.3a: the HBlank dispatch of one frame. `snap(user, first, count)` records the 2D register images of lines first .. first+count-1
   (count == 192 = ONE image standing for every line: the truthful short-cut). Documented function-pointer dispatch (golden rule 9). */
#define HW_LINES 192u
typedef struct HwPumpLineOps {
    void (*snap)(void *u, uint32_t first, uint32_t count);
    void *user;
} HwPumpLineOps;

typedef struct HwHblank {
    uint32_t frames;       /* hwpump_hblank_frame calls (one per game frame that reached the snapshot point) */
    uint32_t shortcut;     /* ... of which took the short-cut: DISPSTAT bit 4 or IE bit 1 clear at the frame's start, so no handler can run */
    uint32_t full;         /* ... of which ran 192 lines */
    uint32_t lines;        /* lines run in full frames (192 per full frame) */
    uint32_t irqs;         /* HBlank IRQs raised (IF bit 1 set) */
    uint32_t ds_frames;    /* frames that started with DISPSTAT bit 4 set (the game asked for the IRQ) */
    uint32_t ie_frames;    /* frames that started with IE bit 1 set */
    uint32_t in_line;      /* 1 while a line runs: VCOUNT reads `line` and DISPSTAT bit 0 is clear (set_lines honours it) */
    uint32_t line;
} HwHblank;

typedef struct HwPump {
    HwPumpOps ops;
    uint8_t *io;
    uint32_t *vblank_buf;      /* HW_VBLANK_COUNT_BUF */
    HwTime time;
    HwTimers tm;
    uint64_t next_vblank;      /* virtual sys tick of the next vblank (line 192) */
    uint32_t vblanks;          /* raised */
    uint32_t overruns;         /* vblanks raised late by >= 1 frame (not caught up) -- WALL only */
    uint32_t forced_vblanks;   /* FRAME: vblanks raised by an exhausted spin budget instead of a waiting yield (a game-side busy-wait that no timer ends) */
    uint64_t frame_spin;       /* FRAME: sys ticks charged since the last vblank */
    uint32_t dispatched;
    HwHblank hb;
    int inited;
} HwPump;

void hwpump_init(HwPump *p, const HwPumpOps *ops, uint8_t *ioreg, uint32_t *vblank_buf);
/* Select the clock. Switching to FRAME before the first vblank also restarts virtual time at 0 (nothing has consumed it yet: called from hw_sys_init). */
void hwpump_set_clock(HwPump *p, HwClock clock);
/* An OS_GetTick: WALL = hwtime_update with the host tick; FRAME = charge HWT_POLL_CYCLES (clamped, budgeted) and bring timers / a forced vblank up to date
   (no dispatch). Returns virtual time. */
uint64_t hwpump_poll(HwPump *p);
void hwpump_run(HwPump *p, HwPumpMode mode);
void hwpump_spin(HwPump *p, uint32_t arm9_cycles);   /* hwtime_spin_cycles + hwpump_run(HWP_NOWAIT) */
/* Bring the timers up to the current time without dispatching or waiting (called before a timer control write). */
void hwpump_sync(HwPump *p);
/* Bet 2.3a: the frame's 192 HBlank lines, run at the game's snapshot point (after the vblank callback, bet 2.2c B3). Per line n: VCOUNT = n, DISPSTAT bit 1 set (bit 0 clear)
   for the callback's duration, IF bit 1 raised only if DISPSTAT bit 4 and IE bit 1 are set (then the existing dispatch_loop runs OS_IRQTable[1] like the vblank's), then
   `lo->snap(n, 1)` AFTER the callback, then bit 1 cleared. No virtual time is charged and the vblank does not move. Short-cut: DISPSTAT bit 4 or IE bit 1 clear at the start
   = no handler can run, so ONE snap(0, 192) and `hb.shortcut++`. Returns the lines run (0 for the short-cut). */
uint32_t hwpump_hblank_frame(HwPump *p, const HwPumpLineOps *lo);

/* ---- host-thread hand-off (cooperative contexts) ----------------------------------------------------------------------------- */
#define HWCTX_MAX   64
#define HWCTX_MAGIC 0x48435831u   /* "HCX1": stored in the saved image word r[12], the record index in r[11] */

typedef struct HwCtxBackend {
    /* a thread that starts parked; its first wake runs entry(arg) */
    void *(*create)(void (*entry)(void *), void *arg, uint32_t stack_bytes, void *u);
    void (*wake)(void *h, void *u);   /* sticky: a wake before the park makes the park return at once */
    void (*park)(void *h, void *u);
    void (*exit_current)(void *h, void *u);   /* end the calling thread and free its handle; never returns */
    void (*kill)(void *h, void *u);           /* end a PARKED thread (not the caller) without running anything on its stack; the process's own main thread is left parked */
    void *user;
} HwCtxBackend;

typedef struct HwCtxRec {
    void *h;
    void *owner;                 /* the OSContext this record last saved into / was started from */
    uintptr_t pc, r0, lr;        /* fresh start image */
    uint8_t used;
} HwCtxRec;

typedef struct HwCtx {
    HwCtxBackend be;
    HwCtxRec rec[HWCTX_MAX];
    int cur;                     /* the running record */
    void *main_h;                /* the handle hwctx_init was given: the process's own thread, which cannot be ended */
    uint32_t restarts;
} HwCtx;

void hwctx_init(HwCtx *c, const HwCtxBackend *be, void *main_handle);
/* OS_SaveContext: mark the context `owner` (its words w11, w12) as belonging to the running record. Always returns 0. */
int hwctx_save(HwCtx *c, void *owner, uint32_t *w11, uint32_t *w12);
/* OS_LoadContext. A saved context (w12 == magic, record in use) wakes its record; any other context is a fresh start image
   (pc, r0, lr) and gets a new host thread. The caller parks until some later load targets its own record; with `retire_me` it ends
   instead. Returns 0, or -1 if no record is free (the caller must treat that as fatal). */
int hwctx_load(HwCtx *c, void *owner, uint32_t w11, uint32_t w12, uintptr_t pc, uintptr_t r0, uintptr_t lr, uint32_t stack_bytes,
               int retire_me);
void *hwctx_cur_owner(const HwCtx *c);
/* Soft reset: end every other context (parked threads, killed without unwinding), start entry(arg) on a FRESH host thread and retire the
   calling one: it ends, or, if it is the process's main thread, parks for good. Returns -1 (nothing changed) if no thread could be created. */
int hwctx_restart(HwCtx *c, void (*entry)(void *), void *arg, uint32_t stack_bytes);

/* ---- BLZ backward decoder (MIi_UncompressBackward) ------------------------------------------------------------------------- */
uint32_t hwblz_uncompress(uint8_t *bottom);

/* ---- DMA 0-3 (bet 1.3 owns the channels; bet 2.1 extends the destination routing) --------------------------------------------- */
#define HWR_DMA0SAD         0x0b0u   /* channel n: SAD, DAD, CNT (u32 each) at + 12n */
#define HWDMA_CHANNELS      4u
#define HWDMA_CNT_COUNT     0x001fffffu
#define HWDMA_CNT_DAR_SHIFT 21u      /* 0 inc, 1 dec, 2 fixed, 3 inc + reload */
#define HWDMA_CNT_SAR_SHIFT 23u      /* 0 inc, 1 dec, 2 fixed, 3 prohibited (treated as inc) */
#define HWDMA_CNT_REPEAT    (1u << 25)
#define HWDMA_CNT_32BIT     (1u << 26)
#define HWDMA_CNT_IRQ       (1u << 30)
#define HWDMA_CNT_ENABLE    (1u << 31)

typedef enum HwDmaRegion {
    HWDMA_UNMAPPED = 0,   /* no such memory here: reads give zero, writes are dropped and counted */
    HWDMA_MAIN,           /* main RAM / anything that is plain host memory */
    HWDMA_VRAM,
    HWDMA_OAM,
    HWDMA_PLTT,
    HWDMA_GXFIFO,         /* the geometry FIFO: a placeholder sink until bet 2.1 */
    HWDMA_IO,
    HWDMA_REGION_COUNT
} HwDmaRegion;

typedef struct HwDmaOps {
    /* Map the DS range [addr, addr + bytes) to host memory and classify it; NULL with *region = HWDMA_GXFIFO means "the FIFO". */
    void *(*xlat)(void *u, uint32_t addr, uint32_t bytes, HwDmaRegion *region);
    void (*gx_word)(void *u, uint32_t word);   /* one word into the geometry FIFO (placeholder sink); may be NULL */
    void *user;
    /* A copy into host memory finished: `dst` is the lowest host address of the destination span, `bytes` its length (bet 2.1b: the VRAM dirty bitmap). May be NULL. */
    void (*wrote)(void *u, HwDmaRegion region, const void *dst, uint32_t bytes);
    /* Bet 2.1c2: a word-wide, word-stepped burst into the FIFO whose source is already in host memory (4-byte aligned): return 1 to take the whole run (the
       sink is then not called for those words), 0 to have them sent one by one. May be NULL. */
    int (*gx_block)(void *u, const uint32_t *src, uint32_t n);
} HwDmaOps;

typedef struct HwDma {
    HwDmaOps ops;
    uint8_t *io;
    uint32_t started[HWDMA_CHANNELS];
    uint32_t bytes[HWDMA_REGION_COUNT];   /* destination bytes written, per region */
    uint32_t dummies;                     /* the SDK's dummy DMA0 rewrite (src = dst = 0): a no-op */
    uint32_t unmapped;
    uint32_t repeat_ignored;
    uint32_t timed;                       /* started with a timing other than "immediate" and run at once */
    uint32_t irqs;
    uint32_t gx_words;
} HwDma;

void hwdma_init(HwDma *d, const HwDmaOps *ops, uint8_t *ioreg);
/* A CNT write happened on channel n (the SDK's MIi_DmaSetParams*): when the enable bit is set, copy synchronously, clear the enable
   bit, and set IF bit 8 + n if the IRQ-on-completion bit is set. Returns the number of units copied (0 when not enabled / dummy). */
uint32_t hwdma_start(HwDma *d, unsigned n);

/* ---- divider and square-root unit (cp_context.c's register images) ------------------------------------------------------- */
#define HWR_DIVCNT      0x280u
#define HWR_DIV_NUMER   0x290u
#define HWR_DIV_DENOM   0x298u
#define HWR_DIV_RESULT  0x2a0u
#define HWR_DIVREM      0x2a8u
#define HWR_SQRTCNT     0x2b0u
#define HWR_SQRT_RESULT 0x2b4u
#define HWR_SQRT_PARAM  0x2b8u
#define HWCP_BUSY       0x8000u
#define HWCP_DIV0       0x4000u

/* The arithmetic alone: mode 0 = 32/32, 1 or 3 = 64/32, 2 = 64/64 (GBATEK). Operands are the register contents (already 64-bit). */
void hwcp_divide(unsigned mode, int64_t numer, int64_t denom, int64_t *quot, int64_t *rem, int *div0);
uint32_t hwcp_isqrt(uint64_t x);
/* The register-level operations: read the inputs from the register block, write results, clear BUSY, set/clear DIV0. */
void hwcp_div_run(uint8_t *ioreg);
void hwcp_sqrt_run(uint8_t *ioreg);

/* ---- PXI / IPC FIFO loopback (the "ARM7" is a table of per-tag responders) ------------------------------------------------- */
#define HWPXI_TAGS        32u
#define HWPXI_FIFO_DEPTH  16u
#define HWPXI_TAG_PM      8u    /* the SDK's PXIFifoTag values */
#define HWPXI_TAG_FS      11u
#define HWPXI_TAG_OS      12u
#define HWPXI_TAG_SOUND   7u
#define HWPXI_TAG_RTC     5u
#define HWPXI_TAG_TOUCH   6u
#define HWPXI_TAG_MIC     9u
#define HWPXI_TAG_WM      10u
#define HWR_PXI_SYNC      0x180u   /* IPCSYNC (u16) */
#define HWR_PXI_CNT       0x184u   /* IPCFIFOCNT (u16) */
#define HWIRQ_FIFO_RECV   18u
#define HWPXI_CNT_SEND_EMPTY 0x0001u
#define HWPXI_CNT_SEND_FULL  0x0002u
#define HWPXI_CNT_SEND_CLEAR 0x0008u
#define HWPXI_CNT_RECV_EMPTY 0x0100u
#define HWPXI_CNT_RECV_FULL  0x0200u
#define HWPXI_CNT_ERR        0x4000u

struct HwPxi;
typedef void (*HwPxiResponder)(struct HwPxi *p, void *user, uint32_t word, unsigned err);

typedef struct HwPxi {
    uint8_t *io;
    HwPxiResponder resp[HWPXI_TAGS];
    void *resp_user[HWPXI_TAGS];
    uint32_t handle_bits;               /* what the ARM7 publishes in OSSystemWork.pxiHandleChecker[PXI_PROC_ARM7] */
    uint32_t recv[HWPXI_FIFO_DEPTH];
    uint32_t head, count;
    uint32_t err_state;                 /* 0 none, 1 latched (shown to the SDK next sync), 2 shown */
    uint32_t sent[HWPXI_TAGS];          /* words the ARM9 sent, per tag */
    uint32_t unanswered[HWPXI_TAGS];    /* sent to a tag with no responder */
    uint32_t replies;
    uint32_t recv_overflow;
} HwPxi;

void hwpxi_init(HwPxi *p, uint8_t *ioreg);
/* Register the ARM7's handler for a tag (it also sets that tag's handle bit). */
void hwpxi_set_responder(HwPxi *p, unsigned tag, HwPxiResponder fn, void *user);
/* Recompute the IPCFIFOCNT status bits from the queues; the write-1-to-clear ERR bit and the write-only SEND_CLEAR read back 0. */
void hwpxi_cnt_sync(HwPxi *p);
/* The ARM9 writes one word to the send FIFO: the tag's responder runs at once. */
void hwpxi_send(HwPxi *p, uint32_t word);
/* The ARM9 reads one word from the receive FIFO (0 and ERR when empty). */
uint32_t hwpxi_recv(HwPxi *p);
/* Responder side: queue a word for the ARM9 and raise the receive-FIFO-not-empty IRQ (IF bit 18). Returns 0, or -1 on overflow. */
int hwpxi_reply(HwPxi *p, unsigned tag, uint32_t data, unsigned err);

/* ---- the ARM7 sound driver's mailbox and cadence (bet 1.5, hw_snd.c) ----------------------------------- */
#define HWSND_PERIOD_TICKS 0xAA8u                                   /* SND_PROC_INTERVAL (nitro/snd/common/main.h:11): the sequencer step, in DS OS ticks = 5.2093 ms */
#define HWSND_PERIOD_SYS   (HWSND_PERIOD_TICKS * HWT_SYS_PER_TICK)  /* ... in 3DS system ticks: 1 396 736 */
#define HWSND_CATCHUP_MAX  8u                                       /* at most this many periodic passes per slice; a longer backlog is dropped and counted (overruns) */
#define HWSND_MAIL_DEPTH   32u                                      /* words the ARM9 may have queued for the ARM7 between two slices: <= 8 command-list pointers
                                                                       (the SDK's command-list limit) + 1 wake-up (wake words coalesce); 32 is slack, overflow is fatal */
#define HWSND_REPLY_DEPTH  16u                                      /* ARM7 -> ARM9 words deferred when the ARM9 receive FIFO is full (alarm notifications); overflow is fatal */
#define HWSND_ALARMS       16u                                      /* the ARM7's private alarm list: SND_ALARM_NUM (8) + the interval alarm + slack */
#define HWSND_SPU_OFF      0x400u                                   /* the SPU register window of the I/O block: SOUNDxCNT .. SNDCAP1LEN */
#define HWSND_SPU_WORDS    72u
#define HWSND_CHANNELS     16u                                      /* SOUND0CNT .. SOUND15CNT, 0x10 bytes each from 0x400 */                                      /* 0x400 .. 0x51f */

/* The ARM7 driver behind the mailbox. All three run the ARM7 sources (prefixed arm7_*, source/hw/hw_snd7.c); the host test fakes them. */
typedef struct HwSndOps {
    void (*enter)(void *u);                                      /* optional: the slice begins / ends (hw_snd7.c: HWOS_Arm7Enter / Leave, so no ARM9 IRQ runs inside it) */
    void (*leave)(void *u);
    void (*boot)(void *u);                                       /* SND_CommandInit + the part of SndThread before its loop: once per (re)start */
    void (*recv)(void *u, uint32_t data, unsigned err);          /* the ARM7's PXI receive callback for one FIFO word */
    int (*run)(void *u, int periodic);                           /* one pass of SndThread's loop body; 0 when there was nothing to do */
    void *user;
} HwSndOps;

typedef struct HwSndAlarm {
    const void *key;                                             /* the OSAlarm the ARM7 code passed (NULL = free slot) */
    uint64_t due, period;                                        /* OS ticks of the slice's virtual time; period 0 = one shot */
    void (*fn)(void *);
    void *arg;
} HwSndAlarm;

typedef struct HwSnd {
    HwSndOps ops;
    uint8_t *io;
    struct HwPxi *pxi;
    uint32_t mail[HWSND_MAIL_DEPTH];
    uint32_t mail_head, mail_count;
    uint32_t reply_ring[HWSND_REPLY_DEPTH];                      /* ARM7 -> ARM9 words the ARM9 FIFO had no room for; drained at the start of the next slice */
    uint32_t reply_head, reply_count;
    HwSndAlarm alarm[HWSND_ALARMS];                              /* the ARM7's own alarms, driven by the slice's virtual time (not the ARM9 OS alarm queue) */
    uint64_t now_sys;                                            /* virtual sys tick of the running slice (OS_GetTick of the ARM7 code) */
    uint8_t booted, in_pump;
    uint64_t boot_sys;                                           /* virtual sys tick of the boot slice: with no overrun, periodic_steps == (last_sys - boot_sys) / HWSND_PERIOD_SYS */
    uint64_t last_sys;                                           /* the cadence anchor: virtual sys tick of the last accounted period boundary */
    uint32_t shadow[HWSND_SPU_WORDS];                            /* the SPU registers as the last slice left them */
    /* counters */
    uint32_t cmd_words, wake_words, replies, reply_overflow;     /* ARM9 -> ARM7 command-list words and wake-ups queued; ARM7 -> ARM9 words sent; reply words LOST (ring full: fatal) */
    uint32_t wake_coalesced, reply_deferred;                     /* wake words merged into one already queued; reply words that waited in the ring */
    uint32_t alarms_set, alarms_fired, alarm_backlog_dropped;
    uint32_t mail_forced;                                        /* the ring was full: drained at once */
    uint32_t slices, steps, periodic_steps, wake_steps, catchup_steps, overruns, reentry, rebases;
    /* the channel busy-bit model (hw_snd.c, bet 1.6b): SOUNDxCNT bit 31 / SNDCAPxCNT bit 7 of a one-shot sound clear when its length has played */
    uint64_t chan_end[HWSND_CHANNELS], cap_end[2];               /* virtual sys tick the key-on will have finished at; 0 = not running a bounded sound */
    uint32_t chan_snap[HWSND_CHANNELS][4];                       /* CNT, SAD, TMR|PNT, LEN at the key-on: a change while busy re-keys the channel */
    uint32_t cap_snap[2][2];
    uint8_t chan_live[HWSND_CHANNELS], cap_live[2];
    uint32_t chan_keyons, chan_expired, chan_unbounded, cap_keyons, cap_expired;
    uint32_t spu_words_changed, spu_slices_changed;              /* diff of the SPU window before/after each slice (a rewrite of the same value is not seen) */
} HwSnd;

void hwsnd_init(HwSnd *s, const HwSndOps *ops, uint8_t *ioreg, struct HwPxi *pxi);   /* counters to zero; the driver boots at the next slice */
void hwsnd_send(HwSnd *s, uint32_t data, unsigned err);          /* the ARM9 wrote one word to the SOUND tag */
int hwsnd_reply(HwSnd *s, uint32_t data);                        /* the ARM7 sends one word to the ARM9 (alarm fired): 0 = sent or deferred to the ring; never -1 for want of room (the DS
                                                                    ARM7 could spin until the ARM9 drained; here it is the same CPU), -1 only with no PXI attached */
/* The ARM7's private alarms (key = its OSAlarm). Ticks are DS OS ticks of the slice's virtual time. */
uint64_t hwsnd_now_tick(const HwSnd *s);
void hwsnd_alarm_set(HwSnd *s, const void *key, uint64_t due_tick, uint64_t period_tick, void (*fn)(void *), void *arg);
void hwsnd_alarm_cancel(HwSnd *s, const void *key);
/* The busy bits (pure, host-tested). Exposed for the test: ticks until a bounded key-on ends (0 = never: loop, PSG, noise, or a zero-length sound). */
uint64_t hwsnd_chan_ticks(uint32_t cnt, uint32_t tmr, uint32_t pnt, uint32_t len);
uint64_t hwsnd_cap_ticks(uint32_t cnt, uint32_t len);
void hwsnd_pump(HwSnd *s, uint64_t now_sys);                     /* the slice: boot, deliver mail, wake-up passes, then the periodic passes due by `now_sys` */

/* ---- the RTC chip (hw_rtc.c): the SDK's RTC command protocol over the host clock ------------------------------------------- */
#define HWRTC_CMD_RESET       0x00u   /* the RTC_PXI_COMMAND_* numbers (rtc/common/fifo.h) */
#define HWRTC_CMD_SET_FORMAT  0x01u
#define HWRTC_CMD_READ_DT     0x10u
#define HWRTC_CMD_READ_DATE   0x11u
#define HWRTC_CMD_READ_TIME   0x12u
#define HWRTC_CMD_READ_PULSE  0x13u
#define HWRTC_CMD_READ_ALARM1 0x14u
#define HWRTC_CMD_READ_ALARM2 0x15u
#define HWRTC_CMD_READ_STAT1  0x16u
#define HWRTC_CMD_READ_STAT2  0x17u
#define HWRTC_CMD_READ_ADJUST 0x18u
#define HWRTC_CMD_READ_FREE   0x19u
#define HWRTC_CMD_WRITE_DT    0x20u
#define HWRTC_CMD_WRITE_DATE  0x21u
#define HWRTC_CMD_WRITE_TIME  0x22u
#define HWRTC_CMD_WRITE_PULSE 0x23u
#define HWRTC_CMD_WRITE_ALARM1 0x24u
#define HWRTC_CMD_WRITE_ALARM2 0x25u
#define HWRTC_CMD_WRITE_STAT1 0x26u
#define HWRTC_CMD_WRITE_STAT2 0x27u
#define HWRTC_CMD_WRITE_ADJUST 0x28u
#define HWRTC_CMD_WRITE_FREE  0x29u
#define HWRTC_RESULT_SUCCESS  0u      /* RTC_PXI_RESULT_* */
#define HWRTC_RESULT_INVALID  1u
#define HWRTC_BUF_BYTES       8u      /* RTCRawData: the 8 bytes of OSSystemWork.real_time_clock */

typedef struct HwRtc {
    int64_t offset;                 /* seconds added to the host clock (config.ini rtc_offset, moved by a "set clock" write) */
    uint16_t status1, status2;      /* the chip's status registers (24-hour format is set at power-on) */
    uint32_t alarm[2];              /* raw alarm words (RTCRawAlarm) */
    uint8_t pulse, adjust, freereg;
    uint32_t commands, reads, writes, invalid;
} HwRtc;

void hwrtc_init(HwRtc *r, int64_t offset);
/* Days since 2000-01-01 <-> civil date, and the raw BCD words of RTCRawDate / RTCRawTime for `secs` seconds since 2000-01-01 00:00:00 (local). */
int64_t hwrtc_days_from_civil(int64_t y, unsigned m, unsigned d);
void hwrtc_civil_from_days(int64_t days, int64_t *y, unsigned *m, unsigned *d);
void hwrtc_raw_from_secs(int64_t secs, uint32_t *date_word, uint32_t *time_word);
/* bet 2.1e (Rule 8, `rtc_fixed`): the fixed clock. `epoch` = unix seconds (UTC); the result = seconds since 2000-01-01 00:00:00 after `vblanks` DS vblanks at
   60 per second (integer division: the clock ticks once per 60 vblanks, it never reads the host clock). */
#define HWRTC_UNIX_2000 946684800LL
int64_t hwrtc_fixed_secs(int64_t epoch, uint64_t vblanks);
typedef struct HwRtcFields {
    int64_t year;          /* full year, 2000.. */
    unsigned mon, day;     /* 1..12, 1..31 */
    unsigned hour, min, sec;
    unsigned wday;         /* 0 = Sunday */
} HwRtcFields;
void hwrtc_fields_from_secs(int64_t secs, HwRtcFields *f);   /* secs since 2000-01-01 00:00:00 -> calendar fields (what the game reads back from the chip) */
int hwrtc_secs_from_raw(uint32_t date_word, uint32_t time_word, int64_t *secs);   /* 0 on success, -1 for an invalid BCD / out-of-range field */
/* One RTC PXI command. `buf` is the 8-byte RTCRawData image (system work): reads fill it, writes consume it. `now` = host seconds since
   2000-01-01 (local time). Returns the HWRTC_RESULT_*; the caller answers with 0x8000 | cmd << 8 | result. */
unsigned hwrtc_command(HwRtc *r, unsigned cmd, uint8_t *buf, int64_t now);

/* ---- the 3DS controls as DS input (hw_input.c) ------------------------------------------------------------------------------- */
#define HWIN_A 0x001u   /* DS KEYINPUT bit layout (+ X, Y at 10, 11); a held key is a 1 here and active-low in the register */
#define HWIN_B 0x002u
#define HWIN_SELECT 0x004u
#define HWIN_START 0x008u
#define HWIN_RIGHT 0x010u
#define HWIN_LEFT 0x020u
#define HWIN_UP 0x040u
#define HWIN_DOWN 0x080u
#define HWIN_R 0x100u
#define HWIN_L 0x200u
#define HWIN_X 0x400u
#define HWIN_Y 0x800u
#define HWIN_ALL 0xfffu
#define HWIN_SCRIPT_MAX 256u
#define HWIN_TOUCH_W 256u   /* the DS touch screen */
#define HWIN_TOUCH_H 192u

typedef struct HwInState {
    uint16_t keys;                  /* held, HWIN_* */
    uint8_t touch;                  /* 1 = pen down */
    uint16_t x, y;                  /* DS touch-screen pixels, valid when touch */
} HwInState;

typedef struct HwInScript {
    uint32_t n;
    uint32_t frame[HWIN_SCRIPT_MAX];
    HwInState st[HWIN_SCRIPT_MAX];
} HwInScript;

uint16_t hwin_keys_from_ctr(uint32_t ctr_held);   /* libctru KEY_* bits -> HWIN_* */
/* A 3DS bottom-screen touch (320x240) -> DS touch screen pixels: the 256x192 area centred on it (offset 32, 24); outside it the point is
   clamped to the nearest edge pixel. */
void hwin_touch_from_ctr(int cx, int cy, uint16_t *x, uint16_t *y);
/* Parse the scripted-input text (one line per change: "<frame> <tokens>", see docs). Returns the number of lines, or -1 on a syntax error
   (line number in *bad_line). Lines must be in ascending frame order. */
int hwin_script_parse(HwInScript *s, const char *text, uint32_t len, uint32_t *bad_line);
/* The state in force at `frame` (the last line with line.frame <= frame; all-released before the first line). */
HwInState hwin_script_at(const HwInScript *s, uint32_t frame);
/* Release P4b: input.txt may start with the first-launch setup screen's own script (its own step counter, hw_setup.c), ended by a line that is exactly `#game` (a comment, so the old
   parser skips it; a trailing CR is allowed). Returns the byte offset of the first byte after that marker line, or 0 when there is no marker (then the whole file is the game's). */
uint32_t hwin_script_split(const char *text, uint32_t len);
/* Registers: KEYINPUT low 10 bits (active low) and the XY/pen word the SDK reads (X bit 10, Y bit 11, DEBUG bit 13 released = 1; fold bit 15 = 0). */
uint16_t hwin_keyinput(uint16_t keys);
uint16_t hwin_xybuf(uint16_t keys);
/* The 12-bit panel coordinate of a DS pixel: pixel centre (x * 16 + 8), the inverse of the calibration the NVRAM image carries. */
uint16_t hwin_raw_from_px(uint16_t px);
/* The calibration points written to NVRAM (ncd.tp): two display points and their raw readings. */
typedef struct HwTpCal {
    uint16_t raw_x1, raw_y1, raw_x2, raw_y2;
    uint8_t dx1, dy1, dx2, dy2;
} HwTpCal;
HwTpCal hwin_tp_cal(void);

/* The ARM7 responders: PM (the SPI command protocol, a 5-register PMIC), FS/CARD (INIT + ACK, no backup device), SND (the mailbox of
   hw_snd.c when ops.snd is set, else swallow), and swallow-only placeholders for OS, RTC, touch panel and mic (owner: bet 1.6). */
typedef struct HwArm7Ops {
    void *(*xlat)(void *u, uint32_t addr);   /* a DS address the ARM9 passed (the CARD command block) to host memory */
    /* a backup request (CARDRequest IDENTIFY..ERASE_SUBSECTOR) on the command block `cmd` (the result is its first word): the CARD_RESULT_*.
       NULL = every backup request answers UNSUPPORTED. */
    int32_t (*card_req)(void *u, unsigned req, void *cmd);
    void *user;
    HwSnd *snd;                              /* the ARM7 sound driver's mailbox (bet 1.5); NULL = the SOUND tag counts and drops (snd_swallowed) */
    int64_t (*rtc_secs)(void *u);            /* the 3DS clock, seconds since 2000-01-01 local; NULL = the RTC tag counts and drops (other_swallowed) */
    int64_t rtc_offset;                      /* config.ini rtc_offset, the chip's power-on offset */
    /* the 8 bytes of OSSystemWork.real_time_clock (which = 0) / the 4 bytes of touch_panel (which = 1); NULL = not available */
    uint8_t *(*syswork)(void *u, unsigned which);
    uint32_t (*wm_req)(void *u, uint32_t cmd_addr);   /* the WM tag: a request buffer address in, the callback record address out (0 = none); NULL = swallowed */
} HwArm7Ops;

typedef struct HwArm7 {
    HwArm7Ops ops;
    uint16_t pm_cmd[16];        /* the multi-word PM command being assembled (index = bits 16-19) */
    uint8_t pmic[5];            /* PMIC registers 0-4 */
    uint32_t card_state;        /* 0 idle, 1 INIT seen (the next word is the command block address) */
    uint32_t card_cmd;
    uint32_t touch_cmd;         /* the command of the SPI touch request being assembled */
    uint32_t pm_commands, card_requests, snd_swallowed, other_swallowed, touch_commands, wm_requests, wm_replies;
    HwRtc rtc;
    HwInState pen;              /* the touch panel as the 3DS sees it, in DS pixels */
    uint8_t auto_on;            /* TP auto-sampling is running */
    uint8_t auto_freq;          /* samples per frame (the AUTO_ON frequency) */
    uint32_t touch_samples, auto_pushes, rtc_commands;
} HwArm7;

/* A new input frame: latch the pen state; when TP auto-sampling is on, write the panel into system work and push `auto_freq` AUTO_SAMPLING words
   (one per sample, as the ARM7 does each sampling interval). */
void hwarm7_input_frame(HwArm7 *a, HwPxi *p, const HwInState *st);

void hwarm7_init(HwArm7 *a, HwPxi *p, const HwArm7Ops *ops);   /* registers every responder on p */

#ifdef __cplusplus
}
#endif

#endif
