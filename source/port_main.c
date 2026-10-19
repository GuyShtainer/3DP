/* port_main.c -- the 3DS process entry: libctru init, the hardware layer, then NitroMain on a host thread (the main thread parks).
 * Compiled with the libctru include path and WITHOUT port-shim/include. Boot milestones go to the console and to sdmc:/3ds/3DP/boot.out. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_benchio.h"
#include "hw_ctr.h"
#include "hw_log.h"
#include "hw_pure.h"
#include "hw_setup.h"
#include "version.h"

void NitroMain(void);
void hw_devs_report(void);
void hw_devs_input_frame(const HwInState *st);
void hw_devs_save_stop(void);
void hw_devs_header_start(void);
void hw_devs_note_watchdog(void);
void hw_devs_field_landed(void);
void hw_devs_render_stop(void);
unsigned hw_sys_init(void);
const HwBenchRun *hw_sys_bench(void);
int hw_world_init(void (*entry)(void));
void HWOS_ResetSystem(void (*doReset)(void));
void __real_OS_WaitIrq(int clear, unsigned int irqFlags);

unsigned int __stacksize__ = 0x40000;   /* the host-thread hand-off frames and the SDK's stack checks want more than libctru's 32 KB */

#define OUT_FILE HWCTR_APP_DIR "/boot.out"
#define OUT_KEEP 32768u
#define OS_IE_V_BLANK_BIT 0x1u
#define INPUT_FILE HWCTR_APP_DIR "/input.txt"       /* optional scripted input (format: hw_input.c); replaces the real controls when present */
#define KEYS_FILE HWCTR_APP_DIR "/keys.txt"          /* bet 2.2c B1: the game-state key lines (change-only), one per game-loop snapshot point */
#define KEYS_KEEP 65536u
#define AUTOEXIT_FILE HWCTR_APP_DIR "/autoexit.txt"   /* optional: decimal frame count after which the gate wants a clean exit */

static char s_out[OUT_KEEP];
static uint32_t s_out_len;
static volatile uint32_t s_frames;
static uint32_t s_autoexit;
static int s_first_wait;
static uint32_t s_first_wait_frame;
static int s_exiting;
static char s_keys[KEYS_KEEP];
static uint32_t s_keys_len, s_keys_dropped;
static HwInScript s_script;
static int s_script_on;

static void out_flush(void)
{
    hwctr_mkdirs(HWCTR_APP_DIR);
    (void)hwctr_file_write(OUT_FILE, s_out, s_out_len);
}

static uint32_t s_out_dropped;

static void out_line(const char *text)
{
    uint32_t n = (uint32_t)strlen(text);
    uint32_t reserve = strncmp(text, "boot: END", 9) == 0 ? 0u : 512u;   /* the END line always fits */

    if (s_out_len + n + 2u + reserve >= OUT_KEEP) {
        s_out_dropped++;
    }
    if (s_out_len + n + 2u + reserve < OUT_KEEP) {
        memcpy(s_out + s_out_len, text, n);
        s_out_len += n;
        s_out[s_out_len++] = '\n';
    }
    hwctr_console_write(text);
    hwctr_console_write("\n");
    out_flush();
}

void port_trace(const char *msg)
{
    char line[96];

    snprintf(line, sizeof line, "boot: %s (frame %u)", msg, (unsigned)s_frames);
    out_line(line);
}

void port_trace_once(const char *msg)
{
    static const char *seen[32];
    unsigned i;

    for (i = 0; i < 32u && seen[i] != NULL; i++) {
        if (seen[i] == msg) {
            return;
        }
    }
    if (i < 32u) {
        seen[i] = msg;
    }
    port_trace(msg);
}

/* NitroMain's loop (and the SDK before it) waits with OS_WaitIrq; the first V_BLANK wait is the last boot milestone. */
void __wrap_OS_WaitIrq(int clear, unsigned int irqFlags)
{
    if (!s_first_wait && (irqFlags & OS_IE_V_BLANK_BIT) != 0u) {
        s_first_wait = 1;
        s_first_wait_frame = s_frames;
        port_trace("first OS_WaitIrq(VBLANK)");
    }
    __real_OS_WaitIrq(clear, irqFlags);
}

/* Bet 2.2c, B1 (Rule 1(b)): the game-state event key, logged at the game's snapshot point (main.c, after the vblank callback). The fields are the ones the
   harness reads from MainRAM (the private melonDS harness): application main-callback address, its exec/proc state, the vblank callback address, and,
   in the field application, the map header id and the avatar x/z. Addresses are resolved to SYMBOL NAMES on the host (nm of the .elf here, the xMAP there).
   Only a change of the key is logged (the vb field is the DS vblank count at the first frame with that key). Trace only: nothing here feeds the game. */
void port_frame_key(unsigned vblanks, int ovl, const void *appMain, int exec, int proc, const void *vblankCb, int haveField, int map, int x, int z)
{
    static int have_last, last_ovl;
    static uintptr_t last_app, last_vbl;
    static int last_exec, last_proc, last_field, last_map, last_x, last_z;
    char line[160];
    int n;

    if (have_last && last_ovl == ovl && last_app == (uintptr_t)appMain && last_vbl == (uintptr_t)vblankCb && last_exec == exec && last_proc == proc && last_field == haveField &&
        last_map == map && last_x == x && last_z == z) {
        return;
    }
    have_last = 1;
    last_ovl = ovl;
    last_app = (uintptr_t)appMain;
    last_vbl = (uintptr_t)vblankCb;
    last_exec = exec;
    last_proc = proc;
    last_field = haveField;
    last_map = map;
    last_x = x;
    last_z = z;
    n = snprintf(line, sizeof line, "key vb=%u ovl=%d app=%08x exec=%d proc=%d vbl=%08x field=%d map=%d x=%d z=%d\n", vblanks, ovl, (unsigned)(uintptr_t)appMain, exec, proc,
                 (unsigned)(uintptr_t)vblankCb, haveField, map, x, z);
    if (n > 0 && s_keys_len + (uint32_t)n < KEYS_KEEP) {
        memcpy(s_keys + s_keys_len, line, (size_t)n);
        s_keys_len += (uint32_t)n;
    } else {
        s_keys_dropped++;
    }
}

/* hwtrace sink: an ordered "ovl: ..." / "app: ..." line, stamped with the frame, into boot.out */
static void trace_sink(const char *line)
{
    char buf[128];

    snprintf(buf, sizeof buf, "%s (frame %u)", line, (unsigned)s_frames);
    out_line(buf);
    if (strncmp(line, "boot: field landed", 18) == 0) {
        hw_devs_field_landed();   /* bet 2.7a: the fps window opens at the landing */
    }
}

static void finish(const char *why)
{
    char line[96];

    if (s_exiting) {
        return;
    }
    s_exiting = 1;
    snprintf(line, sizeof line, "boot: END %s frames=%u after_first_wait=%u", why, (unsigned)s_frames,
             (unsigned)(s_first_wait ? s_frames - s_first_wait_frame : 0u));
    out_line(line);
    hw_devs_save_stop();     /* bet 2.6b: the in-game save reaches the SD before the app ends */
    hw_devs_render_stop();   /* bet 2.3c: the frame in flight is presented (and dumped) before the counters are read */
    hw_devs_report();
    if (s_keys_len != 0u) {
        (void)hwctr_file_write(KEYS_FILE, s_keys, s_keys_len);
    }
    (void)hwlog_dump();
    hwctr_console_save();
    hwbench_finish(hw_sys_bench(), why);   /* bet 2.7a: this entry's logs into bench/<tag>/ and its DONE marker */
    gfxExit();
    exit(0);
}

/* The controls for this frame: the script when input.txt was loaded, else the 3DS buttons, circle pad and bottom-screen touch as DS input. */
static HwInState frame_input(void)
{
    HwInState st;
    touchPosition tp;

    memset(&st, 0, sizeof st);
    if (s_script_on) {
        return hwin_script_at(&s_script, s_frames);
    }
    st.keys = hwin_keys_from_ctr(hidKeysHeld());
    if (hidKeysHeld() & KEY_TOUCH) {
        hidTouchRead(&tp);
        hwin_touch_from_ctr((int)tp.px, (int)tp.py, &st.x, &st.y);
        st.touch = 1;
    }
    return st;
}

static void load_script(void)
{
    static char text[8192];
    uint32_t bad = 0, off;
    int n = hwctr_file_read(INPUT_FILE, text, sizeof text);
    char line[96];

    if (n <= 0) {
        return;
    }
    off = hwin_script_split(text, (uint32_t)n);   /* release P4b: the lines before a `#game` marker are the setup screen's (hw_setup.c) */
    n = hwin_script_parse(&s_script, text + off, (uint32_t)n - off, &bad);
    if (n < 0) {
        snprintf(line, sizeof line, "boot: input.txt line %u is not valid; real controls used", (unsigned)bad);
        out_line(line);
        return;
    }
    s_script_on = 1;
    snprintf(line, sizeof line, "boot: input.txt scripted input, %d lines", n);
    out_line(line);
}

static void frame_hook(void)
{
    HwInState st;

    s_frames++;
    hidScanInput();
    hwctr_console_chord((hidKeysHeld() & (KEY_ZL | KEY_ZR)) == (KEY_ZL | KEY_ZR));   /* bet 2.3c: ZL+ZR toggles the text console on the bottom screen (the game never reads them) */
    if (!aptMainLoop()) {
        finish("HOME/exit");
    }
    if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
        finish("START+SELECT");   /* the real buttons only: a script holding both does not quit */
    }
    st = frame_input();
    hw_devs_input_frame(&st);
    if (s_autoexit != 0u && s_first_wait && s_frames - s_first_wait_frame >= s_autoexit) {
        finish("autoexit");
    }
    hwlog_flush_if_due(s_frames);
}

/* Watchdog: while the frame counter stands still (the game thread spins without a vblank), dump the hwlog every 2 s so the gate can see how far the
   stubs got. Racy against the game thread's logging; it only reads. */
static void watchdog(void *arg)
{
    uint32_t last = 0xffffffffu;

    (void)arg;
    hw_devs_note_watchdog();
    for (;;) {
        svcSleepThread(2000000000LL);
        if (s_exiting) {
            return;
        }
        if (s_frames == last) {
            (void)hwlog_dump();
            hwctr_console_save();
        }
        last = s_frames;
    }
}

int main(void)
{
    char buf[16];
    int n;

    gfxInitDefault();
    hwctr_gfx_init();
    hwctr_console_write("3DP " APP_VERSION_STR " slice 1.6a: boot\n");
    osSetSpeedupEnable(true);
    hw_devs_header_start();   /* bet 2.7a: model, launch kind, measured clock, CPU time limit rc, slider, linear memory, for the run header */
    n = hwctr_file_read(AUTOEXIT_FILE, buf, sizeof buf - 1u);
    if (n > 0) {
        buf[n] = '\0';
        s_autoexit = (uint32_t)atoi(buf);
    }
    if (hw_setup_run() == HWSETUP_QUIT) {   /* release P4b: the first-launch setup screen (no usable ROM, or SELECT held); a normal launch passes straight through */
        (void)hwlog_dump();
        hwctr_console_save();
        gfxExit();
        return 0;
    }
    (void)hw_sys_init();
    load_script();
    if (hw_world_init(NitroMain) != 0) {
        finish("hw_world_init failed");
    }
    hwtrace_set_sink(trace_sink);
    hwctr_set_frame_hook(frame_hook);
    (void)threadCreate(watchdog, NULL, 0x4000, 0x18, -2, true);
    /* The first boot goes through the reset path: restore .data, zero .bss, re-seed the hardware layer, enter NitroMain on a host thread. */
    HWOS_ResetSystem(NULL);
    finish("reset path returned");
    return 0;
}
