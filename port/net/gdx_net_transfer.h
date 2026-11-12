// port/net/gdx_net_transfer.h -- the custom track transfer codec, receiver, sender and host relay (G-NET O4.2).
// Copyright (c) Zorkats
//
// A track moves as its content bytes (gdx_net_track_pack.h), in chunks of at most kChunkData bytes, one reliable
// message each. The host offers the track to every guest. A guest that lacks it asks for it, and the host streams
// the chunks it holds to each asker. The guest that made the track sends its chunks to the host only when the host
// asks for them, so guests never send to each other (ODD 8.1, decision 15).
//
// Pure: no socket, no clock, no thread, no game header. Every time is a nowMs argument. The caller owns the
// channels: it encodes with the functions here and sends the bytes under the lobby type number it assigns (O4.4),
// and what it pops goes through Admit and a decoder before it reaches a receiver. Payloads carry no type byte, so
// one codec serves any type number.
//
// Payloads are little-endian, as in gdx_net_lobby.h. Every decoder checks each length and field before use,
// refuses trailing bytes, and leaves its output alone on failure.
//
//   Offer  id[32], u32 size, u8 title length, title     host to guest, or owner to host in answer to the host's
//                                                       Need. size 1..kMaxBytes, title 0..kTitleMax bytes
//   Chunk  id[32], u16 index, data to the end           host to guest, or owner to host. data 1..kChunkData bytes
//   Have   id[32]                                       guest to host: this guest already holds the track
//   Need   id[32]                                       guest to host: send it to me; host to owner: send it to the host
//   Abort  id[32], u8 AbortReason                       host to a guest or the owner, or owner to host

#pragma once

#include "gdx_net_track_pack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gdx::net::transfer {

// Data bytes in one chunk (decision 6). A chunk message is 1134 bytes, under kMaxMessagePayload.
constexpr size_t kChunkData = 1100;
// A title is an MFS name (GDX_COURSE_TITLE_MAX). A longer one is refused, never cut (decision 15).
constexpr size_t kTitleMax = 20;
// Chunks one transfer may have unacknowledged on one channel. Half the reliable window, so lobby messages always
// find a slot (decision 6).
constexpr size_t kTransferShare = 32;
// Chunks in the largest track.
constexpr size_t kMaxChunks = (track::kMaxBytes + kChunkData - 1) / kChunkData;

// The five kinds. The enum names the kinds for the gate and the counters; it is not a wire value.
enum class Kind : uint8_t {
    Offer = 1,
    Chunk = 2,
    Have = 3,
    Need = 4,
    Abort = 5,
};

// Why a transfer stopped. The numbers are wire values: never renumber them.
enum class AbortReason : uint8_t {
    Cancelled = 1, // the sender stopped on purpose: the room moved on, or the owner left
    Stalled = 2,   // the sender made no progress for longer than the room allows
    Failed = 3,    // the whole track failed its check, so no copy in the room can be trusted
};

struct Offer {
    track::TrackId id{};
    uint32_t size = 0;
    std::string title;
};

struct Chunk {
    track::TrackId id{};
    uint16_t index = 0;
    std::vector<uint8_t> data;
};

struct Abort {
    track::TrackId id{};
    AbortReason reason = AbortReason::Cancelled;
};

// Chunks in a track of size bytes, and the length of chunk index. The caller has checked size.
size_t ChunkCount(size_t size);
size_t ChunkLength(size_t size, size_t index);

// The encoders refuse what their decoder would refuse, so a sender cannot emit a message its peer must drop.
bool EncodeOffer(const Offer& offer, std::vector<uint8_t>& out);
bool EncodeChunk(const track::TrackId& id, size_t index, const uint8_t* data, size_t length,
                 std::vector<uint8_t>& out);
std::vector<uint8_t> EncodeHave(const track::TrackId& id);
std::vector<uint8_t> EncodeNeed(const track::TrackId& id);
bool EncodeAbort(const Abort& value, std::vector<uint8_t>& out);

bool DecodeOffer(const uint8_t* data, size_t len, Offer& out);
bool DecodeChunk(const uint8_t* data, size_t len, Chunk& out);
bool DecodeHave(const uint8_t* data, size_t len, track::TrackId& out);
bool DecodeNeed(const uint8_t* data, size_t len, track::TrackId& out);
bool DecodeAbort(const uint8_t* data, size_t len, Abort& out);

// Why a message was dropped. Counted, not logged, so a flood of bad traffic costs one increment each.
enum class Drop : uint8_t {
    Phase,       // the room says no transfer may run
    WrongSender, // the peer may not send this kind (decision 15)
    Malformed,   // the payload fails its decoder, or an Offer is out of range
    NoOffer,     // a chunk arrived before its track was offered
    WrongTrack,  // the message names another track
    OutOfRange,  // a chunk index past the track's last chunk
    WrongLength, // a chunk whose length is not the length its place holds in the track
    Duplicate,   // a chunk already held, or a second Offer
    Late,        // a message after the transfer has ended
};
constexpr size_t kDropReasons = 9;

class Counters {
public:
    void Count(Drop why) {
        counts_[static_cast<size_t>(why)]++;
    }

    uint32_t Get(Drop why) const {
        return counts_[static_cast<size_t>(why)];
    }

private:
    std::array<uint32_t, kDropReasons> counts_{};
};

// The bytes of one track as this side holds them. The owner holds every chunk; a receiver holds the chunks it has
// taken, in whatever order they came. A chunk is stored once and never replaced.
class TrackBuffer {
public:
    // Null when size is outside 1..kMaxBytes.
    static std::shared_ptr<TrackBuffer> Empty(const track::TrackId& id, size_t size);
    // Null unless the bytes verify against id, so an owner's copy goes through the same check a receiver makes.
    // Every chunk is held.
    static std::shared_ptr<TrackBuffer> Whole(const track::TrackId& id, const uint8_t* bytes, size_t length);

    const track::TrackId& Id() const {
        return id_;
    }

    size_t Size() const {
        return bytes_.size();
    }

    size_t Count() const {
        return have_.size();
    }

    size_t Held() const {
        return held_;
    }

    bool Complete() const {
        return held_ == have_.size();
    }

    bool Has(size_t index) const {
        return index < have_.size() && have_[index];
    }

    // False when the chunk is not held yet. Otherwise data and length point at it.
    bool ChunkAt(size_t index, const uint8_t*& data, size_t& length) const;

    // Stores a chunk, once its index and exact length have been checked here. False when it is held already.
    bool Put(size_t index, const uint8_t* data, size_t length);

    // The whole track. Meaningful once Complete().
    const uint8_t* Bytes() const {
        return bytes_.data();
    }

private:
    TrackBuffer(const track::TrackId& id, size_t size);

    track::TrackId id_;
    std::vector<uint8_t> bytes_;
    std::vector<bool> have_;
    size_t held_ = 0;
};

// Who a transfer message came from, as the receiving side sees it.
enum class Peer : uint8_t {
    Host,  // a guest's view of the host
    Owner, // the host's view of the guest it asked for a track
    Guest, // the host's view of any other guest
};

// The caller builds the route from its own lobby state. Admit trusts it, so it must come from the room's bookkeeping
// and never from anything the peer wrote.
struct Route {
    bool localIsHost = false;
    // True on the one guest the host asked for the track. Only that guest may take the host's Need.
    bool localIsOwner = false;
    Peer from = Peer::Guest;
    // True while the room is in MACHINE or ASSETS, the two phases a track may move in (decision 9).
    bool transferOpen = false;
};

// Decision 15: each kind has one sender and one phase. True when the message may go on to its decoder. A refusal is
// counted under its reason in counters; the caller keeps one Counters for the lobby.
bool Admit(Kind kind, const Route& route, Counters& counters);

// The receiving side of one track: a guest's copy, or the host's copy of the owner's upload. It takes the one
// track it was told to expect, counts every refusal, and verifies the whole buffer once every chunk is in.
class Receiver {
public:
    // Stored states are Waiting, Receiving, Aborted, Ok and Failed. Stalled is derived from the time of the last
    // progress, so the receiver never reads a clock itself.
    enum class State : uint8_t {
        Waiting,
        Receiving,
        Stalled,
        Aborted,
        Ok,
        Failed,
    };

    explicit Receiver(const track::TrackId& expected);

    bool OnOffer(const Offer& offer, uint32_t nowMs);
    bool OnChunk(const Chunk& chunk, uint32_t nowMs);
    // The caller has checked that the Abort came from the legitimate sender (Admit).
    bool OnAbort(const Abort& value);

    // Once a receiving transfer has gone stallMs without a chunk, its state reads Stalled. The room decides what to
    // do about it.
    State StateAt(uint32_t nowMs, uint32_t stallMs) const;
    uint32_t LastProgressMs() const {
        return lastProgressMs_;
    }

    // Empty until the buffer has been verified, then Ok or the verdict that failed it. Never decoded before then.
    track::Verdict Verdict() const {
        return verdict_;
    }

    bool Aborted() const {
        return aborted_;
    }

    // Meaningful when Aborted().
    AbortReason Cause() const {
        return cause_;
    }

    // Set when Verdict() is Ok.
    const gdx::snapshot::Generation& Decoded() const {
        return decoded_;
    }

    const std::string& Title() const {
        return title_;
    }

    // Null until the Offer, and again once the transfer has failed or been aborted, so a relay never sends bytes
    // that did not verify. While the chunks are still arriving it is shared with the relay, which streams from it.
    std::shared_ptr<const TrackBuffer> Buffer() const {
        if (state_ == State::Failed || state_ == State::Aborted) {
            return nullptr;
        }
        return buffer_;
    }

    const Counters& Counts() const {
        return counts_;
    }

private:
    void Finish();

    track::TrackId expected_;
    State state_ = State::Waiting;
    std::shared_ptr<TrackBuffer> buffer_;
    std::string title_;
    uint32_t lastProgressMs_ = 0;
    track::Verdict verdict_ = track::Verdict::Empty;
    bool aborted_ = false;
    AbortReason cause_ = AbortReason::Cancelled;
    gdx::snapshot::Generation decoded_;
    Counters counts_;
};

// Streams one buffer to one peer, in order. It holds its place at the first chunk not yet in the buffer, so a relay
// can start while the owner's chunks are still arriving. The buffer must not be null.
class Sender {
public:
    // Returns false when the channel refuses the payload (QueueFull). The chunk then stays next.
    using EmitFn = std::function<bool(const std::vector<uint8_t>& payload)>;

    explicit Sender(std::shared_ptr<const TrackBuffer> buffer);

    // Emits chunks while the channel's outstanding count, plus what this call has emitted, stays below
    // kTransferShare. Returns the number emitted.
    size_t Pump(uint32_t nowMs, size_t outstanding, const EmitFn& emit);

    bool Finished() const {
        return next_ >= buffer_->Count();
    }

    size_t Next() const {
        return next_;
    }

    uint32_t LastEmitMs() const {
        return lastEmitMs_;
    }

private:
    std::shared_ptr<const TrackBuffer> buffer_;
    size_t next_ = 0;
    uint32_t lastEmitMs_ = 0;
};

// The host's relay for one track: one Sender per guest that asked for it. A guest that said Have is left alone.
class Relay {
public:
    explicit Relay(std::shared_ptr<const TrackBuffer> buffer);

    // A guest asked for the track: it is streamed from the first chunk. Ignored once the guest said Have.
    void Need(uint8_t guest);
    void Have(uint8_t guest);
    void Forget(uint8_t guest);
    bool Streaming(uint8_t guest) const;

    size_t Pump(uint8_t guest, uint32_t nowMs, size_t outstanding, const Sender::EmitFn& emit);

private:
    std::shared_ptr<const TrackBuffer> buffer_;
    std::map<uint8_t, Sender> senders_;
    std::set<uint8_t> holds_;
};

} // namespace gdx::net::transfer
