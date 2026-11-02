#include "gdx_machine_capture.h"
#include "gdx_machine_export.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "gdx_stb_image_write.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>
#include <thread>

using namespace gdx::machine;
struct MachineTestRange { const void* data; size_t bytes; int kind; };
extern "C" size_t GdxMachineTest_BuildNative(int assembled, MachineTestRange* ranges, size_t* counts);
namespace {
struct Cmd { uint32_t w0, pad = 0; uintptr_t w1; };
struct Vtx { int16_t x, y, z, flag, s, t; uint8_t rgba[4]; };
struct Fixture {
    Vtx vertices[64]{};
    uint16_t texture[8]{0xF801, 0x07C1, 0, 0, 0x003F, 0xFFFF, 0, 0};
    std::vector<Cmd> commands;
    std::vector<Cmd> nested;
    std::vector<uint8_t> narrow;
    Layout narrowLayout = Layout::Native;
    Layout dataLayout = Layout::Native;
    Fixture() {
        vertices[1].x = 10;
        vertices[2].y = 10;
        vertices[1].s = 64;
        vertices[2].t = 64;
        for (auto& v : vertices) {
            std::memset(v.rgba, 255, 4);
        }
        commands = {
            {0xFD100000, 0, reinterpret_cast<uintptr_t>(texture)},
            {0xF5100000, 0, 7u << 24},
            {0xF3000000, 0, (7u << 24) | (7u << 12)},
            {0xF5100200, 0, 0},
            {0xF2000000, 0, (4u << 12) | 4u},
            {0xD7000002, 0, 0xFFFFFFFF},
            {0xFC000000 | (6u << 20) | (1u << 15) | (7u << 12) | (7u << 9) | 4u,
             0, (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (7u << 15) |
                (7u << 12) | (6u << 9) | (7u << 6) | (7u << 3)},
            {0x01040080, 0, reinterpret_cast<uintptr_t>(vertices)},
            {0x05000204, 0, 0},
            {0xDF000000, 0, 0}
        };
    }
    static bool ReadFixture(void* user, uintptr_t address, Resource kind, size_t bytes, Span& out) {
        auto& f = *static_cast<Fixture*>(user);
        const auto set = [&](const void* p, size_t size, Layout layout) {
            const auto begin = reinterpret_cast<uintptr_t>(p);
            if (address < begin || address - begin > size || bytes > size - (address - begin)) {
                return false;
            }
            out = {reinterpret_cast<const uint8_t*>(address), size - (address - begin), layout};
            return true;
        };
        if (kind == Resource::Commands) {
            if (address == 0x3000000 && bytes <= f.narrow.size()) {
                out = {f.narrow.data(), f.narrow.size(), f.narrowLayout};
                return true;
            }
            if (set(f.nested.data(), f.nested.size() * sizeof(Cmd), Layout::Wide)) {
                return true;
            }
            return set(f.commands.data(), f.commands.size() * sizeof(Cmd), Layout::Wide);
        }
        if (kind == Resource::Vertices) {
            if (address == 0x3001000) {
                address = reinterpret_cast<uintptr_t>(f.vertices);
            }
            return set(f.vertices, sizeof(f.vertices), f.dataLayout);
        }
        if (address == 0x3002000) {
            address = reinterpret_cast<uintptr_t>(f.texture);
        }
        return set(f.texture, sizeof(f.texture), f.dataLayout);
    }
    Request RequestFor() {
        Request r;
        r.roots[0] = {reinterpret_cast<uintptr_t>(commands.data()), commands.size()};
        r.read = ReadFixture;
        r.user = this;
        return r;
    }
};

void MustFail(Fixture& f, const char* error) {
    const auto result = Capture(f.RequestFor());
    assert(!result.mesh);
    assert(result.error.find(error) != std::string::npos);
}

Cmd Combine(unsigned a, unsigned b, unsigned c, unsigned d, unsigned alpha = 5) {
    return {0xFC000000 | (a << 20) | (c << 15) | (7u << 12) | (7u << 9) | 4u,
            0, (b << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (d << 15) |
               (7u << 12) | (alpha << 9) | (7u << 6) | (7u << 3)};
}

void TestStateAndOwnership() {
    Fixture f;
    f.commands[6] = Combine(1, 5, 8, 5);
    auto request = f.RequestFor();
    request.environment = {16, 32, 64, 128};
    const auto result = Capture(request);
    assert(result.mesh);
    assert(result.mesh->pixels[0] == 255 && result.mesh->pixels[3] == 128);
    assert(std::abs(result.mesh->vertices[1].uv[0] - 65535.0f / 65536.0f) < 0.00001f);
    assert(result.mesh->vertices[2].normal[2] == 1);
    f.texture[0] = 0;
    f.vertices[1].x = 90;
    f.commands.clear();
    assert(result.mesh->vertices[1].position[0] == 10 && result.mesh->pixels[0] == 255);
    Cache cache;
    cache.Publish(0, result);
    auto retained = cache.Get(0);
    cache.Publish(0, {nullptr, "latest capture failed"});
    assert(!cache.Get(0).mesh && !cache.Get(0).error.empty());
    assert(retained.mesh && retained.mesh->faces.size() == 1);
    assert(!cache.Get(30).mesh);
    std::thread publisher([&]() {
        for (int i = 0; i < 1000; ++i) {
            cache.Publish(0, i % 2 ? result : Result{nullptr, "failure"});
        }
    });
    for (int i = 0; i < 1000; ++i) {
        const auto current = cache.Get(0);
        assert(current.mesh || !current.error.empty());
    }
    publisher.join();
}

void TestMaterials() {
    Fixture paint;
    paint.commands[6] = Combine(15, 15, 31, 5);
    paint.nested = {{0xFB000000, 0, 0}, {0xDF000000, 0, 0}};
    auto paintRequest = paint.RequestFor();
    paintRequest.roots[2] = paintRequest.roots[0];
    paintRequest.roots[0] = {reinterpret_cast<uintptr_t>(paint.nested.data()), paint.nested.size()};
    paintRequest.environment = {32, 64, 96, 128};
    const auto painted = Capture(paintRequest);
    assert(painted.mesh && painted.mesh->pixels[0] == 32 && painted.mesh->pixels[3] == 128);
    const Cmd combines[]{Combine(15, 15, 31, 5), Combine(15, 5, 1, 5), Combine(6, 15, 1, 7, 6),
                         Combine(3, 5, 1, 5), Combine(1, 5, 8, 5)};
    const unsigned expected[]{80, 0, 255, 255, 255};
    for (unsigned i = 0; i < 5; ++i) {
        Fixture f;
        f.commands[6] = combines[i];
        auto r = f.RequestFor();
        r.environment = {80, 40, 20, 99};
        auto result = Capture(r);
        assert(result.mesh && result.mesh->pixels[0] == expected[i]);
        assert(result.mesh->pixels[3] == (i == 2 ? 255 : 99));
    }
    Fixture ia;
    ia.texture[0] = 0x0F1E;
    ia.commands[0].w0 = 0xFD700000;
    ia.commands[1].w0 = 0xF5700000;
    ia.commands[3].w0 = 0xF5600200;
    ia.commands[6] = Combine(1, 5, 8, 5);
    auto r = ia.RequestFor();
    r.environment = {0, 0, 0, 255};
    const auto result = Capture(r);
    assert(result.mesh);
    const uint8_t byte = reinterpret_cast<const uint8_t*>(ia.texture)[0];
    const int nibble = byte >> 4;
    const int intensity = (nibble >> 1) * 255 / 7;
    assert(result.mesh->pixels[0] == ((nibble & 1) ? intensity : 0));

    Fixture tiles;
    tiles.commands[3].w1 = 0x00000300;
    tiles.commands.insert(tiles.commands.end() - 1, {0xF5100200, 0, 1u << 24});
    tiles.commands.insert(tiles.commands.end() - 1, {0xF2000000, 0, (1u << 24) | (4u << 12) | 4u});
    tiles.commands.insert(tiles.commands.end() - 1, {0xD7000102, 0, 0x80008000});
    tiles.commands.insert(tiles.commands.end() - 1, {0x05000204, 0, 0});
    const auto two = Capture(tiles.RequestFor());
    assert(two.mesh && two.mesh->faces.size() == 2 && two.mesh->materials.size() == 2);
    assert(two.mesh->materials[0].wrapS == 3 && two.mesh->materials[1].wrapS == 0);
    assert(std::abs(two.mesh->vertices[4].uv[0] - 0.5f) < 0.00001f);

    Fixture shift;
    shift.commands[3].w1 = 1;
    shift.commands[4].w0 |= 4u << 12;
    shift.commands[4].w1 = (8u << 12) | 4u;
    auto shifted = Capture(shift.RequestFor());
    assert(shifted.mesh && std::abs(shifted.mesh->vertices[1].uv[0]) < 0.0001f);
}

void TestListsAndFailures() {
    Fixture f;
    f.nested = {f.commands[8], f.commands[9]};
    f.commands[8] = {0xDE000000, 0, reinterpret_cast<uintptr_t>(f.nested.data())};
    assert(Capture(f.RequestFor()).mesh);
    f.nested[0] = {0xDE000000, 0, reinterpret_cast<uintptr_t>(f.commands.data())};
    MustFail(f, "Cyclic");
    Fixture invalid;
    invalid.commands[8].w0 = 0x05000280;
    MustFail(invalid, "unloaded");
    invalid.commands[8].w0 = 0x05000203;
    MustFail(invalid, "unloaded");
    invalid.commands[8].w0 = 0xDA000000;
    MustFail(invalid, "Unsupported");
    invalid.commands[7].w1 = 1;
    MustFail(invalid, "unavailable");
    Fixture noEnd;
    noEnd.commands.pop_back();
    MustFail(noEnd, "no end");
    Fixture tail;
    tail.nested = {tail.commands[8], tail.commands[9]};
    tail.commands[8] = {0xDE010000, 0, reinterpret_cast<uintptr_t>(tail.nested.data())};
    tail.commands.pop_back();
    assert(Capture(tail.RequestFor()).mesh);
    Fixture depth;
    depth.nested.resize(130);
    for (size_t i = 0; i < 64; ++i) {
        depth.nested[i * 2] = {0xDE000000, 0, reinterpret_cast<uintptr_t>(&depth.nested[i * 2 + 2])};
        depth.nested[i * 2 + 1] = {0xDF000000, 0, 0};
    }
    depth.commands[8] = {0xDE000000, 0, reinterpret_cast<uintptr_t>(depth.nested.data())};
    MustFail(depth, "nesting");
    Fixture badVertex;
    badVertex.commands[7].w0 = 0x01041080;
    MustFail(badVertex, "vertex load");
    Fixture badTexture;
    badTexture.commands[2].w1 |= 4095u << 12;
    MustFail(badTexture, "TMEM");
    Fixture badMaterial;
    badMaterial.commands[6].w0 = 0xFC000000;
    MustFail(badMaterial, "combiner");
    Fixture materialBudget;
    materialBudget.commands.pop_back();
    for (unsigned i = 0; i <= MaxMaterials; ++i) {
        materialBudget.commands.push_back({0xFB000000, 0, i});
        materialBudget.commands.push_back({0x05000204, 0, 0});
    }
    materialBudget.commands.push_back({0xDF000000, 0, 0});
    MustFail(materialBudget, "material/pixel budget");
    Fixture tooMany;
    tooMany.commands.insert(tooMany.commands.end() - 1, MaxFaces, tooMany.commands[8]);
    MustFail(tooMany, "triangle budget");
    Fixture commands;
    commands.commands.assign(MaxCommands + 1, {0, 0, 0});
    MustFail(commands, "extent");
    Fixture lighting;
    lighting.vertices[0].rgba[2] = 127;
    lighting.commands.insert(lighting.commands.begin() + 7, {0xD9FFFFFF, 0, 0x20000});
    const auto lit = Capture(lighting.RequestFor());
    assert(lit.mesh && lit.mesh->vertices[0].normal[2] > 0.99f && lit.mesh->vertices[0].shade[0] == 255);
    Fixture bigData;
    bigData.dataLayout = Layout::BigEndian;
    for (auto& vertex : bigData.vertices) {
        auto* bytes = reinterpret_cast<uint8_t*>(&vertex);
        for (size_t offset = 0; offset < 12; offset += 2) {
            std::swap(bytes[offset], bytes[offset + 1]);
        }
    }
    for (auto& color : bigData.texture) {
        color = uint16_t((color << 8) | (color >> 8));
    }
    const auto bigResult = Capture(bigData.RequestFor());
    assert(bigResult.mesh && bigResult.mesh->vertices[1].position[0] == 10 && bigResult.mesh->pixels[0] == 255);

    for (const auto layout : {Layout::Native, Layout::BigEndian}) {
        Fixture narrow;
        narrow.narrowLayout = layout;
        for (size_t i = 0; i < narrow.commands.size(); ++i) {
            const uint32_t words[2]{narrow.commands[i].w0, i == 0 ? 0x3002000u :
                                  i == 7 ? 0x3001000u : uint32_t(narrow.commands[i].w1)};
            for (uint32_t word : words) {
                if (layout == Layout::BigEndian) {
                    for (int byte = 3; byte >= 0; --byte) {
                        narrow.narrow.push_back(uint8_t(word >> (byte * 8)));
                    }
                } else {
                    const auto* bytes = reinterpret_cast<const uint8_t*>(&word);
                    narrow.narrow.insert(narrow.narrow.end(), bytes, bytes + 4);
                }
            }
        }
        auto request = narrow.RequestFor();
        request.roots[0] = {0x3000000, narrow.commands.size()};
        const auto result = Capture(request);
        assert(result.mesh && result.mesh->faces.size() == 1);
    }
}

void Benchmark() {
    Fixture f;
    f.commands.insert(f.commands.end() - 1, 511, f.commands[8]);
    auto request = f.RequestFor();
    for (int i = 0; i < 32; ++i) {
        assert(Capture(request).mesh);
    }
    const auto begin = std::chrono::steady_clock::now();
    size_t retained = 0;
    for (int i = 0; i < 200; ++i) {
        const auto result = Capture(request);
        assert(result.mesh && result.mesh->faces.size() == 512);
        retained = result.mesh->RetainedBytes();
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 200;
    std::printf("synthetic-machine-512: warmup=32 samples=200 mean_us=%.3f retained_bytes=%zu\n", us, retained);
}

Result NativeFixture(int assembled) {
    MachineTestRange ranges[7];
    size_t counts[3];
    assert(GdxMachineTest_BuildNative(assembled, ranges, counts) == 7);
    Request request;
    for (int i = 0; i < 3; ++i) {
        request.roots[i] = {reinterpret_cast<uintptr_t>(ranges[i].data), counts[i]};
    }
    request.user = ranges;
    request.read = [](void* user, uintptr_t address, Resource kind, size_t bytes, Span& span) {
        const auto* ranges = static_cast<MachineTestRange*>(user);
        for (int i = 0; i < 7; ++i) {
            const auto begin = reinterpret_cast<uintptr_t>(ranges[i].data);
            if (ranges[i].kind == int(kind) && address >= begin && address - begin <= ranges[i].bytes &&
                bytes <= ranges[i].bytes - (address - begin)) {
                span = {reinterpret_cast<const uint8_t*>(address), ranges[i].bytes - (address - begin),
                        kind == Resource::Commands ? Layout::Wide : Layout::Native};
                return true;
            }
        }
        return false;
    };
    return Capture(request);
}

void TestNativeMacros() {
    for (int assembled = 0; assembled < 2; ++assembled) {
        const auto captured = NativeFixture(assembled);
        if (!captured.mesh) {
            std::fprintf(stderr, "native macro capture: %s\n", captured.error.c_str());
        }
        assert(captured.mesh && captured.mesh->faces.size() == (assembled ? 6 : 4));
        assert(captured.mesh->materials.size() == (assembled ? 3 : 2));
        assert(captured.mesh->materials[0].wrapS == 2);
        assert(captured.mesh->materials[1].wrapS == 1);
        assert(captured.mesh->pixels[0] == 255 && captured.mesh->pixels[1] == 0);
    }
}

void TestExportErrors(const Mesh& mesh) {
    std::atomic<bool> cancel{true};
    std::atomic<unsigned> progress{0};
    const auto path = std::filesystem::temp_directory_path() / "gdx-machine-cancel-must-not-create";
    assert(!Export(mesh, path, 0, cancel, progress).success);
    cancel.store(false);
    auto invalid = mesh;
    invalid.faces[0].material = 999;
    assert(!Export(invalid, path, 0, cancel, progress).success);
    invalid = mesh;
    invalid.materials[0].pixels = 0xFFFFFFFF;
    assert(!Export(invalid, path, 0, cancel, progress).success);
    assert(!Export(mesh, path, -1, cancel, progress).success);
}
}

int main(int argc, char** argv) {
    Fixture f;
    auto request = f.RequestFor();
    if (argc == 3) {
        f.commands[6] = Combine(1, 5, 8, 5);
        f.commands[3].w1 = (1u << 8) | (2u << 18);
        f.vertices[0].rgba[0] = 128;
        f.vertices[0].rgba[3] = 0;
        request.environment[3] = 128;
    }
    auto result = Capture(request);
    if (argc == 3 && std::strcmp(argv[1], "--export-native-fixture") == 0) {
        result = NativeFixture(1);
    }
    if (!result.mesh) {
        std::fprintf(stderr, "capture failed: %s\n", result.error.c_str());
    }
    assert(result.mesh);
    if (argc != 3 || std::strcmp(argv[1], "--export-native-fixture") != 0) {
        assert(result.mesh->faces.size() == 1);
        assert(result.mesh->vertices[1].position[0] == 10);
        assert(result.mesh->materials.size() == 1);
        assert(result.mesh->pixels[0] == 255 && result.mesh->pixels[1] == 0);
    }
    if (argc == 3) {
        std::atomic<bool> cancel{false};
        std::atomic<unsigned> progress{0};
        const auto exported = Export(*result.mesh, argv[2], 0, cancel, progress);
        if (std::strcmp(argv[1], "--export-error") == 0) {
            assert(!exported.success && !exported.error.empty());
        } else {
            assert(exported.success && progress.load() == 100);
            std::puts(exported.directory.string().c_str());
        }
        return 0;
    }
    TestStateAndOwnership();
    TestMaterials();
    TestListsAndFailures();
    TestNativeMacros();
    TestExportErrors(*result.mesh);
    Benchmark();
    std::puts("machine capture tests passed");
}
