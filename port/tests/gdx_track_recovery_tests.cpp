#include "gdx_track_recovery.h"
#include "gdx_recovery_file.h"
#include "gdx_recovery_metadata.h"
#include "gdx_course_registry.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_settings.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

#include <cassert>
#include <cstdio>
#include <map>
#include <cstring>

using namespace gdx::recovery;

struct Memory : Backend {
    std::map<int, Bytes> live;
    std::map<int, Bytes> disk;
    Bytes saved;
    int calls = 0;
    int fail = -1;
    bool after = false;
    bool step() {
        return calls++ != fail || after;
    }
    bool finish() {
        return !after || calls - 1 != fail;
    }
    static int key(const Change& c) {
        return int(c.kind) * 32 + c.slot;
    }
    bool read(const Record&, const Change& c, Bytes& out) override {
        if (!step()) return false;
        out = live[key(c)];
        return finish();
    }
    bool write(const Record&, const Change& c, const Bytes& in) override {
        if (!step()) return false;
        live[key(c)] = in;
        return finish();
    }
    bool flush(const Change&) override {
        if (!step()) return false;
        disk = live;
        return finish();
    }
    bool journal(const Record& r) override {
        if (!step()) return false;
        return encode(r, saved) && finish();
    }
};

static Record fixture(Memory& m) {
    Record r;
    r.disk = 123;
    r.directory = 7;
    r.id = 1;
    r.name = "LONG TRACK NAME";
    r.extension = "CRSD";
    for (int kind = 1; kind <= 7; ++kind) {
        Change c;
        c.kind = Kind(kind);
        c.before = {uint8_t(kind), 42};
        if (kind == 6) c.before.resize(9);
        if (kind == 7) c.before.resize(21);
        r.changes.push_back(c);
        m.live[Memory::key(c)] = c.before;
    }
    m.live[999] = {88, 99};
    m.disk = m.live;
    return r;
}

static void metadataTests() {
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    Record r;
    r.name = "RECOVERY";
    r.extension = "CRSD";
    const char* id = "RECOVERY.CRSD";
    GdxCourseExtRecord ext{};
    ext.kind = GDX_COURSE_KIND_BGM;
    ext.scope = GDX_COURSE_SCOPE_COURSE;
    std::strcpy(ext.key, "bgm.custom.unregistered");
    assert(gdx_course_ext_restore(id, &ext, 1) == 0);
    assert(gdx_course_title_set(id, "Full track title") == 0);
    GdxBranchSet forks{};
    forks.ringCount = 8;
    forks.forkCount = 1;
    forks.forks[0].forkPoint = 1;
    forks.forks[0].mergePoint = 3;
    forks.forks[0].nodeCount = 1;
    assert(gdx_branch_store_set(id, &forks) == 0);
    GdxCourseSettings settings{};
    settings.ringCount = 8;
    settings.lavaMode = GDX_SETTINGS_LAVA_NEVER_KILLS;
    settings.nodes[0][0].magnetRange = 150;
    assert(gdx_settings_store_set(id, &settings) == 0);
    Record unrelated = r;
    unrelated.name = "UNRELATED";
    for (int kind = 2; kind <= 5; ++kind) {
        Bytes before, empty, after, other;
        assert(readMetadata(r, Kind(kind), before) && !before.empty());
        r.changes = {{Kind(kind), 0, before}};
        assert(canRestoreMetadata(r));
        auto wrong = r;
        wrong.name = "WRONG";
        assert(!writeMetadata(wrong, Kind(kind), before));
        assert(writeMetadata(unrelated, Kind(kind), {}));
        assert(writeMetadata(r, Kind(kind), {}));
        assert(readMetadata(r, Kind(kind), empty) && empty.empty());
        assert(canRestoreMetadata(r));
        assert(writeMetadata(r, Kind(kind), before));
        assert(readMetadata(r, Kind(kind), after) && before == after);
        assert(readMetadata(unrelated, Kind(kind), other) && other.empty());
        assert(writeMetadata(r, Kind(kind), {}));
        for (int i = 0; i < 128; ++i) {
            char key[26];
            std::snprintf(key, sizeof(key), "OTHER%d.CRSD", i);
            if (kind == 2) gdx_course_ext_restore(key, &ext, 1);
            if (kind == 3) gdx_course_title_set(key, "Unrelated title");
            if (kind == 4) gdx_branch_store_set(key, &forks);
            if (kind == 5) gdx_settings_store_set(key, &settings);
        }
        assert(!writeMetadata(wrong, Kind(kind), before));
        assert(!writeMetadata(r, Kind(kind), before));
        assert(!canRestoreMetadata(r));
        assert(readMetadata(r, Kind(kind), after) && after.empty());
        Record free = r;
        free.name = "OTHER0";
        assert(writeMetadata(free, Kind(kind), {}));
        assert(canRestoreMetadata(r));
        assert(writeMetadata(r, Kind(kind), before));
    }
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
}

static void reseal(Bytes& bytes) {
    const auto crc = gdx_course_registry_crc32(bytes.data(), bytes.size() - 4);
    for (int i = 0; i < 4; ++i) {
        bytes[bytes.size() - 4 + i] = uint8_t(crc >> (8 * i));
    }
}

int main() {
    metadataTests();
    Memory m;
    auto r = fixture(m);
    assert(matchesCupReferences(r, 1, false));
    assert(!matchesCupReferences(r, 0, false));
    assert(matchesCupReferences(r, 0, true));
    assert(matchesCupReferences(r, 1, true));
    for (int slot = 1; slot < 32; ++slot) {
        assert(!matchesCupReferences(r, 1 | (uint32_t(1) << slot), false));
        assert(!matchesCupReferences(r, uint32_t(1) << slot, true));
    }
    assert(beginDelete(r, m) == Result::Ok);
    assert(r.state == State::Deleted);
    assert(m.live[999] == Bytes({88, 99}));
    assert(beginRestore(r, m) == Result::Ok);
    for (const auto& c : r.changes) assert(m.disk[Memory::key(c)] == c.before);

    const int deleteCalls = [] {
        Memory b;
        auto record = fixture(b);
        assert(beginDelete(record, b) == Result::Ok);
        return b.calls;
    }();
    for (bool after : {false, true}) {
      for (int fail = 0; fail < deleteCalls; ++fail) {
        Memory b;
        auto record = fixture(b);
        const auto before = b.disk;
        b.fail = fail;
        b.after = after;
        assert(beginDelete(record, b) != Result::Ok);
        b.live = b.disk;
        b.fail = -1;
        if (b.saved.empty()) {
            assert(b.disk == before);
        } else {
            Record recovered;
            assert(decode(b.saved, recovered));
            assert(resume(recovered, b) == Result::Ok);
            assert(recovered.state == State::Deleted);
            for (const auto& c : recovered.changes) assert(b.disk[Memory::key(c)].empty());
            assert(b.disk[999] == before.at(999));
            assert(resume(recovered, b) == Result::Ok);
        }
      }
    }
    Memory base;
    auto deleted = fixture(base);
    assert(beginDelete(deleted, base) == Result::Ok);
    const auto savedBase = base;
    base.calls = 0;
    assert(beginRestore(deleted, base) == Result::Ok);
    const int restoreCalls = base.calls;
    for (bool after : {false, true}) {
      for (int fail = 0; fail < restoreCalls; ++fail) {
        Memory b = savedBase;
        Record record;
        assert(decode(b.saved, record));
        b.calls = 0;
        b.fail = fail;
        b.after = after;
        assert(beginRestore(record, b) != Result::Ok);
        b.live = b.disk;
        b.fail = -1;
        assert(decode(b.saved, record));
        if (record.state == State::Deleted) {
            assert(beginRestore(record, b) == Result::Ok);
        } else {
            assert(resume(record, b) == Result::Ok);
        }
        for (const auto& c : record.changes) assert(b.disk[Memory::key(c)] == c.before);
      }
    }
    for (int kind = 1; kind <= 7; ++kind) {
        Memory b = savedBase;
        Record record;
        assert(decode(b.saved, record));
        b.live[kind * 32] = {99};
        const auto unchanged = b.live;
        const auto journal = b.saved;
        assert(beginRestore(record, b) == Result::Conflict);
        assert(b.live == unchanged && b.saved == journal);
    }
    for (bool restoring : {false, true}) {
        for (int kind = 1; kind <= 7; ++kind) {
            struct Changing : Memory {
                Kind watched = Kind::Track;
                int reads = 0;
                bool armed = false;
                bool read(const Record& record, const Change& change, Bytes& out) override {
                    if (armed && change.kind == watched && ++reads == 3) {
                        live[key(change)] = {99};
                    }
                    return Memory::read(record, change, out);
                }
            } b;
            auto record = fixture(b);
            if (restoring) {
                assert(beginDelete(record, b) == Result::Ok);
                b.reads = 0;
            }
            b.watched = Kind(kind);
            b.armed = true;
            const auto result = restoring ? beginRestore(record, b) : beginDelete(record, b);
            assert(result == Result::Conflict);
            assert(b.live[kind * 32] == Bytes({99}));
        }
    }
    Bytes journal;
    assert(encode(r, journal));
    for (size_t size = 0; size < journal.size(); ++size) {
        Record invalid;
        assert(!decode(Bytes(journal.begin(), journal.begin() + size), invalid));
    }
    for (size_t byte = 0; byte < journal.size(); ++byte) {
        auto bad = journal;
        bad[byte] ^= 0xff;
        Record invalid;
        assert(!decode(bad, invalid));
    }
    for (size_t offset : {size_t(4), size_t(26), size_t(27), size_t(32),
                          size_t(36 + r.name.size() + 2)}) {
        auto bad = journal;
        bad[offset] = 255;
        reseal(bad);
        Record invalid;
        assert(!decode(bad, invalid));
    }
    {
        auto bad = journal;
        std::fill(bad.begin() + 8, bad.begin() + 16, 0);
        reseal(bad);
        Record invalid;
        assert(!decode(bad, invalid));
    }
    Memory all;
    auto full = fixture(all);
    full.changes.resize(5);
    for (int slot = 0; slot < 24; ++slot) {
        for (auto kind : {Kind::CentSlot, Kind::CenlSlot}) {
            Change c;
            c.kind = kind;
            c.slot = uint8_t(slot);
            if (kind == Kind::CenlSlot) {
                c.before = Bytes(21, 42);
                c.before.back() = 0;
            }
            full.changes.push_back(c);
            all.live[Memory::key(c)] = c.before;
        }
    }
    all.disk = all.live;
    assert(full.changes.size() == MaxChanges);
    assert(matchesCupReferences(full, 0xFFFFFF, false));
    for (int slot = 0; slot < 24; ++slot) {
        const uint32_t removed = 0xFFFFFF & ~(uint32_t(1) << slot);
        assert(!matchesCupReferences(full, removed, false));
        assert(matchesCupReferences(full, removed, true));
    }
    assert(beginDelete(full, all) == Result::Ok);
    assert(beginRestore(full, all) == Result::Ok);
    for (const auto& c : full.changes) assert(all.disk[Memory::key(c)] == c.before);
    for (const auto kind : {Kind::CentSlot, Kind::CenlSlot}) {
        auto malformed = r;
        auto& cup = *std::find_if(malformed.changes.begin(), malformed.changes.end(),
            [kind](const Change& change) { return change.kind == kind; });
        cup.before.assign(kind == Kind::CentSlot ? 9 : 21, 42);
        assert(!encode(malformed, journal));
        cup.before.back() = 0;
        cup.before.front() = 0;
        assert(!encode(malformed, journal));
    }
    auto duplicate = r;
    duplicate.changes.push_back(duplicate.changes.front());
    assert(!encode(duplicate, journal));
    duplicate = r;
    duplicate.changes.back().slot = 24;
    assert(!encode(duplicate, journal));
    duplicate = r;
    duplicate.changes.front().before.resize(65537);
    assert(!encode(duplicate, journal));
    const auto dir = std::filesystem::temp_directory_path() /
        ("gdx-recovery-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(dir));
    assert(prepareDirectory(dir));
    {
        FileLock first(dir / "unit.lock");
        FileLock second(dir / "unit.lock");
        assert(first.held() && !second.held());
    }
    {
        FileLock released(dir / "unit.lock");
        assert(released.held());
    }
    const auto path = dir / "synthetic.gtr";
    assert(encode(r, journal));
    assert(writeFile(path, journal));
    Bytes loaded;
    assert(readFile(path, loaded) && loaded == journal);
    assert(!readFile(path, loaded, 1));
    assert(writeFile(path, Bytes{1, 2, 3}));
    assert(readFile(path, loaded) && loaded == Bytes({1, 2, 3}));
    assert(!writeFile(dir / "missing" / "file", journal));
    assert(std::filesystem::remove(path));
    assert(std::filesystem::remove(dir / "unit.lock"));
    assert(std::filesystem::remove(dir));
    std::printf("Failure injection: %d delete and %d restore boundaries.\n", deleteCalls, restoreCalls);
    std::puts("All track recovery tests passed.");
}
