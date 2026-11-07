// port/tests/gdx_alt_assets_tests.cpp -- the alt/ switch against a real ResourceManager.
//
// The fixture archive holds two textures, "fixture/tex" and "fixture/plain", and an alt/ entry
// for the first one only. The textures differ in width, so the width says which one loaded.

#define NOMINMAX
#include "../gdx_alt_assets.h"

#include "fast/resource/factory/TextureFactory.h"
#include "fast/resource/type/Texture.h"
#include "ship/Context.h"
#include "ship/resource/ResourceLoader.h"
#include "ship/resource/ResourceManager.h"

#include <zip.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}

void PutU32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(uint8_t(value >> shift));
    }
}

// OTEX v1: the 64-byte resource header, then type, width, height, flags, hByteScale, vPixelScale,
// size and the pixels. Type 2 is RGBA16, two bytes per texel.
std::vector<uint8_t> Otex(uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes(64, 0);
    bytes[1] = 1;
    bytes[4] = 'X';
    bytes[5] = 'E';
    bytes[6] = 'T';
    bytes[7] = 'O';
    bytes[8] = 1;
    const uint32_t size = width * height * 2;
    for (uint32_t field : { 2u, width, height, 0u, std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(1.0f),
                            size }) {
        PutU32(bytes, field);
    }
    bytes.resize(bytes.size() + size, 0x5A);
    return bytes;
}

std::string WriteArchive(const std::filesystem::path& dir) {
    // zip_source_buffer does not copy, so the payloads must outlive zip_close.
    std::vector<std::pair<const char*, std::vector<uint8_t>>> entries = {
        { "fixture/tex", Otex(2, 2) },
        { "alt/fixture/tex", Otex(4, 2) },
        { "fixture/plain", Otex(8, 2) },
        { "textures/pack/both", Otex(2, 2) },
        { "alt/textures/pack/both", Otex(4, 2) },
        { "alt/textures/pack/altonly", Otex(4, 2) },
        { "textures/pack/packonly", Otex(2, 2) },
    };
    const auto path = (dir / "alt-fixture.o2r").string();
    int error = 0;
    zip_t* zip = zip_open(path.c_str(), ZIP_CREATE | ZIP_EXCL, &error);
    if (zip == nullptr) {
        throw std::runtime_error("cannot create fixture archive");
    }
    for (auto& [name, bytes] : entries) {
        zip_source_t* source = zip_source_buffer(zip, bytes.data(), bytes.size(), 0);
        if (source == nullptr || zip_file_add(zip, name, source, ZIP_FL_OVERWRITE) < 0) {
            zip_source_free(source);
            zip_discard(zip);
            throw std::runtime_error("cannot add fixture entry");
        }
    }
    if (zip_close(zip) != 0) {
        zip_discard(zip);
        throw std::runtime_error("cannot write fixture archive");
    }
    return path;
}

uint32_t WidthOf(Ship::ResourceManager& rm, const char* path) {
    auto tex = std::dynamic_pointer_cast<Fast::Texture>(rm.LoadResource(path));
    return (tex != nullptr) ? tex->Width : 0;
}

void Run(Ship::ResourceManager& rm) {
    Check(!rm.IsAltAssetsEnabled(), "alt assets start off");
    Check(WidthOf(rm, "fixture/tex") == 2, "off: the base texture loads");
    const auto plainBefore = rm.LoadResource("fixture/plain");
    Check(plainBefore != nullptr, "off: a texture without an alt/ entry loads");

    // The behavior the helper exists for: a cached base resource wins over its alt/ entry.
    rm.SetAltAssetsEnabled(true);
    Check(WidthOf(rm, "fixture/tex") == 2, "on, before the refresh: the cached base is still served");

    Check(GdxAltAssetsDirtyShadowed(rm) == 1, "the refresh marks only the shadowed resource");
    Check(WidthOf(rm, "fixture/tex") == 4, "on, after the refresh: the alt/ entry loads");
    Check(rm.LoadResource("fixture/plain") == plainBefore, "a texture without an alt/ entry keeps its resource");
    Check(GdxAltAssetsDirtyShadowed(rm) == 0, "a second refresh finds nothing cached to mark");

    // Off needs no refresh: the alt/ resource is cached under its own path.
    rm.SetAltAssetsEnabled(false);
    Check(WidthOf(rm, "fixture/tex") == 2, "off again: the base texture loads");
    Check(WidthOf(rm, "alt/fixture/tex") == 4, "the alt/ entry stays loadable by its own path");

    rm.SetAltAssetsEnabled(true);
    GdxAltAssetsDirtyShadowed(rm);
    Check(WidthOf(rm, "fixture/tex") == 4, "on again: the alt/ entry loads");
}

void PackChannel(Ship::ResourceManager& rm) {
    auto& am = *rm.GetArchiveManager();
    Check(GdxAltAssetsPackOverridePath(am, "both", false) == "textures/pack/both", "pack: off serves the pack entry");
    Check(GdxAltAssetsPackOverridePath(am, "both", true) == "alt/textures/pack/both", "pack: on prefers the alt/ entry");
    Check(GdxAltAssetsPackOverridePath(am, "altonly", false).empty(), "pack: an alt/-only entry is unused when off");
    Check(GdxAltAssetsPackOverridePath(am, "altonly", true) == "alt/textures/pack/altonly",
          "pack: an alt/-only entry is served when on");
    Check(GdxAltAssetsPackOverridePath(am, "packonly", true) == "textures/pack/packonly",
          "pack: on falls back to the pack entry");
    Check(GdxAltAssetsPackOverridePath(am, "missing", true).empty(), "pack: no entry gives no path");
    // The chosen path is what the bridge hands the renderer, so it has to load as it is.
    Check(WidthOf(rm, GdxAltAssetsPackOverridePath(am, "altonly", true).c_str()) == 4,
          "pack: the alt/ path loads as a texture");
}

} // namespace

// SDL2main renames main, and needs this signature.
int main(int, char**) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ctx = Ship::Context::CreateUninitializedInstance("Alt assets tests", "alt-assets-tests", "alt-tests.json");
    ctx->InitConfiguration();
    ctx->InitConsoleVariables();
    try {
        const auto dir = std::filesystem::temp_directory_path() /
                         ("gdx-alt-fixture-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(dir);
        ctx->InitResourceManager({ WriteArchive(dir) }, {}, 1, true);
        auto rm = ctx->GetResourceManager();
        rm->GetResourceLoader()->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(),
                                                         RESOURCE_FORMAT_BINARY, "Texture", 0x4F544558, 1);
        Run(*rm);
        PackChannel(*rm);
    } catch (const std::exception& ex) {
        std::printf("FAIL fixture: %s\n", ex.what());
        ++failures;
    }
    std::printf("alt assets failures: %d\n", failures);
    // Context teardown requires a real window, as in gdx_hd_texture_tests.
    std::_Exit(failures != 0);
}
