// port/tests/gdx_net_track_save_tests.cpp -- unit tests for "Save to my tracks" (gdx_net_track_save.h).
// Copyright (c) Zorkats
//
// A real track is built the way an author's copy builds it (as gdx_net_room_course_tests.cpp does), so the hash
// check on the results page is the real one. The lobby's copy, the content import and the Edit Cup are stubs
// here: the import's own tests cover the disk (gdx_content_import_disk_tests.c, gdx_course_snapshot_io_tests.cpp).
// The stubs can finish an install at once, as the port's disk thread does, or leave it pending.

#include "gdx_content_import.h"
#include "gdx_dev_gates.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_course_snapshot.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_snapshot_state.h"
#include "gdx_net_own_tracks.h"
#include "gdx_net_track_save.h"
#include "net/gdx_net_track_pack.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void* GdxSnapshotTest_Create(int count);

// The module's log lines (port_log.h): every developer gate off, so nothing reaches a log file.
int gGdxDevGateCache[GDX_GATE_COUNT];
int gGdxDevGateCacheReady = 1;
void (*gdx_port_log_tap)(const char*) = nullptr;

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

const char* const kCourseId = "SAVETEST.CRSD";
constexpr int kPoints = 10;

// The lobby's copy of the race's track.
bool sServe;
gdx::net::track::TrackId sServeId{};
std::vector<unsigned char> sServeBytes;

// This copy's Edit Cup and disk.
char sCup[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1];
int sCupRc;
bool sOwn;
gdx::net::track::TrackId sOwnId{};
int sForgets;

std::string sNameTitle;
int sNameRc;
int sNameCalls;

std::vector<unsigned char> sStaged;
std::string sStagedName;
int sStageCalls;

int sBeginRc;
int sBeginCalls;
std::string sBeginName;
bool sFinishAtOnce; // the port's disk thread: the install is over when begin returns
bool sPending;
bool sDone;
int sFinishRc;
bool sFinishFull;
int sCollected;

} // namespace

extern "C" int gdx_lan_race_track(unsigned char* id, unsigned char* out, unsigned long capacity, unsigned long* length) {
    if (!sServe || sServeBytes.size() > capacity) {
        return 0;
    }
    std::memcpy(id, sServeId.data(), sServeId.size());
    std::memcpy(out, sServeBytes.data(), sServeBytes.size());
    *length = static_cast<unsigned long>(sServeBytes.size());
    return 1;
}

extern "C" int gdx_net_own_track(int slot, unsigned char* id, char* title, int titleSize) {
    (void) title;
    (void) titleSize;
    if (!sOwn || slot != 4) {
        return 0;
    }
    std::memcpy(id, sOwnId.data(), sOwnId.size());
    return 1;
}

extern "C" void gdx_net_own_tracks_forget(void) {
    sForgets++;
}

extern "C" int gdx_content_cup_list(char outNames[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1]) {
    std::memcpy(outNames, sCup, sizeof(sCup));
    return sCupRc;
}

extern "C" int gdx_content_import_room_track_name(const char* title, char* out, int outSize) {
    sNameCalls++;
    sNameTitle = title != nullptr ? title : "";
    std::snprintf(out, static_cast<size_t>(outSize), "%s", "KEPT 2");
    return sNameRc;
}

extern "C" int gdx_snapshot_import_stage_content(const unsigned char* bytes, unsigned long length, const char* name,
                                                 void* base) {
    sStageCalls++;
    sStaged.assign(bytes, bytes + length);
    sStagedName = name;
    std::memset(base, 0x5A, GDX_CONTENT_TRACK_PAYLOAD_SIZE);
    return 1;
}

extern "C" int gdx_content_import_room_track_begin(const char* name, const uint8_t* payload) {
    sBeginCalls++;
    sBeginName = name;
    if (sBeginRc != GDX_CONTENT_OK || payload == nullptr || payload[0] != 0x5A) {
        return sBeginRc != GDX_CONTENT_OK ? sBeginRc : GDX_CONTENT_ERR_IO;
    }
    sPending = !sFinishAtOnce;
    sDone = true;
    return GDX_CONTENT_OK;
}

extern "C" int gdx_content_import_pending(void) {
    return sPending ? 1 : 0;
}

extern "C" int gdx_content_import_result(int32_t* outMfsError) {
    if (sPending) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (!sDone) {
        return GDX_CONTENT_ERR_NOT_FOUND;
    }
    sDone = false;
    sCollected++;
    if (outMfsError != nullptr) {
        *outMfsError = sFinishRc == GDX_CONTENT_OK ? 0 : 0xF1;
    }
    return sFinishRc;
}

extern "C" int gdx_content_import_room_track_full(void) {
    return sFinishFull ? 1 : 0;
}

namespace {

bool BuildTrack(std::vector<unsigned char>& bytes, gdx::net::track::TrackId& id) {
    using namespace gdx::snapshot;
    gdx_course_store_clear();
    gdx_branch_store_clear();
    gdx_settings_store_clear();
    gdx_course_registry_clear_extensions();
    if (gdx_course_title_set(kCourseId, "Save test") != GDX_COURSE_OK) {
        return false;
    }
    Generation author;
    author.disk = 1;
    author.courseId = kCourseId;
    if (!captureState(author, GdxSnapshotTest_Create(kPoints)) || !content(author, bytes)) {
        return false;
    }
    id = gdx::net::track::Identify(bytes.data(), bytes.size());
    gdx_course_store_clear();
    return gdx::net::track::Verify(bytes.data(), bytes.size(), id, nullptr) == gdx::net::track::Verdict::Ok;
}

std::vector<unsigned char> sTrack;
gdx::net::track::TrackId sTrackId{};

void Reset() {
    sServe = true;
    sServeId = sTrackId;
    sServeBytes = sTrack;
    std::memset(sCup, 0, sizeof(sCup));
    std::snprintf(sCup[0], sizeof(sCup[0]), "%s", "MINE");
    sCupRc = GDX_CONTENT_OK;
    sOwn = false;
    sForgets = 0;
    sNameTitle.clear();
    sNameRc = GDX_CONTENT_OK;
    sNameCalls = 0;
    sStaged.clear();
    sStagedName.clear();
    sStageCalls = 0;
    sBeginRc = GDX_CONTENT_OK;
    sBeginCalls = 0;
    sBeginName.clear();
    sFinishAtOnce = true;
    sPending = false;
    sDone = false;
    sFinishRc = GDX_CONTENT_OK;
    sFinishFull = false;
    sCollected = 0;
}

unsigned sNextRace = 100; // every test starts on a race the module has not seen

GdxNetRoomView Results(unsigned race) {
    GdxNetRoomView view;
    std::memset(&view, 0, sizeof(view));
    view.mode = GDX_NET_ROOM_MODE_JOINED;
    view.phase = GDX_NET_ROOM_PHASE_RESULTS;
    view.track = GDX_NET_ROOM_TRACK_CUSTOM;
    view.trackHeld = 1;
    view.raceIndex = race;
    std::memcpy(view.trackId, sTrackId.data(), sTrackId.size());
    std::snprintf(view.trackTitle, sizeof(view.trackTitle), "%s", "Sky Loop");
    return view;
}

void TestNoRow() {
    Reset();
    GdxNetRoomView view = Results(sNextRace++);
    view.phase = GDX_NET_ROOM_PHASE_LOBBY;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_NONE, "no row in the lobby");
    view.phase = GDX_NET_ROOM_PHASE_RACING;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_NONE, "no row during the race");
    view = Results(sNextRace++);
    view.track = 3;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_NONE, "no row for a stock track");
    view = Results(sNextRace++);
    view.trackHeld = 0;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_NONE, "no row for a track this copy does not hold");
    view = Results(sNextRace++);
    view.mode = GDX_NET_ROOM_MODE_IDLE;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_NONE, "no row outside a room");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_NONE && sBeginCalls == 0,
          "pressing a row that is not there does nothing");
    Check(gdx_net_track_save_state(nullptr) == GDX_NET_TRACK_SAVE_NONE, "no view, no row");
}

void TestSave() {
    Reset();
    GdxNetRoomView view = Results(sNextRace++);
    view.mode = GDX_NET_ROOM_MODE_HOSTING;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_READY, "the results of a custom track offer the save");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVED, "an install the disk finishes at once is saved");
    Check(sStageCalls == 1 && sStaged == sTrack, "the room's exact content bytes are staged");
    Check(sStagedName == "KEPT 2" && sBeginName == "KEPT 2", "staged and installed under the free name");
    Check(sNameCalls == 1 && sNameTitle == "SKY LOOP", "the name comes from the room title as the screen shows it");
    Check(sForgets == 1, "the vote reads the Edit Cup again");
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_SAVED, "the row keeps saying so");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVED && sBeginCalls == 1, "a second press saves nothing");

    // The next race on the same track starts over.
    view.raceIndex = sNextRace++;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_READY, "a new race asks again");
}

void TestOwn() {
    Reset();
    sOwn = true;
    sOwnId = sTrackId;
    const GdxNetRoomView view = Results(sNextRace++);
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_OWN, "a track from this copy's Edit Cup says so");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_OWN && sBeginCalls == 0, "and is not saved twice");
}

void TestFullCup() {
    Reset();
    for (int i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; ++i) {
        std::snprintf(sCup[i], sizeof(sCup[i]), "CUP %d", i);
    }
    const GdxNetRoomView view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FULL, "a full Edit Cup is said");
    Check(sStageCalls == 0 && sBeginCalls == 0, "and nothing is imported");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FULL && sBeginCalls == 0, "a full cup is not pressed again");

    // The disk thread finds the cup full after all (filled since the check): still nothing, and the same words.
    Reset();
    sFinishRc = GDX_CONTENT_ERR_IO;
    sFinishFull = true;
    const GdxNetRoomView late = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&late) == GDX_NET_TRACK_SAVE_FULL, "a cup filled before the install says full");
    Check(sForgets == 0, "nothing changed in the Edit Cup");

    // The import refuses for a full cup at its own check.
    Reset();
    sBeginRc = GDX_CONTENT_CUP_ERR_FULL;
    const GdxNetRoomView refused = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&refused) == GDX_NET_TRACK_SAVE_FULL, "the import's full answer is said");
}

void TestRefusals() {
    // The lobby's copy no longer matches what the room raced: nothing is staged.
    Reset();
    sServeBytes[sServeBytes.size() / 2] ^= 1;
    GdxNetRoomView view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED, "changed bytes fail the hash");
    Check(sStageCalls == 0 && sBeginCalls == 0, "and are not staged");
    // A press after the lobby hands over the right copy saves it.
    sServeBytes = sTrack;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVED, "trying again saves it");

    // Another track's copy.
    Reset();
    sServeId[0] ^= 0xFF;
    view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED && sStageCalls == 0, "another track's copy fails");

    // The lobby holds nothing.
    Reset();
    sServe = false;
    view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED && sStageCalls == 0, "no copy fails");

    // A busy disk is a moment, not a failure: the row stays pressable.
    Reset();
    sCupRc = GDX_CONTENT_ERR_BUSY;
    view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_READY && sStageCalls == 0, "a busy cup read waits");
    sCupRc = GDX_CONTENT_OK;
    sBeginRc = GDX_CONTENT_IMPORT_ERR_PENDING;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_READY, "another import in flight waits");
    sBeginRc = GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_READY, "a live race waits");
    sBeginRc = GDX_CONTENT_IMPORT_ERR_VALIDATION;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED, "a refused import fails");
    sBeginRc = GDX_CONTENT_OK;
    sNameRc = GDX_CONTENT_IMPORT_ERR_NO_NAME;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED, "no free name fails");
    sNameRc = GDX_CONTENT_OK;
    sFinishRc = GDX_CONTENT_ERR_IO;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_FAILED, "a disk error fails");
    Check(sForgets == 0, "and the Edit Cup is not read again");
    sFinishRc = GDX_CONTENT_OK;
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVED, "a failure may be tried again");
}

void TestPending() {
    // The install is still on the disk thread: the row waits, then says what happened.
    Reset();
    sFinishAtOnce = false;
    GdxNetRoomView view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVING, "a queued install is saving");
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_SAVING, "while the disk works");
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVING && sBeginCalls == 1, "a press meanwhile does nothing");
    sPending = false;
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_SAVED, "the finished install is collected");
    Check(sCollected == 1 && sForgets == 1, "once");

    // The page closes before the install ends: poll collects it all the same, and the same results page later
    // says it was saved.
    Reset();
    sFinishAtOnce = false;
    view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVING, "queued");
    GdxNetRoomView lobby = view;
    lobby.phase = GDX_NET_ROOM_PHASE_LOBBY;
    Check(gdx_net_track_save_state(&lobby) == GDX_NET_TRACK_SAVE_NONE, "the page is gone");
    sPending = false;
    gdx_net_track_save_poll();
    Check(sCollected == 1 && sForgets == 1, "the result is collected without the page");
    gdx_net_track_save_poll();
    Check(sCollected == 1, "and only once");
    Check(gdx_net_track_save_state(&view) == GDX_NET_TRACK_SAVE_SAVED, "the same results say it was saved");

    // A new race starts before the old install ends: the old answer does not land on the new race's row.
    Reset();
    sFinishAtOnce = false;
    view = Results(sNextRace++);
    Check(gdx_net_track_save_begin(&view) == GDX_NET_TRACK_SAVE_SAVING, "queued for the first race");
    GdxNetRoomView next = Results(sNextRace++);
    Check(gdx_net_track_save_state(&next) == GDX_NET_TRACK_SAVE_READY, "the next race offers the save");
    sPending = false;
    Check(gdx_net_track_save_state(&next) == GDX_NET_TRACK_SAVE_READY, "and keeps offering it");
    Check(sCollected == 1, "the old install was collected");
}

} // namespace

int main() {
    const bool built = BuildTrack(sTrack, sTrackId);
    Check(built, "the author's track builds");
    if (built) {
        TestNoRow();
        TestSave();
        TestOwn();
        TestFullCup();
        TestRefusals();
        TestPending();
    }
    std::printf("gdx_net_track_save_tests: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
