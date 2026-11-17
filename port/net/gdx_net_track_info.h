// port/net/gdx_net_track_info.h: the creator names and the song of a room's custom track (protocol 15).
// Copyright (c) Zorkats
//
// A track travels as its content bytes, which hold neither its creators nor its song file: those live beside
// the track on the owner's copy (gdx_course_meta.h, gdx_course_music.h). Info carries them, once per race:
//
//   1. The room names a custom track and its owner (gdx_net_track_flow.h).
//   2. The host asks the owner with InfoNeed, unless the host is the owner. The host asks even when it holds
//      the track already, which is why Info is not part of the track's Offer: a host that holds the track never
//      hears the owner's Offer.
//   3. The owner answers with Info once it is ready: the song's hash runs off the game thread, so an owner may
//      answer a second or so late.
//   4. The host keeps the owner's Info and sends it to every other guest, latecomers included.
//
// Like the track flow these classes are pure: no socket, no clock, no thread. The messages to send come back as
// Outgoing (gdx_net_room.h). Info and InfoNeed may move in any phase while the room's track is custom: they are
// small, and the song transfer they start keeps to MACHINE and ASSETS on its own.
//
//   InfoNeed  track id[32]                                        host to owner
//   Info      track id[32], u16 meta length, meta,                owner to host, host to guest
//             u8 has song (0 or 1), then when 1:
//             song id[32], u32 song size
//
// meta is a GXM1 block (gdx_course_meta.h), 0..kInfoMetaMax bytes, bound to the CRC-32 of the 32 track id
// bytes. This file checks only its length; the game decodes it. The song id is the SHA-256 of the song file
// and its size is 1..transfer::kSongMaxBytes; the song moves under types 43 to 47 (gdx_net_transfer.h).

#pragma once

#include "gdx_net_lobby.h"
#include "gdx_net_room.h"
#include "gdx_net_track_pack.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

namespace gdx::net::lobby {

// A GXM1 block with an author, an Edited by and a signature is under 200 bytes; this leaves room for later tags.
constexpr size_t kInfoMetaMax = 512;

struct TrackInfo {
    track::TrackId track{};
    std::vector<uint8_t> meta; // empty when the track has no creator names
    bool hasSong = false;
    track::TrackId song{};
    uint32_t songSize = 0;
};

std::vector<uint8_t> EncodeInfoNeed(const track::TrackId& id);
bool DecodeInfoNeed(const uint8_t* data, size_t len, track::TrackId& out);
// Refuses what DecodeInfo would refuse. The decoder refuses trailing bytes and leaves out alone on failure.
bool EncodeInfo(const TrackInfo& info, std::vector<uint8_t>& out);
bool DecodeInfo(const uint8_t* data, size_t len, TrackInfo& out);

// What this copy knows of one of its own tracks. Unknown: not one of this copy's tracks. Pending: its song is
// still being hashed. Called on the lobby thread, so it must be quick.
enum class InfoReady : uint8_t { Unknown, Pending, Ready };
using InfoSource = std::function<InfoReady(const track::TrackId& id, TrackInfo& out)>;

class HostInfoFlow {
public:
    explicit HostInfoFlow(InfoSource own = InfoSource());

    // Once per tick, after the room ran. track and owner: RoomMachine::CurrentTrack and TrackOwner. guests: the
    // joined players other than the host.
    void Sync(const TrackChoice& track, uint8_t owner, uint16_t raceIndex, const std::vector<uint8_t>& guests,
              std::vector<Outgoing>& out);

    // A message of type 41 or 42 from a joined guest. Only the owner the host asked may send Info, once.
    bool Receive(uint8_t from, uint8_t type, const uint8_t* data, size_t len);

    // The race's Info, or null until it is known.
    const TrackInfo* Info() const {
        return known_ ? &info_ : nullptr;
    }

private:
    void Reset();

    InfoSource own_;
    bool active_ = false;
    uint16_t race_ = 0;
    track::TrackId id_{};
    uint8_t asked_ = kSystemId; // the owner InfoNeed went to
    bool known_ = false;
    TrackInfo info_;
    std::set<uint8_t> sent_;
};

class ClientInfoFlow {
public:
    explicit ClientInfoFlow(InfoSource own = InfoSource());

    // Once per tick. track: the room's track as the last room message gave it.
    void Sync(const TrackChoice& track, uint16_t raceIndex, std::vector<Outgoing>& out);

    // A message of type 41 or 42 from the host. What to send back goes to out (Outgoing::to is unused).
    bool Receive(uint8_t type, const uint8_t* data, size_t len, std::vector<Outgoing>& out);

    // The race's Info: this copy's own when the track is one of its own, else the one the host sent.
    const TrackInfo* Info() const {
        return known_ ? &info_ : nullptr;
    }

private:
    void Reset();
    void Answer(std::vector<Outgoing>& out);

    InfoSource own_;
    bool active_ = false;
    uint16_t race_ = 0;
    track::TrackId id_{};
    bool asked_ = false; // the host sent InfoNeed and has no answer yet
    bool known_ = false;
    TrackInfo info_;
};

} // namespace gdx::net::lobby
