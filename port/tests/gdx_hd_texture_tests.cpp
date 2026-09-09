#define NOMINMAX
#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"
#include "fast/lus_gbi.h"
#include "fast/resource/factory/TextureFactory.h"
#include "ship/Context.h"
#include "ship/resource/ResourceLoader.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/O2rArchive.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

extern "C" void gdx_dbg_logf(const char*, ...) {}
extern "C" void gdx_ck(const char*) {}
extern "C" void gdx_cki(const char*, int) {}
extern "C" int gdx_workshop_texture_dump_enabled() {
    return 0;
}
extern "C" void gdx_workshop_dump_texture(const void*, size_t, const char*, const unsigned char*, int, int, int, int) {}
static const uint8_t* sAtlasSource;
static const char* sAtlasPath;
extern "C" int gdx_workshop_texture_packs_enabled() {
    return sAtlasPath != nullptr;
}
extern "C" const char* GDiffuser_LookupLoadedAssetKeyContaining(const void* addr, size_t* offset) {
    *offset = 0;
    return addr == sAtlasSource ? "fixture/atlas" : nullptr;
}
extern "C" const char* GdxWorkshopLookupAtlasTileOverride(const char*, size_t, int, int, int, int) {
    return sAtlasPath;
}

namespace {
class CaptureBackend final : public Fast::GfxRenderingAPI {
  public:
    uint32_t width = 0, height = 0, uploads = 0, nextId = 1;
    std::vector<uint8_t> pixels;
    std::vector<float> vertices;
    const char* GetName() override {
        return "texture-fixture";
    }
    int GetMaxTextureSize() override {
        return 4096;
    }
    Fast::GfxClipParameters GetClipParameters() override {
        return {};
    }
    void UnloadShader(Fast::ShaderProgram*) override {}
    void LoadShader(Fast::ShaderProgram*) override {}
    void ClearShaderCache() override {}
    Fast::ShaderProgram* CreateAndLoadNewShader(uint64_t, uint64_t) override {
        return reinterpret_cast<Fast::ShaderProgram*>(this);
    }
    Fast::ShaderProgram* LookupShader(uint64_t, uint64_t) override {
        return nullptr;
    }
    void ShaderGetInfo(Fast::ShaderProgram*, uint8_t* inputs, bool textures[2]) override {
        *inputs = 0;
        textures[0] = true;
        textures[1] = false;
    }
    uint32_t NewTexture() override {
        return nextId++;
    }
    void SelectTexture(int, uint32_t) override {}
    void UploadTexture(const uint8_t* data, uint32_t w, uint32_t h) override {
        width = w;
        height = h;
        ++uploads;
        pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    }
    void SetSamplerParameters(int, bool, uint32_t, uint32_t) override {}
    void SetDepthTestAndMask(bool, bool) override {}
    void SetZmodeDecal(bool) override {}
    void SetViewport(int, int, int, int) override {}
    void SetScissor(int, int, int, int) override {}
    void SetUseAlpha(bool) override {}
    void DrawTriangles(float* data, size_t count, size_t) override {
        vertices.assign(data, data + count);
    }
    void Init() override {}
    void OnResize() override {}
    void StartFrame() override {}
    void EndFrame() override {}
    void FinishRender() override {}
    int CreateFramebuffer() override {
        return 0;
    }
    void UpdateFramebufferParameters(int, uint32_t, uint32_t, uint32_t, bool, bool, bool, bool,
                                     Fast::GdxFramebufferFormat) override {}
    void StartDrawToFramebuffer(int, float) override {}
    void CopyFramebuffer(int, int, int, int, int, int, int, int, int, int) override {}
    void ClearFramebuffer(bool, bool) override {}
    void ReadFramebufferToCPU(int, uint32_t, uint32_t, uint16_t*) override {}
    void ResolveMSAAColorBuffer(int, int) override {}
    std::unordered_map<std::pair<float, float>, uint16_t, Fast::hash_pair_ff>
    GetPixelDepth(int, const std::set<std::pair<float, float>>&) override {
        return {};
    }
    void* GetFramebufferTextureId(int) override {
        return nullptr;
    }
    void SelectTextureFb(int) override {}
    void DeleteTexture(uint32_t) override {}
    void SetTextureFilter(Fast::FilteringMode) override {}
    Fast::FilteringMode GetTextureFilter() override {
        return {};
    }
    void SetSrgbMode() override {}
    ImTextureID GetTextureById(int) override {
        return {};
    }
    void SetCurrentPrimDepth(float) override {}
    void SetCurrentAlphaCompareThreshold(float) override {}
};

int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}

uint16_t Pattern(uint32_t x, uint32_t y) {
    return static_cast<uint16_t>(((x % 32) << 11) | ((y % 32) << 6) | (((x + y) % 32) << 1) | ((x + y) % 3 != 0));
}

std::shared_ptr<Fast::Texture> Texture(uint32_t width, uint32_t height, uint32_t sx, uint32_t sy) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Path = "fixture/rgba16";
    auto tex = std::make_shared<Fast::Texture>(init);
    tex->Type = Fast::TextureType::RGBA16bpp;
    tex->Width = static_cast<uint16_t>(width * sx);
    tex->Height = static_cast<uint16_t>(height * sy);
    tex->HByteScale = static_cast<float>(sx);
    tex->VPixelScale = static_cast<float>(sy);
    tex->ImageDataSize = tex->Width * tex->Height * 2;
    tex->ImageData = new uint8_t[tex->ImageDataSize];
    for (uint32_t y = 0; y < tex->Height; ++y) {
        for (uint32_t x = 0; x < tex->Width; ++x) {
            const auto p = Pattern(x, y);
            tex->ImageData[(y * tex->Width + x) * 2] = p >> 8;
            tex->ImageData[(y * tex->Width + x) * 2 + 1] = p & 255;
        }
    }
    return tex;
}

struct Fixture {
    CaptureBackend backend;
    Fast::Interpreter interpreter;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(4096u * 4096u * 4u);
    Fixture() {
        interpreter.mRapi = &backend;
        interpreter.mTexUploadBuffer = scratch.data();
        interpreter.mTextureCache.map.reserve(1024);
    }
    void Source(const std::shared_ptr<Fast::Texture>& tex, uint32_t nativeWidth) {
        auto& source = interpreter.mRdp->texture_to_load;
        source.addr = tex->ImageData;
        source.siz = G_IM_SIZ_16b;
        source.width = nativeWidth;
        source.raw_tex_metadata = {tex->Width, tex->Height, tex->HByteScale, tex->VPixelScale, tex, tex->Type};
    }
    void Tile(uint32_t width, uint32_t height, uint16_t tmem = 0, uint8_t wrap = G_TX_CLAMP, uint8_t maskS = 0,
              uint8_t maskT = 0) {
        interpreter.GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, (width * 2 + 7) / 8, tmem, 0, 0, wrap, maskT, 0, wrap,
                                 maskS, 0);
        interpreter.GfxDpSetTileSize(0, 0, 0, (width - 1) * 4, (height - 1) * 4);
        interpreter.GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0);
    }
    bool Pixels(uint32_t width, uint32_t height, uint32_t originX = 0, uint32_t originY = 0) {
        if (backend.width != width || backend.height != height) {
            std::printf("  upload %ux%u; expected %ux%u\n", backend.width, backend.height, width, height);
            return false;
        }
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint16_t p = Pattern(x + originX, y + originY);
                const uint8_t rgba[] = {uint8_t(((p >> 11) & 31) * 255 / 31), uint8_t(((p >> 6) & 31) * 255 / 31),
                                        uint8_t(((p >> 1) & 31) * 255 / 31), uint8_t((p & 1) ? 255 : 0)};
                if (std::memcmp(&backend.pixels[(y * width + x) * 4], rgba, 4) != 0) {
                    std::printf("  incorrect pixel at %u,%u\n", x, y);
                    return false;
                }
            }
        }
        return true;
    }
};

void Whole(uint32_t sx, uint32_t sy, bool sentinel) {
    Fixture f;
    const auto tex = Texture(16, 8, sx, sy);
    f.Source(tex, sentinel ? 1 : 16);
    f.Tile(16, 8);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 16 * 8 - 1, 512);
    f.interpreter.ImportTexture(0, 0, false);
    char label[96];
    std::snprintf(label, sizeof(label), "whole RGBA16 %ux%u scale, sentinel=%d", sx, sy, sentinel);
    Check(f.Pixels(16 * sx, 8 * sy), label);
}

void Interior(uint16_t word) {
    Fixture f;
    auto tex = Texture(16, 8, 4, 4);
    f.Source(tex, 16);
    f.Tile(16, 8);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 127, 512);
    const auto& view = f.interpreter.mRdp->loaded_texture[word];
    const uint32_t nativeX = (word * 8 % 32) / 2;
    const uint32_t nativeY = word * 8 / 32;
    const size_t expectedOffset = (nativeY * 4 * tex->Width + nativeX * 4) * 2;
    Check(view.addr == tex->ImageData + expectedOffset, "interior resource origin");
    f.Tile(4, 2, word);
    f.interpreter.ImportTexture(0, 0, false);
    Check(f.Pixels(16, 8, nativeX * 4, nativeY * 4), "interior RGBA16 pixels and rows");
}

void Subrectangle() {
    Fixture f;
    auto tex = Texture(16, 8, 4, 4);
    f.Source(tex, 16);
    f.Tile(4, 2);
    f.interpreter.GfxDpLoadTile(7, 4 * 4, 2 * 4, 7 * 4, 3 * 4);
    f.interpreter.ImportTexture(0, 0, false);
    Check(f.Pixels(16, 8, 16, 8), "LOADTILE nonzero origin and full source stride");
}

void LayoutAndBounds() {
    for (const uint8_t wrap : {G_TX_WRAP, G_TX_MIRROR, G_TX_CLAMP}) {
        Fixture f;
        auto tex = Texture(16, 8, 4, 4);
        f.Source(tex, 16);
        f.Tile(8, 4, 0, wrap, 3, 2);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 127, 512);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.Pixels(32, 16), "native mask period with HD upload");
        const auto uploads = f.backend.uploads;
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == uploads, "HD cache hit avoids upload");
        f.interpreter.TextureCacheDelete(tex->ImageData);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == uploads + 1 && f.Pixels(32, 16), "HD cache invalidation reloads pixels");
    }
    {
        Fixture f;
        auto tex = Texture(16, 8, 4, 4);
        f.Source(tex, 16);
        f.Tile(6, 3);
        f.interpreter.GfxDpLoadTile(7, 8, 4, 28, 12);
        f.Tile(6, 2, 2);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.Pixels(24, 8, 8, 8), "LOADTILE interior after padded TMEM row");
    }
    {
        Fixture f;
        auto tex = Texture(16, 8, 4, 4);
        f.Source(tex, 4);
        f.Tile(4, 2);
        f.interpreter.GfxDpLoadBlock(7, 4, 2, 7, 512);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.Pixels(16, 8, 16, 8), "LOADBLOCK nonzero source origin");
    }
    for (int invalid = 0; invalid < 7; ++invalid) {
        Fixture f;
        auto tex = Texture(16, 8, 4, 4);
        if (invalid == 0) {
            tex->ImageDataSize -= 1;
        }
        if (invalid == 1) {
            tex->HByteScale = std::numeric_limits<float>::quiet_NaN();
        }
        if (invalid == 2) {
            tex->VPixelScale = 0;
        }
        if (invalid == 3) {
            tex->HByteScale = 1.5f;
        }
        f.Source(tex, 16);
        f.Tile(16, 8);
        const uint32_t x = invalid == 4 ? 16 : invalid == 5 ? UINT32_MAX : 0;
        const uint32_t y = invalid == 6 ? UINT32_MAX : 0;
        f.interpreter.GfxDpLoadBlock(7, x, y, 127, 512);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == 0 && f.interpreter.mTextureCache.map.empty(),
              "invalid HD view rejected before cache/upload");
    }
    {
        Fixture f;
        auto tex = Texture(1025, 1, 4, 4);
        f.Source(tex, 1025);
        f.Tile(1025, 1);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 1024, 0);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == 0 && f.interpreter.mTextureCache.map.empty(), "backend upload capacity enforced");
    }
}

void PrepareBlendTriangle(Fixture& f, const std::shared_ptr<Fast::Texture>& tex) {
    f.Source(tex, 16);
    f.Tile(16, 8, 0, G_TX_WRAP, 4, 3);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 127, 512);
    f.interpreter.GfxDpSetCombineMode(uint32_t(G_CCMUX_TEXEL0) << 13, 0, 0, 0);
    auto* v = f.interpreter.mRsp->loaded_vertices;
    v[0] = {-0.5f, -0.5f, 0, 1, 0, 0, {120, 0, 0, 0}, 0};
    v[1] = {0.5f, -0.5f, 0, 1, 16 * 32, 0, {0, 180, 0, 127}, 0};
    v[2] = {-0.5f, 0.5f, 0, 1, 0, 8 * 32, {0, 0, 240, 255}, 0};
}

void BlendCoefficients() {
    Fixture f;
    auto tex = Texture(16, 8, 1, 1);
    PrepareBlendTriangle(f, tex);
    // Captured outline word; keep the oracle independent of the production selector expression.
    constexpr uint32_t outline = 0xAFAA0210u;
    auto check = [&](uint32_t mode, uint8_t fogAlpha, bool constant, bool fog) {
        f.interpreter.mRdp->other_mode_l = mode;
        f.interpreter.mRdp->fog_color = {31, 63, 95, fogAlpha};
        f.interpreter.ResetGeometryDiagnostics();
        f.interpreter.GfxSpTri1(0, 1, 2, false);
        f.interpreter.Flush();
        Check(f.backend.vertices.size() == 30, "blend triangle submits position, UV and blend coefficients");
        if (f.backend.vertices.size() != 30) {
            return;
        }
        const auto color = fog ? f.interpreter.mRdp->fog_color : f.interpreter.mRdp->blend_color;
        for (size_t i = 0; i < 3; ++i) {
            const float expected = constant ? 1.0f :
                (fog ? f.interpreter.mRsp->loaded_vertices[i].color.a : fogAlpha) / 255.0f;
            Check(f.backend.vertices[i * 10 + 6] == color.r / 255.0f &&
                  f.backend.vertices[i * 10 + 7] == color.g / 255.0f &&
                  f.backend.vertices[i * 10 + 8] == color.b / 255.0f, "blend RGB preserved");
            Check(f.backend.vertices[i * 10 + 9] == expected, "blend equation coefficient");
        }
        const auto& diagnostics = f.interpreter.GetGeometryDiagnostics();
        Check(diagnostics.fogTriangles == 1 &&
              diagnostics.minFogFactor == (fog ? 0.0f : (constant ? 1.0f : fogAlpha / 255.0f)) &&
              diagnostics.maxFogFactor == (fog ? 1.0f : (constant ? 1.0f : fogAlpha / 255.0f)),
              "blend diagnostics match submission");
    };
    for (bool replay : {false, true}) {
        if (replay) {
            f.interpreter.ResetRdpForReplay();
            PrepareBlendTriangle(f, tex);
        }
        if (replay) {
            f.interpreter.mRdp->env_color = {0, 0, 0, 0};
        } else {
            f.interpreter.mRdp->env_color = {14, 99, 165, 255};
        }
        for (uint8_t color : {0, 80}) {
            f.interpreter.mRdp->blend_color = {color, uint8_t(color * 2), uint8_t(color * 3), 255};
            for (uint8_t alpha : {0, 127, 255}) {
                check(outline, alpha, true, false);
                check(outline ^ 0x2010u, alpha, true, false);
            }
        }
    }
    for (uint32_t shift : {16u, 18u, 20u, 22u, 24u, 26u, 28u}) {
        check(outline ^ (1u << shift), 127, false, false);
    }
    check(outline ^ (1u << 30), 127, false, true);
}

void BlendBenchmark(uint32_t mode, const char* name) {
    Fixture f;
    auto tex = Texture(16, 8, 1, 1);
    PrepareBlendTriangle(f, tex);
    f.interpreter.mRdp->other_mode_l = mode;
    auto submit = [&]() {
        f.interpreter.GfxSpTri1(0, 1, 2, false);
        f.interpreter.Flush();
    };
    for (int n = 0; n < 32; ++n) {
        submit();
    }
    double samples[200];
    for (double& sample : samples) {
        const auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < 100; ++n) {
            submit();
        }
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100;
    }
    std::sort(std::begin(samples), std::end(samples));
    std::printf("BENCH %s warmup=32 samples=200 draws_per_sample=100 median_us=%.3f p95_us=%.3f\n",
                name, samples[100], samples[190]);
}

void DrawCoordinates() {
    std::vector<float> baseline;
    for (uint32_t scale : {1u, 2u, 4u, 10u}) {
        Fixture f;
        auto tex = Texture(16, 8, scale, scale);
        f.Source(tex, 16);
        f.Tile(16, 8, 0, G_TX_WRAP, 4, 3);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 127, 512);
        f.interpreter.GfxDpSetCombineMode(uint32_t(G_CCMUX_TEXEL0) << 13, 0, 0, 0);
        auto* v = f.interpreter.mRsp->loaded_vertices;
        v[0] = {-0.5f, -0.5f, 0, 1, 0, 0, {255, 255, 255, 255}, 0};
        v[1] = {0.5f, -0.5f, 0, 1, 16 * 32, 0, {255, 255, 255, 255}, 0};
        v[2] = {-0.5f, 0.5f, 0, 1, 0, 8 * 32, {255, 255, 255, 255}, 0};
        f.interpreter.GfxSpTri1(0, 1, 2, false);
        f.interpreter.Flush();
        Check(!f.backend.vertices.empty() && f.backend.uploads == 1, "textured triangle reaches capture backend");
        if (scale == 1) {
            baseline = f.backend.vertices;
        } else {
            Check(f.backend.vertices == baseline, "HD draw coordinates preserve native UV units");
        }
    }
}

void TileOverfetch() {
    for (const bool block : {false, true}) {
        for (const uint32_t y : {0u, 5u}) {
            Fixture f;
            auto tex = Texture(16, 8, 4, 4);
            f.Source(tex, 16);
            f.Tile(16, 8 - y);
            if (block) {
                f.interpreter.GfxDpLoadBlock(7, 0, y, 255, 512);
            } else {
                f.interpreter.GfxDpLoadTile(7, 0, y * 4, 60, (y + 16 - 1) * 4);
            }
            f.interpreter.ImportTexture(0, 0, false);
            Check(f.Pixels(64, (8 - y) * 4, 0, y * 4),
                  block ? "LOADBLOCK fixed-height band clips to available image rows"
                        : "LOADTILE fixed-height band clips to available image rows");
            const auto& last = f.interpreter.mRdp->loaded_texture[63];
            Check(last.tmem_start == 0 && last.tmem_word_count == 64 && last.addr == nullptr,
                  "unreadable overfetch retains TMEM ownership without source access");
        }
    }
}

void NativeControls() {
    {
        Fixture f;
        auto tex = Texture(16, 8, 1, 1);
        f.Source(tex, 16);
        f.interpreter.mRdp->texture_to_load.raw_tex_metadata = {};
        f.Tile(16, 8);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 127, 512);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.Pixels(16, 8), "native RGBA16 TMEM decode unchanged");
        const auto first = f.interpreter.mRdp->tmem[0];
        uint8_t replacement[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        f.interpreter.mRdp->texture_to_load.addr = replacement;
        f.interpreter.GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 1, 7, 0, 0, 0, 0, 0, 0, 0);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 3, 0);
        Check(f.interpreter.mRdp->tmem[0] == first && std::memcmp(f.interpreter.mRdp->tmem + 8, replacement, 8) == 0 &&
                  f.interpreter.mRdp->loaded_texture[0].addr == nullptr,
              "native overlapping upload invalidates old range");
    }
    for (bool ci : {false, true}) {
        Fixture f;
        uint8_t sourceBytes[16];
        std::memset(sourceBytes, ci ? 0x12 : 0xF8, sizeof(sourceBytes));
        auto& source = f.interpreter.mRdp->texture_to_load;
        source.addr = sourceBytes;
        source.siz = G_IM_SIZ_8b;
        source.width = ci ? 16 : 8;
        const uint8_t fmt = ci ? G_IM_FMT_CI : G_IM_FMT_IA;
        const uint8_t siz = ci ? G_IM_SIZ_4b : G_IM_SIZ_8b;
        f.interpreter.GfxDpSetTile(fmt, siz, 1, 0, 0, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
        f.interpreter.GfxDpSetTileSize(0, 0, 0, ci ? 60 : 28, 4);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 15, 0);
        uint8_t palette[256] = {};
        palette[2] = 0xF8;
        palette[3] = 1;
        palette[4] = 7;
        palette[5] = 0xC1;
        f.interpreter.mRdp->palettes[0] = palette;
        f.interpreter.mRdp->palette_dram_addr[0] = palette;
        f.interpreter.ImportTexture(0, 0, false);
        bool ok = f.backend.width == (ci ? 16 : 8) && f.backend.height == 2;
        for (size_t p = 0; ok && p < f.backend.pixels.size() / 4; ++p) {
            const uint8_t expected[] = {uint8_t(ci && (p & 1) ? 0 : 255), uint8_t(!ci || (p & 1) ? 255 : 0),
                                        uint8_t(ci ? 0 : 255), uint8_t(ci ? 255 : 136)};
            ok = std::memcmp(&f.backend.pixels[p * 4], expected, 4) == 0;
        }
        Check(ok, ci ? "native CI4 palette control" : "native IA8 TMEM control");
    }
}

void Atlas(const std::shared_ptr<Ship::Context>& ctx) {
    auto tex = Texture(12, 16, 4, 4);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("gdx-hd-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    std::vector<uint8_t> bytes(64, 0);
    bytes[1] = 1;
    bytes[4] = 'X';
    bytes[5] = 'E';
    bytes[6] = 'T';
    bytes[7] = 'O';
    bytes[8] = 1;
    const uint32_t fields[] = {2,
                               tex->Width,
                               tex->Height,
                               0,
                               std::bit_cast<uint32_t>(4.0f),
                               std::bit_cast<uint32_t>(4.0f),
                               tex->ImageDataSize};
    for (auto field : fields) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(uint8_t(field >> shift));
        }
    }
    bytes.insert(bytes.end(), tex->ImageData, tex->ImageData + tex->ImageDataSize);
    const auto archivePath = (dir / "fixture.o2r").string();
    int error = 0;
    auto* zip = zip_open(archivePath.c_str(), ZIP_CREATE | ZIP_EXCL, &error);
    if (zip == nullptr) {
        throw std::runtime_error("cannot create fixture archive");
    }
    auto* payload = zip_source_buffer(zip, bytes.data(), bytes.size(), 0);
    if (payload == nullptr || zip_file_add(zip, "atlas", payload, 0) < 0 || zip_close(zip) != 0) {
        throw std::runtime_error("cannot write fixture archive");
    }
    ctx->InitResourceManager({archivePath}, {}, 1, true);
    ctx->GetResourceManager()->GetResourceLoader()->RegisterResourceFactory(
        std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY, "Texture", 0x4F544558, 1);
    auto loadedFixture = ctx->GetResourceManager()->LoadResourceProcess("atlas");
    Check(loadedFixture != nullptr, "atlas OTEX fixture loaded");
    Fixture f;
    std::vector<uint8_t> native(12 * 16 * 2);
    auto& source = f.interpreter.mRdp->texture_to_load;
    source.addr = native.data();
    source.siz = G_IM_SIZ_16b;
    source.width = 12;
    f.Tile(12, 16);
    sAtlasSource = native.data();
    sAtlasPath = "atlas";
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 12 * 16 - 1, 683);
    Check(f.interpreter.mRdp->loaded_texture[0].full_image_line_size_bytes == 96, "late atlas physical row stride");
    f.interpreter.ImportTexture(0, 0, false);
    Check(f.Pixels(48, 64), "late atlas full-resolution pixels");
    {
        Fixture padded;
        auto& tileSource = padded.interpreter.mRdp->texture_to_load;
        tileSource.addr = native.data();
        tileSource.siz = G_IM_SIZ_16b;
        tileSource.width = 12;
        padded.Tile(6, 3);
        padded.interpreter.GfxDpLoadTile(7, 0, 0, 20, 8);
        padded.Tile(6, 2, 2);
        padded.interpreter.ImportTexture(0, 0, false);
        Check(padded.Pixels(24, 8, 0, 4), "late atlas LOADTILE retains padded TMEM row");
    }
    for (const bool invalid : {false, true}) {
        Fixture miss;
        auto& tileSource = miss.interpreter.mRdp->texture_to_load;
        tileSource.addr = native.data();
        tileSource.siz = G_IM_SIZ_16b;
        tileSource.width = 12;
        sAtlasSource = invalid ? native.data() : nullptr;
        if (invalid) {
            std::static_pointer_cast<Fast::Texture>(loadedFixture)->ImageDataSize -= 1;
        }
        miss.Tile(12, 16);
        miss.interpreter.GfxDpLoadBlock(7, 0, 0, 12 * 16 - 1, 683);
        miss.interpreter.ImportTexture(0, 0, false);
        Check(miss.backend.width == 12 && miss.backend.height == 16 &&
                  std::all_of(miss.backend.pixels.begin(), miss.backend.pixels.end(), [](uint8_t p) { return p == 0; }),
              invalid ? "invalid atlas payload retains native pixels" : "atlas lookup miss retains native pixels");
    }
    sAtlasSource = nullptr;
    sAtlasPath = nullptr;
    std::printf("Atlas fixture: %s\n", archivePath.c_str());
}

std::shared_ptr<Fast::Texture> CloudTexture(uint32_t scale) {
    auto tex = Texture(64, 32, scale, scale);
    tex->Type = Fast::TextureType::GrayscaleAlpha8bpp;
    tex->ImageDataSize = tex->Width * tex->Height;
    for (uint32_t y = 0; y < tex->Height; ++y) {
        for (uint32_t x = 0; x < tex->Width; ++x) {
            tex->ImageData[y * tex->Width + x] = uint8_t(((x + y * 3) & 15) << 4) | uint8_t((x * 5 + y) & 15);
        }
    }
    return tex;
}

void CloudLoad(Fixture& f, const std::shared_ptr<Fast::Texture>& tex) {
    f.Source(tex, 1);
    f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_16b, 0, 0, 7, 0, G_TX_WRAP, 5, 0, G_TX_WRAP, 6, 0);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 1023, 256);
    f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_8b, 8, 0, 0, 0, G_TX_WRAP, 5, 0, G_TX_WRAP, 6, 0);
    f.interpreter.GfxDpSetTileSize(0, 0, 0, 63 * 4, 31 * 4);
}

bool CloudPixels(const Fixture& f, uint32_t width, uint32_t height, uint32_t ox = 0, uint32_t oy = 0) {
    if (f.backend.width != width || f.backend.height != height) {
        std::printf("  IA8 upload %ux%u; expected %ux%u\n", f.backend.width, f.backend.height, width, height);
        return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t intensity = uint8_t(((x + ox + (y + oy) * 3) & 15) * 17);
            const uint8_t alpha = uint8_t(((x + ox) * 5 + y + oy) & 15) * 17;
            const uint8_t expected[] = {intensity, intensity, intensity, alpha};
            if (std::memcmp(&f.backend.pixels[(y * width + x) * 4], expected, 4) != 0) {
                return false;
            }
        }
    }
    return true;
}

void Clouds() {
    std::vector<float> nativeVertices;
    for (uint32_t scale : {1u, 2u, 4u}) {
        Fixture f;
        auto tex = CloudTexture(scale);
        CloudLoad(f, tex);
        f.interpreter.ImportTexture(0, 0, false);
        Check(CloudPixels(f, 64 * scale, 32 * scale), "IA8 sentinel full image and exact intensity/alpha");
        const auto uploads = f.backend.uploads;
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == uploads, "IA8 cached import does not upload again");
        f.interpreter.GfxDpSetCombineMode(uint32_t(G_CCMUX_TEXEL0) << 13, 0, 0, 0);
        auto* v = f.interpreter.mRsp->loaded_vertices;
        v[0] = {-0.5f, -0.5f, 0, 1, 0, 0, {255, 255, 255, 255}, 0};
        v[1] = {0.5f, -0.5f, 0, 1, 64 * 32, 0, {255, 255, 255, 255}, 0};
        v[2] = {-0.5f, 0.5f, 0, 1, 0, 32 * 32, {255, 255, 255, 255}, 0};
        f.interpreter.GfxSpTri1(0, 1, 2, false);
        f.interpreter.Flush();
        if (scale == 1) {
            nativeVertices = f.backend.vertices;
        } else {
            Check(!nativeVertices.empty() && nativeVertices == f.backend.vertices, "IA8 preserves native wrap UVs");
        }
    }
}

void CloudViews() {
    for (uint32_t scale : {2u, 4u}) {
        Fixture f;
        auto tex = CloudTexture(scale);
        f.Source(tex, 64);
        f.interpreter.mRdp->texture_to_load.siz = G_IM_SIZ_8b;
        f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_8b, 2, 0, 7, 0, 0, 0, 0, 0, 0, 0);
        f.interpreter.GfxDpLoadTile(7, 4 * 4, 3 * 4, 16 * 4, 10 * 4);
        f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_8b, 2, 0, 0, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
        f.interpreter.GfxDpSetTileSize(0, 0, 0, 12 * 4, 7 * 4);
        f.interpreter.ImportTexture(0, 0, false);
        Check(CloudPixels(f, 13 * scale, 8 * scale, 4 * scale, 3 * scale),
              "IA8 LOADTILE origin and packed source stride");
        f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_8b, 2, 3, 0, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
        f.interpreter.GfxDpSetTileSize(0, 0, 0, 4 * 4, 6 * 4);
        f.interpreter.ImportTexture(0, 0, false);
        Check(CloudPixels(f, 5 * scale, 7 * scale, 12 * scale, 4 * scale),
              "IA8 interior view after padded TMEM row");
    }
    {
        Fixture f;
        auto tex = CloudTexture(4);
        CloudLoad(f, tex);
        f.interpreter.ImportTexture(0, 0, false);
        auto replacement = CloudTexture(4);
        replacement->ImageData[0] = 0xAB;
        CloudLoad(f, replacement);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.pixels[0] == 170 && f.backend.pixels[3] == 187 && f.backend.uploads == 2,
              "IA8 replacement does not reuse old cache pixels");
        f.Source(Texture(8, 4, 1, 1), 8);
        f.interpreter.GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 2, 8, 7, 0, 0, 0, 0, 0, 0, 0);
        f.interpreter.GfxDpLoadBlock(7, 0, 0, 31, 0);
        Check(f.interpreter.mRdp->loaded_texture[0].addr == nullptr,
              "native overlapping load invalidates IA8 resource view");
    }
    {
        Fixture f;
        CloudLoad(f, CloudTexture(4));
        f.interpreter.mRdp->texture_tile[0].siz = G_IM_SIZ_16b;
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == 0, "IA8 resource cannot be decoded through a wider format");
    }
    for (int invalid = 0; invalid < 3; ++invalid) {
        Fixture f;
        auto tex = CloudTexture(4);
        if (invalid == 0) {
            --tex->ImageDataSize;
        } else if (invalid == 1) {
            tex->HByteScale = 0.5f;
        } else {
            tex->Width = 65535;
        }
        CloudLoad(f, tex);
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == 0, "invalid IA8 resource rejected without upload");
    }
}

std::shared_ptr<Fast::Texture> StripeTexture(uint32_t scale, bool pattern = false) {
    auto tex = Texture(16, 16, scale, scale);
    tex->Type = Fast::TextureType::GrayscaleAlpha4bpp;
    tex->ImageDataSize = tex->Width * tex->Height / 2;
    std::memset(tex->ImageData, 0, tex->ImageDataSize);
    for (uint32_t y = 0; y < tex->Height; ++y) {
        for (uint32_t x = 0; x < tex->Width; ++x) {
            const uint8_t value = pattern ? ((x + y * 3) & 15) : (x / scale >= 3 && x / scale < 13 ? 15 : 0);
            const uint32_t pixel = y * tex->Width + x;
            tex->ImageData[pixel / 2] |= value << (pixel % 2 == 0 ? 4 : 0);
        }
    }
    return tex;
}

void StripeTile(Fixture& f, uint32_t width = 16, uint32_t height = 16, uint16_t tmem = 0x50) {
    f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_4b, (width + 15) / 16, tmem, 0, 0,
                              G_TX_WRAP, height == 16 ? 4 : 0, 0, G_TX_MIRROR | G_TX_CLAMP,
                              width == 16 ? 4 : 0, 0);
    f.interpreter.GfxDpSetTileSize(0, 0, 0, (width - 1) * 4, (height - 1) * 4);
}

void StripeLoad(Fixture& f, const std::shared_ptr<Fast::Texture>& tex) {
    f.Source(tex, 1);
    f.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_16b, 0, 0x50, 7, 0, G_TX_WRAP, 4, 0,
                              G_TX_MIRROR | G_TX_CLAMP, 4, 0);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, 63, 2048);
    StripeTile(f);
}

bool StripePixels(const Fixture& f, uint32_t scale, uint32_t width = 16, uint32_t height = 16,
                  uint32_t ox = 0, uint32_t oy = 0, bool pattern = false) {
    if (f.backend.width != width * scale || f.backend.height != height * scale) {
        return false;
    }
    for (uint32_t y = 0; y < height * scale; ++y) {
        for (uint32_t x = 0; x < width * scale; ++x) {
            const uint8_t value = pattern ? ((x + ox * scale + (y + oy * scale) * 3) & 15)
                                         : (x / scale >= 3 && x / scale < 13 ? 15 : 0);
            const uint8_t i = value >> 1;
            const uint8_t intensity = i * 36;
            const uint8_t expected[] = {intensity, intensity, intensity, uint8_t(value & 1 ? 255 : 0)};
            if (std::memcmp(&f.backend.pixels[(y * width * scale + x) * 4], expected, 4) != 0) {
                return false;
            }
        }
    }
    return true;
}

void Stripes() {
    std::vector<float> nativeVertices;
    for (uint32_t scale : {1u, 2u, 3u, 4u}) {
        Fixture f;
        StripeLoad(f, StripeTexture(scale));
        f.interpreter.ImportTexture(0, 0, false);
        Check(StripePixels(f, scale), "IA4 real stripe block load and full mask");
        const auto uploads = f.backend.uploads;
        f.interpreter.ImportTexture(0, 0, false);
        Check(f.backend.uploads == uploads, "IA4 cache reuse");
        f.interpreter.GfxDpSetCombineMode(uint32_t(G_CCMUX_TEXEL0) << 13, 0, 0, 0);
        auto* v = f.interpreter.mRsp->loaded_vertices;
        v[0] = {-0.5f, -0.5f, 0, 1, 0, 0, {255, 255, 255, 255}, 0};
        v[1] = {0.5f, -0.5f, 0, 1, 16 * 32, 0, {255, 255, 255, 255}, 0};
        v[2] = {-0.5f, 0.5f, 0, 1, 0, 16 * 32, {255, 255, 255, 255}, 0};
        f.interpreter.GfxSpTri1(0, 1, 2, false);
        f.interpreter.Flush();
        if (scale == 1) {
            nativeVertices = f.backend.vertices;
        } else {
            Check(!nativeVertices.empty() && nativeVertices == f.backend.vertices, "IA4 native mirror/wrap UVs");
            Fixture view;
            view.Source(StripeTexture(scale, true), 16);
            view.interpreter.mRdp->texture_to_load.siz = G_IM_SIZ_4b;
            view.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_4b, 1, 0x50, 7, 0, 0, 0, 0, 0, 0, 0);
            view.interpreter.GfxDpLoadTile(7, 4, 8, 8 * 4, 6 * 4);
            StripeTile(view, 8, 5);
            view.interpreter.ImportTexture(0, 0, false);
            Check(StripePixels(view, scale, 8, 5, 1, 2, true), "IA4 LOADTILE odd nibble origin and lower rows");
            StripeTile(view, 8, 4, 0x51);
            view.interpreter.ImportTexture(0, 0, false);
            Check(StripePixels(view, scale, 8, 4, 1, 3, true), "IA4 padded interior TMEM row");
        }
    }
    Fixture invalid;
    auto tex = StripeTexture(4);
    --tex->ImageDataSize;
    StripeLoad(invalid, tex);
    invalid.interpreter.ImportTexture(0, 0, false);
    Check(invalid.backend.uploads == 0, "IA4 truncated resource rejected");
    Fixture nibble;
    auto narrow = StripeTexture(4, true);
    narrow->HByteScale = 1;
    for (uint32_t x : {0u, 1u}) {
        nibble.Source(narrow, 64);
        nibble.interpreter.mRdp->texture_to_load.siz = G_IM_SIZ_4b;
        nibble.interpreter.GfxDpSetTile(G_IM_FMT_IA, G_IM_SIZ_4b, 1, 0x50, 7, 0, 0, 0, 0, 0, 0, 0);
        nibble.interpreter.GfxDpLoadTile(7, x * 4, 0, (x + 3) * 4, 4);
        StripeTile(nibble, 4, 2);
        nibble.interpreter.ImportTexture(0, 0, false);
        Check(nibble.backend.width == 4 && nibble.backend.height == 8 &&
                  nibble.backend.pixels[3] == (x == 0 ? 0 : 255), "IA4 same-byte nibble views stay distinct");
    }
    Check(nibble.backend.uploads == 2, "IA4 cache key includes nibble origin");
}

void StripeBenchmark(uint32_t scale) {
    Fixture f;
    StripeLoad(f, StripeTexture(scale));
    double samples[200];
    for (int n = 0; n < 232; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTextureIA4(0, false);
        if (n >= 32) {
            samples[n - 32] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        }
    }
    std::sort(samples, samples + 200);
    std::printf("BENCH IA4 stripe scale=%u warmup=32 samples=200 median_us=%.3f p95_us=%.3f upload=%ux%u\n",
                scale, samples[100], samples[190], f.backend.width, f.backend.height);
    f.interpreter.ImportTexture(0, 0, false);
    const auto uploads = f.backend.uploads;
    for (int n = 0; n < 232; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTexture(0, 0, false);
        if (n >= 32) {
            samples[n - 32] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        }
    }
    std::sort(samples, samples + 200);
    std::printf("CACHE IA4 stripe scale=%u warmup=32 samples=200 median_us=%.3f p95_us=%.3f new_uploads=%u\n",
                scale, samples[100], samples[190], f.backend.uploads - uploads);
}

void CloudBenchmark(uint32_t scale) {
    Fixture f;
    auto tex = CloudTexture(scale);
    CloudLoad(f, tex);
    double samples[200];
    for (int n = 0; n < 232; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTextureIA8(0, false);
        if (n >= 32) {
            samples[n - 32] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        }
    }
    std::sort(samples, samples + 200);
    std::printf("BENCH IA8 cloud scale=%u warmup=32 samples=200 median_us=%.3f p95_us=%.3f upload=%ux%u bytes=%zu\n",
                scale, samples[100], samples[190], f.backend.width, f.backend.height, f.backend.pixels.size());
    f.interpreter.ImportTexture(0, 0, false);
    const auto uploads = f.backend.uploads;
    for (int n = 0; n < 232; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTexture(0, 0, false);
        if (n >= 32) {
            samples[n - 32] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        }
    }
    std::sort(samples, samples + 200);
    std::printf("CACHE IA8 cloud scale=%u warmup=32 samples=200 median_us=%.3f p95_us=%.3f new_uploads=%u\n",
                scale, samples[100], samples[190], f.backend.uploads - uploads);
}

void Benchmark(uint32_t w, uint32_t h, uint32_t scale) {
    Fixture f;
    auto tex = Texture(w, h, scale, scale);
    f.Source(tex, w);
    f.Tile(w, h);
    f.interpreter.GfxDpLoadBlock(7, 0, 0, w * h - 1, 0);
    for (int n = 0; n < 32; ++n) {
        f.interpreter.ImportTextureRgba16(0, 0, false, false);
    }
    std::vector<double> samples;
    for (int n = 0; n < 200; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTextureRgba16(0, 0, false, false);
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("BENCH RGBA16 %ux%u scale=%u warmup=32 samples=200 "
                "median_us=%.3f p95_us=%.3f upload_bytes=%zu\n",
                w, h, scale, samples[100], samples[190], f.backend.pixels.size());
    f.interpreter.ImportTexture(0, 0, false);
    const auto uploads = f.backend.uploads;
    for (int n = 0; n < 32; ++n) {
        f.interpreter.ImportTexture(0, 0, false);
    }
    for (int n = 0; n < 200; ++n) {
        const auto start = std::chrono::steady_clock::now();
        f.interpreter.ImportTexture(0, 0, false);
        samples[n] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    std::sort(samples.begin(), samples.end());
    std::printf("CACHE RGBA16 %ux%u scale=%u warmup=32 samples=200 median_us=%.3f p95_us=%.3f new_uploads=%u\n", w, h,
                scale, samples[100], samples[190], f.backend.uploads - uploads);
}
} // namespace

int main(int argc, char**) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ctx = Ship::Context::CreateUninitializedInstance("HD texture tests", "hd-texture-tests", "hd-tests.json");
    ctx->InitConfiguration();
    ctx->InitConsoleVariables();
    if (argc > 1) {
        BlendBenchmark(0xAFAA0210u, "constant-blend");
        BlendBenchmark(0, "ordinary-textured-triangle");
        for (const auto scale : {1u, 4u}) {
            StripeBenchmark(scale);
            CloudBenchmark(scale);
            Benchmark(32, 32, scale);
            Benchmark(12, 16, scale);
            Benchmark(180, 245, scale);
        }
        // Context teardown requires a real window; this harness deliberately has
        // none.
        std::_Exit(0);
    }
    for (const auto scale : {1u, 2u, 4u, 10u}) {
        Whole(scale, scale, false);
        Whole(scale, scale, true);
    }
    Whole(2, 4, false);
    Interior(1);
    Interior(4);
    Subrectangle();
    LayoutAndBounds();
    DrawCoordinates();
    BlendCoefficients();
    TileOverfetch();
    NativeControls();
    Clouds();
    CloudViews();
    Stripes();
    try {
        Atlas(ctx);
    } catch (const std::exception& ex) {
        std::printf("Atlas fixture error: %s\n", ex.what());
        ++failures;
    }
    std::printf("HD texture regression failures: %d\n", failures);
    // Context teardown requires a real window; all capture fixtures have already
    // been destroyed.
    std::_Exit(failures != 0);
}
