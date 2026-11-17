// port/gdx_net_room_course.cpp -- a G-NET room's custom track, raced from memory. See gdx_net_room_course.h.
// Copyright (c) Zorkats

#include "gdx_net_room_course.h"

#include "gdx_course_bounds.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_meta.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_course_snapshot.h"
#include "gdx_course_snapshot_state.h"
#include "gdx_course_state.h"
#include "gdx_net_race_room.h"
#include "net/gdx_net_track_pack.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using gdx::snapshot::Bytes;
using gdx::snapshot::Metadata;

struct RoomCourse {
    bool prepared = false; // the room's custom track is decoded and waits for its Course_Load
    bool active = false;   // the course raced is that track
    bool refused = false;
    Bytes runtime;         // the CourseContext image Course_Load copies
    bool haveForks = false;
    GdxBranchSet forks{};
    bool haveSettings = false;
    GdxCourseSettings settings{};
    bool haveSkybox = false;
    GdxCourseExtRecord skybox{};
    gdx::net::track::TrackId id{};
    bool haveInfo = false; // the owner's Info reached the lobby (protocol 15)
    GdxCourseMeta meta{};  // its creator names; empty when it has none or its block did not decode
    std::string songPath;  // its song, once the lobby holds it whole; empty until then
};

RoomCourse& Course() {
    static RoomCourse course;
    return course;
}

void Drop() {
    RoomCourse& course = Course();
    course.prepared = course.active = course.refused = false;
    course.runtime.clear();
    course.haveForks = course.haveSettings = course.haveSkybox = false;
    course.forks = GdxBranchSet{};
    course.settings = GdxCourseSettings{};
    course.skybox = GdxCourseExtRecord{};
    course.id = gdx::net::track::TrackId{};
    course.haveInfo = false;
    course.meta = GdxCourseMeta{};
    course.songPath.clear();
    gdx_course_bounds_force_extended(0);
}

// Decodes the metadata of a verified track into course. False when a block is there but does not decode.
bool DecodeMetadata(const gdx::snapshot::Generation& generation, RoomCourse& course) {
    const uint32_t binding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(generation.courseId.data()), generation.courseId.size());
    const Bytes& forks = generation.metadata[size_t(Metadata::Forks)];
    if (!forks.empty()) {
        if (gdx_branch_decode(forks.data(), forks.size(), binding, &course.forks) != 0) {
            return false;
        }
        course.haveForks = true;
    }
    const Bytes& settings = generation.metadata[size_t(Metadata::Settings)];
    if (!settings.empty()) {
        if (gdx_settings_decode(settings.data(), settings.size(), binding, &course.settings) != 0) {
            return false;
        }
        course.haveSettings = true;
    }
    const Bytes& extensions = generation.metadata[size_t(Metadata::Extensions)];
    if (!extensions.empty()) {
        static GdxCourseExtBlock block;
        block = GdxCourseExtBlock{};
        if (gdx_course_ext_decode(extensions.data(), extensions.size(), binding, &block) != 0) {
            return false;
        }
        for (int i = 0; i < block.count; i++) {
            if (block.records[i].kind == GDX_COURSE_KIND_SKYBOX && block.records[i].scope == GDX_COURSE_SCOPE_COURSE) {
                course.skybox = block.records[i];
                course.haveSkybox = true;
            }
        }
    }
    return true;
}

// Asks the lobby for the track's Info and song. The song may still be on its way at prepare(), so it is asked
// again when the race arms its music; a path once found stays.
void RefreshInfo(RoomCourse& course) {
    static std::vector<unsigned char> meta(512);
    static std::vector<char> path(4096);
    unsigned long length = 0;
    if (!gdx_lan_race_info(course.id.data(), meta.data(), static_cast<unsigned long>(meta.size()), &length,
                           path.data(), static_cast<unsigned long>(path.size()))) {
        return;
    }
    if (!course.haveInfo) {
        course.haveInfo = true;
        // A block bound to another track, or damaged, shows no names.
        const uint32_t bind = gdx_course_registry_crc32(course.id.data(), course.id.size());
        if (length == 0 || gdx_meta_decode(meta.data(), length, bind, &course.meta) != GDX_META_OK) {
            course.meta = GdxCourseMeta{};
        }
    }
    if (course.songPath.empty() && path[0] != 0) {
        course.songPath = path.data();
    }
}

} // namespace

extern "C" int gdx_net_room_course_prepare(int track) {
    Drop();
    if (track != GDX_NET_ROOM_COURSE_TRACK_CUSTOM) {
        return 1;
    }

    static Bytes bytes;
    bytes.assign(gdx::net::track::kMaxBytes, 0);
    gdx::net::track::TrackId id{};
    unsigned long length = 0;
    if (!gdx_lan_race_track(id.data(), bytes.data(), static_cast<unsigned long>(bytes.size()), &length)) {
        return 0;
    }
    // The lobby verified these bytes when they arrived. They are checked again here, on the thread that
    // decodes them, so nothing reaches the course image that this copy did not hash itself.
    gdx::snapshot::Generation generation;
    if (gdx::net::track::Verify(bytes.data(), length, id, &generation) != gdx::net::track::Verdict::Ok) {
        return 0;
    }

    RoomCourse& course = Course();
    // A room races every track at the extended ceiling, whatever this copy's own setting: the track's author
    // may have built above the stock one, and every copy must accept the same points (plan O4.8, risk L1).
    gdx_course_bounds_force_extended(1);
    if (!gdx::snapshot::expandRuntime(generation, course.runtime) || !DecodeMetadata(generation, course)) {
        Drop();
        return 0;
    }
    course.id = id;
    RefreshInfo(course);
    course.prepared = true;
    return 1;
}

extern "C" int gdx_net_room_course_take(int courseIndex, int roomRace) {
    RoomCourse& course = Course();
    course.active = course.prepared && roomRace && courseIndex == GDX_NET_ROOM_COURSE_INDEX;
    return course.active ? 1 : 0;
}

extern "C" int gdx_net_room_course_copy(void* runtime, unsigned long size) {
    const RoomCourse& course = Course();
    if (!course.active || runtime == nullptr || size != course.runtime.size()) {
        return 0;
    }
    std::memcpy(runtime, course.runtime.data(), course.runtime.size());
    return 1;
}

extern "C" int gdx_net_room_course_active(void) {
    return Course().active ? 1 : 0;
}

extern "C" void gdx_net_room_course_refuse(void) {
    Course().refused = true;
}

extern "C" int gdx_net_room_course_refused(void) {
    return Course().refused ? 1 : 0;
}

extern "C" void gdx_net_room_course_apply_forks(void) {
    const RoomCourse& course = Course();
    gdx_branch_set_active(course.active && course.haveForks ? &course.forks : nullptr);
}

extern "C" void gdx_net_room_course_apply_settings(int ringCount) {
    const RoomCourse& course = Course();
    if (!course.active || !course.haveSettings) {
        gdx_settings_set_active(nullptr);
        return;
    }
    static GdxCourseSettings settings;
    settings = course.settings;
    if (settings.ringCount != ringCount) {
        std::memset(settings.ring, 0, sizeof(settings.ring));
        std::memset(settings.nodes, 0, sizeof(settings.nodes));
        settings.ringCount = 0;
    }
    gdx_settings_set_active(&settings);
}

extern "C" int gdx_net_room_course_skybox(void) {
    const RoomCourse& course = Course();
    if (!course.active || !course.haveSkybox) {
        return -1;
    }
    int missing = 0;
    gdx_course_boot();
    const int stock = gdx_course_registry_resolve(GDX_COURSE_KIND_SKYBOX, course.skybox.key, &missing);
    return missing != 0 ? -1 : stock;
}

extern "C" int gdx_net_room_course_meta(struct GdxCourseMeta* out) {
    RoomCourse& course = Course();
    if (out == nullptr || !course.active) {
        return 0;
    }
    if (!course.haveInfo) {
        RefreshInfo(course);
    }
    *out = course.meta;
    return 1;
}

extern "C" const char* gdx_net_room_course_song(void) {
    RoomCourse& course = Course();
    if (!course.active) {
        return nullptr;
    }
    if (course.songPath.empty()) {
        RefreshInfo(course);
    }
    return course.songPath.empty() ? nullptr : course.songPath.c_str();
}

extern "C" void gdx_net_room_course_end(void) {
    Drop();
}
