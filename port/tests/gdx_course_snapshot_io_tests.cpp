#include "gdx_course_snapshot_io.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_snapshot_state.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_dev_gates.h"
#include "gdx_recovery_metadata.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" void* GdxSnapshotTest_Create(int count);
#ifdef GDX_SNAPSHOT_MFS_TEST
#include "gdx_track_recovery_runtime.h"
extern "C" int GdxSnapshotMfsTest_Run(void);
extern "C" int GdxSnapshotMfsTest_RollbackFailure(void);
extern "C" int gMfsError;
void GdxSnapshotRecoveryTest_Run(const std::filesystem::path& root);
int GdxSnapshotRecoveryTest_RunChild(const char* executable, const char* root, bool relative);
#endif
namespace {
std::filesystem::path root;
unsigned long long disk = 17;
void* fiber = reinterpret_cast<void*>(1);
bool blocked = false;
bool relativeRecoveryPath = false;
}
extern "C" {
int gGdxDevGateCache[GDX_GATE_COUNT];
int gGdxDevGateCacheReady = 1;
void (*gdx_port_log_tap)(const char*) = nullptr;
unsigned long long gdx_disk_save_identity(void) { return disk; }
int gdx_disk_save_recovery_path(char* out, unsigned int capacity) {
    const auto value = relativeRecoveryPath ? std::string("./saves/test.gdd.recovery") :
        (root / "recovery").string();
    return std::snprintf(out, capacity, "%s", value.c_str()) > 0;
}
int gdx_course_boot(void) { return 0; }
int gdx_course_state_recovery_ready(void) { return 1; }
#ifndef GDX_SNAPSHOT_MFS_TEST
int gdx_recovery_blocks_writes(void) { return blocked; }
void* gdx_snapshot_native_thread(void) { return fiber; }
#endif
int gdx_course_music_export_key(const char*, const char*) { return 0; }
int gdx_course_music_adopt(const char*, const char* key, char* out, int cap) {
    std::snprintf(out, cap, "%s", key);
    return 1;
}
}

int main(int argc, char** argv) {
    using namespace gdx::snapshot;
    root = std::filesystem::temp_directory_path() / ("gdx-snapshot-io-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef GDX_SNAPSHOT_MFS_TEST
    if (argc < 3 || std::strcmp(argv[1], "--fixture-root") != 0) {
        assert(std::filesystem::create_directory(root));
        const int result = GdxSnapshotRecoveryTest_RunChild(argv[0], root.string().c_str(),
            argc == 2 && std::strcmp(argv[1], "--relative-path") == 0);
        // The runtime holds its disk lock until process exit, as it does in the game.
        std::filesystem::remove_all(root);
        return result;
    }
    root = argv[2];
    relativeRecoveryPath = argc == 4 && std::strcmp(argv[3], "--relative-path") == 0;
#else
    assert(std::filesystem::create_directory(root));
    relativeRecoveryPath = argc == 2 && std::strcmp(argv[1], "--relative-path") == 0;
#endif
    const auto originalDirectory = std::filesystem::current_path();
    if (relativeRecoveryPath) {
        assert(std::filesystem::create_directory(root / "saves"));
        std::filesystem::current_path(root);
    }
#ifndef GDX_SNAPSHOT_MFS_TEST
    assert(gdx_snapshot_begin(1));
    assert(gdx_snapshot_busy());
    fiber = reinterpret_cast<void*>(2);
    assert(!gdx_snapshot_begin(0) && gdx_snapshot_blocks_writes());
    gdx_snapshot_end();
    assert(gdx_snapshot_busy());
    fiber = reinterpret_cast<void*>(1);
    gdx_snapshot_end();
    blocked = true;
    assert(!gdx_snapshot_begin(1));
    blocked = false;
    assert(!gdx_snapshot_busy());
#endif
#ifdef GDX_SNAPSHOT_MFS_TEST
    GdxContentRecovery_RunOnDiskThread();
    assert(!gdx_recovery_blocks_writes());
#endif

    Bytes base(BaseBytes), restored(gdx_course_snapshot_runtime_bytes(), 0x31);
    GdxBranchEditPoint legacyTail[GDX_BRANCH_EDIT_MAX_POINTS - 64]{};
    assert(gdx_course_snapshot_capture(GdxSnapshotTest_Create(6), base.data(), legacyTail) == 7);
    assert(gdx_snapshot_load(3, "GHOST00", "GOST", base.data(), restored.data()));
    assert(restored[1] == 6);
    assert(gdx_course_title_set("LONG.CRSD", "Saved generation") == 0);
    assert(gdx_snapshot_prepare(3, "LONG", "CRSD", GdxSnapshotTest_Create(127), nullptr, base.data()));
    assert(base[8] & 0x80);
    float x[127]{}, z[127]{};
    GdxBranchSet previewForks{};
    assert(gdx_snapshot_preview(3, "LONG", base.data(), x, z, 127, &previewForks) == 127);
    assert(x[126] == 252 && z[126] == -126);
    assert(!gdx_snapshot_preview(3, "LONG", base.data(), x, z, 64, &previewForks));
    assert(!gdx_snapshot_preview(4, "LONG", base.data(), x, z, 127, &previewForks));

    assert(gdx_snapshot_validate(3, "LONG", "CRSD", base.data()));
    assert(!gdx_snapshot_validate(4, "LONG", "CRSD", base.data()));
    assert(!gdx_snapshot_validate(3, "OTHER", "CRSD", base.data()));
    assert(gdx_course_title_set("LONG.CRSD", "Unsaved live edit") == 0);
    const auto path = (root / "Track.GDXC").string();
    gdx_snapshot_export_begin();
    auto portableBase = base;
    assert(gdx_snapshot_export_member(3, "LONG", "CRSD", portableBase.data(), 1));
    assert(gdx_snapshot_export_write(path.c_str(), 0xA117));
    assert(gdx_snapshot_package_member(path.c_str(), 0xA117, "LONG", "CRSD", portableBase.data()));
    assert(!gdx_snapshot_package_member(path.c_str(), 0xA118, "LONG", "CRSD", portableBase.data()));
    assert(gdx_snapshot_import_stage(path.c_str(), 0xA117));
    disk = 18;
#ifdef GDX_SNAPSHOT_MFS_TEST
    GdxContentRecovery_RunOnDiskThread();
    assert(!gdx_recovery_blocks_writes());
#endif
    assert(!gdx_snapshot_validate(3, "LONG", "CRSD", base.data()));
    assert(gdx_snapshot_import_publish(7, "LONG", "CRSD", portableBase.data()));
    assert(gdx_snapshot_import_restore(7, "LONG", "CRSD", portableBase.data()));
    char title[GDX_COURSE_TITLE_MAX + 1]{};
    assert(gdx_course_title_get("LONG.CRSD", title, sizeof(title)));
    assert(std::strcmp(title, "Saved generation") == 0);
    assert(gdx_snapshot_load(7, "LONG", "CRSD", portableBase.data(), restored.data()));
    assert(restored[1] == 127);
    assert(gdx_snapshot_alias(7, "LONG", "CRSD", portableBase.data(), 8, "COPY", "CRSE"));
    assert(gdx_snapshot_load(8, "COPY", "CRSE", portableBase.data(), restored.data()));
    assert(!gdx_snapshot_import_stage(path.c_str(), 0));
    assert(!gdx_snapshot_import_publish(9, "LONG", "CRSD", portableBase.data()));
#ifdef GDX_SNAPSHOT_MFS_TEST
    const int nativeFailure = GdxSnapshotMfsTest_Run();
    if (nativeFailure) {
        std::fprintf(stderr, "Native MFS fixture failed at line %d (MFS error %d)\n", nativeFailure, gMfsError);
    }
    assert(nativeFailure == 0);
    const int rollbackFailure = GdxSnapshotMfsTest_RollbackFailure();
    if (rollbackFailure) {
        std::fprintf(stderr, "Rollback failure fixture failed at line %d (MFS error %d)\n", rollbackFailure, gMfsError);
    }
    assert(rollbackFailure == 0);
    GdxSnapshotRecoveryTest_Run(root);
#endif
    gdx_course_store_clear();
    for (int i = 0; i < GDX_COURSE_TITLE_SLOTS - 1; ++i) {
        const auto id = "FILL" + std::to_string(i) + ".CRSD";
        assert(gdx_course_title_set(id.c_str(), "Existing") == 0);
    }
    std::vector<gdx::recovery::Record> batch(2);
    for (int i = 0; i < 2; ++i) {
        auto& r = batch[size_t(i)];
        r.disk = disk;
        r.directory = 7;
        r.name = "BATCH" + std::to_string(i);
        r.extension = "CRSD";
        const auto id = r.name + "." + r.extension;
        Bytes encoded(128);
        const auto binding = gdx_course_registry_crc32(
            reinterpret_cast<const unsigned char*>(id.data()), id.size());
        const int n = gdx_course_title_encode("Imported", binding, encoded.data(), encoded.size());
        assert(n > 0);
        encoded.resize(size_t(n));
        r.changes.push_back({gdx::recovery::Kind::Title, 0, encoded});
        assert(gdx::recovery::canRestoreMetadata(r));
    }
    assert(!gdx::recovery::canRestoreMetadata(batch));
    assert(!gdx_course_title_get("BATCH0.CRSD", title, sizeof(title)));
    gdx_course_store_clear();
    assert(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()) &&
           root.filename().string().find("gdx-snapshot-io-") == 0);
    if (relativeRecoveryPath) {
        assert(std::filesystem::is_directory(root / "saves/test.gdd.recovery.snapshots"));
        std::filesystem::current_path(originalDirectory);
    }
#ifndef GDX_SNAPSHOT_MFS_TEST
    std::filesystem::remove_all(root);
#endif
    std::puts("All complete snapshot I/O tests passed");
}
