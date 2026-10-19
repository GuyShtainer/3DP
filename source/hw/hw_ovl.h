/* hw_ovl.h -- the 122 overlays and the main binary as RAM ranges that are restored. Pure C: no SDK, no
 * libctru. On the 3DS everything is linked once; the DS "loads" an overlay by overwriting its RAM window with a fresh image, so the port
 * restores that overlay's .data from a pristine copy and zeroes its .bss. The ranges come from the linker (tools/overlay_map.py writes the
 * script and the symbol table); the host test supplies fake ones. */
#ifndef HW_OVL_H
#define HW_OVL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HW_OVL_COUNT 122u

/* The linker's view of one block: [start, end) of .data, .bss and the .sinit table (one function pointer per object that has one). */
typedef struct HwOvlSyms {
    char *data_start, *data_end;
    char *bss_start, *bss_end;
    void (**sinit_start)(void);
    void (**sinit_end)(void);
    const char *name;
} HwOvlSyms;

typedef struct HwOvlSection {
    uint8_t *data;
    uint32_t data_size;
    uint8_t *bss;
    uint32_t bss_size;
    const uint8_t *pristine;          /* the copy of .data taken at init; NULL if data_size == 0 */
    void (**sinit)(void);
    uint32_t sinit_count;
    uint32_t loads, starts, unloads;
    uint8_t loaded, started;
} HwOvlSection;

typedef struct HwOvl {
    HwOvlSection *sec;                /* count entries (g_overlay_sections in the 3DS build) */
    uint32_t count;
    HwOvlSection main;                /* the main binary: restored by a soft reset, never by an overlay load */
    uint32_t pristine_used;
    uint32_t pristine_size;
} HwOvl;

/* Compute every section size from the symbols, copy every .data once into `pristine` (pristine_size bytes: main first, then overlays in
   order) and return the number of pristine bytes used, or -1 if the buffer is too small or the symbols are inconsistent. Taken once, before
   any game code has run, so the copy is the link-time image. */
int32_t hwovl_init(HwOvl *o, HwOvlSection *storage, uint32_t count, const HwOvlSyms *syms, const HwOvlSyms *main_syms, uint8_t *pristine,
                   uint32_t pristine_size);

/* FS_LoadOverlayImage: .data back to the pristine image, .bss zeroed, marked loaded. Returns 0, or -1 for a bad id. */
int hwovl_load(HwOvl *o, uint32_t id);
/* FS_StartOverlay: run the overlay's static initialisers (NitroStaticInit tables) in order, mark started. Returns the number run, or -1. */
int hwovl_start(HwOvl *o, uint32_t id);
/* FS_UnloadOverlay: mark unloaded (the RAM is left as it is, as on the DS). Returns 0, or -1. */
int hwovl_unload(HwOvl *o, uint32_t id);
/* Soft reset: every overlay is unloaded and un-started, its range restored to the link-time image. */
void hwovl_reset_overlays(HwOvl *o);
/* Soft reset: the main binary's .data back to the pristine image, .bss zeroed. */
void hwovl_reset_main(HwOvl *o);

#ifdef __cplusplus
}
#endif

#endif
