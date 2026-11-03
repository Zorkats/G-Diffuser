#include "gdx_content_import.h"
#include "gdx_content_io.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_course_state.h"
#include "gdx_recovery_file.h"
#include "gdx_track_recovery_runtime.h"
#include "disk_savefile.h"
#include <cassert>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" void GdxSnapshotMfsTest_SetThread(void* thread);
extern "C" void* gdx_snapshot_native_thread(void);
namespace {
using namespace gdx::recovery;
Bytes recoveryTrack;
std::string recoveryProbe;
bool interruptNextYield = false;
int interruptions = 0;

void rejectOtherOwner(const std::string& path) {
    void* owner = gdx_snapshot_native_thread();
    const uint8_t byte = 9;
    GdxSnapshotMfsTest_SetThread(reinterpret_cast<void*>(2));
    assert(!gdx_recovery_write_await(path.c_str(), &byte, 1));
    if (gdx_snapshot_busy()) {
        assert(!gdx_snapshot_begin(1));
    }
    assert(!std::filesystem::exists(path));
    gdx_snapshot_end();
    GdxSnapshotMfsTest_SetThread(owner);
}
}

int GdxSnapshotRecoveryTest_RunChild(const char* executable, const char* root, bool relative) {
#ifdef _WIN32
    const auto quotedExecutable = '"' + std::string(executable) + '"';
    const auto quotedRoot = '"' + std::string(root) + '"';
    const char* args[]{quotedExecutable.c_str(), "--fixture-root", quotedRoot.c_str(),
                       relative ? "--relative-path" : nullptr, nullptr};
    const auto result = _spawnv(_P_WAIT, executable, args);
    return result < 0 ? 1 : int(result);
#else
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        if (relative) {
            execl(executable, executable, "--fixture-root", root, "--relative-path", static_cast<char*>(nullptr));
        } else {
            execl(executable, executable, "--fixture-root", root, static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    int status = 0;
    int waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

extern "C" {
int GdxSnapshotRecoveryTest_Verify(const void* bytes, unsigned int size) {
    char directory[2048];
    assert(gdx_disk_save_recovery_path(directory, sizeof(directory)));
    Bytes actual;
    return readFile(std::string(directory) + ".gdd", actual) && actual.size() == size &&
        std::memcmp(actual.data(), bytes, size) == 0;
}
int GdxSnapshotRecoveryTest_Flush(const void* bytes, unsigned int size, int fail) {
    char directory[2048];
    assert(gdx_disk_save_recovery_path(directory, sizeof(directory)));
    const auto path = std::string(directory) + (fail ? ".missing/disk.gdd" : ".gdd");
    return gdx_recovery_write_await(path.c_str(), static_cast<const uint8_t*>(bytes), size);
}
void* gdx_recovery_native_thread(void) {
    return gdx_snapshot_native_thread();
}
void gdx_yield(void) {
    if (interruptNextYield) {
        interruptNextYield = false;
        rejectOtherOwner(recoveryProbe + ".other");
        ++interruptions;
    }
    std::this_thread::yield();
}
int GdxContentRecovery_EnqueueDiskOp(void) {
    return 0;
}
int gdx_content_mfs_busy(void) {
    return 0;
}
int gdx_content_import_pending(void) {
    return 0;
}
int gdx_content_cup_pending(void) {
    return 0;
}
int gdx_input_in_gameplay(void) {
    return 0;
}
int gdx_course_state_edit_file_id(char*, int) {
    return 0;
}
int gdx_course_state_recovery_flush(int) {
    return 0;
}
unsigned gdx_recovery_native_directory(void) {
    return 3;
}
int gdx_recovery_native_prepare(void) {
    rejectOtherOwner(recoveryProbe + ".other");
    const uint8_t byte = 9;
    return gdx_recovery_write_await(recoveryProbe.c_str(), &byte, 1);
}
int gdx_recovery_native_read(const char*, const char*, uint8_t* out, size_t cap, size_t* size) {
    assert(cap >= recoveryTrack.size());
    *size = recoveryTrack.size();
    if (*size) {
        std::memcpy(out, recoveryTrack.data(), *size);
    }
    return 1;
}
int gdx_recovery_native_write(const char*, const char*, const uint8_t* bytes, size_t size) {
    recoveryTrack.clear();
    if (size) {
        recoveryTrack.assign(bytes, bytes + size);
    }
    return 1;
}
int gdx_recovery_native_cups(uint8_t* cent, uint8_t* cenl) {
    std::memset(cent, 0, 216);
    std::memset(cenl, 0, 508);
    return 1;
}
int gdx_recovery_native_cup_slot(int, int, const uint8_t*, size_t) {
    assert(false);
    return 0;
}
int gdx_recovery_native_restore_ready(const char*, const char*, const uint8_t*, size_t, int) {
    assert(false);
    return 0;
}
}

void GdxSnapshotRecoveryTest_Run(const std::filesystem::path& root) {
    const auto path = (root / "writer.gdd").string();
    recoveryProbe = (root / "recovery-owner.gdd").string();
    const Bytes expected{1, 2, 3, 4};
    Bytes actual;
    assert(!gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    assert(!std::filesystem::exists(path));
    GdxSnapshotMfsTest_SetThread(nullptr);
    assert(!gdx_snapshot_begin(1));
    assert(!gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    GdxSnapshotMfsTest_SetThread(reinterpret_cast<void*>(1));
    assert(gdx_snapshot_begin(1));
    assert(gdx_snapshot_begin(1));
    GdxSnapshotMfsTest_SetThread(nullptr);
    assert(!gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    GdxSnapshotMfsTest_SetThread(reinterpret_cast<void*>(1));
    interruptNextYield = true;
    assert(gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    gdx_yield();
    assert(interruptions == 1 && gdx_snapshot_busy());
    assert(readFile(path, actual) && actual == expected);
    assert(!gdx_recovery_write_await(nullptr, expected.data(), expected.size()));
    assert(!gdx_recovery_write_await(path.c_str(), nullptr, 1));
    const auto missing = root / "missing" / "writer.gdd";
    assert(!gdx_recovery_write_await(missing.string().c_str(), expected.data(), expected.size()));
    assert(!std::filesystem::exists(missing));
    assert(readFile(path, actual) && actual == expected);
    gdx_snapshot_end();
    assert(gdx_snapshot_busy());
    assert(gdx_recovery_write_await(path.c_str(), nullptr, 0));
    assert(readFile(path, actual) && actual.empty());
    assert(gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    gdx_snapshot_end();
    assert(!gdx_snapshot_busy());
    assert(!gdx_recovery_write_await(path.c_str(), nullptr, 0));
    assert(readFile(path, actual) && actual == expected);

    char directory[2048];
    assert(gdx_disk_save_recovery_path(directory, sizeof(directory)));
    assert(std::filesystem::create_directory(directory));
    const auto journal = std::filesystem::path(directory) / "1.gtr";
    Record record;
    record.disk = gdx_disk_save_identity();
    record.id = 1;
    record.directory = 3;
    record.name = "RECOVERY";
    record.extension = "CRSD";
    recoveryTrack = expected;
    record.changes.push_back({Kind::Track, 0, expected});
    for (int kind = 2; kind <= 5; ++kind) {
        record.changes.push_back({Kind(kind), 0, {}});
    }
    assert(encode(record, actual) && writeFile(journal, actual));
    GdxContentRecovery_RunOnDiskThread();
    assert(!gdx_recovery_blocks_writes());
    assert(recoveryTrack.empty());
    assert(readFile(journal, actual) && decode(actual, record) && record.state == State::Deleted);
    assert(readFile(recoveryProbe, actual) && actual == Bytes{9});
    assert(!gdx_recovery_write_await(path.c_str(), nullptr, 0));

    assert(writeFile(journal, Bytes{0}));
    GdxContentRecovery_RunOnDiskThread();
    assert(gdx_recovery_blocks_writes());
    assert(!gdx_snapshot_begin(1));
    assert(gdx_snapshot_begin(0));
    assert(!gdx_recovery_write_await(path.c_str(), expected.data(), expected.size()));
    gdx_snapshot_end();
    assert(std::filesystem::remove(journal));
    GdxContentRecovery_RunOnDiskThread();
    assert(!gdx_recovery_blocks_writes());
}
