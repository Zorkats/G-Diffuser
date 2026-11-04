#include "gdx_course_snapshot_state.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <fstream>
#include <iterator>

extern "C" void* GdxSnapshotTest_Create(int count);
extern "C" int GdxSnapshotTest_InvalidValues(int which, int index);

namespace {
struct Memory : gdx::snapshot::Backend {
    std::map<std::string, gdx::snapshot::Bytes> files;
    int fail = -1;
    int calls = 0;
    bool after = false;
    gdx::snapshot::Result read(const std::string& name, gdx::snapshot::Bytes& out) override {
        using gdx::snapshot::Result;
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
    bool publish(const std::string& name, const gdx::snapshot::Bytes& bytes) override {
        const bool failed = calls++ == fail;
        if (failed && !after) {
            return false;
        }
        return files.emplace(name, bytes).second && !failed;
    }
};

bool metadataHistoryTests() {
    using namespace gdx::snapshot;
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    Generation first;
    first.disk = 1;
    first.courseId = "GENERATED.CRSD";
    auto* runtime = GdxSnapshotTest_Create(10);
    assert(gdx_course_title_set(first.courseId.c_str(), "Alpha") == 0);
    assert(captureState(first, runtime));
    Memory backend;
    assert(publish(backend, first) == Result::Ok);
    const auto history = backend.files;
    assert(gdx_course_title_set(first.courseId.c_str(), "Bravo") == 0);
    auto second = first;
    assert(captureState(second, runtime));
    assert(first.metadata != second.metadata);
    if (first.base == second.base || publish(backend, second) != Result::Ok) {
        std::fputs("Changed same-length title reuses a historical snapshot key\n", stderr);
        return false;
    }
    for (const auto& entry : history) {
        assert(backend.files.at(entry.first) == entry.second);
    }
    Bytes restored(gdx_course_snapshot_runtime_bytes());
    auto selected = first;
    assert(loadState(backend, selected, first.base, restored.data()) == Result::Ok);
    char title[128]{};
    assert(gdx_course_title_get(first.courseId.c_str(), title, sizeof(title)));
    assert(std::strcmp(title, "Alpha") == 0);
    assert(loadState(backend, selected, second.base, restored.data()) == Result::Ok);
    assert(gdx_course_title_get(first.courseId.c_str(), title, sizeof(title)));
    assert(std::strcmp(title, "Bravo") == 0);
    assert(backend.files.size() == 2);
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    return true;
}

void legacyVersionTests() {
    using namespace gdx::snapshot;
    for (int count : {10, 100}) {
        gdx_course_store_clear();
        gdx_branch_store_clear();
        gdx_settings_store_clear();
        const auto path = std::filesystem::path(GDX_SNAPSHOT_TEST_SOURCE_DIR) /
            ("gdx_course_snapshot_v1_" + std::to_string(count) + ".gcs");
        std::ifstream input(path, std::ios::binary);
        assert(input);
        const Bytes original((std::istreambuf_iterator<char>(input)), {});
        Generation legacy;
        assert(decode(original, legacy) && legacy.version == 1 && legacy.pointCount == count);
        Bytes encoded;
        assert(encode(legacy, encoded) && encoded == original);
        Bytes runtime(gdx_course_snapshot_runtime_bytes());
        assert(restoreState(legacy, runtime.data()));
        char title[128]{};
        assert(gdx_course_title_get(legacy.courseId.c_str(), title, sizeof(title)));
        assert(std::strcmp(title, "Alpha") == 0);
        Memory backend;
        assert(publish(backend, legacy) == Result::Ok);
        const auto history = backend.files;
        auto upgraded = legacy;
        assert(prepareSave(backend, upgraded, runtime.data(), legacy.base) == Result::Ok);
        assert(upgraded.version == 2 && upgraded.base != legacy.base);
        assert(encode(upgraded, encoded));
        Generation decoded;
        assert(decode(encoded, decoded) && decoded.version == 2);
        for (const auto& entry : history) {
            assert(backend.files.at(entry.first) == entry.second);
        }
        auto selected = legacy;
        assert(loadState(backend, selected, legacy.base, runtime.data()) == Result::Ok);
        assert(selected.version == 1);
        assert(loadState(backend, selected, upgraded.base, runtime.data()) == Result::Ok);
        assert(selected.version == 2);
        auto moved = legacy;
        assert(rebind(moved, 999, 7, "COPIED.CRSD") && moved.base == legacy.base && moved.version == 1);
        assert(portable(moved, true) && moved.version == 2);
        assert(packageEncode({moved}, 123, encoded));
        std::vector<Generation> members;
        assert(packageDecode(encoded, 123, members) && members.size() == 1);
        assert(members.front().version == 2);
        assert(restoreState(members.front(), runtime.data()));
    }
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
}

void publicationTests() {
    using namespace gdx::snapshot;
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    Generation identity;
    identity.disk = 7;
    identity.directory = 3;
    identity.courseId = "RECOVER.CRSD";
    auto* runtime = GdxSnapshotTest_Create(64);
    Bytes legacy(BaseBytes);
    GdxBranchEditPoint tail[63]{};
    assert(gdx_course_snapshot_capture(runtime, legacy.data(), tail));
    Bytes original(static_cast<unsigned char*>(runtime), static_cast<unsigned char*>(runtime) +
                   gdx_course_snapshot_runtime_bytes());
    Memory backend;
    Bytes restored(original.size(), 0x39);
    assert(loadState(backend, identity, legacy, restored.data()) == Result::Ok);
    assert(restored == original && identity.base == legacy);
    Generation originalBackup;
    assert(resolve(backend, identity.disk, identity.directory, identity.courseId, legacy, originalBackup) == Result::Ok);
    assert(gdx_course_title_set(identity.courseId.c_str(), "New generation") == 0);
    const auto checkpoint = backend.files;
    for (int failure = 0; failure < 4; ++failure) {
        for (const bool after : {false, true}) {
            backend.files = checkpoint;
            backend.calls = 0;
            backend.fail = failure;
            backend.after = after;
            auto next = identity;
            auto status = prepareSave(backend, next, GdxSnapshotTest_Create(100), legacy);
            const auto selectedBase = status == Result::Ok ? next.base : legacy;
            backend.fail = -1;
            Memory restarted;
            restarted.files = backend.files;
            Generation selected = identity;
            Bytes selectedRuntime(original.size(), 0x4B);
            assert(loadState(restarted, selected, selectedBase, selectedRuntime.data()) == Result::Ok);
            assert(selected.pointCount == (status == Result::Ok ? 100 : 64));
            Generation old;
            assert(resolve(restarted, identity.disk, identity.directory, identity.courseId, legacy, old) == Result::Ok);
            assert(old.metadata == originalBackup.metadata);
        }
    }
    Memory absent;
    auto refused = identity;
    assert(prepareSave(absent, refused, GdxSnapshotTest_Create(100), legacy) == Result::Missing);
    assert(absent.files.empty() && refused.base == legacy);
    backend.files = checkpoint;
    backend.fail = -1;
    auto next = identity;
    assert(prepareSave(backend, next, GdxSnapshotTest_Create(100), legacy) == Result::Ok);
    Generation missing = next;
    restored.assign(restored.size(), 0x4A);
    assert(loadState(absent, missing, next.base, restored.data()) == Result::Missing);
    assert(restored == Bytes(restored.size(), 0x4A));
}
}

int main() {
    using namespace gdx::snapshot;
    if (!metadataHistoryTests()) {
        return 1;
    }
    for (int which = 0; which < 5; ++which) {
        for (int index : {0, 63, 64, 126}) {
            assert(GdxSnapshotTest_InvalidValues(which, index) == 3);
        }
    }
    legacyVersionTests();
    publicationTests();
    constexpr const char* id = "ROUNDTRIP.CRSD";
    for (int count = 0; count <= 127; ++count) {
        gdx_course_store_clear();
        gdx_branch_store_clear();
        gdx_settings_store_clear();
        assert(gdx_course_title_set(id, "Original title") == 0);
        GdxCourseExtRecord music{};
        music.kind = GDX_COURSE_KIND_BGM;
        music.scope = GDX_COURSE_SCOPE_COURSE;
        std::strcpy(music.key, "bgm.custom.snapshot");
        assert(gdx_course_ext_restore(id, &music, 1) == 0);
        GdxCourseSettings settings{};
        settings.ringCount = uint8_t(count);
        settings.magnetRange = 123;
        if (count) {
            settings.ring[count - 1].pitWidth = 75;
        }
        if (count > 73 && count < 127) {
            GdxBranchSet forks{};
            forks.ringCount = uint8_t(count);
            forks.extendedCount = uint8_t(count - 64);
            forks.forkCount = 1;
            auto& fork = forks.forks[0];
            fork.forkPoint = 70;
            fork.mergePoint = 72;
            fork.nodeCount = 1;
            fork.flags = GDX_BRANCH_FLAG_FIRST_VALUES;
            std::memset(fork.firstParts, -1, sizeof(fork.firstParts));
            fork.firstParts[0] = 2;
            fork.firstExtras.pitWidth = 95;
            fork.nodes[0].pos[0] = 100;
            fork.nodes[0].pos[1] = 5;
            fork.nodes[0].radiusLeft = 80;
            fork.nodes[0].radiusRight = 90;
            settings.nodes[0][0].pitWidth = 50;
            assert(gdx_branch_store_set(id, &forks) == 0);
        }
        assert(gdx_settings_store_set(id, &settings) == 0);
        void* runtime = GdxSnapshotTest_Create(count);
        const auto size = gdx_course_snapshot_runtime_bytes();
        Bytes original(static_cast<unsigned char*>(runtime), static_cast<unsigned char*>(runtime) + size);
        Generation generation;
        generation.disk = 7;
        generation.directory = 9;
        generation.courseId = id;
        assert(captureState(generation, runtime));
        assert(generation.pointCount == count);
        assert(generation.base[8] & 0x80);
        assert(gdx_course_title_set(id, "Later title") == 0);
        gdx_settings_store_clear();
        gdx_branch_store_clear();
        Bytes restored(size, 0x5A);
        assert(restoreState(generation, restored.data()));
        assert(restored == original);
        char title[128]{};
        assert(gdx_course_title_get(id, title, sizeof(title)));
        assert(std::strcmp(title, "Original title") == 0);
        GdxCourseSettings loaded{};
        assert(gdx_settings_store_get(id, &loaded));
        assert(std::memcmp(&loaded, &settings, sizeof(settings)) == 0);
        int records = 0;
        const auto* extras = gdx_course_ext_records(id, &records);
        assert(records == 1 && extras && std::memcmp(extras, &music, sizeof(music)) == 0);
        if (count > 73 && count < 127) {
            GdxBranchSet forks{};
            assert(gdx_branch_store_get(id, &forks));
            assert(forks.extendedCount == count - 64 && forks.forkCount == 1);
            assert(forks.forks[0].forkPoint == 70 && forks.forks[0].firstParts[0] == 2 &&
                   forks.forks[0].firstExtras.pitWidth == 95 && forks.forks[0].nodes[0].pos[0] == 100);
        }
        auto bad = generation;
        bad.nativeChecksum++;
        Bytes untouched(size, 0x3A);
        assert(!restoreState(bad, untouched.data()));
        assert(untouched == Bytes(size, 0x3A));
        assert(gdx_course_title_get(id, title, sizeof(title)));
        assert(std::strcmp(title, "Original title") == 0);
    }
    {
        Generation generation;
        generation.disk = 7;
        generation.directory = 9;
        generation.courseId = id;
        assert(captureState(generation, GdxSnapshotTest_Create(127)));
        gdx_settings_store_clear();
        GdxCourseSettings other{};
        other.ringCount = 5;
        other.magnetRange = 100;
        for (int i = 0; i < GDX_BRANCH_STORE_SLOTS; ++i) {
            char otherId[26];
            std::snprintf(otherId, sizeof(otherId), "FULL%d.CRSD", i);
            assert(gdx_settings_store_set(otherId, &other) == 0);
        }
        assert(gdx_course_title_set(id, "Unchanged on refusal") == 0);
        Bytes untouched(gdx_course_snapshot_runtime_bytes(), 0x39);
        assert(!restoreState(generation, untouched.data()));
        assert(untouched == Bytes(untouched.size(), 0x39));
        char title[128]{};
        assert(gdx_course_title_get(id, title, sizeof(title)));
        assert(std::strcmp(title, "Unchanged on refusal") == 0);
    }
    std::puts("All complete snapshot state tests passed");
}
