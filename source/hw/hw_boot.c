/* hw_boot.c -- soft reset and first entry over hw_ovl (the pristine images) and hw_ctx (host-thread contexts). Pure C. */
#include <assert.h>

#include "hw_boot.h"

static void enter(void *arg)
{
    HwBoot *b = (HwBoot *)arg;

    assert(b != NULL && b->entry != NULL);
    b->boots++;
    b->entry();
}

void hwboot_init(HwBoot *b, HwOvl *ovl, HwCtx *ctx, const HwBootOps *ops, void (*entry)(void), volatile uint32_t *param, uint32_t stack_bytes)
{
    assert(b != NULL && ovl != NULL && ctx != NULL && ops != NULL);
    assert(entry != NULL && param != NULL);
    b->ovl = ovl;
    b->ctx = ctx;
    b->ops = *ops;
    b->entry = entry;
    b->param = param;
    b->stack_bytes = stack_bytes;
    b->boots = 0u;
}

int hwboot_start(HwBoot *b)
{
    assert(b != NULL && b->ctx != NULL);
    assert(b->entry != NULL);
    return hwctx_restart(b->ctx, enter, b, b->stack_bytes);
}

int hwboot_reset(HwBoot *b, uint32_t parameter)
{
    assert(b != NULL && b->ovl != NULL);
    assert(b->param != NULL);
    hwovl_reset_main(b->ovl);              /* the game and SDK objects' .data/.bss (the hardware layer is not in these ranges) */
    hwovl_reset_overlays(b->ovl);
    if (b->ops.reseed != NULL) {
        b->ops.reseed(b->ops.user);
    }
    *b->param = parameter;                 /* OS_ResetSystem(parameter): the next boot reads it back with OS_GetResetParameter */
    return hwctx_restart(b->ctx, enter, b, b->stack_bytes);
}
