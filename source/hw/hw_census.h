/* hw_census.h -- the runtime feature census. Pure C: <stdint.h> / <stddef.h> only.
 *
 * Observation only (Rule 2): the census reads what already flows through the hardware layer and never writes a register, a FIFO word, a VRAM byte or
 * a timing. Three feeds:
 *   HWCEN_GxWord(addr, word)   every word of the four GX input paths, from the observer hook of hw_gx.c: addr 0 = a word of the GX FIFO (packed command
 *                              words and their parameters, decoded with ONE packing state shared by all paths, as the hardware does), addr 0x440..0x5FF =
 *                              a store to a command port (command id = (addr - 0x400) / 4, parameters counted per store);
 *   HWCEN_VramCnt(bank, v)     every VRAMCNT_A..I byte the SDK hook saw (hw_vram.c observer): transitions inside one frame are not lost;
 *   HWCEN_FrameEnd(io, oam)    once per frame, after the game's vblank work: samples the 2D register blocks of engine A (0x04000000) and B (0x04001000),
 *                              the OAM (A at +0, B at +0x400) and the 3D register state, and turns the per-frame "seen" flags into counters.
 * Output: HWCEN_Format() = `feature=<number of frames it was seen in>` lines (every feature, zeros included), `set=<comma list of values seen>` lines
 * and a few numbers. EXACT COUNTS DRIFT between binaries (virtual time, the private design notes): compare seen / not seen and nonzero-ness.
 *
 * Every field decoded is cited where it is decoded (hw_census.c) against NitroSDK-4.2.30001/include/nitro/hw/ARM9/ioreg_{GX,G2,G3,G3X}.h and
 * nitro/gx/{g2_oam,g3,gx_bgcnt}.h. Fixed tables only: no allocation, no file I/O (hw_devs.c writes the text). */
#ifndef HW_CENSUS_H
#define HW_CENSUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWCEN_DUMP_FRAMES  300u      /* HWCEN_FrameEnd returns 1 every this many frames: write the file */
#define HWCEN_TEXT_MAX     16384u    /* the largest HWCEN_Format output the caller must provide room for (the census text is about 8 KB) */
#define HWCEN_VIEWPORTS    8u        /* distinct VIEWPORT words kept */

/* Zero every counter, set and the stream state. */
void HWCEN_Reset(void);

void HWCEN_GxWord(uint32_t addr, uint32_t word);
/* n FIFO words (addr 0) in program order, equivalent to n HWCEN_GxWord(0, w) calls but cheaper (bet 2.1c2). Port stores (addr != 0) go through HWCEN_GxWord:
   their state (pidx) is separate from the FIFO's, so the two may be interleaved in any order. */
void HWCEN_GxBlock(const uint32_t *w, uint32_t n);
void HWCEN_VramCnt(uint32_t bank, uint32_t value);
/* Returns 1 when the periodic dump is due (every HWCEN_DUMP_FRAMES frames), else 0. `io` = the 0x4000000 register block (at least 0x1070 bytes),
   `oam` = 0x800 bytes (engine A's 128 entries, then engine B's); either may be NULL (that part is skipped). */
int HWCEN_FrameEnd(const uint8_t *io, const uint8_t *oam);

/* The text for hwcensus.txt: returns the length (truncated at cap - 1, NUL-terminated). */
size_t HWCEN_Format(char *buf, size_t cap);

/* Lookups by the output name (host tests, hwlog summary). Count = frames the feature was seen in; 0 for an unknown name. Set = the bitmask of values seen. */
uint32_t HWCEN_Count(const char *name);
int HWCEN_IsFeature(const char *name);
uint32_t HWCEN_Set(const char *name);
int HWCEN_IsSet(const char *name);
uint32_t HWCEN_CmdFrames(uint32_t cmd);   /* frames a G3 command id (0x10..0x72) was seen in */
uint32_t HWCEN_CmdTotal(uint32_t cmd);    /* words-level occurrences (drifts between binaries) */
uint32_t HWCEN_Frames(void);
uint32_t HWCEN_Features(void);            /* the number of features in the table (2D x 2 engines + 3D) */

#ifdef __cplusplus
}
#endif

#endif
