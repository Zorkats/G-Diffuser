// port/net/gdx_net_track_info.cpp: the creator names and the song of a room's custom track (gdx_net_track_info.h).
// Copyright (c) Zorkats

#include "gdx_net_track_info.h"

#include "gdx_net_transfer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace gdx::net::lobby {

namespace {

constexpr size_t kIdLen = track::kIdBytes;
constexpr size_t kSongLen = kIdLen + 4; // song id, u32 size

void PutU16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool SongSizeOk(uint32_t size) {
    return size != 0 && size <= transfer::kSongMaxBytes;
}

} // namespace

std::vector<uint8_t> EncodeInfoNeed(const track::TrackId& id) {
    return std::vector<uint8_t>(id.begin(), id.end());
}

bool DecodeInfoNeed(const uint8_t* data, size_t len, track::TrackId& out) {
    if (data == nullptr || len != kIdLen) {
        return false;
    }
    std::memcpy(out.data(), data, kIdLen);
    return true;
}

bool EncodeInfo(const TrackInfo& info, std::vector<uint8_t>& out) {
    if (info.meta.size() > kInfoMetaMax || (info.hasSong && !SongSizeOk(info.songSize))) {
        return false;
    }
    std::vector<uint8_t> bytes(info.track.begin(), info.track.end());
    PutU16(bytes, static_cast<uint32_t>(info.meta.size()));
    bytes.insert(bytes.end(), info.meta.begin(), info.meta.end());
    bytes.push_back(info.hasSong ? 1 : 0);
    if (info.hasSong) {
        bytes.insert(bytes.end(), info.song.begin(), info.song.end());
        PutU16(bytes, info.songSize & 0xFFFFu);
        PutU16(bytes, info.songSize >> 16);
    }
    out = std::move(bytes);
    return true;
}

bool DecodeInfo(const uint8_t* data, size_t len, TrackInfo& out) {
    // The meta length comes before the meta, so the whole length is known before anything after it is read.
    if (data == nullptr || len < kIdLen + 2) {
        return false;
    }
    const size_t metaLen = GetU16(data + kIdLen);
    const size_t flagAt = kIdLen + 2 + metaLen;
    if (metaLen > kInfoMetaMax || len < flagAt + 1) {
        return false;
    }
    const uint8_t hasSong = data[flagAt];
    if (hasSong > 1 || len != flagAt + 1 + (hasSong != 0 ? kSongLen : 0)) {
        return false;
    }
    TrackInfo info;
    std::memcpy(info.track.data(), data, kIdLen);
    info.meta.assign(data + kIdLen + 2, data + flagAt);
    info.hasSong = hasSong != 0;
    if (info.hasSong) {
        std::memcpy(info.song.data(), data + flagAt + 1, kIdLen);
        info.songSize = GetU32(data + flagAt + 1 + kIdLen);
        if (!SongSizeOk(info.songSize)) {
            return false;
        }
    }
    out = std::move(info);
    return true;
}

HostInfoFlow::HostInfoFlow(InfoSource own) : own_(std::move(own)) {
}

void HostInfoFlow::Reset() {
    active_ = false;
    asked_ = kSystemId;
    known_ = false;
    info_ = TrackInfo();
    sent_.clear();
}

void HostInfoFlow::Sync(const TrackChoice& track, uint8_t owner, uint16_t raceIndex,
                        const std::vector<uint8_t>& guests, std::vector<Outgoing>& out) {
    const bool custom = track.track == kTrackCustom;
    if (active_ && (!custom || raceIndex != race_ || track.custom.id != id_)) {
        Reset();
    }
    if (!custom) {
        return;
    }
    if (!active_) {
        active_ = true;
        race_ = raceIndex;
        id_ = track.custom.id;
    }
    if (!known_) {
        if (owner == kHostId) {
            TrackInfo mine;
            if (own_ && own_(id_, mine) == InfoReady::Ready && mine.track == id_) {
                info_ = std::move(mine);
                known_ = true;
            }
        } else if (owner != kSystemId && owner != asked_) {
            // A pick or a lottery may name the owner after the track; a new owner is asked again.
            out.push_back({ owner, kMsgTrackInfoNeed, EncodeInfoNeed(id_) });
            asked_ = owner;
        }
    }

    // A guest that left is forgotten, so a new player given its id gets the Info too.
    for (auto it = sent_.begin(); it != sent_.end();) {
        it = std::find(guests.begin(), guests.end(), *it) == guests.end() ? sent_.erase(it) : std::next(it);
    }
    if (!known_) {
        return;
    }
    std::vector<uint8_t> bytes;
    if (!EncodeInfo(info_, bytes)) {
        return;
    }
    for (uint8_t guest : guests) {
        // The owner knows its own track's Info.
        if (guest == kHostId || guest == owner || sent_.count(guest) != 0) {
            continue;
        }
        out.push_back({ guest, kMsgTrackInfo, bytes });
        sent_.insert(guest);
    }
}

bool HostInfoFlow::Receive(uint8_t from, uint8_t type, const uint8_t* data, size_t len) {
    if (type != kMsgTrackInfo || !active_ || known_ || from != asked_) {
        return false;
    }
    TrackInfo info;
    if (!DecodeInfo(data, len, info) || info.track != id_) {
        return false;
    }
    info_ = std::move(info);
    known_ = true;
    return true;
}

ClientInfoFlow::ClientInfoFlow(InfoSource own) : own_(std::move(own)) {
}

void ClientInfoFlow::Reset() {
    active_ = false;
    asked_ = false;
    known_ = false;
    info_ = TrackInfo();
}

// The host asked and this copy's own Info is ready: the answer goes out once. A track that is not this copy's
// gets no answer.
void ClientInfoFlow::Answer(std::vector<Outgoing>& out) {
    if (!asked_ || !own_) {
        return;
    }
    TrackInfo mine;
    const InfoReady ready = own_(id_, mine);
    if (ready == InfoReady::Pending) {
        return;
    }
    asked_ = false;
    std::vector<uint8_t> bytes;
    if (ready == InfoReady::Ready && mine.track == id_ && EncodeInfo(mine, bytes)) {
        out.push_back({ kHostId, kMsgTrackInfo, std::move(bytes) });
    }
}

void ClientInfoFlow::Sync(const TrackChoice& track, uint16_t raceIndex, std::vector<Outgoing>& out) {
    const bool custom = track.track == kTrackCustom;
    if (active_ && (!custom || raceIndex != race_ || track.custom.id != id_)) {
        Reset();
    }
    if (!custom) {
        return;
    }
    if (!active_) {
        active_ = true;
        race_ = raceIndex;
        id_ = track.custom.id;
    }
    if (!known_ && own_) {
        TrackInfo mine;
        if (own_(id_, mine) == InfoReady::Ready && mine.track == id_) {
            info_ = std::move(mine);
            known_ = true;
        }
    }
    Answer(out);
}

bool ClientInfoFlow::Receive(uint8_t type, const uint8_t* data, size_t len, std::vector<Outgoing>& out) {
    if (!active_) {
        return false;
    }
    if (type == kMsgTrackInfoNeed) {
        track::TrackId id{};
        if (!DecodeInfoNeed(data, len, id) || id != id_) {
            return false;
        }
        asked_ = true;
        Answer(out);
        return true;
    }
    if (type != kMsgTrackInfo) {
        return false;
    }
    TrackInfo info;
    if (!DecodeInfo(data, len, info) || info.track != id_) {
        return false;
    }
    if (!known_) { // this copy's own Info wins over what the host relays
        info_ = std::move(info);
        known_ = true;
    }
    return true;
}

} // namespace gdx::net::lobby
