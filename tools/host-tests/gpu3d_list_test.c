/* gpu3d_list_test.c -- bet 2.5a host test: the polygon-list reader / writer round trip, the translucent rule, the viewport and depth functions against hand-computed values,
 * and (when the oracle dumps exist) against every vertex of the real harness dumps in build/oracle/p-*. Also writes a synthetic list for the Python reader
 * (read back by an independent reader in the private tree): usage: gpu3d_list_test OUTDIR   (OUTDIR/synthetic_list.bin + synthetic_list.txt). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu3d_list.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void synthetic(const char *outdir)
{
    Gpu3dList l;
    Gpu3dPoly p;
    Gpu3dVert v;
    uint8_t *buf;
    size_t sz = gpu3d_list_size(2, 7);
    char path[512];
    FILE *f;
    int i, err;
    memset(&l, 0, sizeof l);
    l.key = 2240; l.disp3dcnt = 0x3039; l.clear_color = 0x1F0000u | 0x7FFFu; l.clear_depth = 0x7FFF; l.swap_bits = 2; l.viewport = 0xBFFF0000u;
    l.npolys = 2; l.nverts = 7; l.tex_hash = 0x0123456789ABCDEFull; l.alpha_ref = 5; l.fog_color = 0x00112233; l.fog_offset = 0x40;
    l.render_count = 2; l.degenerate_count = 0; l.flags = 1;
    for (i = 0; i < 8; i++) l.edge_color[i] = (uint16_t)(0x1111 * (i + 1));
    for (i = 0; i < 32; i++) l.toon[i] = (uint16_t)(0x0101 * i);
    buf = calloc(1, sz);
    gpu3d_list_put_header(buf, &l);
    memset(&p, 0, sizeof p);
    p.attr = 0x1F0080u | (5u << 24); p.texparam = 0x8A200123u; p.texpalette = 0x0123; p.nverts = 3; p.flags = GPU3D_PF_FACING; p.first = 0; p.ytop = 5; p.ybottom = 90; p.sortkey = 0x5A05;
    gpu3d_list_put_poly(buf, 0, &p, 2);
    p.attr = 0x100040u; p.texparam = 0; p.texpalette = 0; p.nverts = 4; p.flags = GPU3D_PF_TRANSLUCENT | GPU3D_PF_WBUFFER; p.first = 3; p.ytop = -3; p.ybottom = 191; p.sortkey = 0x1BF00;
    gpu3d_list_put_poly(buf, 1, &p, 2);
    for (i = 0; i < 7; i++) {
        memset(&v, 0, sizeof v);
        v.x = (i - 3) * 0x1000 + 17; v.y = -(i * 0x777); v.z = 0x7FFFFF - i; v.w = 0x1000 * (i + 1); v.r = (uint8_t)(i * 4); v.g = (uint8_t)(31 - i); v.b = (uint8_t)i;
        v.s = (int16_t)(i * 16 - 40); v.t = (int16_t)(-i * 33); v.sx = (int16_t)(i * 37); v.sy = (int16_t)(191 - i); v.depth24 = 0xFFFE00u - (uint32_t)i * 0x200u;
        gpu3d_list_put_vert(buf, (uint32_t)i, &v, 2);
    }
    err = gpu3d_list_open(&l, buf, sz);
    CHECK(err == GPU3D_ERR_OK, "synthetic list does not open: %s", gpu3d_list_strerror(err));
    CHECK(l.key == 2240 && l.npolys == 2 && l.nverts == 7 && l.tex_hash == 0x0123456789ABCDEFull && l.toon[31] == 0x1F1F && l.edge_color[7] == 0x8888, "header round trip");
    gpu3d_list_poly(&l, 1, &p);
    CHECK(p.nverts == 4 && p.first == 3 && p.ytop == -3 && p.ybottom == 191 && p.flags == (GPU3D_PF_TRANSLUCENT | GPU3D_PF_WBUFFER), "polygon round trip");
    gpu3d_list_vert(&l, 6, &v);
    CHECK(v.x == 3 * 0x1000 + 17 && v.y == -(6 * 0x777) && v.w == 0x7000 && v.s == 56 && v.t == -198 && v.depth24 == 0xFFFE00u - 6 * 0x200u, "vertex round trip");
    snprintf(path, sizeof path, "%s/synthetic_list.bin", outdir);
    f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sz, f); fclose(f); }
    snprintf(path, sizeof path, "%s/synthetic_list.txt", outdir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "key %u disp3dcnt %u clear_color %u viewport %u npolys %u nverts %u tex_hash %llu toon31 %u edge7 %u\n", l.key, l.disp3dcnt, l.clear_color, l.viewport, l.npolys, l.nverts,
                (unsigned long long)l.tex_hash, l.toon[31], l.edge_color[7]);
        for (i = 0; i < 2; i++) { gpu3d_list_poly(&l, (uint32_t)i, &p); fprintf(f, "poly %d %u %u %u %u %u %u %d %d %u\n", i, p.attr, p.texparam, p.texpalette, p.nverts, p.flags, p.first, p.ytop, p.ybottom, p.sortkey); }
        for (i = 0; i < 7; i++) {
            gpu3d_list_vert(&l, (uint32_t)i, &v);
            fprintf(f, "vert %d %d %d %d %d %u %u %u %d %d %d %d %u\n", i, v.x, v.y, v.z, v.w, v.r, v.g, v.b, v.s, v.t, v.sx, v.sy, v.depth24);
        }
        fclose(f);
    }
    /* refusals */
    CHECK(gpu3d_list_open(&l, buf, 100) == GPU3D_ERR_SHORT, "short header");
    CHECK(gpu3d_list_open(&l, buf, sz - 1) == GPU3D_ERR_SHORT, "short body");
    buf[0] = 'X';
    CHECK(gpu3d_list_open(&l, buf, sz) == GPU3D_ERR_MAGIC, "magic");
    buf[0] = 'P';
    buf[4] = 2;
    CHECK(gpu3d_list_open(&l, buf, sz) == GPU3D_ERR_VERSION, "version");
    buf[4] = 1;
    buf[GPU3D_HDR_BYTES + 10] = 2;
    CHECK(gpu3d_list_open(&l, buf, sz) == GPU3D_ERR_RANGE, "2-vertex polygon refused");
    buf[GPU3D_HDR_BYTES + 10] = 3;
    buf[GPU3D_HDR_BYTES + 12] = 6;
    CHECK(gpu3d_list_open(&l, buf, sz) == GPU3D_ERR_RANGE, "vertex range past the array refused");
    free(buf);
}

static void hand_values(void)
{
    int32_t sx, sy;
    /* translucent rule: alpha 1..30 or format A3I5 (1) / A5I3 (6) */
    CHECK(gpu3d_is_translucent(31u << 16, 0) == 0, "alpha 31, no texture: opaque");
    CHECK(gpu3d_is_translucent(0u << 16, 0) == 0, "alpha 0 (wireframe): not translucent");
    CHECK(gpu3d_is_translucent(1u << 16, 0) == 1 && gpu3d_is_translucent(30u << 16, 0) == 1, "alpha 1 and 30: translucent");
    CHECK(gpu3d_is_translucent(31u << 16, 1u << 26) == 1, "A3I5 at alpha 31: translucent");
    CHECK(gpu3d_is_translucent(31u << 16, 6u << 26) == 1, "A5I3 at alpha 31: translucent");
    CHECK(gpu3d_is_translucent(31u << 16, 2u << 26) == 0 && gpu3d_is_translucent(31u << 16, 3u << 26) == 0, "4-colour / 16-colour at alpha 31: opaque");
    CHECK(gpu3d_is_translucent(31u << 16, 7u << 26) == 0 && gpu3d_is_translucent(31u << 16, 4u << 26) == 0, "formats 4 and 7 at alpha 31: opaque");
    /* viewport 0,0..255,191 (0xBFFF0000): the centre of clip space is the centre of the screen, x = -w the left edge, x = +w one past the right edge */
    gpu3d_viewport_xy(0xBFFF0000u, 0, 0, 0x1000, &sx, &sy);
    CHECK(sx == 128 && sy == 96, "centre -> 128,96 (got %d,%d)", sx, sy);
    gpu3d_viewport_xy(0xBFFF0000u, -0x1000, 0x1000, 0x1000, &sx, &sy);
    CHECK(sx == 0 && sy == 0, "(-w,+w) -> top left (got %d,%d)", sx, sy);
    gpu3d_viewport_xy(0xBFFF0000u, 0x1000, -0x1000, 0x1000, &sx, &sy);
    CHECK(sx == 256 && sy == 192, "(+w,-w) -> 256,192 (got %d,%d)", sx, sy);
    gpu3d_viewport_xy(0xBFFF0000u, 0x800, 0x400, 0x2000, &sx, &sy);   /* x/w = 0.25, y/w = 0.125: sx = (1.25)*128 = 160, sy = (1-0.125)*96 = 84 */
    CHECK(sx == 160 && sy == 84, "(0.25,0.125) -> 160,84 (got %d,%d)", sx, sy);
    gpu3d_viewport_xy(0xBF7F4010u, 0, 0, 0x1000, &sx, &sy);          /* x1=16 y1=64 x2=127 y2=191: width 112, height 128, top 0 -> centre 16+56=72, 64 */
    CHECK(sx == 72 && sy == 64, "sub-viewport centre (got %d,%d)", sx, sy);
    /* depth: Z mode, z/w = -1 -> 0 (15-bit zero), z/w = 0 -> 0x3FFF steps, z/w = +1 -> clamps at 0x7FFF steps; W mode is w itself */
    CHECK(gpu3d_depth24_z(-0x1000, 0x1000) == 0u, "z/w=-1 -> 0 (got %x)", gpu3d_depth24_z(-0x1000, 0x1000));
    CHECK(gpu3d_depth24_z(0, 0x1000) == 0x3FFFu * 0x200u, "z/w=0 (got %x)", gpu3d_depth24_z(0, 0x1000));
    CHECK(gpu3d_depth24_z(0x1000, 0x1000) == 0x7FFFu * 0x200u, "z/w=+1 (got %x)", gpu3d_depth24_z(0x1000, 0x1000));
    CHECK(gpu3d_depth24_z(0x800, 0x1000) == (0x2000u + 0x3FFFu) * 0x200u, "z/w=0.5 (got %x)", gpu3d_depth24_z(0x800, 0x1000));
    CHECK(gpu3d_depth24_z(5, 0) == 0x7FFE00u, "w=0 -> far");
    {
        int32_t ws[3] = { 0x123456, 0x1000, 0x20 }, small[2] = { 0xFFFF, 3 };
        CHECK(gpu3d_w_drop(ws, 3) == 8u, "largest w 0x123456 is 21 bits -> 24 -> drop 8 (got %u)", gpu3d_w_drop(ws, 3));
        CHECK(gpu3d_w_drop(small, 2) == 0u, "w that fits 16 bits: nothing dropped");
        CHECK(gpu3d_depth24_w(0x123456, 8) == 0x123400u && gpu3d_depth24_w(0x123456, 0) == 0x123456u, "W mode is w with the low bits dropped");
    }
}

/* every vertex of a real dump: the viewport function and the depth function against the producer's own values */
static void real_dumps(const char *root)
{
    static const char *dirs[] = { "p-run", "p-title", "p-twinleaf" };
    unsigned d;
    for (d = 0; d < 3; d++) {
        char dir[512];
        DIR *dp;
        struct dirent *e;
        unsigned frames = 0, polys = 0, verts = 0, bad_xy = 0, bad_z = 0, bad_flag = 0, bad_count = 0;
        snprintf(dir, sizeof dir, "%s/%s", root, dirs[d]);
        dp = opendir(dir);
        if (!dp) { printf("gpu3d_list_test: %s absent, real-dump checks skipped\n", dir); continue; }
        while ((e = readdir(dp)) != NULL) {
            char path[1024];
            size_t n;
            uint8_t *b;
            Gpu3dList l;
            uint32_t i, j;
            if (strncmp(e->d_name, "poly_", 5) != 0) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            b = slurp(path, &n);
            if (!b) { CHECK(0, "cannot read %s", path); continue; }
            if (gpu3d_list_open(&l, b, n) != GPU3D_ERR_OK) { CHECK(0, "%s does not open", path); free(b); continue; }
            frames++;
            if (l.render_count != l.npolys) bad_count++;
            for (i = 0; i < l.npolys; i++) {
                Gpu3dPoly p;
                gpu3d_list_poly(&l, i, &p);
                int32_t ws[GPU3D_MAX_PV];
                uint32_t drop;
                polys++;
                for (j = 0; j < p.nverts; j++) { Gpu3dVert v0; gpu3d_list_vert(&l, p.first + j, &v0); ws[j] = v0.w; }
                drop = gpu3d_w_drop(ws, p.nverts);
                if (!!(p.flags & GPU3D_PF_TRANSLUCENT) != gpu3d_is_translucent(p.attr, p.texparam)) bad_flag++;
                for (j = 0; j < p.nverts; j++) {
                    Gpu3dVert v;
                    int32_t sx, sy;
                    uint32_t dz;
                    gpu3d_list_vert(&l, p.first + j, &v);
                    verts++;
                    gpu3d_viewport_xy(l.viewport, v.x, v.y, v.w, &sx, &sy);
                    if (sx != v.sx || sy != v.sy) bad_xy++;
                    dz = (p.flags & GPU3D_PF_WBUFFER) ? gpu3d_depth24_w(v.w, drop) : gpu3d_depth24_z(v.z, v.w);
                    if (dz != v.depth24) bad_z++;
                }
            }
            free(b);
        }
        closedir(dp);
        printf("gpu3d_list_test: %s: %u frames, %u polygons, %u vertices: screen position mismatches %u, depth mismatches %u, translucent flag mismatches %u, frames with count mismatch %u\n",
               dirs[d], frames, polys, verts, bad_xy, bad_z, bad_flag, bad_count);
        CHECK(bad_flag == 0 && bad_count == 0, "%s: flags / counts", dirs[d]);
        CHECK(bad_xy == 0, "%s: screen position mismatches", dirs[d]);
        CHECK(bad_z == 0, "%s: depth mismatches", dirs[d]);
    }
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    const char *root = argc > 2 ? argv[2] : "build/oracle";
    hand_values();
    synthetic(out);
    real_dumps(root);
    printf("gpu3d_list_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
