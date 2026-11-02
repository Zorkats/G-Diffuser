#include <PR/mbi.h>
#include <stddef.h>
#include <string.h>

typedef struct MachineTestRange {
    const void* data;
    size_t bytes;
    int kind;
} MachineTestRange;

static Gfx setup[16];
static Gfx textures[16];
static Gfx model[32];
static Gfx parts[3][16];
static Vtx vertices[64];
static unsigned short rgba[256];
static unsigned char ia[128];

size_t GdxMachineTest_BuildNative(int assembled, MachineTestRange* ranges, size_t* counts) {
    Gfx* gfx;
    int i;
    memset(vertices, 0, sizeof(vertices));
    for (i = 0; i < 64; ++i) {
        vertices[i].v.ob[0] = (i % 3 == 1) ? 10 : 0;
        vertices[i].v.ob[1] = (i % 3 == 2) ? 10 : 0;
        vertices[i].v.tc[0] = (i % 3 == 1) ? 480 : 0;
        vertices[i].v.tc[1] = (i % 3 == 2) ? 480 : 0;
        memset(vertices[i].v.cn, 255, 4);
    }
    for (i = 0; i < 256; ++i) {
        rgba[i] = 0xF801;
    }
    memset(ia, 0xFF, sizeof(ia));
    gfx = setup;
    gSPClearGeometryMode(gfx++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(gfx++, G_SHADE | G_SHADING_SMOOTH);
    gDPLoadMultiBlock(gfx++, rgba, 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
                      G_TX_CLAMP, G_TX_WRAP, 4, 4, 0, 0);
    gSPEndDisplayList(gfx++);
    counts[0] = (size_t)(gfx - setup);
    gfx = textures;
    gDPLoadMultiBlock_4b(gfx++, ia, 0x40, 1, G_IM_FMT_IA, 16, 16, 0,
                         G_TX_MIRROR, G_TX_CLAMP, 4, 4, 0, 0);
    gSPEndDisplayList(gfx++);
    counts[1] = (size_t)(gfx - textures);
    for (i = 0; i < 3; ++i) {
        gfx = parts[i];
        gDPSetEnvColor(gfx++, 20 + i * 40, 100, 200, 255);
        gDPSetCombineLERP(gfx++, TEXEL0, ENVIRONMENT, TEXEL0_ALPHA, ENVIRONMENT,
                          0, 0, 0, ENVIRONMENT, COMBINED, 0, SHADE, 0, 0, 0, 0, COMBINED);
        gSPTexture(gfx++, 0xFFFF, 0xFFFF, 0, i == 0 ? 0 : 1, G_ON);
        gSPVertex(gfx++, vertices, 64, 0);
        gSP2Triangles(gfx++, 0, 1, 2, 0, 60, 61, 62, 0);
        gSPEndDisplayList(gfx++);
    }
    gfx = model;
    for (i = 0; i < (assembled ? 3 : 2); ++i) {
        gSPDisplayList(gfx++, parts[i]);
    }
    gSPEndDisplayList(gfx++);
    counts[2] = (size_t)(gfx - model);
    ranges[0] = (MachineTestRange){setup, counts[0] * sizeof(Gfx), 0};
    ranges[1] = (MachineTestRange){textures, counts[1] * sizeof(Gfx), 0};
    ranges[2] = (MachineTestRange){model, counts[2] * sizeof(Gfx), 0};
    ranges[3] = (MachineTestRange){parts, sizeof(parts), 0};
    ranges[4] = (MachineTestRange){vertices, sizeof(vertices), 1};
    ranges[5] = (MachineTestRange){rgba, sizeof(rgba), 2};
    ranges[6] = (MachineTestRange){ia, sizeof(ia), 2};
    return 7;
}
