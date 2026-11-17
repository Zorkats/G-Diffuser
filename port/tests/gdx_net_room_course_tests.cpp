// port/tests/gdx_net_room_course_tests.cpp -- unit tests for a G-NET room's custom track raced from memory
// (gdx_net_room_course.h).
// Copyright (c) Zorkats
//
// A real track is built the way an author's copy builds it: a course in the snapshot fixture, its title, settings
// and skybox record in the stores, captured and cut to its content bytes. The stores are then emptied, so every
// check below proves the race reads the room's copy and nothing this copy has saved. The lobby, the extended
// height switch and the boot are the three functions this file supplies.

#include "gdx_course_branch_format.h"
#include "gdx_course_meta.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_course_snapshot.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_snapshot_state.h"
#include "gdx_net_room_course.h"
#include "net/gdx_net_track_pack.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void* GdxSnapshotTest_Create(int count);

namespace {

int sFailures;
int sChecks;

void Check(bool ok, const char* what) {
    sChecks++;
    if (!ok) {
        sFailures++;
        std::printf("FAIL: %s\n", what);
    }
}

// The lobby's copy of the room's track, as gdx_lan_race_track hands it over.
bool sServe;
gdx::net::track::TrackId sId{};
std::vector<unsigned char> sBytes;
int sForced = -1;

const char* const kCourseId = "ROOMTEST.CRSD";
constexpr int kPoints = 10;
constexpr int kSkyStock = 3;

} // namespace

extern "C" int gdx_lan_race_track(unsigned char* id, unsigned char* out, unsigned long capacity, unsigned long* length) {
    if (!sServe || sBytes.size() > capacity) {
        return 0;
    }
    std::memcpy(id, sId.data(), sId.size());
    std::memcpy(out, sBytes.data(), sBytes.size());
    *length = static_cast<unsigned long>(sBytes.size());
    return 1;
}

// The lobby's Info for the room's track (protocol 15): none unless a test sets one.
namespace {
bool sInfo;
std::vector<unsigned char> sMeta;
std::string sSong;
int sInfoCalls;
} // namespace

extern "C" int gdx_lan_race_info(const unsigned char* id, unsigned char* meta, unsigned long metaCap,
                                 unsigned long* metaLength, char* songPath, unsigned long songPathCap) {
    sInfoCalls++;
    *metaLength = 0;
    songPath[0] = '\0';
    if (!sInfo || std::memcmp(id, sId.data(), sId.size()) != 0 || sMeta.size() > metaCap ||
        sSong.size() >= songPathCap) {
        return 0;
    }
    std::memcpy(meta, sMeta.data(), sMeta.size());
    *metaLength = static_cast<unsigned long>(sMeta.size());
    std::memcpy(songPath, sSong.c_str(), sSong.size() + 1);
    return 1;
}

extern "C" void gdx_course_bounds_force_extended(int on) {
    sForced = on;
}

extern "C" int gdx_course_boot(void) {
    return 0;
}

namespace {

// The author's track: content bytes, identity, and the runtime image the race must get.
bool BuildTrack(std::vector<unsigned char>& bytes, gdx::net::track::TrackId& id, gdx::snapshot::Bytes& runtime) {
    using namespace gdx::snapshot;
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    gdx_course_registry_clear_extensions();
    if (gdx_course_registry_register(GDX_COURSE_KIND_SKYBOX, "test.room.sky", "Room sky", kSkyStock) != GDX_COURSE_OK) {
        return false;
    }
    if (gdx_course_title_set(kCourseId, "Room test") != GDX_COURSE_OK) {
        return false;
    }
    GdxCourseSettings settings{};
    settings.ringCount = kPoints;
    settings.lavaMode = GDX_SETTINGS_LAVA_NEVER_KILLS;
    settings.ring[2].pitWidth = 5;
    if (gdx_settings_store_set(kCourseId, &settings) != GDX_SETTINGS_OK) {
        return false;
    }
    GdxCourseExtRecord sky{};
    sky.kind = GDX_COURSE_KIND_SKYBOX;
    sky.scope = GDX_COURSE_SCOPE_COURSE;
    sky.fallbackId = kSkyStock;
    std::snprintf(sky.key, sizeof(sky.key), "%s", "test.room.sky");
    if (gdx_course_ext_restore(kCourseId, &sky, 1) != GDX_COURSE_OK) {
        return false;
    }

    Generation author;
    author.disk = 1;
    author.courseId = kCourseId;
    if (!captureState(author, GdxSnapshotTest_Create(kPoints)) || !content(author, bytes)) {
        return false;
    }
    id = gdx::net::track::Identify(bytes.data(), bytes.size());
    Generation verified;
    if (gdx::net::track::Verify(bytes.data(), bytes.size(), id, &verified) != gdx::net::track::Verdict::Ok ||
        !expandRuntime(verified, runtime)) {
        return false;
    }

    // From here on this copy has saved nothing about the track.
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    return true;
}

void TestStockTrack() {
    sServe = false;
    sForced = -1;
    Check(gdx_net_room_course_prepare(3) == 1, "a stock track needs no room course");
    Check(sForced == 0, "a stock track does not force the extended height");
    Check(gdx_net_room_course_take(24, 1) == 0, "a stock track's race takes no room course");
    Check(gdx_net_room_course_active() == 0, "nothing is active on a stock track");
}

void TestNothingToRace(const std::vector<unsigned char>& bytes, const gdx::net::track::TrackId& id) {
    sServe = false;
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 0, "no verified copy: no race");
    Check(gdx_net_room_course_take(24, 1) == 0, "no verified copy: nothing to take");

    // Bytes that are not the promised track never reach the decoder.
    sServe = true;
    sBytes = bytes;
    sId = id;
    sBytes[sBytes.size() / 2] ^= 0x40;
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 0, "a changed byte is refused");
    Check(sForced == 0, "a refused track leaves the height setting alone");
    sBytes = bytes;
    sId[0] ^= 1;
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 0, "another identity is refused");
    sId = id;
    Check(gdx_net_room_course_take(24, 1) == 0, "a refused track is never taken");
}

void TestRace(const std::vector<unsigned char>& bytes, const gdx::net::track::TrackId& id,
              const gdx::snapshot::Bytes& runtime) {
    sServe = true;
    sBytes = bytes;
    sId = id;
    sForced = -1;
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 1, "the room's track prepares");
    Check(sForced == 1, "a room course forces the extended height");
    Check(gdx_net_room_course_active() == 0, "prepared is not yet raced");

    // Only the room's own race takes it, and only in the first Edit Cup slot.
    Check(gdx_net_room_course_take(5, 1) == 0, "a stock course load does not take it");
    Check(gdx_net_room_course_take(25, 1) == 0, "another Edit Cup slot does not take it");
    Check(gdx_net_room_course_take(24, 0) == 0, "a load that is not the room's race does not take it");
    Check(gdx_net_room_course_active() == 0, "and leaves nothing active");
    Check(gdx_net_room_course_take(24, 1) == 1, "the room's race takes it");
    Check(gdx_net_room_course_active() == 1, "the room course is active");

    std::vector<unsigned char> image(gdx_course_snapshot_runtime_bytes(), 0xA5);
    Check(gdx_net_room_course_copy(image.data(), static_cast<unsigned long>(image.size() - 1)) == 0,
          "a buffer of another size is not written");
    Check(image[0] == 0xA5, "and keeps its bytes");
    Check(gdx_net_room_course_copy(image.data(), static_cast<unsigned long>(image.size())) == 1, "the image copies");
    Check(image.size() == runtime.size() && std::memcmp(image.data(), runtime.data(), image.size()) == 0,
          "the image is the author's course");

    gdx_net_room_course_apply_forks();
    const GdxBranchSet* forks = gdx_branch_active_set();
    Check(forks != nullptr && forks->ringCount == kPoints && forks->forkCount == 0, "the track's fork block is active");

    gdx_net_room_course_apply_settings(kPoints);
    const GdxCourseSettings* settings = gdx_settings_active();
    Check(settings != nullptr && settings->lavaMode == GDX_SETTINGS_LAVA_NEVER_KILLS, "the track's settings are active");
    Check(settings != nullptr && settings->ring[2].pitWidth == 5, "with its point values on its own ring");
    gdx_net_room_course_apply_settings(kPoints + 1);
    settings = gdx_settings_active();
    Check(settings != nullptr && settings->lavaMode == GDX_SETTINGS_LAVA_NEVER_KILLS && settings->ringCount == 0 &&
              settings->ring[2].pitWidth == 0,
          "on another ring only the course-wide values stay");

    Check(gdx_net_room_course_skybox() == kSkyStock, "the skybox record resolves in this session");
    gdx_course_registry_clear_extensions();
    Check(gdx_net_room_course_skybox() == -1, "an unknown skybox key keeps the course's own byte");

    // A load for something else drops the active state; the room's race can take it again (a Retry's reload).
    Check(gdx_net_room_course_take(24, 0) == 0 && gdx_net_room_course_active() == 0, "another load drops it");
    gdx_net_room_course_apply_forks();
    Check(gdx_branch_active_set() == nullptr, "and its forks");
    gdx_net_room_course_apply_settings(kPoints);
    Check(gdx_settings_active() == nullptr, "and its settings");
    Check(gdx_net_room_course_take(24, 1) == 1, "the room's race takes it again");

    Check(gdx_net_room_course_refused() == 0, "nothing refused yet");
    gdx_net_room_course_refuse();
    Check(gdx_net_room_course_refused() == 1, "a refusal sticks");

    // Nothing was saved anywhere along the way.
    GdxCourseSettings stored{};
    GdxBranchSet storedForks{};
    int records = -1;
    Check(!gdx_settings_store_get(kCourseId, &stored) && !gdx_settings_store_get("CONTENT.CRSD", &stored),
          "no settings were stored");
    Check(!gdx_branch_store_get(kCourseId, &storedForks) && !gdx_branch_store_get("CONTENT.CRSD", &storedForks),
          "no forks were stored");
    Check(gdx_course_ext_records("CONTENT.CRSD", &records) == nullptr || records == 0, "no records were stored");

    gdx_net_room_course_end();
    Check(gdx_net_room_course_active() == 0 && gdx_net_room_course_refused() == 0, "the end drops everything");
    Check(sForced == 0, "and the extended height");
    Check(gdx_net_room_course_take(24, 1) == 0, "an ended course is not taken again");
}

// Protocol 15: the creator names and the song the lobby holds for the room's track. The names must be bound to the
// track's id; the song path may arrive after the race was prepared, so the race asks again until it has one.
std::vector<unsigned char> Names(const char* author, const char* editedBy, uint32_t bind) {
    GdxCourseMeta meta;
    std::memset(&meta, 0, sizeof(meta));
    std::snprintf(meta.author, sizeof(meta.author), "%s", author);
    std::snprintf(meta.editedBy, sizeof(meta.editedBy), "%s", editedBy);
    std::vector<unsigned char> block(GDX_META_BLOCK_MAX_BYTES);
    const int length = gdx_meta_encode(&meta, bind, block.data(), block.size());
    block.resize(length > 0 && length <= 512 ? static_cast<size_t>(length) : 0);
    return block;
}

void TestInfo(const std::vector<unsigned char>& bytes, const gdx::net::track::TrackId& id) {
    const uint32_t bind = gdx_course_registry_crc32(id.data(), id.size());
    sServe = true;
    sBytes = bytes;
    sId = id;
    GdxCourseMeta meta;

    sInfo = false;
    Check(gdx_net_room_course_meta(&meta) == 0, "no names before the race is taken");
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 1 && gdx_net_room_course_take(24, 1) == 1,
          "the room's track races without Info");
    Check(gdx_net_room_course_meta(&meta) == 1 && meta.author[0] == '\0', "no Info: no names");
    Check(gdx_net_room_course_song() == nullptr, "no Info: no song, so the stock music plays");

    // The Info arrives after the race was prepared: the next question reads it.
    sInfo = true;
    sMeta = Names("ZORKATS", "ERNST FAN", bind);
    sSong = "C:/cache/music/ernst.flac";
    Check(!sMeta.empty(), "the names encode");
    Check(gdx_net_room_course_meta(&meta) == 1 && std::strcmp(meta.author, "ZORKATS") == 0 &&
              std::strcmp(meta.editedBy, "ERNST FAN") == 0,
          "late Info gives the author and the editor");
    Check(gdx_net_room_course_song() != nullptr && std::strcmp(gdx_net_room_course_song(), sSong.c_str()) == 0,
          "late Info gives the song");

    // Names bound to another track are not shown, and the song still plays.
    sMeta = Names("SOMEONE", "", bind ^ 1u);
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 1 && gdx_net_room_course_take(24, 1) == 1,
          "the track prepares again");
    Check(gdx_net_room_course_meta(&meta) == 1 && meta.author[0] == '\0', "names bound to another track are dropped");
    Check(gdx_net_room_course_song() != nullptr, "the song does not depend on the names");

    // The song finishes after the names arrived: the names are read once, the song path until it is there.
    sMeta = Names("ZORKATS", "", bind);
    sSong.clear();
    Check(gdx_net_room_course_prepare(GDX_NET_ROOM_COURSE_TRACK_CUSTOM) == 1 && gdx_net_room_course_take(24, 1) == 1,
          "the track prepares a third time");
    Check(gdx_net_room_course_song() == nullptr, "a song still on its way is not played");
    sMeta = Names("CHANGED", "", bind);
    sSong = "C:/cache/music/late.ogg";
    const int calls = sInfoCalls;
    Check(gdx_net_room_course_song() != nullptr && std::strcmp(gdx_net_room_course_song(), sSong.c_str()) == 0,
          "a song that finished later is played");
    Check(sInfoCalls == calls + 1, "the lobby is asked once, then the path is kept");
    Check(gdx_net_room_course_meta(&meta) == 1 && std::strcmp(meta.author, "ZORKATS") == 0,
          "the names are the ones read first");

    gdx_net_room_course_end();
    Check(gdx_net_room_course_song() == nullptr && gdx_net_room_course_meta(&meta) == 0, "the end drops the Info");
    sInfo = false;
    sMeta.clear();
    sSong.clear();
}

} // namespace

int main() {
    std::vector<unsigned char> bytes;
    gdx::net::track::TrackId id{};
    gdx::snapshot::Bytes runtime;
    const bool built = BuildTrack(bytes, id, runtime);
    Check(built, "the author's track builds");
    if (built) {
        TestStockTrack();
        TestNothingToRace(bytes, id);
        TestRace(bytes, id, runtime);
        TestInfo(bytes, id);
    }
    std::printf("gdx_net_room_course_tests: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
