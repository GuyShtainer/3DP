/* hw_devs.h -- public face of source/hw/hw_devs.c (the device units behind the HWDEV_* hooks). SDK-world header. */
#ifndef HW_DEVS_H
#define HW_DEVS_H

#include "hw_card.h"
#include "hw_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Gpu2dFrame;
struct Gpu3d;

/* Idempotent; every hook calls it. Registers the loopback ARM7's responders and the DMA unit. */
void hw_devs_init(void);
/* Read-only views (counters for the hwlog). */
const HwDma *hw_devs_dma(void);
const HwPxi *hw_devs_pxi(void);
const HwCard *hw_devs_card(void);
/* A soft reset: the loopback ARM7, DMA and the card transfer come up fresh; the ROM block cache and the flash image survive. */
void hw_devs_reset(void);
void hw_devs_input_frame(const HwInState *st);   /* per frame: KEYINPUT + XY registers and the touch pen */
void hw_devs_ds_vblank(void);   /* bet 2.4b: the DS vertical blank was raised (hw_pump.c via hw_os.c): latch the geometry engine's swapped list */
const struct Gpu3d *hw_devs_gpu3d(void);   /* the geometry engine (NULL until the first GX word or register read) */
uint32_t hw_devs_snapshot_point(void);   /* bet 2.2c, B3: the game reached its snapshot point (main.c, after the vblank callback): seal the pending gxtrace frame's state. Returns the DS vblank count. Trace only, no effect on the game. */
const struct Gpu2dFrame *hw_devs_gpu2d(void);   /* bet 2.3a: the latest per-line 2D record (NULL before the first snapshot point) */
void hw_devs_wide_cfg(uint32_t wide, uint32_t stereo, uint32_t slider_force);   /* bet 2.8: config.ini `wide` / `stereo` (stereo needs wide) / `stereo_slider` (stored + 1, 0 = read the hardware) */
uint32_t hw_devs_wide_on(void);
uint32_t hw_devs_stereo_on(void);
void hw_devs_rendersync(uint32_t on);   /* bet 2.3c: config.ini `rendersync`: wait for the renderer instead of dropping the frame */
void hw_devs_polydump(uint32_t from, uint32_t count);   /* bet 2.5b: config.ini `polydump`: write the PLY1 list and the TEX1 texture memory of the snapshot points of DS vblanks from .. from + count - 1 to sdmc:/3ds/3DP/poly/ (count 0 = off) */
void hw_devs_fbdump(uint32_t from, uint32_t count);   /* bet 2.3c: config.ini `fbdump`: write the composited frames of DS vblanks from .. from + count - 1 to sdmc:/3ds/3DP/fb/<vb>.bin (count 0 = off) */
void hw_devs_save_stop(void);     /* bet 2.6b: the exit path, before hw_devs_render_stop: the save write-back in flight finishes and a pending one is written (debounce waived) */
void hw_devs_render_stop(void);   /* bet 2.3c: wait for the frame in flight and end the render thread (the exit path, before hw_devs_report) */
void hw_devs_census_enable(unsigned on);   /* bet 2.1c2: register (on) or leave out (off) the feature census observers; call once at init */
void hw_devs_hw27_cfg(uint32_t render_core, uint32_t hwcheck, uint32_t clock_frame);   /* bet 2.7a: the config values the run header reports (hw_sys.c) */
void hw_devs_hw27_bench(const char *tag);   /* bet 2.7a: the bench entry's tag for the run header (hw_bench) */
void hw_devs_header_start(void);            /* bet 2.7a: the run header's boot-time facts (model, launch, clock, linear memory), once, after osSetSpeedupEnable */
void hw_devs_note_watchdog(void);           /* bet 2.7a: the watchdog thread reports the core it runs on */
void hw_devs_field_landed(void);            /* bet 2.7a: the avatar landed on a field map: the fps window opens (port_main.c trace_sink) */
void hw_devs_report(void);   /* one hwlog line: ARM9->ARM7 words per PXI tag, and what the placeholder tags (OS, RTC, MIC) swallowed */
/* The first 0x200 bytes of the ROM file (the card header). 0 on success, -1 when the file is missing or short (logged). */
int hw_devs_rom_header(uint8_t *dst512);
/* The backup image: loaded from the .sav (0xFF-filled past it or when absent). Read-only view for tests and the hwlog. */
const uint8_t *hw_devs_flash(uint32_t *size);
const uint8_t *hw_devs_save_image(size_t *len);   /* bet 2.6a: loads platinum.sav into the flash image early (NULL = no file); hw_sys_init reads the console identity from it */
/* Replace the ROM reader (host tests): a NULL ops restores the sdmc file. */
void hw_devs_set_rom_ops(const HwCardOps *ops);

#ifdef __cplusplus
}
#endif

#endif
