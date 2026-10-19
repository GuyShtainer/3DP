/* hw_dma.c -- DS DMA channels 0-3 as synchronous copies at the instant the SDK writes CNT with the enable bit (the SDK's
 * MIi_DmaSetParams* call hwdma_start; the registers themselves are plain memory). Pure C: the address translation is a callback, so the
 * host test runs it over fake DS addresses. Design: the private design notes ("DMA").
 *  - width 16 or 32 bits, source and destination increment / decrement / fixed, count 1..0x200000 units (0 means 0x200000).
 *  - the enable bit is cleared on completion, so MI_WaitDma / MIi_Wait_AfterDMA pass; IF bit 8 + n is set when the IRQ bit is on, and the
 *    SDK's own DMA IRQ handler fires the registered callback.
 *  - routed by destination: main RAM, VRAM, OAM and palette are copies (counted per region); the geometry FIFO goes word by word to a
 *    placeholder sink that bet 2.1 replaces; an unmapped range is counted and skipped.
 *  - timing other than "immediate" (vblank, hblank, display, card, GX FIFO) runs at once too: there is no scanline machinery before
 *    bet 2.1 / 1.4; `timed` counts them. Repeat is ignored (one shot; counted). */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

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

void hwdma_init(HwDma *d, const HwDmaOps *ops, uint8_t *ioreg)
{
    assert(d != NULL && ops != NULL && ioreg != NULL);
    assert(ops->xlat != NULL);
    memset(d, 0, sizeof *d);
    d->ops = *ops;
    d->io = ioreg;
}

/* signed step of one unit for an address-control field */
static int32_t step_of(uint32_t mode, uint32_t width)
{
    if (mode == 1u) {
        return -(int32_t)width;
    }
    if (mode == 2u) {
        return 0;
    }
    return (int32_t)width;   /* 0 inc, 3 inc (+ reload, which only matters when repeating) */
}

/* the lowest DS address a transfer touches, and the bytes it spans, so one xlat call covers the whole range */
static void span_of(uint32_t addr, int32_t step, uint32_t units, uint32_t width, uint32_t *lo, uint32_t *len)
{
    if (step == 0) {
        *lo = addr;
        *len = width;
    } else if (step > 0) {
        *lo = addr;
        *len = units * width;
    } else {
        *lo = addr - (units - 1u) * width;
        *len = units * width;
    }
}

static void gx_burst(HwDma *d, const uint8_t *src, int32_t sstep, uint32_t units, uint32_t width)
{
    uint32_t i;

    if (d->ops.gx_block != NULL && width == 4u && sstep == 4 && units != 0u && ((uintptr_t)src & 3u) == 0u) {
        if (d->ops.gx_block(d->ops.user, (const uint32_t *)(const void *)src, units) != 0) {
            d->gx_words += units;
            return;
        }
    }
    for (i = 0; i < units; i++) {
        uint32_t w = 0;
        memcpy(&w, src, width);
        src += sstep;
        d->gx_words++;
        if (d->ops.gx_word) {
            d->ops.gx_word(d->ops.user, w);
        }
    }
}

uint32_t hwdma_start(HwDma *d, unsigned n)
{
    uint32_t off, sad, dad, cnt, units, width, slo, slen, dlo, dlen;
    int32_t sstep, dstep;
    HwDmaRegion sreg = HWDMA_UNMAPPED, dreg = HWDMA_UNMAPPED;
    uint8_t *src, *dst, *dbase;
    uint32_t i;

    assert(d != NULL && n < HWDMA_CHANNELS);
    if (n >= HWDMA_CHANNELS) {
        return 0;
    }
    off = HWR_DMA0SAD + 12u * n;
    sad = rd32(d->io, off);
    dad = rd32(d->io, off + 4u);
    cnt = rd32(d->io, off + 8u);
    if (!(cnt & HWDMA_CNT_ENABLE)) {
        return 0;
    }
    if (sad == 0u && dad == 0u) {   /* MIi_DUMMY_*: the SDK's stall-the-CPU rewrite of DMA0, a one-halfword no-op */
        d->dummies++;
        wr32(d->io, off + 8u, cnt & ~HWDMA_CNT_ENABLE);
        return 0;
    }
    width = (cnt & HWDMA_CNT_32BIT) ? 4u : 2u;
    units = cnt & HWDMA_CNT_COUNT;
    if (units == 0u) {
        units = 0x200000u;
    }
    sad &= ~(width - 1u);   /* the DMA unit ignores the low address bits */
    dad &= ~(width - 1u);
    sstep = step_of((cnt >> HWDMA_CNT_SAR_SHIFT) & 3u, width);
    dstep = step_of((cnt >> HWDMA_CNT_DAR_SHIFT) & 3u, width);
    d->started[n]++;
    if (cnt & HWDMA_CNT_REPEAT) {
        d->repeat_ignored++;
    }
    if (((cnt >> 27) & 7u) != 0u) {
        d->timed++;
    }

    span_of(sad, sstep, units, width, &slo, &slen);
    span_of(dad, dstep, units, width, &dlo, &dlen);
    src = (uint8_t *)d->ops.xlat(d->ops.user, slo, slen, &sreg);
    dst = (uint8_t *)d->ops.xlat(d->ops.user, dlo, dlen, &dreg);
    if (src != NULL) {
        src += (sad - slo);
    }
    dbase = dst;
    if (dst != NULL) {
        dst += (dad - dlo);
    }
    if (src == NULL) {
        d->unmapped++;   /* an unreadable source reads as zero below */
    }
    if (dreg == HWDMA_GXFIFO) {
        if (src != NULL) {
            gx_burst(d, src, sstep, units, width);
        } else {
            for (i = 0; i < units && d->ops.gx_word != NULL; i++) {
                d->ops.gx_word(d->ops.user, 0u);
            }
            d->gx_words += units;
        }
        d->bytes[HWDMA_GXFIFO] += units * width;
    } else if (dst == NULL) {
        d->unmapped++;
    } else {
        for (i = 0; i < units; i++) {
            if (src != NULL) {
                memmove(dst, src, width);   /* unit by unit, in order, so an overlapping range behaves as on the bus */
                src += sstep;
            } else {
                memset(dst, 0, width);
            }
            dst += dstep;
        }
        d->bytes[dreg < HWDMA_REGION_COUNT ? dreg : HWDMA_UNMAPPED] += units * width;
        if (d->ops.wrote != NULL) {
            d->ops.wrote(d->ops.user, dreg, dbase, dlen);
        }
    }
    wr32(d->io, off + 8u, cnt & ~HWDMA_CNT_ENABLE);
    if (cnt & HWDMA_CNT_IRQ) {
        wr32(d->io, HWR_IF, rd32(d->io, HWR_IF) | (1u << (HWIRQ_DMA0 + n)));
        d->irqs++;
    }
    return units;
}
