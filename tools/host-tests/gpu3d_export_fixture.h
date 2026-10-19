/* gpu3d_export_fixture.h -- plain types shared by gpu3d_export_fixture.c (which sees gpu3d.h) and gpu3d_export_test.c (which sees the 2.5 lane's gpu3d_list.h): the two headers
 * declare the same type names, so they never meet in one translation unit. */
#ifndef GPU3D_EXPORT_FIXTURE_H
#define GPU3D_EXPORT_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

typedef struct FixVtx { int32_t pos[4]; uint8_t rgb[3]; int16_t s, t, sx, sy; uint32_t depth; } FixVtx;
typedef struct FixPoly { uint32_t attr, texparam; uint16_t texpal; uint8_t nverts, flags; int16_t ytop, ybottom; uint32_t sortkey; uint32_t first; } FixPoly;
typedef struct Fixture { uint32_t npolys, nverts; FixPoly poly[16]; FixVtx vtx[64]; uint32_t viewport_cmd, wbuf; } Fixture;

/* runs a hand-built scene through the engine, seals and latches it, exports it into buf; fills *fx with what the engine's own accessors hold; returns the byte count */
size_t fixture_run(uint8_t *buf, size_t cap, Fixture *fx);
#endif
