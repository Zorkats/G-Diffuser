// port/net/gdx_net_transfer.cpp -- the custom track transfer codec, receiver, sender and host relay (G-NET O4.2).
// Copyright (c) Zorkats

#include "gdx_net_transfer.h"

#include "gdx_net_channel.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace gdx::net::transfer {
namespace {

// Header sizes, in bytes. The chunk header is only its id and index, so a chunk with no data is refused: a
// zero-length chunk could never fill its place in the track.
constexpr size_t kIdLen = track::kIdBytes;
constexpr size_t kOfferFixed = kIdLen + 4 + 1; // id, u32 size, u8 title length
constexpr size_t kChunkFixed = kIdLen + 2;     // id, u16 index
constexpr size_t kAbortLen = kIdLen + 1;       // id, u8 reason

// The reliable channel carries one message per datagram at most, so a full chunk has to fit its payload limit
// or the largest chunk could never be sent.
static_assert(kChunkFixed + kChunkData <= kMaxMessagePayload, "a full chunk must fit one channel payload");
// A transfer takes half the reliable window and the lobby keeps the other half, so a lobby message never waits
// for a slot behind the track (decision 6).
static_assert(kTransferShare * 2 <= kReliableWindow, "a transfer must leave half the reliable window free");

void PutU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    PutU16(out, static_cast<uint16_t>(value & 0xFFFFu));
    PutU16(out, static_cast<uint16_t>(value >> 16));
}

// The callers check the length first, so these never read past the payload.
uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void PutId(std::vector<uint8_t>& out, const track::TrackId& id) {
    out.insert(out.end(), id.begin(), id.end());
}

void ReadId(const uint8_t* data, track::TrackId& out) {
    std::memcpy(out.data(), data, kIdLen);
}

bool ValidReason(uint8_t code) {
    return code >= static_cast<uint8_t>(AbortReason::Cancelled) && code <= static_cast<uint8_t>(AbortReason::Failed);
}

// An ended receiver takes nothing more. Its verdict or its abort is final, so a late message is counted, not applied.
bool Ended(Receiver::State state) {
    return state == Receiver::State::Aborted || state == Receiver::State::Ok || state == Receiver::State::Failed;
}

std::vector<uint8_t> EncodeIdOnly(const track::TrackId& id) {
    std::vector<uint8_t> bytes;
    PutId(bytes, id);
    return bytes;
}

// Have and Need carry only the identity, so both decoders accept exactly one id and nothing after it.
bool DecodeIdOnly(const uint8_t* data, size_t len, track::TrackId& out) {
    if (data == nullptr || len != kIdLen) {
        return false;
    }
    ReadId(data, out);
    return true;
}

} // namespace

size_t ChunkCount(size_t size) {
    return (size + kChunkData - 1) / kChunkData;
}

size_t ChunkLength(size_t size, size_t index) {
    const size_t count = ChunkCount(size);
    if (index >= count) {
        return 0;
    }
    if (index + 1 == count) {
        return size - index * kChunkData;
    }
    return kChunkData;
}

bool EncodeOffer(const Offer& offer, std::vector<uint8_t>& out) {
    if (offer.size == 0 || offer.size > track::kMaxBytes || offer.title.size() > kTitleMax) {
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(kOfferFixed + offer.title.size());
    PutId(bytes, offer.id);
    PutU32(bytes, offer.size);
    bytes.push_back(static_cast<uint8_t>(offer.title.size()));
    bytes.insert(bytes.end(), offer.title.begin(), offer.title.end());
    out = std::move(bytes);
    return true;
}

bool EncodeChunk(const track::TrackId& id, size_t index, const uint8_t* data, size_t length,
                 std::vector<uint8_t>& out) {
    if (index >= kMaxChunks || data == nullptr || length == 0 || length > kChunkData) {
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(kChunkFixed + length);
    PutId(bytes, id);
    PutU16(bytes, static_cast<uint16_t>(index));
    bytes.insert(bytes.end(), data, data + length);
    out = std::move(bytes);
    return true;
}

std::vector<uint8_t> EncodeHave(const track::TrackId& id) {
    return EncodeIdOnly(id);
}

std::vector<uint8_t> EncodeNeed(const track::TrackId& id) {
    return EncodeIdOnly(id);
}

bool EncodeAbort(const Abort& value, std::vector<uint8_t>& out) {
    const uint8_t code = static_cast<uint8_t>(value.reason);
    if (!ValidReason(code)) {
        return false;
    }
    std::vector<uint8_t> bytes;
    PutId(bytes, value.id);
    bytes.push_back(code);
    out = std::move(bytes);
    return true;
}

bool DecodeOffer(const uint8_t* data, size_t len, Offer& out) {
    if (data == nullptr || len < kOfferFixed) {
        return false;
    }
    // The title length is the byte before the title, so the total length is known before any field is read.
    const size_t titleLen = data[kOfferFixed - 1];
    if (titleLen > kTitleMax || len != kOfferFixed + titleLen) {
        return false;
    }
    const uint32_t size = GetU32(data + kIdLen);
    if (size == 0 || size > track::kMaxBytes) {
        return false;
    }
    Offer offer;
    ReadId(data, offer.id);
    offer.size = size;
    offer.title.assign(reinterpret_cast<const char*>(data + kOfferFixed), titleLen);
    out = std::move(offer);
    return true;
}

bool DecodeChunk(const uint8_t* data, size_t len, Chunk& out) {
    if (data == nullptr || len <= kChunkFixed || len > kChunkFixed + kChunkData) {
        return false;
    }
    const uint16_t index = GetU16(data + kIdLen);
    if (static_cast<size_t>(index) >= kMaxChunks) {
        return false;
    }
    Chunk chunk;
    ReadId(data, chunk.id);
    chunk.index = index;
    chunk.data.assign(data + kChunkFixed, data + len);
    out = std::move(chunk);
    return true;
}

bool DecodeHave(const uint8_t* data, size_t len, track::TrackId& out) {
    return DecodeIdOnly(data, len, out);
}

bool DecodeNeed(const uint8_t* data, size_t len, track::TrackId& out) {
    return DecodeIdOnly(data, len, out);
}

bool DecodeAbort(const uint8_t* data, size_t len, Abort& out) {
    if (data == nullptr || len != kAbortLen || !ValidReason(data[kIdLen])) {
        return false;
    }
    Abort value;
    ReadId(data, value.id);
    value.reason = static_cast<AbortReason>(data[kIdLen]);
    out = value;
    return true;
}

TrackBuffer::TrackBuffer(const track::TrackId& id, size_t size)
    : id_(id), bytes_(size, static_cast<uint8_t>(0)), have_(ChunkCount(size), false) {
}

std::shared_ptr<TrackBuffer> TrackBuffer::Empty(const track::TrackId& id, size_t size) {
    if (size == 0 || size > track::kMaxBytes) {
        return nullptr;
    }
    return std::shared_ptr<TrackBuffer>(new TrackBuffer(id, size));
}

std::shared_ptr<TrackBuffer> TrackBuffer::Whole(const track::TrackId& id, const uint8_t* bytes, size_t length) {
    // The whole buffer is verified before any copy is made, so an owner's copy and a received copy meet the same bar.
    if (track::Verify(bytes, length, id, nullptr) != track::Verdict::Ok) {
        return nullptr;
    }
    std::shared_ptr<TrackBuffer> buffer = Empty(id, length);
    if (buffer == nullptr) {
        return nullptr;
    }
    std::copy(bytes, bytes + length, buffer->bytes_.data());
    std::fill(buffer->have_.begin(), buffer->have_.end(), true);
    buffer->held_ = buffer->have_.size();
    return buffer;
}

bool TrackBuffer::ChunkAt(size_t index, const uint8_t*& data, size_t& length) const {
    if (!Has(index)) {
        return false;
    }
    data = bytes_.data() + index * kChunkData;
    length = ChunkLength(bytes_.size(), index);
    return true;
}

bool TrackBuffer::Put(size_t index, const uint8_t* data, size_t length) {
    // Index and length are checked before ChunkLength is asked for the place, which would be meaningless past the end.
    if (index >= have_.size() || have_[index] || data == nullptr || length != ChunkLength(bytes_.size(), index)) {
        return false;
    }
    std::copy(data, data + length, bytes_.data() + index * kChunkData);
    have_[index] = true;
    held_++;
    return true;
}

bool Admit(Kind kind, const Route& route, Counters& counters) {
    if (!route.transferOpen) {
        counters.Count(Drop::Phase);
        return false;
    }
    const bool fromHostToGuest = !route.localIsHost && route.from == Peer::Host;
    const bool fromOwnerToHost = route.localIsHost && route.from == Peer::Owner;
    const bool fromGuestToHost = route.localIsHost && route.from == Peer::Guest;
    bool allowed = false;
    switch (kind) {
    case Kind::Offer:
        // The owner answers the host's Need with an Offer, so the host knows the size before the first chunk
        // (O4.6, gdx_net_track_flow.h).
        allowed = fromHostToGuest || fromOwnerToHost;
        break;
    case Kind::Chunk:
        allowed = fromHostToGuest || fromOwnerToHost;
        break;
    case Kind::Have:
        allowed = fromGuestToHost;
        break;
    case Kind::Need:
        // A guest asks the host, and the host asks only the owner to upload. The owner is the one guest that
        // holds the whole track, so the route must say so: Admit cannot infer it from the peer.
        allowed = fromGuestToHost || (fromHostToGuest && route.localIsOwner);
        break;
    case Kind::Abort:
        allowed = fromHostToGuest || fromOwnerToHost;
        break;
    }
    if (!allowed) {
        counters.Count(Drop::WrongSender);
    }
    return allowed;
}

Receiver::Receiver(const track::TrackId& expected) : expected_(expected) {
}

bool Receiver::OnOffer(const Offer& offer, uint32_t nowMs) {
    if (Ended(state_)) {
        counts_.Count(Drop::Late);
        return false;
    }
    if (offer.id != expected_) {
        counts_.Count(Drop::WrongTrack);
        return false;
    }
    if (state_ != State::Waiting) {
        counts_.Count(Drop::Duplicate);
        return false;
    }
    // A bad size or title is refused before any buffer exists, so an oversize Offer costs nothing.
    if (offer.title.size() > kTitleMax || offer.size == 0 || offer.size > track::kMaxBytes) {
        counts_.Count(Drop::Malformed);
        return false;
    }
    buffer_ = TrackBuffer::Empty(expected_, offer.size);
    title_ = offer.title;
    state_ = State::Receiving;
    lastProgressMs_ = nowMs;
    return true;
}

bool Receiver::OnChunk(const Chunk& chunk, uint32_t nowMs) {
    if (Ended(state_)) {
        counts_.Count(Drop::Late);
        return false;
    }
    if (buffer_ == nullptr) {
        counts_.Count(Drop::NoOffer);
        return false;
    }
    if (chunk.id != expected_) {
        counts_.Count(Drop::WrongTrack);
        return false;
    }
    const size_t index = chunk.index;
    if (index >= buffer_->Count()) {
        counts_.Count(Drop::OutOfRange);
        return false;
    }
    if (chunk.data.empty() || chunk.data.size() > kChunkData) {
        counts_.Count(Drop::Malformed);
        return false;
    }
    if (chunk.data.size() != ChunkLength(buffer_->Size(), index)) {
        counts_.Count(Drop::WrongLength);
        return false;
    }
    if (buffer_->Has(index)) {
        counts_.Count(Drop::Duplicate);
        return false;
    }
    buffer_->Put(index, chunk.data.data(), chunk.data.size());
    lastProgressMs_ = nowMs;
    if (buffer_->Complete()) {
        Finish();
    }
    return true;
}

bool Receiver::OnAbort(const Abort& value) {
    if (Ended(state_)) {
        counts_.Count(Drop::Late);
        return false;
    }
    if (value.id != expected_) {
        counts_.Count(Drop::WrongTrack);
        return false;
    }
    aborted_ = true;
    cause_ = value.reason;
    state_ = State::Aborted;
    // A relay must never stream what an aborted transfer left behind, so the bytes go with the state.
    buffer_.reset();
    return true;
}

Receiver::State Receiver::StateAt(uint32_t nowMs, uint32_t stallMs) const {
    if (state_ == State::Receiving && nowMs - lastProgressMs_ >= stallMs) {
        return State::Stalled;
    }
    return state_;
}

void Receiver::Finish() {
    gdx::snapshot::Generation decoded;
    verdict_ = track::Verify(buffer_->Bytes(), buffer_->Size(), expected_, &decoded);
    if (verdict_ == track::Verdict::Ok) {
        decoded_ = std::move(decoded);
        state_ = State::Ok;
        return;
    }
    // A failed buffer is dropped at once: the verdict says it cannot be trusted, so nothing may relay it.
    state_ = State::Failed;
    buffer_.reset();
}

Sender::Sender(std::shared_ptr<const TrackBuffer> buffer) : buffer_(std::move(buffer)) {
}

size_t Sender::Pump(uint32_t nowMs, size_t outstanding, const EmitFn& emit) {
    size_t emitted = 0;
    // The share counts what the channel already holds, so a transfer never takes more than its half of the window.
    while (next_ < buffer_->Count() && outstanding + emitted < kTransferShare) {
        const uint8_t* data = nullptr;
        size_t length = 0;
        if (!buffer_->ChunkAt(next_, data, length)) {
            // The owner has not sent this chunk yet. The stream waits here and resumes when it arrives.
            break;
        }
        std::vector<uint8_t> payload;
        if (!EncodeChunk(buffer_->Id(), next_, data, length, payload) || !emit(payload)) {
            // A refused payload means the channel is full. The chunk stays next, so the order never breaks.
            break;
        }
        next_++;
        emitted++;
        lastEmitMs_ = nowMs;
    }
    return emitted;
}

Relay::Relay(std::shared_ptr<const TrackBuffer> buffer) : buffer_(std::move(buffer)) {
}

void Relay::Need(uint8_t guest) {
    if (buffer_ == nullptr || holds_.count(guest) != 0) {
        return;
    }
    // try_emplace keeps an existing stream, so a repeated Need never restarts a finished or half-run transfer.
    senders_.try_emplace(guest, buffer_);
}

void Relay::Have(uint8_t guest) {
    holds_.insert(guest);
    senders_.erase(guest);
}

void Relay::Forget(uint8_t guest) {
    senders_.erase(guest);
    holds_.erase(guest);
}

bool Relay::Streaming(uint8_t guest) const {
    const auto it = senders_.find(guest);
    return it != senders_.end() && !it->second.Finished();
}

size_t Relay::Pump(uint8_t guest, uint32_t nowMs, size_t outstanding, const Sender::EmitFn& emit) {
    const auto it = senders_.find(guest);
    if (it == senders_.end()) {
        return 0;
    }
    return it->second.Pump(nowMs, outstanding, emit);
}

} // namespace gdx::net::transfer
