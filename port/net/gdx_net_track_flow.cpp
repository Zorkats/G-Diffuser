// port/net/gdx_net_track_flow.cpp: the room's custom track flow (gdx_net_track_flow.h).
// Copyright (c) Zorkats

#include "gdx_net_track_flow.h"

#include <algorithm>
#include <utility>

namespace gdx::net::lobby {

namespace {

using transfer::Abort;
using transfer::AbortReason;
using transfer::Admit;
using transfer::Kind;
using transfer::Peer;
using transfer::Receiver;
using transfer::Route;
using transfer::TrackBuffer;

bool KindOf(uint8_t type, Kind& kind) {
    switch (type) {
    case kMsgTrackOffer:
        kind = Kind::Offer;
        return true;
    case kMsgTrackChunk:
        kind = Kind::Chunk;
        return true;
    case kMsgTrackHave:
        kind = Kind::Have;
        return true;
    case kMsgTrackNeed:
        kind = Kind::Need;
        return true;
    case kMsgTrackAbort:
        kind = Kind::Abort;
        return true;
    default:
        return false;
    }
}

bool Decoded(Kind kind, const uint8_t* data, size_t len, transfer::Offer& offer, transfer::Chunk& chunk,
             track::TrackId& id, Abort& abort) {
    switch (kind) {
    case Kind::Offer:
        if (!transfer::DecodeOffer(data, len, offer)) {
            return false;
        }
        id = offer.id;
        return true;
    case Kind::Chunk:
        if (!transfer::DecodeChunk(data, len, chunk)) {
            return false;
        }
        id = chunk.id;
        return true;
    case Kind::Have:
        return transfer::DecodeHave(data, len, id);
    case Kind::Need:
        return transfer::DecodeNeed(data, len, id);
    case Kind::Abort:
        if (!transfer::DecodeAbort(data, len, abort)) {
            return false;
        }
        id = abort.id;
        return true;
    }
    return false;
}

bool OfferFor(const std::shared_ptr<const TrackBuffer>& buffer, const std::string& title, std::vector<uint8_t>& out) {
    transfer::Offer offer;
    offer.id = buffer->Id();
    offer.size = static_cast<uint32_t>(buffer->Size());
    offer.title = title;
    return transfer::EncodeOffer(offer, out);
}

// The whole track from the store, verified, or null.
std::shared_ptr<const TrackBuffer> LoadWhole(const TrackStore& store, const track::TrackId& id) {
    std::vector<uint8_t> bytes;
    if (!store.load || !store.load(id, bytes) || bytes.empty()) {
        return nullptr;
    }
    return TrackBuffer::Whole(id, bytes.data(), bytes.size());
}

void Keep(const TrackStore& store, const std::shared_ptr<const TrackBuffer>& buffer) {
    if (store.store) {
        store.store(buffer->Id(), std::vector<uint8_t>(buffer->Bytes(), buffer->Bytes() + buffer->Size()));
    }
}

} // namespace

HostTrackFlow::HostTrackFlow(TrackStore store) : store_(std::move(store)) {
}

void HostTrackFlow::Abort(uint8_t to, AbortReason reason, std::vector<Outgoing>& out) const {
    transfer::Abort value;
    value.id = id_;
    value.reason = reason;
    std::vector<uint8_t> bytes;
    if (transfer::EncodeAbort(value, bytes)) {
        out.push_back({ to, kMsgTrackAbort, std::move(bytes) });
    }
}

// Every stream this flow started is told it is over: the guests it offered the track to, and the owner when
// the host asked it to upload. A guest that already holds the track ignores the Abort.
void HostTrackFlow::Stop(AbortReason reason, std::vector<Outgoing>& out) {
    if (active_) {
        for (uint8_t guest : offered_) {
            Abort(guest, reason, out);
        }
        if (needSent_ && upload_ != nullptr && held_ == nullptr) {
            Abort(owner_, reason, out);
        }
    }
    active_ = false;
    needSent_ = false;
    failed_ = false;
    owner_ = kSystemId;
    title_.clear();
    upload_.reset();
    buffer_.reset();
    held_.reset();
    relay_.reset();
    offered_.clear();
}

void HostTrackFlow::Begin(const TrackChoice& track, uint8_t owner, uint16_t raceIndex, RoomNews& news) {
    active_ = true;
    race_ = raceIndex;
    id_ = track.custom.id;
    title_ = track.custom.title;
    owner_ = owner;
    std::shared_ptr<const TrackBuffer> whole = LoadWhole(store_, id_);
    if (whole != nullptr) {
        buffer_ = whole;
        held_ = whole;
        relay_ = std::make_unique<transfer::Relay>(whole);
        news.held.push_back(kHostId);
        return;
    }
    if (owner_ == kHostId) {
        // The host named a track it cannot load: nobody else has it.
        failed_ = true;
        news.failed = true;
        return;
    }
    upload_ = std::make_unique<Receiver>(id_);
}

void HostTrackFlow::Sync(const TrackChoice& track, uint8_t owner, uint16_t raceIndex, bool open,
                         const std::vector<uint8_t>& guests, uint32_t nowMs, std::vector<Outgoing>& out,
                         RoomNews& news) {
    (void)nowMs;
    const bool custom = track.track == kTrackCustom;
    if (active_ && (!custom || raceIndex != race_ || track.custom.id != id_)) {
        Stop(AbortReason::Cancelled, out);
    }
    open_ = open;
    if (!custom) {
        return;
    }
    if (!active_) {
        Begin(track, owner, raceIndex, news);
    }
    if (failed_ || !open) {
        return;
    }

    // A guest that left is forgotten, so a new player given its id starts clean.
    for (auto it = offered_.begin(); it != offered_.end();) {
        if (std::find(guests.begin(), guests.end(), *it) == guests.end()) {
            if (relay_ != nullptr) {
                relay_->Forget(*it);
            }
            it = offered_.erase(it);
        } else {
            ++it;
        }
    }

    if (upload_ != nullptr && held_ == nullptr) {
        if (!needSent_) {
            out.push_back({ owner_, kMsgTrackNeed, transfer::EncodeNeed(id_) });
            needSent_ = true;
        }
        if (upload_->Aborted() || (upload_->Verdict() != track::Verdict::Empty &&
                                   upload_->Verdict() != track::Verdict::Ok)) {
            // Nobody else can have the bytes (decision 11). What was relayed so far cannot verify anywhere.
            news.failed = true;
            failed_ = true;
            for (uint8_t guest : offered_) {
                Abort(guest, AbortReason::Failed, out);
            }
            offered_.clear();
            relay_.reset();
            buffer_.reset();
            return;
        }
        if (upload_->Verdict() == track::Verdict::Ok) {
            held_ = upload_->Buffer();
            Keep(store_, held_);
            news.held.push_back(kHostId);
        }
    }

    if (buffer_ == nullptr) {
        return;
    }
    for (uint8_t guest : guests) {
        if (guest == owner_ || guest == kHostId || offered_.count(guest) != 0) {
            continue;
        }
        std::vector<uint8_t> bytes;
        if (OfferFor(buffer_, title_, bytes)) {
            out.push_back({ guest, kMsgTrackOffer, std::move(bytes) });
            offered_.insert(guest);
        }
    }
}

bool HostTrackFlow::Receive(uint8_t from, uint8_t type, const uint8_t* data, size_t len, uint32_t nowMs,
                            std::vector<Outgoing>& out, RoomNews& news) {
    (void)out;
    Kind kind = Kind::Offer;
    if (!KindOf(type, kind)) {
        return false;
    }
    Route route;
    route.localIsHost = true;
    route.from = active_ && from == owner_ ? Peer::Owner : Peer::Guest;
    route.transferOpen = open_ && active_ && !failed_;
    if (!Admit(kind, route, counts_)) {
        return false;
    }
    transfer::Offer offer;
    transfer::Chunk chunk;
    track::TrackId id{};
    transfer::Abort abort;
    if (!Decoded(kind, data, len, offer, chunk, id, abort)) {
        counts_.Count(transfer::Drop::Malformed);
        return false;
    }
    if (id != id_) {
        counts_.Count(transfer::Drop::WrongTrack);
        return false;
    }
    switch (kind) {
    case Kind::Offer:
        // Only the owner, and only after the host asked (Admit checked the sender).
        if (upload_ == nullptr || !needSent_ || !upload_->OnOffer(offer, nowMs)) {
            return false;
        }
        buffer_ = upload_->Buffer();
        relay_ = std::make_unique<transfer::Relay>(buffer_);
        return true;
    case Kind::Chunk:
        return upload_ != nullptr && upload_->OnChunk(chunk, nowMs);
    case Kind::Have:
        if (relay_ != nullptr) {
            relay_->Have(from);
        }
        news.held.push_back(from);
        return true;
    case Kind::Need:
        if (relay_ == nullptr || offered_.count(from) == 0) {
            counts_.Count(transfer::Drop::NoOffer);
            return false;
        }
        relay_->Need(from);
        return true;
    case Kind::Abort:
        return upload_ != nullptr && upload_->OnAbort(abort);
    }
    return false;
}

size_t HostTrackFlow::Pump(uint8_t guest, size_t outstanding, uint32_t nowMs, const transfer::Sender::EmitFn& emit) {
    if (!active_ || !open_ || failed_ || relay_ == nullptr) {
        return 0;
    }
    return relay_->Pump(guest, nowMs, outstanding, emit);
}

TrackProgress HostTrackFlow::Progress() const {
    TrackProgress p;
    p.active = active_;
    p.held = held_ != nullptr;
    p.failed = failed_;
    if (buffer_ != nullptr) {
        p.chunks = buffer_->Held();
        p.of = buffer_->Count();
    }
    return p;
}

ClientTrackFlow::ClientTrackFlow(TrackStore store) : store_(std::move(store)) {
}

void ClientTrackFlow::Reset() {
    active_ = false;
    owner_ = false;
    title_.clear();
    download_.reset();
    upload_.reset();
    held_.reset();
}

void ClientTrackFlow::Sync(const TrackChoice& track, bool owner, uint16_t raceIndex, bool open, uint32_t nowMs) {
    (void)nowMs;
    const bool custom = track.track == kTrackCustom;
    if (active_ && (!custom || raceIndex != race_ || track.custom.id != id_)) {
        Reset();
    }
    open_ = open;
    if (!custom) {
        return;
    }
    if (active_) {
        owner_ = owner_ || owner; // the pick or the lottery that names the owner may land after the room
        return;
    }
    active_ = true;
    owner_ = owner;
    race_ = raceIndex;
    id_ = track.custom.id;
    title_ = track.custom.title;
    held_ = LoadWhole(store_, id_); // the owner's own track, or one this copy kept from an earlier room
}

bool ClientTrackFlow::Receive(uint8_t type, const uint8_t* data, size_t len, uint32_t nowMs,
                              std::vector<Outgoing>& out) {
    Kind kind = Kind::Offer;
    if (!KindOf(type, kind)) {
        return false;
    }
    Route route;
    route.localIsHost = false;
    route.localIsOwner = active_ && owner_;
    route.from = Peer::Host;
    route.transferOpen = open_ && active_;
    if (!Admit(kind, route, counts_)) {
        return false;
    }
    transfer::Offer offer;
    transfer::Chunk chunk;
    track::TrackId id{};
    transfer::Abort abort;
    if (!Decoded(kind, data, len, offer, chunk, id, abort)) {
        counts_.Count(transfer::Drop::Malformed);
        return false;
    }
    if (id != id_) {
        counts_.Count(transfer::Drop::WrongTrack);
        return false;
    }
    switch (kind) {
    case Kind::Offer:
        if (held_ != nullptr) {
            out.push_back({ kHostId, kMsgTrackHave, transfer::EncodeHave(id_) });
            return true;
        }
        if (download_ != nullptr) {
            counts_.Count(transfer::Drop::Duplicate);
            return false;
        }
        download_ = std::make_unique<Receiver>(id_);
        if (!download_->OnOffer(offer, nowMs)) {
            download_.reset();
            return false;
        }
        out.push_back({ kHostId, kMsgTrackNeed, transfer::EncodeNeed(id_) });
        return true;
    case Kind::Chunk: {
        if (download_ == nullptr || !download_->OnChunk(chunk, nowMs)) {
            return false;
        }
        if (download_->Verdict() == track::Verdict::Ok && held_ == nullptr) {
            held_ = download_->Buffer();
            Keep(store_, held_);
            out.push_back({ kHostId, kMsgTrackHave, transfer::EncodeHave(id_) });
        }
        return true;
    }
    case Kind::Need: {
        // The host asks the owner to upload. Admit let it through only on the owner's copy.
        if (upload_ != nullptr) {
            counts_.Count(transfer::Drop::Duplicate);
            return false;
        }
        std::vector<uint8_t> bytes;
        if (held_ == nullptr || !OfferFor(held_, title_, bytes)) {
            transfer::Abort failed;
            failed.id = id_;
            failed.reason = AbortReason::Failed;
            std::vector<uint8_t> abortBytes;
            if (transfer::EncodeAbort(failed, abortBytes)) {
                out.push_back({ kHostId, kMsgTrackAbort, std::move(abortBytes) });
            }
            return true;
        }
        out.push_back({ kHostId, kMsgTrackOffer, std::move(bytes) });
        upload_ = std::make_unique<transfer::Sender>(held_);
        return true;
    }
    case Kind::Abort:
        upload_.reset(); // the host stopped asking
        if (held_ != nullptr) {
            return true; // a copy that holds the track has nothing left to stop
        }
        return download_ == nullptr || download_->OnAbort(abort);
    case Kind::Have:
        return false; // Admit refuses it from the host
    }
    return false;
}

size_t ClientTrackFlow::Pump(size_t outstanding, uint32_t nowMs, const transfer::Sender::EmitFn& emit) {
    if (!active_ || !open_ || upload_ == nullptr || upload_->Finished()) {
        return 0;
    }
    return upload_->Pump(nowMs, outstanding, emit);
}

TrackProgress ClientTrackFlow::Progress() const {
    TrackProgress p;
    p.active = active_;
    p.held = held_ != nullptr;
    if (download_ != nullptr) {
        p.failed = download_->Aborted() ||
                   (download_->Verdict() != track::Verdict::Empty && download_->Verdict() != track::Verdict::Ok);
        const std::shared_ptr<const TrackBuffer> buffer = download_->Buffer();
        if (buffer != nullptr) {
            p.chunks = buffer->Held();
            p.of = buffer->Count();
        }
    } else if (held_ != nullptr) {
        p.chunks = held_->Count();
        p.of = held_->Count();
    }
    return p;
}

} // namespace gdx::net::lobby
