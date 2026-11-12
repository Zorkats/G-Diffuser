// port/net/gdx_net_track_flow.h: who sends which custom track message, and when, in a room (G-NET O4.6).
// Copyright (c) Zorkats
//
// gdx_net_transfer.h moves the bytes of one track; this file decides the moves for a room. The host runs one
// HostTrackFlow, each client one ClientTrackFlow. Both are pure like RoomMachine: no socket, no clock, no
// thread. Every call takes nowMs, and the messages to send come back as Outgoing (gdx_net_room.h) under the
// lobby's type numbers 26 to 30. Chunks are the exception: the caller hands Pump a function that sends one on
// the peer's channel and says whether the channel took it, so a full channel keeps the chunk for later.
//
// The flow of one race (decisions 9, 11, 15):
//   1. The room names a custom track (a winning ballot or a pick) and its owner, the player who named it.
//   2. Once MACHINE opens, the host asks the owner for it with Need, unless the host is the owner or already
//      holds the track. The owner answers with an Offer, which carries the size, and then the chunks. This
//      Offer from the owner to the host is the one addition to decision 15: without it the host could not
//      know the size before the first chunk.
//   3. As soon as the host has a buffer, whole or still filling, it offers the track to every other guest. A
//      guest that holds it says Have; one that lacks it says Need, and the host streams the chunks it has,
//      holding at the first one still missing.
//   4. A copy whose whole buffer verified keeps it in the cache (TrackStore::store) and tells the host with
//      Have. The host tells the room (RoomNews), and the room decides who races.
//   5. If the host's copy fails its check, or the owner aborts, the host aborts every stream and the room
//      draws a stock track. When the room's track changes, every stream is cancelled.

#pragma once

#include "gdx_net_lobby.h"
#include "gdx_net_room.h"
#include "gdx_net_transfer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace gdx::net::lobby {

// Where a copy finds a track's verified content bytes, and where it keeps one it downloaded. Both run on the
// lobby's thread, so a load must be quick: the cache, or a track of this copy's own that the game resolved
// beforehand (decision 8). Either may be empty: a copy without a store lacks every track and keeps none.
struct TrackStore {
    std::function<bool(const track::TrackId& id, std::vector<uint8_t>& bytes)> load;
    std::function<void(const track::TrackId& id, const std::vector<uint8_t>& bytes)> store;
};

// What the host's flow tells its room. held: players who now hold the track, the host included; the caller
// passes each to RoomMachine::TrackHeld. failed: the host cannot get the track (RoomMachine::TrackFailed).
struct RoomNews {
    std::vector<uint8_t> held;
    bool failed = false;
};

// A copy's view of its own download, for the room screen.
struct TrackProgress {
    bool active = false;   // the room's track is custom
    bool held = false;     // this copy holds it, verified
    bool failed = false;   // its download failed or was aborted
    size_t chunks = 0;     // received so far
    size_t of = 0;         // in the whole track; 0 before the Offer
};

class HostTrackFlow {
public:
    explicit HostTrackFlow(TrackStore store = TrackStore());

    // Once per tick, after the room ran. track and owner: RoomMachine::CurrentTrack and TrackOwner. open:
    // the room is in MACHINE or ASSETS, the two phases a track may move in. guests: the joined players other
    // than the host.
    void Sync(const TrackChoice& track, uint8_t owner, uint16_t raceIndex, bool open,
              const std::vector<uint8_t>& guests, uint32_t nowMs, std::vector<Outgoing>& out, RoomNews& news);

    // A message of type 26 to 30 from a joined guest. false when it was refused; the reason is counted.
    bool Receive(uint8_t from, uint8_t type, const uint8_t* data, size_t len, uint32_t nowMs,
                 std::vector<Outgoing>& out, RoomNews& news);

    // Streams chunks to one guest while its channel has room (outstanding: its reliable slots in use).
    size_t Pump(uint8_t guest, size_t outstanding, uint32_t nowMs, const transfer::Sender::EmitFn& emit);

    // The verified track of the race being set up, for the host's own game. Null until it verified.
    std::shared_ptr<const transfer::TrackBuffer> Held() const {
        return held_;
    }

    TrackProgress Progress() const;

    const transfer::Counters& Counts() const {
        return counts_;
    }

private:
    void Begin(const TrackChoice& track, uint8_t owner, uint16_t raceIndex, RoomNews& news);
    void Stop(transfer::AbortReason reason, std::vector<Outgoing>& out);
    void Abort(uint8_t to, transfer::AbortReason reason, std::vector<Outgoing>& out) const;

    TrackStore store_;
    bool active_ = false;
    bool open_ = false;
    uint16_t race_ = 0;
    track::TrackId id_{};
    std::string title_;
    uint8_t owner_ = kSystemId;
    bool needSent_ = false;
    bool failed_ = false;
    std::unique_ptr<transfer::Receiver> upload_;           // the owner's upload, when the host lacks the track
    std::shared_ptr<const transfer::TrackBuffer> buffer_;  // what the relay streams from, whole or filling
    std::shared_ptr<const transfer::TrackBuffer> held_;    // the same buffer once verified
    std::unique_ptr<transfer::Relay> relay_;
    std::set<uint8_t> offered_;
    transfer::Counters counts_;
};

class ClientTrackFlow {
public:
    explicit ClientTrackFlow(TrackStore store = TrackStore());

    // Once per tick. track: the room's track as the last room message gave it. owner: this copy named it.
    // open as for the host.
    void Sync(const TrackChoice& track, bool owner, uint16_t raceIndex, bool open, uint32_t nowMs);

    // A message of type 26 to 30 from the host. What to send back goes to out (Outgoing::to is unused).
    bool Receive(uint8_t type, const uint8_t* data, size_t len, uint32_t nowMs, std::vector<Outgoing>& out);

    // The owner's upload to the host, while the channel has room.
    size_t Pump(size_t outstanding, uint32_t nowMs, const transfer::Sender::EmitFn& emit);

    std::shared_ptr<const transfer::TrackBuffer> Held() const {
        return held_;
    }

    TrackProgress Progress() const;

    const transfer::Counters& Counts() const {
        return counts_;
    }

private:
    void Reset();

    TrackStore store_;
    bool active_ = false;
    bool open_ = false;
    bool owner_ = false;
    uint16_t race_ = 0;
    track::TrackId id_{};
    std::string title_;
    std::unique_ptr<transfer::Receiver> download_;
    std::unique_ptr<transfer::Sender> upload_;
    std::shared_ptr<const transfer::TrackBuffer> held_;
    transfer::Counters counts_;
};

} // namespace gdx::net::lobby
