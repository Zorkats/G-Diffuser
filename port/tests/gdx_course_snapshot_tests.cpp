#include "gdx_recovery_file.h"
#include "gdx_course_snapshot.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <map>

namespace {
using namespace gdx::snapshot;

uint32_t binding(const Generation& g) {
    return gdx_course_registry_crc32(reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
}

void metadata(Generation& g, bool longCourse) {
    Bytes buffer(65536);
    auto store = [&](Metadata section, int count) {
        assert(count > 0);
        g.metadata[size_t(section)].assign(buffer.begin(), buffer.begin() + count);
    };
    GdxCourseExtRecord music{};
    music.kind = GDX_COURSE_KIND_BGM;
    music.scope = GDX_COURSE_SCOPE_COURSE;
    music.fallbackId = 0;
    std::memcpy(music.key, "bgm.custom.snapshot", sizeof("bgm.custom.snapshot"));
    store(Metadata::Extensions, gdx_course_ext_encode(&music, 1, binding(g), buffer.data(), buffer.size()));
    store(Metadata::Title, gdx_course_title_encode("Snapshot test", binding(g), buffer.data(), buffer.size()));
    GdxCourseSettings settings{};
    settings.ringCount = g.pointCount;
    settings.magnetRange = 200;
    if (g.pointCount) {
        settings.ring[g.pointCount - 1].pitWidth = 100;
    }
    store(Metadata::Settings, gdx_settings_encode(&settings, binding(g), buffer.data(), buffer.size()));
    if (longCourse) {
        GdxBranchSet forks{};
        forks.ringCount = g.pointCount;
        forks.extendedCount = g.pointCount - 64;
        forks.baseCrc = gdx_course_registry_crc32(g.base.data(), CourseBytes);
        std::memcpy(&forks.courseChecksum, g.base.data() + 4, 4);
        for (int i = 0; i < forks.extendedCount; ++i) {
            auto& p = forks.extended[i];
            p.pos[0] = float(i * 10);
            p.pos[1] = 250.0f;
            p.pos[2] = float(i * 17);
            p.radiusLeft = 100;
            p.radiusRight = 200;
            p.bankAngle = int16_t(i * 3);
            p.pit = -1;
            p.dash = -1;
            p.dirt = -1;
            p.ice = -1;
            p.jump = -1;
            p.landmine = -1;
            p.gate = -1;
            p.building = -1;
            p.sign = -1;
        }
        store(Metadata::Forks, gdx_branch_encode(&forks, binding(g), buffer.data(), buffer.size()));
    }
}

Generation fixture(int count = 100, unsigned char version = 1) {
    Generation g;
    g.disk = 0x1020304050607080ULL;
    g.directory = 3;
    g.courseId = "FORK TEST.CRSD";
    g.pointCount = uint8_t(count);
    g.base.assign(BaseBytes, 0);
    g.base[1] = uint8_t((std::min)(count, 64));
    g.base[4] = version;
    metadata(g, count > 64);
    return g;
}

void same(const Generation& a, const Generation& b) {
    assert(a.disk == b.disk && a.directory == b.directory && a.courseId == b.courseId);
    assert(a.pointCount == b.pointCount && a.nativeChecksum == b.nativeChecksum &&
           a.base == b.base && a.metadata == b.metadata);
}

struct Memory final : Backend {
    std::map<std::string, Bytes> files;
    int calls = 0;
    int fail = -1;
    bool afterWrite = false;
    bool corruptWrite = false;
    Result read(const std::string& name, Bytes& out) override {
        if (calls++ == fail) {
            return Result::Io;
        }
        const auto it = files.find(name);
        if (it == files.end()) {
            return Result::Missing;
        }
        out = it->second;
        return Result::Ok;
    }
    bool publish(const std::string& name, const Bytes& bytes) override {
        const bool failed = calls++ == fail;
        if (failed && !afterWrite) {
            return false;
        }
        if (!files.emplace(name, bytes).second) {
            return false;
        }
        if (corruptWrite) {
            files[name].back() ^= 1;
        }
        return !failed;
    }
};

void codecTests() {
    for (int n = 0; n <= 127; ++n) {
        const auto original = fixture(n);
        Bytes bytes;
        assert(encode(original, bytes));
        Generation decoded;
        assert(decode(bytes, decoded));
        same(original, decoded);
        Bytes again;
        assert(encode(decoded, again) && again == bytes);
    }
    const auto original = fixture(127);
    Bytes bytes;
    assert(encode(original, bytes));
    for (size_t i = 0; i < bytes.size(); i += i < 64 ? 1 : 337) {
        auto corrupt = bytes;
        corrupt[i] ^= 1;
        auto untouched = fixture(65);
        assert(!decode(corrupt, untouched));
        same(untouched, fixture(65));
    }
    for (size_t n = 0; n < bytes.size(); n += n < 68 ? 1 : 991) {
        const Bytes truncated(bytes.begin(), bytes.begin() + n);
        Generation decoded;
        assert(!decode(truncated, decoded));
    }
    auto bad = original;
    bad.pointCount = 128;
    Bytes out{9, 8, 7};
    assert(!encode(bad, out) && out == Bytes({9, 8, 7}));
    bad = original;
    bad.metadata[size_t(Metadata::Forks)].clear();
    assert(!encode(bad, out));
    bad = original;
    bad.base[20] ^= 1;
    assert(!encode(bad, out));
    bad = original;
    bad.courseId = "../ESCAPE.CRSD";
    assert(!encode(bad, out));
    bad = original;
    bad.disk = 0;
    assert(!encode(bad, out));
    bad = original;
    bad.metadata[size_t(Metadata::Settings)].back() ^= 1;
    assert(!encode(bad, out));
    bad = original;
    bad.base.resize(BaseBytes - 1);
    assert(!encode(bad, out));
}

void repositoryTests() {
    const auto first = fixture(100, 1);
    const auto second = fixture(127, 2);
    Memory memory;
    assert(publish(memory, first) == Result::Ok);
    assert(publish(memory, first) == Result::Ok);
    assert(publish(memory, second) == Result::Ok);
    Generation loaded;
    assert(resolve(memory, first.disk, first.directory, first.courseId, first.base, loaded) == Result::Ok);
    same(first, loaded);
    assert(resolve(memory, second.disk, second.directory, second.courseId, second.base, loaded) == Result::Ok);
    same(second, loaded);
    auto conflict = first;
    conflict.metadata[size_t(Metadata::Title)].clear();
    assert(publish(memory, conflict) == Result::Conflict);
    assert(resolve(memory, first.disk, first.directory, first.courseId, first.base, loaded) == Result::Ok);
    same(first, loaded);
    assert(resolve(memory, first.disk + 1, first.directory, first.courseId, first.base, loaded) == Result::Missing);
    same(first, loaded);
    assert(resolve(memory, first.disk, first.directory + 1, first.courseId, first.base, loaded) == Result::Missing);
    auto corrupt = memory;
    auto& candidate = corrupt.files[key(first.disk, first.directory, first.courseId, first.base)];
    Bytes wrong;
    assert(encode(second, wrong));
    candidate = wrong;
    assert(resolve(corrupt, first.disk, first.directory, first.courseId, first.base, loaded) == Result::Conflict);
    same(first, loaded);
    candidate.back() ^= 1;
    assert(resolve(corrupt, first.disk, first.directory, first.courseId, first.base, loaded) == Result::Invalid);

    Memory baseline;
    assert(publish(baseline, first) == Result::Ok);
    baseline.calls = 0;
    auto successful = baseline;
    assert(publish(successful, second) == Result::Ok);
    const int boundaries = successful.calls;
    for (int point = 0; point < boundaries; ++point) {
        for (bool after : {false, true}) {
            auto interrupted = baseline;
            interrupted.fail = point;
            interrupted.afterWrite = after;
            auto base = first.base;
            if (publish(interrupted, second) == Result::Ok) {
                base = second.base;
            }
            interrupted.calls = 0;
            interrupted.fail = -1;
            assert(resolve(interrupted, first.disk, first.directory, first.courseId, base, loaded) == Result::Ok);
            same(base == first.base ? first : second, loaded);
            assert(publish(interrupted, second) == Result::Ok);
            assert(resolve(interrupted, second.disk, second.directory, second.courseId, second.base, loaded) == Result::Ok);
            same(second, loaded);
        }
    }
    auto silent = baseline;
    silent.corruptWrite = true;
    assert(publish(silent, second) != Result::Ok);
    std::printf("Snapshot failure injection: %d publication boundaries, before/after write.\n", boundaries);
}
}

static void packageTests() {
    using namespace gdx::snapshot;
    std::vector<Generation> members;
    for (int i = 0; i < 6; ++i) {
        auto g = fixture(65 + i * 12);
        assert(seal(g));
        assert(rebind(g, g.disk, g.directory, "MEMBER" + std::to_string(i) + ".CRSD"));
        g.base[CourseBytes + 7] = 0x32;
        assert(seal(g));
        assert(portable(g, true));
        assert(g.disk == 1 && g.directory == 0 && g.base[CourseBytes + 7] == 0);
        members.push_back(g);
    }
    Bytes bytes;
    assert(packageEncode(members, 123, bytes));
    std::vector<Generation> restored;
    assert(packageDecode(bytes, 123, restored));
    assert(restored.size() == 6);
    for (size_t i = 0; i < members.size(); ++i) { same(members[i], restored[i]); }
    const auto original = bytes;
    assert(!packageDecode(bytes, 124, restored));
    assert(restored.size() == 6);
    for (size_t i = 0; i < bytes.size(); i += 97) {
        bytes = original;
        bytes[i] ^= 1;
        assert(!packageDecode(bytes, 123, restored));
    }
    bytes = original;
    for (size_t size : {size_t(0), size_t(15), original.size() - 1}) {
        Bytes truncated(bytes.begin(), bytes.begin() + size);
        assert(!packageDecode(truncated, 123, restored));
    }
    members.push_back(members[0]);
    assert(!packageEncode(members, 123, bytes));
    members.resize(2);
    members[1] = members[0];
    assert(!packageEncode(members, 123, bytes));
    members.clear();
    assert(!packageEncode(members, 123, bytes));
}

int main() {
    packageTests();
    {
        auto first = fixture();
        assert(seal(first));
        assert(first.base[8] & 0x80);
        auto moved = first;
        assert(rebind(moved, 0x88776655, 7, "RENAMED.CRSE"));
        assert(moved.base == first.base && moved.nativeChecksum == first.nativeChecksum);
        Bytes movedBytes;
        assert(encode(moved, movedBytes));
        Generation decoded;
        assert(decode(movedBytes, decoded));
        same(moved, decoded);
        auto invalid = moved;
        assert(!rebind(invalid, 0, 0, "../bad.CRSD"));
        same(invalid, moved);
        auto second = first;
        Bytes title(512);
        int n = gdx_course_title_encode("A changed title", binding(second), title.data(), title.size());
        assert(n > 0);
        second.metadata[size_t(Metadata::Title)].assign(title.begin(), title.begin() + n);
        assert(seal(second));
        assert(first.base != second.base);
        Memory backend;
        assert(publish(backend, first) == Result::Ok);
        assert(publish(backend, second) == Result::Ok);
        assert(publish(backend, moved) == Result::Ok);
        Generation selected;
        assert(resolve(backend, moved.disk, moved.directory, moved.courseId, moved.base, selected) == Result::Ok);
        same(moved, selected);
        assert(resolve(backend, moved.disk, moved.directory, first.courseId, moved.base, selected) == Result::Missing);
        Generation loaded;
        assert(resolve(backend, first.disk, first.directory, first.courseId, first.base, loaded) == Result::Ok);
        same(first, loaded);
    }
    namespace fs = std::filesystem;
    using namespace gdx::recovery;
    const auto root = fs::temp_directory_path() /
        ("gdx-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    const auto path = root / "generation.gcs";
    const Bytes first{1, 2, 3};
    const Bytes second{4, 5, 6};
    assert(publishFile(path, first));
    assert(!publishFile(path, second));
    Bytes loaded;
    assert(readFile(path, loaded));
    assert(loaded == first);
    assert(fs::remove(path));
    codecTests();
    repositoryTests();
    {
        gdx::snapshot::Files files(root);
        const auto firstGeneration = fixture();
        assert(gdx::snapshot::publish(files, firstGeneration) == gdx::snapshot::Result::Ok);
        const auto name = gdx::snapshot::key(firstGeneration.disk, firstGeneration.directory,
                                             firstGeneration.courseId, firstGeneration.base);
        {
            gdx::snapshot::Files restarted(root);
            gdx::snapshot::Generation resolved;
            assert(gdx::snapshot::resolve(restarted, firstGeneration.disk, firstGeneration.directory,
                                         firstGeneration.courseId, firstGeneration.base, resolved) ==
                   gdx::snapshot::Result::Ok);
            same(firstGeneration, resolved);
        }
        Bytes ignored;
        assert(files.read("../escape.gcs", ignored) == gdx::snapshot::Result::Io);
        assert(!files.publish("../escape.gcs", {1}));
        assert(fs::remove(root / name));
    }
    assert(fs::remove(root));
    std::puts("All immutable course snapshot tests passed");
}
