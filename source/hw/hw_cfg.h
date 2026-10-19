/* hw_cfg.h -- sdmc:/3ds/3DP/config.ini: the DS owner identity the game compares against its save (design notes kept privately),
 * Pure C (no SDK, no libctru): parse and format are host-tested, the file I/O is hw_sys.c's. */
#ifndef HW_CFG_H
#define HW_CFG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWCFG_NAME_MAX   10u
#define HWCFG_ALLOWED_CHANNELS_DEFAULT 0x1041u
#define HWCFG_GXTRACE_MAX 600u   /* longest gxtrace window, frames */
#define HWCFG_POLYDUMP_MAX 64u   /* longest polydump window, DS vblanks (about 0.6 MB of texture memory is written once per distinct content, ~35 KB per list) */
#define HWCFG_FBDUMP_MAX  200u   /* longest fbdump window, frames (393 KB each: 79 MB) */
#define HWCFG_RTC_FIXED_MIN 946684800LL    /* 2000-01-01 00:00:00 UTC */
#define HWCFG_RTC_FIXED_MAX 4102444799LL   /* 2099-12-31 23:59:59 UTC */
#define HWCFG_FILE       HWCTR_APP_DIR "/config.ini"
#define HWCFG_ROM_MAX    80u   /* the longest `rom =` path (release P4b); the config line cap is 120 bytes */

typedef struct HwCfg {
    uint16_t name[HWCFG_NAME_MAX];   /* owner nickname, UCS-2, ASCII only in the ini */
    uint8_t name_len;
    uint8_t color;                   /* favourite colour 0..15 */
    uint8_t birth_month;             /* 1..12 */
    uint8_t birth_day;               /* 1..31 */
    uint8_t language;                /* 0 JP, 1 EN, 2 FR, 3 DE, 4 IT, 5 ES */
    uint8_t mac[6];
    uint16_t allowed_channels;       /* firmware allowed-channel mask, bit n-1 = channel n (default 0x1041: channels 1, 7, 13) */
    int64_t rtc_offset;              /* seconds, added to the RTC by the DS "set clock" screen */
    int64_t rtc_fixed;               /* `rtc_fixed = <unix epoch seconds>` (bet 2.1e, Rule 8): the DS RTC starts at this date/time and advances with the pump's DS vblank count (60/s), not the host clock; overrides rtc_offset. Not written by hwcfg_format. */
    int64_t id_rtc_offset;           /* bet 2.6a: the RTC offset the game READS (NVRAM option.rtcOffset) when id_rtc_set; set by the save-identity adoption. It is identity only: the clock still follows rtc_fixed or host time + rtc_offset (a save's value, e.g. 0xFF00000000, would otherwise move the date by millennia). */
    uint8_t id_rtc_set;              /* 1 once a save's identity was adopted (then id_rtc_offset, and mac, are the save's / the config section's) */
    uint8_t rtc_fixed_set;           /* 1 once `rtc_fixed` was given (default 0 = today's host clock + rtc_offset) */
    uint8_t census;                  /* `census = 0|1` (bet 2.1c2): the runtime feature census (hw_census.c); default 0 = observers never registered. Not written by hwcfg_format. */
    uint8_t clock_frame;             /* `clock = frame|wall` (bet 2.2a): 1 = frame-driven virtual time (hw_pure.h HwClock). The parser default is WALL: a missing key (the shipped .cia config) keeps today's behaviour; the emulator gates write `clock = frame`. Which one the .cia ships is decided at 2.7 from measurement. Not written by hwcfg_format. */
    uint8_t gx_seal_frame_end;       /* `gxseal = snapshot|frame-end` (bet 2.2c, B3 A/B control): frame-end = the pre-2.2c seal at the vblank hook (no snapshot point). Default snapshot. Not written by hwcfg_format. */
    uint32_t gx_trace_from, gx_trace_count;   /* `gxtrace = FROM:COUNT` (bet 2.1a): a frame window of the GX input stream; count 0 = off. Not written by hwcfg_format. */
    uint8_t render_sync;             /* `rendersync = 0|1` (bet 2.3c): 1 = the game thread waits for the previous frame before every offer (lossless; the wait is stolen from the game clock): the emulator gates, whose renderer is slower than a frame. Default 0 = drop. Not written by hwcfg_format. */
    uint32_t polydump_from, polydump_count;   /* `polydump = FROM:COUNT` (bet 2.5b, emu-poly-export): the port's PLY1 lists of the snapshot points whose DS vblank is in the window go to sdmc:/3ds/3DP/poly/. Not written by hwcfg_format. */
    uint32_t fbdump_from, fbdump_count;   /* `fbdump = FROM:COUNT` (bet 2.3c): the composited 2D frames of the DS vblank window FROM..FROM+COUNT-1 are written to sdmc:/3ds/3DP/fb/<vb>.bin (hw_screen.h); count 0 = off. Not written by hwcfg_format. */
    uint8_t render_core;             /* `render_core = 0|1|2` (bet 2.7a, default 2): the core the render thread asks for (0 = the game's own core, the A/B control of the hardware session; 1 and 2 = the application's second core / the system core). The core it GOT is logged. Not written by hwcfg_format. */
    uint8_t hwcheck;                 /* `hwcheck = 0|1|2|3` (bet 2.7a, default 0): 1 = run the on-device D24S8 layout + readback-freshness self-check before the first game frame; 2 = the same with the planted de-tile fault (must report FAIL); 3 (2.7(b)) = the planted missing depth merge (only the translucent frame must report FAIL). Not written by hwcfg_format. */
    uint8_t wide;                    /* `wide = 0|1` (bet 2.8, default 0): the 400 x 240 top screen ("reveal, not stretch"). Not written by hwcfg_format. */
    uint8_t stereo;                  /* `stereo = 0|1` (bet 2.8, default 0): per-eye 3D in the field, scaled by the 3D slider; needs wide = 1. Not written by hwcfg_format. */
    uint8_t stereo_slider;           /* `stereo_slider = 0..100` (bet 2.8): the 3D slider in percent, in place of the hardware's (the emulator gates' stereo evidence: Azahar's slider is a
                                        setting); stored + 1, 0 = the key is absent and the hardware slider is read. Not written by hwcfg_format. */
    char rom[HWCFG_ROM_MAX + 1u];    /* `rom = <path>` (release P4b): the player's own dump, pointed at (read only); ignored when rom_set is 0 */
    uint8_t rom_set;                 /* 1 once a valid `rom` line was given */
    uint8_t setup_off;               /* `setup = 0` (release P4b): the first-launch setup screen is forbidden (the emulator gates); default 0 = it may open. Not written by hwcfg_format. */
} HwCfg;

void hwcfg_defaults(HwCfg *c);
/* Apply every `key = value` line of the text on top of *c. Unknown keys and out-of-range values are skipped (and counted).
   Returns the number of keys applied; *skipped (may be NULL) gets the number of lines that were ignored. */
uint32_t hwcfg_parse(HwCfg *c, const char *text, size_t len, uint32_t *skipped);
/* The ini text for *c, with comments; returns the length (truncated at cap - 1, always NUL-terminated), 0 on bad arguments. */
size_t hwcfg_format(const HwCfg *c, char *buf, size_t cap);

/* ---- bet 2.6a: the per-save identity section ---------------------------------------------------------------------------------------------------------------
   [save <TID>-<SID>]            decimal ids of the save's trainer
   mac = aa:bb:cc:dd:ee:ff       the MAC the game finds in NVRAM
   rtc_offset = <s64>            the RTC offset the game finds in NVRAM (identity only)
   adopted_from_save = 1         1 = use this section; 0 = ignore it and keep the flat keys above ("keep the config's values")
   hwcfg_parse() applies the flat keys that come BEFORE the first `[` line and ignores everything after it (sections are read by hwcfg_save_lookup only), so the old
   flat files keep their meaning and unknown sections or keys are harmless. */
typedef struct HwCfgSave {
    uint8_t found;               /* the section exists */
    uint8_t adopted;             /* adopted_from_save (default 1 when the key is absent) */
    uint8_t mac_set, rtc_set;    /* the section carries a valid mac / rtc_offset line */
    uint8_t mac[6];
    int64_t rtc_offset;
} HwCfgSave;

/* Finds the FIRST `[save TID-SID]` section of the text (later duplicates are ignored). Returns out->found. Bad values inside the section are skipped. */
int hwcfg_save_lookup(const char *text, size_t len, uint32_t tid, uint32_t sid, HwCfgSave *out);
/* The section text to append for a save seen for the first time (ends with a newline; a leading blank line separates it); returns the length, 0 on bad arguments/short buffer. */
size_t hwcfg_format_save(uint32_t tid, uint32_t sid, const uint8_t mac[6], int64_t rtc_offset, char *buf, size_t cap);

#define HWCFG_ID_NEW      1   /* no section yet: the save's identity was adopted into *c; the caller appends hwcfg_format_save() to the file */
#define HWCFG_ID_SECTION  2   /* the section's values (edit them to override) were adopted into *c; a key the section lacks falls back to the save's value */
#define HWCFG_ID_OPTOUT   3   /* the section says adopted_from_save = 0: *c is untouched, the flat keys stay in force */
/* Decide the console identity for a save whose identity is (tid, sid, mac, rtc): sets c->mac and c->id_rtc_offset/id_rtc_set for NEW and SECTION. */
int hwcfg_adopt_identity(HwCfg *c, const char *text, size_t len, uint32_t tid, uint32_t sid, const uint8_t mac[6], int64_t rtc_offset);

#ifdef __cplusplus
}
#endif

#endif
