/* gpu3d_rec_test.c -- bet 2.5b: the frame record's 3D half (gpu3d_rec.c) and the reader's expanded-vertex limit (gpu3d_list.c).
 *   1. reader limit: a list of 2048 polygons x 10 vertices (20480 expanded vertices, what the DS RAM can hold) opens; 20481 is refused;
 *   2. record isolation after engine mutation: the record owns its list bytes, so running the engine on (another scene, another seal) leaves them untouched;
 *   3. VRAM mutation after the seal: the decoded texels are copies, so changing the texture memory afterwards does not change what the render thread uploads;
 *   4. the cache plan: the mirror is advanced in the record only (an uncommitted = dropped record changes nothing), a committed plan hits, changed bytes miss;
 *   5. the slot-view gather and the flat views give the same keys. */
#include <stdio.h>
#include <string.h>

#include "gpu3d_list.h"
#include "gpu3d_rec.h"
#include "gpu3d_rec_fixture.h"
#include "gpu3d_tex.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t g_tex[4][0x20000], g_pal[6][0x4000];
static uint8_t g_flat_tex[G3R_TEXMEM_BYTES], g_flat_pal[G3R_PALMEM_BYTES];
static uint8_t g_scratch[G3R_SCRATCH_BYTES];
static uint32_t g_pool[G3R_POOL_WORDS], g_pool2[G3R_POOL_WORDS];
static uint8_t g_list[G3R_LIST_MAX], g_list2[G3R_LIST_MAX], g_big[G3R_LIST_MAX + 32u];
static Gpu3dRec g_rec, g_rec2;

static const uint8_t *slot_view(uint32_t k) { return k < 4u ? g_tex[k] : NULL; }
static const uint8_t *pltt_view(uint32_t k) { return k < 6u ? g_pal[k] : NULL; }

static void fill_vram(uint8_t seed)
{
    uint32_t i;

    for (i = 0u; i < 0x20000u; i++) {
        g_tex[0][i] = (uint8_t)(seed + i * 7u);
    }
}

static uint32_t sum32(const uint32_t *p, uint32_t n)
{
    uint32_t i, h = 2166136261u;

    for (i = 0u; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void test_reader_limit(void)
{
    Gpu3dList l, src;
    Gpu3dPoly p;
    uint32_t i;

    CHECK(G3R_LIST_MAX == gpu3d_list_size(GPU3D_MAX_POLYS, GPU3D_MAX_VERTS));
    CHECK(GPU3D_MAX_POLYS == 2048u && GPU3D_MAX_VERTS == 20480u && GPU3D_MAX_PV == 10u);
    memset(g_big, 0, sizeof g_big);
    memset(&src, 0, sizeof src);
    src.npolys = GPU3D_MAX_POLYS;
    src.nverts = GPU3D_MAX_VERTS;
    src.render_count = GPU3D_MAX_POLYS;
    gpu3d_list_put_header(g_big, &src);
    memset(&p, 0, sizeof p);
    p.nverts = 10u;
    for (i = 0u; i < GPU3D_MAX_POLYS; i++) {
        p.first = i * 10u;
        gpu3d_list_put_poly(g_big, i, &p, GPU3D_MAX_POLYS);
    }
    CHECK(gpu3d_list_open(&l, g_big, G3R_LIST_MAX) == GPU3D_ERR_OK);
    CHECK(l.npolys == 2048u && l.nverts == 20480u);
    src.nverts = GPU3D_MAX_VERTS + 1u;
    gpu3d_list_put_header(g_big, &src);
    CHECK(gpu3d_list_open(&l, g_big, G3R_LIST_MAX + 32u) == GPU3D_ERR_COUNTS);
    src.nverts = GPU3D_MAX_VERTS;
    src.npolys = GPU3D_MAX_POLYS + 1u;
    gpu3d_list_put_header(g_big, &src);
    CHECK(gpu3d_list_open(&l, g_big, G3R_LIST_MAX + 64u) == GPU3D_ERR_COUNTS);
    /* the old 6144 limit would have refused a list the DS can really hold: a strip of 2048 quads has 8192 expanded vertices */
    src.npolys = 2048u;
    src.nverts = 8192u;
    gpu3d_list_put_header(g_big, &src);
    p.nverts = 4u;
    for (i = 0u; i < GPU3D_MAX_POLYS; i++) {
        p.first = i * 4u;
        gpu3d_list_put_poly(g_big, i, &p, GPU3D_MAX_POLYS);
    }
    CHECK(gpu3d_list_open(&l, g_big, G3R_LIST_MAX) == GPU3D_ERR_OK);
}

int main(void)
{
    G3rMirror empty, committed;
    G3rSrc src, flat;
    size_t n;
    uint32_t i, pool_sum, texel_words;
    uint8_t copy[4096];
    uint64_t key0;

    test_reader_limit();
    gpu3d_rec_mirror_init(&empty);
    fill_vram(3u);
    memset(&src, 0, sizeof src);
    src.tex_slot = slot_view;
    src.tex_pltt = pltt_view;
    src.scratch = g_scratch;

    /* 2. the record owns its list: export scene 0, plan, then run the engine on with scene 1 */
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.list = g_list;
    g_rec.list_cap = sizeof g_list;
    g_rec.texels = g_pool;
    g_rec.texel_cap = G3R_POOL_WORDS;
    n = recfx_export(g_rec.list, g_rec.list_cap, 0, 1u);
    CHECK(n > 256u);
    g_rec.list_len = (uint32_t)n;
    CHECK(gpu3d_rec_textures(&g_rec, &empty, &src) == 0);
    CHECK(g_rec.npolys == 2u && g_rec.ntex == 1u);
    CHECK(g_rec.poly_tex[0] == 0u && g_rec.poly_tex[1] == 0u);
    CHECK(g_rec.st.lookups == 2u && g_rec.st.hits == 1u && g_rec.st.keys == 1u && g_rec.st.decodes == 1u);
    CHECK(g_rec.tex[0].upload == 1u && g_rec.tex[0].slot == 0u && g_rec.tex[0].w == 8u && g_rec.tex[0].h == 8u && g_rec.tex[0].texel_off == 0u);
    CHECK(g_rec.texel_used == 64u && g_rec.st.texel_bytes == 256u);
    CHECK(g_rec.st.hashed_bytes == 128u);        /* 8 x 8 direct-colour texels, no palette */
    CHECK(g_rec.mirror.frame == 1u && g_rec.mirror.slot[0].used == 1u && g_rec.mirror.slot[0].key == g_rec.tex[0].key);
    CHECK(empty.frame == 0u && empty.slot[0].used == 0u);   /* planning never touches the committed mirror: a dropped record changes nothing */
    key0 = g_rec.tex[0].key;
    CHECK(gpu3d_rec_bytes(&g_rec) == (uint64_t)n + 256u + 2u * 2u + 1u * sizeof(G3rTex));
    memcpy(copy, g_rec.list, sizeof copy);
    {
        size_t keep = n;

        CHECK(recfx_export(g_list2, sizeof g_list2, 1, 1u) > 256u);   /* the engine moves on: another scene sealed */
        CHECK(memcmp(copy, g_rec.list, sizeof copy) == 0 && g_rec.list_len == keep);
        CHECK(memcmp(g_list2, g_rec.list, 256u) != 0);   /* and it really is a different list */
    }

    /* 3. VRAM changes after the plan do not change the uploaded texels */
    pool_sum = sum32(g_pool, g_rec.texel_used);
    texel_words = g_rec.texel_used;
    fill_vram(99u);
    CHECK(sum32(g_pool, texel_words) == pool_sum);

    /* 4. the cache plan against a committed mirror: same bytes = hit, no upload; changed bytes = another key, a new slot */
    fill_vram(3u);
    committed = g_rec.mirror;                    /* the render thread took the record */
    memset(&g_rec2, 0, sizeof g_rec2);
    g_rec2.list = g_list;
    g_rec2.list_cap = sizeof g_list;
    g_rec2.list_len = g_rec.list_len;
    g_rec2.texels = g_pool2;
    g_rec2.texel_cap = G3R_POOL_WORDS;
    CHECK(gpu3d_rec_textures(&g_rec2, &committed, &src) == 0);
    CHECK(g_rec2.ntex == 1u && g_rec2.tex[0].upload == 0u && g_rec2.tex[0].slot == 0u && g_rec2.tex[0].key == key0);
    CHECK(g_rec2.st.decodes == 0u && g_rec2.st.texel_bytes == 0u && g_rec2.texel_used == 0u && g_rec2.mirror.frame == 2u);
    CHECK(gpu3d_rec_textures(&g_rec2, &empty, &src) == 0 && g_rec2.tex[0].upload == 1u && g_rec2.st.decodes == 1u);   /* the earlier record was dropped: the plan starts over */
    fill_vram(5u);
    CHECK(gpu3d_rec_textures(&g_rec2, &committed, &src) == 0);
    CHECK(g_rec2.tex[0].key != key0 && g_rec2.tex[0].upload == 1u && g_rec2.tex[0].slot == 1u);
    CHECK(g_rec2.mirror.slot[0].used == 1u && g_rec2.mirror.slot[1].used == 1u && g_rec2.mirror.slot[1].key == g_rec2.tex[0].key);

    /* 5. the gathered slot views and the flat views give the same key */
    fill_vram(3u);
    for (i = 0u; i < 4u; i++) {
        memcpy(g_flat_tex + (size_t)i * 0x20000u, g_tex[i], 0x20000u);
    }
    for (i = 0u; i < 6u; i++) {
        memcpy(g_flat_pal + (size_t)i * 0x4000u, g_pal[i], 0x4000u);
    }
    memset(&flat, 0, sizeof flat);
    flat.flat_tex = g_flat_tex;
    flat.flat_pal = g_flat_pal;
    CHECK(gpu3d_rec_textures(&g_rec2, &empty, &flat) == 0 && g_rec2.tex[0].key == key0);

    /* the texture switch (DISP3DCNT bit 0 off): no textures at all, every polygon untextured */
    n = recfx_export(g_rec2.list, g_rec2.list_cap, 0, 0u);
    g_rec2.list_len = (uint32_t)n;
    CHECK(gpu3d_rec_textures(&g_rec2, &empty, &src) == 0 && g_rec2.ntex == 0u && g_rec2.poly_tex[0] == G3R_NO_TEX && g_rec2.st.lookups == 0u);
    /* and a list that is not PLY1 is refused with the reader's code, leaving no textures */
    g_rec2.list_len = 100u;
    CHECK(gpu3d_rec_textures(&g_rec2, &empty, &src) == GPU3D_ERR_SHORT && g_rec2.ntex == 0u);
    gpu3d_rec_reset(&g_rec2);
    CHECK(g_rec2.list_len == 0u && g_rec2.ntex == 0u);

    printf("gpu3d_rec_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
