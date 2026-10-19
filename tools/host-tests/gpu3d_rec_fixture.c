/* gpu3d_rec_fixture.c -- the engine side of the frame-record test (see gpu3d_rec_fixture.h). */
#include <string.h>

#include "gpu3d.h"
#include "gpu3d_rec_fixture.h"

static void cmd(Gpu3d *g, uint32_t op, uint32_t a)
{
    gpu3d_port(g, 0x400u + 4u * op, a);
}

static void vtx16(Gpu3d *g, int x, int y, int z)
{
    gpu3d_port(g, 0x400u + 4u * 0x23u, (uint32_t)(x & 0xffff) | ((uint32_t)(y & 0xffff) << 16));
    gpu3d_port(g, 0x400u + 4u * 0x23u, (uint32_t)(z & 0xffff));
}

size_t recfx_export(uint8_t *buf, size_t cap, int scene, uint32_t disp3dcnt)
{
    static Gpu3d g;
    static int started;
    Gpu3dExportRegs regs;

    if (!started) {
        gpu3d_reset(&g);
        started = 1;
    }
    cmd(&g, 0x60u, 0xbfff0000u);                 /* VIEWPORT: the full 256 x 192 */
    cmd(&g, 0x10u, 0u);
    cmd(&g, 0x15u, 0u);                          /* projection = identity */
    cmd(&g, 0x10u, 1u);
    cmd(&g, 0x15u, 0u);                          /* position = identity */
    if (scene == 0) {
        cmd(&g, 0x29u, 0x1f00c0u);               /* opaque, both faces */
        cmd(&g, 0x2au, (7u << 26) | (0x100u >> 3));   /* TEXIMAGE_PARAM: direct colour, 8 x 8, at texture offset 0x100 */
        cmd(&g, 0x40u, 0u);
        cmd(&g, 0x20u, 0x7fffu);
        cmd(&g, 0x22u, 0x00200010u);
        vtx16(&g, -0x800, -0x800, 0);
        vtx16(&g, 0x800, -0x800, 0);
        vtx16(&g, 0, 0x800, 0);
        cmd(&g, 0x40u, 0u);
        vtx16(&g, -0x400, -0x400, 0);
        vtx16(&g, 0x400, -0x400, 0);
        vtx16(&g, 0, 0x400, 0);
    } else {
        cmd(&g, 0x29u, 0x1f00c0u);
        cmd(&g, 0x2au, 0u);                      /* no texture */
        cmd(&g, 0x40u, 1u);
        cmd(&g, 0x20u, 0x1fu);
        vtx16(&g, -0x200, -0x200, 0);
        vtx16(&g, 0x200, -0x200, 0);
        vtx16(&g, 0x200, 0x200, 0);
        vtx16(&g, -0x200, 0x200, 0);
    }
    cmd(&g, 0x50u, 0u);                          /* SWAP_BUFFERS */
    gpu3d_vblank(&g);
    memset(&regs, 0, sizeof regs);
    regs.key = (uint32_t)scene + 1u;
    regs.disp3dcnt = disp3dcnt;
    return gpu3d_export_list(&g, &regs, buf, cap);
}
