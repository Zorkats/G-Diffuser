// port/gdx_net_track_save.cpp -- "Save to my tracks" for a G-NET room's custom track. See gdx_net_track_save.h.
// Copyright (c) Zorkats

#include "gdx_net_track_save.h"

#include "gdx_content_import.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_net_own_tracks.h"
#include "gdx_net_race_room.h"
#include "gdx_net_text.h"
#include "net/gdx_net_track_pack.h"
#include "port_log.h"

#include <cstring>
#include <vector>

namespace {

// One race's track: the row's answer is kept for it and for nothing else.
struct Key {
    bool set = false;
    unsigned race = 0;
    unsigned char id[GDX_NET_ROOM_TRACK_ID_BYTES]{};
};

struct Save {
    Key key;
    int state = GDX_NET_TRACK_SAVE_NONE;
    bool pending = false; // an install is queued, perhaps for a race whose page has closed
    Key pendingKey;
    char name[GDX_CONTENT_CUP_NAME_MAX + 1]{};
};

Save& State() {
    static Save save;
    return save;
}

Key KeyOf(const GdxNetRoomView* view) {
    Key key;
    key.set = true;
    key.race = view->raceIndex;
    std::memcpy(key.id, view->trackId, sizeof(key.id));
    return key;
}

bool Same(const Key& a, const Key& b) {
    return a.set && b.set && a.race == b.race && std::memcmp(a.id, b.id, sizeof(a.id)) == 0;
}

bool Shows(const GdxNetRoomView* view) {
    return view != nullptr &&
           (view->mode == GDX_NET_ROOM_MODE_HOSTING || view->mode == GDX_NET_ROOM_MODE_JOINED) &&
           view->phase == GDX_NET_ROOM_PHASE_RESULTS && view->track == GDX_NET_ROOM_TRACK_CUSTOM &&
           view->trackHeld != 0;
}

// The track is one of this copy's own Edit Cup tracks: saving it would only make a second copy.
bool Own(const unsigned char* id) {
    for (int slot = 0; slot < GDX_NET_OWN_TRACK_SLOTS; ++slot) {
        unsigned char own[GDX_NET_ROOM_TRACK_ID_BYTES]{};
        if (gdx_net_own_track(slot, own, nullptr, 0) && std::memcmp(own, id, sizeof(own)) == 0) {
            return true;
        }
    }
    return false;
}

int Start(Save& save, const GdxNetRoomView* view) {
    char cup[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1];
    int rc = gdx_content_cup_list(cup);
    if (rc == GDX_CONTENT_ERR_BUSY) {
        return GDX_NET_TRACK_SAVE_READY;
    }
    if (rc != GDX_CONTENT_OK) {
        gdx_port_logf("[net-save] the Edit Cup cannot be read (%d)\n", rc);
        return GDX_NET_TRACK_SAVE_FAILED;
    }
    bool room = false;
    for (const auto& slot : cup) {
        room = room || slot[0] == '\0';
    }
    if (!room) {
        return GDX_NET_TRACK_SAVE_FULL;
    }

    static std::vector<unsigned char> bytes;
    bytes.assign(gdx::net::track::kMaxBytes, 0);
    gdx::net::track::TrackId id{};
    unsigned long length = 0;
    if (!gdx_lan_race_track(id.data(), bytes.data(), static_cast<unsigned long>(bytes.size()), &length) ||
        std::memcmp(id.data(), view->trackId, id.size()) != 0) {
        gdx_port_logf("[net-save] this copy no longer holds the race's track\n");
        return GDX_NET_TRACK_SAVE_FAILED;
    }
    // Hashed again on the thread that decodes it, as the race load does (gdx_net_room_course.cpp).
    if (gdx::net::track::Verify(bytes.data(), length, id, nullptr) != gdx::net::track::Verdict::Ok) {
        gdx_port_logf("[net-save] the race's track does not check out\n");
        return GDX_NET_TRACK_SAVE_FAILED;
    }

    char title[GDX_NET_ROOM_TEXT_BYTES];
    gdx_net_text_filter(GDX_NET_ROOM_FONT_TITLE, view->trackTitle, title, static_cast<int>(sizeof(title)));
    char name[GDX_CONTENT_CUP_NAME_MAX + 1];
    rc = gdx_content_import_room_track_name(title, name, static_cast<int>(sizeof(name)));
    if (rc == GDX_CONTENT_ERR_BUSY) {
        return GDX_NET_TRACK_SAVE_READY;
    }
    if (rc != GDX_CONTENT_OK) {
        gdx_port_logf("[net-save] no disk name for the track (%d)\n", rc);
        return GDX_NET_TRACK_SAVE_FAILED;
    }
    static unsigned char base[GDX_CONTENT_TRACK_PAYLOAD_SIZE];
    if (!gdx_snapshot_import_stage_content(bytes.data(), length, name, base)) {
        gdx_port_logf("[net-save] the track could not be staged for import\n");
        return GDX_NET_TRACK_SAVE_FAILED;
    }

    rc = gdx_content_import_room_track_begin(name, base);
    switch (rc) {
        case GDX_CONTENT_OK:
            save.pending = true;
            save.pendingKey = save.key;
            std::memcpy(save.name, name, sizeof(save.name));
            return GDX_NET_TRACK_SAVE_SAVING;
        case GDX_CONTENT_CUP_ERR_FULL:
            return GDX_NET_TRACK_SAVE_FULL;
        case GDX_CONTENT_ERR_BUSY:
        case GDX_CONTENT_IMPORT_ERR_PENDING:
        case GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY:
            return GDX_NET_TRACK_SAVE_READY; // another disk job: the player may press again
        default:
            gdx_port_logf("[net-save] the import was refused (%d)\n", rc);
            return GDX_NET_TRACK_SAVE_FAILED;
    }
}

} // namespace

extern "C" void gdx_net_track_save_poll(void) {
    Save& save = State();
    if (!save.pending || gdx_content_import_pending()) {
        return;
    }
    int32_t mfsError = 0;
    const int rc = gdx_content_import_result(&mfsError);
    save.pending = false;
    int result = GDX_NET_TRACK_SAVE_FAILED;
    if (rc == GDX_CONTENT_OK) {
        result = GDX_NET_TRACK_SAVE_SAVED;
        gdx_net_own_tracks_forget(); // the Edit Cup changed: the vote reads its slots again
        gdx_port_logf("[net-save] saved as %s in the Edit Cup\n", save.name);
    } else if (gdx_content_import_room_track_full()) {
        result = GDX_NET_TRACK_SAVE_FULL;
        gdx_port_logf("[net-save] the Edit Cup filled up before the save: nothing imported\n");
    } else {
        gdx_port_logf("[net-save] saving %s failed (%d, disk error %d)\n", save.name, rc, static_cast<int>(mfsError));
    }
    if (Same(save.pendingKey, save.key)) {
        save.state = result;
    }
}

extern "C" int gdx_net_track_save_state(const GdxNetRoomView* view) {
    gdx_net_track_save_poll();
    if (!Shows(view)) {
        return GDX_NET_TRACK_SAVE_NONE;
    }
    Save& save = State();
    const Key key = KeyOf(view);
    if (!Same(save.key, key)) {
        save.key = key;
        save.state = Own(view->trackId) ? GDX_NET_TRACK_SAVE_OWN : GDX_NET_TRACK_SAVE_READY;
    }
    return save.state;
}

extern "C" int gdx_net_track_save_begin(const GdxNetRoomView* view) {
    const int now = gdx_net_track_save_state(view);
    if (now != GDX_NET_TRACK_SAVE_READY && now != GDX_NET_TRACK_SAVE_FAILED) {
        return now;
    }
    Save& save = State();
    save.state = Start(save, view);
    gdx_net_track_save_poll(); // the port's disk thread runs the install before the queue call returns
    return save.state;
}
