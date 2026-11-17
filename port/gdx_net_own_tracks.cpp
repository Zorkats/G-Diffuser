// port/gdx_net_own_tracks.cpp -- see gdx_net_own_tracks.h.
// Copyright (c) Zorkats
//
// G-NET decision 8 has this read on the disk fiber, handing its result to the lobby worker through a queue.
// It runs on the game thread instead, under the busy guard that the content window's preview and the export
// already use: the payload read and the snapshot peek both refuse while the disk thread owns the save, and
// neither writes. The result reaches the worker through gdx_lan_room_keep_track, which takes the session's
// own lock.

#include "gdx_net_own_tracks.h"

#include "gdx_content_io.h"
#include "gdx_course_meta.h"
#include "gdx_course_music.h"
#include "gdx_course_registry.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_edit_cup.h"
#include "gdx_net_room_view.h"
#include "net/gdx_net_lobby.h"
#include "net/gdx_net_track_info.h"
#include "net/gdx_net_track_pack.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
// course_context.c: the Edit Cup's track names, its first six rows (gdx_content_import.c has the same split).
extern char gEditCupTrackNames[GDX_EDIT_CUP_ROWS][GDX_EDIT_CUP_NAME_SIZE];
extern uint16_t gWorkingDirectory;
}

namespace {

namespace track = gdx::net::track;
namespace lobby = gdx::net::lobby;

struct Slot {
    bool known = false; // the answer below is kept
    bool ok = false;
    std::string name;   // the slot's name when it was read
    track::TrackId id{};
    std::string title;
};

std::array<Slot, GDX_NET_OWN_TRACK_SLOTS> sSlots;

void Collect(const unsigned char* bytes, unsigned long length, void* ctx) {
    static_cast<std::vector<uint8_t>*>(ctx)->assign(bytes, bytes + length);
}

// The slot's name as a room shows it: its printable bytes, without the spaces at either end. A name with
// none is called Custom track.
std::string TitleOf(const std::string& name) {
    std::string title;
    for (char c : name) {
        title.push_back((c >= 0x20 && c <= 0x7E) ? c : ' ');
    }
    const size_t first = title.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "Custom track";
    }
    title = title.substr(first, title.find_last_not_of(' ') - first + 1);
    if (title.size() > lobby::kTrackTitleMax) {
        title.resize(lobby::kTrackTitleMax);
    }
    return title;
}

// What a room's racers need beside the track's bytes (protocol 15): its creator names as a GXM1 block bound to
// the CRC-32 of the track id, and the file of its song. Only the names and the signature go: the staff time,
// splits and ghost are the owner's records, and a room race keeps none. Either is left empty when the slot
// has none.
void Extras(const std::string& name, const track::TrackId& id, std::vector<uint8_t>& meta, std::string& song) {
    meta.clear();
    song.clear();
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];
    if (!gdx_course_music_course_id(name.c_str(), "CRSD", courseId, static_cast<int>(sizeof(courseId)))) {
        return;
    }
    GdxCourseMeta stored;
    if (gdx_meta_store_get(courseId, &stored) && (stored.author[0] != '\0' || stored.editedBy[0] != '\0')) {
        GdxCourseMeta names;
        std::memset(&names, 0, sizeof(names));
        std::memcpy(names.author, stored.author, sizeof(names.author));
        std::memcpy(names.editedBy, stored.editedBy, sizeof(names.editedBy));
        names.hasSignature = stored.hasSignature;
        std::memcpy(names.signature, stored.signature, sizeof(names.signature));
        // The encoder wants room for every record it could write; the names and the signature use far less, and
        // the Info message carries at most kInfoMetaMax bytes.
        std::vector<unsigned char> block(GDX_META_BLOCK_MAX_BYTES);
        const int length =
            gdx_meta_encode(&names, gdx_course_registry_crc32(id.data(), id.size()), block.data(), block.size());
        if (length > 0 && static_cast<size_t>(length) <= lobby::kInfoMetaMax) {
            meta.assign(block.begin(), block.begin() + length);
        }
    }
    char path[GDX_COURSE_PATH_MAX];
    if (gdx_course_music_course_song(courseId, path, static_cast<int>(sizeof(path)))) {
        song = path;
    }
}

// Reads the slot's track. false with known set when the slot holds no track a room can race, and with known
// clear when the disk was busy, so a later call tries again.
void Read(Slot& slot) {
    slot.known = false;
    slot.ok = false;
    if (slot.name.empty()) {
        slot.known = true;
        return;
    }
    std::vector<uint8_t> payload(GDX_CONTENT_TRACK_PAYLOAD_SIZE);
    const int rc = gdx_content_load_track_payload(slot.name.c_str(), payload.data(), payload.size());
    if (rc == GDX_CONTENT_ERR_BUSY) {
        return;
    }
    slot.known = true;
    std::vector<uint8_t> content;
    if (rc != GDX_CONTENT_OK ||
        !gdx_snapshot_content_peek(gWorkingDirectory, slot.name.c_str(), "CRSD", payload.data(), Collect, &content) ||
        content.empty()) {
        return;
    }
    // Checked as a racer who downloads it would check it, so a track that would be refused is never offered.
    const track::TrackId id = track::Identify(content.data(), content.size());
    if (track::Verify(content.data(), content.size(), id, nullptr) != track::Verdict::Ok) {
        return;
    }
    slot.id = id;
    slot.title = TitleOf(slot.name);
    slot.ok = true;
    std::vector<uint8_t> meta;
    std::string song;
    Extras(slot.name, id, meta, song);
    gdx_lan_room_keep_track(id.data(), content.data(), static_cast<unsigned long>(content.size()), meta.data(),
                            static_cast<unsigned long>(meta.size()), song.c_str());
}

} // namespace

extern "C" int gdx_net_own_track(int slot, unsigned char* id, char* title, int titleSize) {
    if (slot < 0 || slot >= GDX_NET_OWN_TRACK_SLOTS) {
        return 0;
    }
    Slot& s = sSlots[static_cast<size_t>(slot)];
    const char* row = gEditCupTrackNames[slot];
    const std::string name(row, strnlen(row, GDX_EDIT_CUP_NAME_SIZE));
    if (!s.known || s.name != name) {
        s.name = name;
        Read(s);
    }
    if (!s.ok) {
        return 0;
    }
    if (id != nullptr) {
        std::copy(s.id.begin(), s.id.end(), id);
    }
    if (title != nullptr && titleSize > 0) {
        std::snprintf(title, static_cast<size_t>(titleSize), "%s", s.title.c_str());
    }
    return 1;
}

extern "C" void gdx_net_own_tracks_forget(void) {
    for (Slot& s : sSlots) {
        s = Slot();
    }
}
