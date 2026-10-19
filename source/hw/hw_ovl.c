/* hw_ovl.c -- overlay data/bss restore over linker-defined ranges. Pure C (golden rules: no goto, bounded loops, no allocation, two asserts
 * per function, checked returns). Design: the private design notes. */
#include <assert.h>
#include <string.h>

#include "hw_ovl.h"

static int fill_section(HwOvlSection *s, const HwOvlSyms *y, uint8_t *pristine, uint32_t room)
{
    ptrdiff_t dn, bn, sn;

    assert(s != NULL && y != NULL);
    assert(pristine != NULL || room == 0u);
    dn = y->data_end - y->data_start;
    bn = y->bss_end - y->bss_start;
    sn = y->sinit_end - y->sinit_start;
    if (dn < 0 || bn < 0 || sn < 0 || (uint32_t)dn > room) {
        return -1;
    }
    memset(s, 0, sizeof *s);
    s->data = (uint8_t *)y->data_start;
    s->data_size = (uint32_t)dn;
    s->bss = (uint8_t *)y->bss_start;
    s->bss_size = (uint32_t)bn;
    s->sinit = y->sinit_start;
    s->sinit_count = (uint32_t)sn;
    if (dn > 0) {
        memcpy(pristine, s->data, (size_t)dn);
        s->pristine = pristine;
    }
    return (int)dn;
}

int32_t hwovl_init(HwOvl *o, HwOvlSection *storage, uint32_t count, const HwOvlSyms *syms, const HwOvlSyms *main_syms, uint8_t *pristine,
                   uint32_t pristine_size)
{
    uint32_t i, used = 0u;
    int n;

    assert(o != NULL && storage != NULL && syms != NULL && main_syms != NULL);
    assert(pristine != NULL && count > 0u);
    memset(o, 0, sizeof *o);
    o->sec = storage;
    o->count = count;
    o->pristine_size = pristine_size;
    n = fill_section(&o->main, main_syms, pristine, pristine_size);
    if (n < 0) {
        return -1;
    }
    used += (uint32_t)n;
    for (i = 0; i < count; i++) {
        n = fill_section(&o->sec[i], &syms[i], pristine + used, pristine_size - used);
        if (n < 0) {
            return -1;
        }
        used += (uint32_t)n;
    }
    o->pristine_used = used;
    return (int32_t)used;
}

static void restore(HwOvlSection *s)
{
    assert(s != NULL);
    assert(s->data_size == 0u || s->pristine != NULL);
    if (s->data_size != 0u) {
        memcpy(s->data, s->pristine, s->data_size);
    }
    if (s->bss_size != 0u) {
        memset(s->bss, 0, s->bss_size);
    }
}

int hwovl_load(HwOvl *o, uint32_t id)
{
    assert(o != NULL && o->sec != NULL);
    if (id >= o->count) {
        return -1;
    }
    restore(&o->sec[id]);
    o->sec[id].loaded = 1u;
    o->sec[id].started = 0u;
    o->sec[id].loads++;
    return 0;
}

int hwovl_start(HwOvl *o, uint32_t id)
{
    uint32_t i, ran = 0u;
    HwOvlSection *s;

    assert(o != NULL && o->sec != NULL);
    if (id >= o->count) {
        return -1;
    }
    s = &o->sec[id];
    for (i = 0; i < s->sinit_count; i++) {
        if (s->sinit[i] != NULL) {
            s->sinit[i]();
            ran++;
        }
    }
    s->started = 1u;
    s->starts++;
    return (int)ran;
}

int hwovl_unload(HwOvl *o, uint32_t id)
{
    assert(o != NULL && o->sec != NULL);
    if (id >= o->count) {
        return -1;
    }
    o->sec[id].loaded = 0u;
    o->sec[id].started = 0u;
    o->sec[id].unloads++;
    return 0;
}

void hwovl_reset_overlays(HwOvl *o)
{
    uint32_t i;

    assert(o != NULL && o->sec != NULL);
    assert(o->count > 0u);
    for (i = 0; i < o->count; i++) {
        restore(&o->sec[i]);
        o->sec[i].loaded = 0u;
        o->sec[i].started = 0u;
    }
}

void hwovl_reset_main(HwOvl *o)
{
    assert(o != NULL);
    assert(o->main.data_size == 0u || o->main.pristine != NULL);
    restore(&o->main);
}
