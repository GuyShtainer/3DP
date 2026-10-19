/* hw_ctx.c -- cooperative contexts: the SDK scheduler kept, the CPU context switch replaced by a hand-off between host threads
 * Pure C over a backend (libctru LightEvent threads on the 3DS, pthreads in the host test). */
#include <assert.h>

#include "hw_pure.h"

static void fresh_main(void *arg);

void hwctx_init(HwCtx *c, const HwCtxBackend *be, void *main_handle)
{
    int i;

    assert(c != NULL && be != NULL);
    assert(be->create && be->wake && be->park && be->exit_current && be->kill);
    c->be = *be;
    for (i = 0; i < HWCTX_MAX; i++) {
        c->rec[i].used = 0;
        c->rec[i].h = 0;
        c->rec[i].owner = 0;
    }
    c->rec[0].used = 1;
    c->rec[0].h = main_handle;
    c->cur = 0;
    c->main_h = main_handle;
    c->restarts = 0;
}

int hwctx_save(HwCtx *c, void *owner, uint32_t *w11, uint32_t *w12)
{
    assert(c != NULL && owner != NULL);
    assert(c->cur >= 0 && c->cur < HWCTX_MAX && c->rec[c->cur].used);
    c->rec[c->cur].owner = owner;
    *w11 = (uint32_t)c->cur;
    *w12 = HWCTX_MAGIC;
    return 0;
}

void *hwctx_cur_owner(const HwCtx *c)
{
    assert(c != NULL);
    assert(c->cur >= 0 && c->cur < HWCTX_MAX);
    return c->rec[c->cur].owner;
}

/* runs on the new host thread after its first wake: entry(arg), then lr (OS_ExitThread), then park for good */
static void fresh_main(void *arg)
{
    HwCtxRec *r = (HwCtxRec *)arg;
    void (*entry)(void *);
    void (*after)(void);

    assert(r != NULL && r->pc != 0u);
    entry = (void (*)(void *))r->pc;
    after = (void (*)(void))r->lr;

    entry((void *)r->r0);
    if (after) {
        after();
    }
}

int hwctx_load(HwCtx *c, void *owner, uint32_t w11, uint32_t w12, uintptr_t pc, uintptr_t r0, uintptr_t lr, uint32_t stack_bytes,
               int retire_me)
{
    int me = c->cur;
    int target = -1;
    int i;

    assert(c != NULL && owner != NULL);
    assert(me >= 0 && me < HWCTX_MAX && c->rec[me].used);

    if (w12 == HWCTX_MAGIC && w11 < HWCTX_MAX && c->rec[w11].used && c->rec[w11].owner == owner) {
        target = (int)w11;
    } else {
        for (i = 1; i < HWCTX_MAX; i++) {
            if (!c->rec[i].used) {
                target = i;
                break;
            }
        }
        if (target < 0) {
            return -1;
        }
        c->rec[target].used = 1;
        c->rec[target].owner = owner;
        c->rec[target].pc = pc;
        c->rec[target].r0 = r0;
        c->rec[target].lr = lr;
        c->rec[target].h = c->be.create(fresh_main, &c->rec[target], stack_bytes, c->be.user);
        if (!c->rec[target].h) {
            c->rec[target].used = 0;
            return -1;
        }
    }
    if (target == me) {
        return 0;   /* loading the context that is already running */
    }
    c->cur = target;
    if (retire_me) {
        void *h = c->rec[me].h;
        c->rec[me].used = 0;
        c->rec[me].h = 0;
        c->be.wake(c->rec[target].h, c->be.user);
        c->be.exit_current(h, c->be.user);
        return 0;   /* not reached */
    }
    c->be.wake(c->rec[target].h, c->be.user);
    c->be.park(c->rec[me].h, c->be.user);
    return 0;
}

int hwctx_restart(HwCtx *c, void (*entry)(void *), void *arg, uint32_t stack_bytes)
{
    int me, target = -1, i;
    void *old, *h;

    assert(c != NULL && entry != NULL);
    me = c->cur;
    assert(me >= 0 && me < HWCTX_MAX && c->rec[me].used);
    old = c->rec[me].h;
    /* the new thread first: if none can be made, nothing has been touched */
    for (i = 0; i < HWCTX_MAX; i++) {
        if (!c->rec[i].used) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return -1;
    }
    c->rec[target].owner = 0;
    c->rec[target].pc = (uintptr_t)entry;
    c->rec[target].r0 = (uintptr_t)arg;
    c->rec[target].lr = 0u;
    h = c->be.create(fresh_main, &c->rec[target], stack_bytes, c->be.user);
    if (!h) {
        return -1;
    }
    for (i = 0; i < HWCTX_MAX; i++) {                  /* every other context is parked on a hand-off: end it */
        if (c->rec[i].used && i != me) {
            c->be.kill(c->rec[i].h, c->be.user);
            c->rec[i].used = 0;
            c->rec[i].h = 0;
        }
    }
    c->rec[me].used = 0;
    c->rec[me].h = 0;
    c->rec[target].used = 1;
    c->rec[target].h = h;
    c->cur = target;
    c->restarts++;
    c->be.wake(h, c->be.user);
    if (old == c->main_h) {
        for (;;) {                                     /* the process's own thread cannot end: it stays parked, nothing ever wakes it */
            c->be.park(old, c->be.user);
        }
    }
    c->be.exit_current(old, c->be.user);
    return 0;   /* not reached */
}
