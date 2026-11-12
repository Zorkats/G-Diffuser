// port/tests/gdx_net_transfer_tests.cpp -- custom track transfer: codec, receiver, pacing, relay and lobby wait (G-NET O4.2).
// Copyright (c) Zorkats
//
// Usage: gdx_net_transfer_tests
//
// Runs the codec and the state machines on a real track (a fixture with forks, several chunks long) and on a
// synthetic 91-chunk track. The lobby wait test runs two real gdx::net::Channel objects over a fake wire with a
// one-way latency. It drops the first chunk datagram in one run, and measures how long a lobby message sent behind
// the transfer waits at the guest in each run.

#include "gdx_net_transfer.h"

#include "gdx_net_channel.h"
#include "gdx_net_track_pack.h"

#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The transfer keeps its own copy of the title cap so the module stays free of game headers. This pins the copy.
static_assert(static_cast<size_t>(GDX_COURSE_TITLE_MAX) == gdx::net::transfer::kTitleMax,
              "the transfer title cap must match the MFS course title length");

namespace {

using gdx::net::Channel;
using gdx::net::ChannelConfig;
using gdx::net::LinkState;
using gdx::net::Message;
using gdx::net::QueueResult;
using gdx::net::Role;
using gdx::snapshot::Bytes;
using gdx::snapshot::Generation;
using gdx::snapshot::Metadata;
namespace track = gdx::net::track;
namespace transfer = gdx::net::transfer;
using transfer::Abort;
using transfer::AbortReason;
using transfer::Chunk;
using transfer::Counters;
using transfer::Drop;
using transfer::Kind;
using transfer::Offer;
using transfer::Peer;
using transfer::Receiver;
using transfer::Relay;
using transfer::Route;
using transfer::Sender;
using transfer::TrackBuffer;

// The lobby and transfer type numbers are the test's choice. The channel never reads them.
constexpr uint8_t kLobbyType = 0x20;
constexpr uint8_t kOfferType = 0x30;
constexpr uint8_t kChunkType = 0x31;
// The channel's data packets: type 3 at byte 2, then a 15-byte header before the first message.
constexpr uint8_t kPacketData = 3;
constexpr size_t kDataHeaderBytes = 15;
// 91 chunks: enough for the transfer to fill its share of the window, and then drain it.
constexpr size_t kWireTrackBytes = 100000;
constexpr uint32_t kOneWayMs = 20;
constexpr uint32_t kLobbyDelayMs = 3;
constexpr uint32_t kHandshakeLimitMs = 2000;
constexpr uint32_t kTransferLimitMs = 20000;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// xorshift64 with a fixed seed: every run builds the same synthetic tracks and the same mutations.
uint64_t gRng = 0x2545F4914F6CDD1DULL;

uint32_t Next() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return static_cast<uint32_t>(gRng >> 32);
}

uint32_t Binding(const Generation& g) {
    return gdx_course_registry_crc32(reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
}

// A saved track as the snapshot tests build one: a title and per-track settings, and forks past 64 points.
Generation Fixture(int count) {
    Generation g;
    g.disk = 0x1020304050607080ULL;
    g.directory = 3;
    g.courseId = "PACK TEST.CRSD";
    g.pointCount = uint8_t(count);
    g.base.assign(gdx::snapshot::BaseBytes, 0);
    g.base[1] = uint8_t(std::min(count, 64));
    g.base[4] = 1;
    for (size_t i = 16; i < gdx::snapshot::CourseBytes; i++) {
        g.base[i] = uint8_t(i * 7);
    }
    Bytes buffer(65536);
    auto store = [&](Metadata section, int n) {
        if (n > 0) {
            g.metadata[size_t(section)].assign(buffer.begin(), buffer.begin() + n);
        }
    };
    store(Metadata::Title, gdx_course_title_encode("Pack test", Binding(g), buffer.data(), buffer.size()));
    GdxCourseSettings settings{};
    settings.ringCount = g.pointCount;
    settings.magnetRange = 200;
    store(Metadata::Settings, gdx_settings_encode(&settings, Binding(g), buffer.data(), buffer.size()));
    if (count > 64) {
        GdxBranchSet forks{};
        forks.ringCount = g.pointCount;
        forks.extendedCount = count - 64;
        forks.baseCrc = gdx_course_registry_crc32(g.base.data(), gdx::snapshot::CourseBytes);
        std::memcpy(&forks.courseChecksum, g.base.data() + 4, 4);
        for (int i = 0; i < forks.extendedCount; ++i) {
            auto& p = forks.extended[i];
            p.pos[0] = float(i * 10);
            p.pos[1] = 250.0f;
            p.pos[2] = float(i * 17);
            p.radiusLeft = 100;
            p.radiusRight = 200;
            p.pit = p.dash = p.dirt = p.ice = p.jump = p.landmine = p.gate = p.building = p.sign = -1;
        }
        store(Metadata::Forks, gdx_branch_encode(&forks, Binding(g), buffer.data(), buffer.size()));
    }
    return g;
}

// A track as the owner holds it: its content bytes, the identity they hash to, and the Offer that announces it.
struct Track {
    Bytes bytes;
    track::TrackId id{};
    Offer offer;
};

bool BuildTrack(int count, Track& out) {
    Bytes content;
    if (!gdx::snapshot::content(Fixture(count), content)) {
        return false;
    }
    out.bytes = std::move(content);
    out.id = track::Identify(out.bytes.data(), out.bytes.size());
    out.offer.id = out.id;
    out.offer.size = static_cast<uint32_t>(out.bytes.size());
    out.offer.title = "Pack test";
    return true;
}

// Random bytes identified by their own hash. Only the transfer layer sees them, and no receiver runs the whole-track
// check on them, so they never need to decode as a track.
Track Synthetic(size_t size) {
    Track t;
    t.bytes.resize(size);
    for (size_t i = 0; i < size; i++) {
        t.bytes[i] = static_cast<unsigned char>(Next());
    }
    t.id = track::Identify(t.bytes.data(), t.bytes.size());
    t.offer.id = t.id;
    t.offer.size = static_cast<uint32_t>(size);
    t.offer.title = "Synthetic";
    return t;
}

// A buffer that holds every chunk, as an owner's copy does, without running the whole-track check.
std::shared_ptr<TrackBuffer> FullBuffer(const Track& t) {
    std::shared_ptr<TrackBuffer> buffer = TrackBuffer::Empty(t.id, t.bytes.size());
    for (size_t i = 0; i < buffer->Count(); i++) {
        buffer->Put(i, t.bytes.data() + i * transfer::kChunkData, transfer::ChunkLength(t.bytes.size(), i));
    }
    return buffer;
}

Chunk ChunkOf(const Track& t, size_t index) {
    Chunk chunk;
    const size_t length = transfer::ChunkLength(t.bytes.size(), index);
    const unsigned char* start = t.bytes.data() + index * transfer::kChunkData;
    chunk.id = t.id;
    chunk.index = static_cast<uint16_t>(index);
    chunk.data.assign(start, start + length);
    return chunk;
}

std::vector<uint8_t> IdBytes(const track::TrackId& id) {
    return std::vector<uint8_t>(id.begin(), id.end());
}

// An Offer as the wire would carry it, with the size and title length the caller picks. The encoder would never
// write these fields, so this is how a test sends them.
Bytes RawOffer(const track::TrackId& id, uint32_t size, uint8_t titleLen, const std::string& title) {
    Bytes raw = IdBytes(id);
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        raw.push_back(static_cast<uint8_t>(size >> shift));
    }
    raw.push_back(titleLen);
    raw.insert(raw.end(), title.begin(), title.end());
    return raw;
}

Route MakeRoute(bool host, bool owner, Peer from, bool open) {
    Route route;
    route.localIsHost = host;
    route.localIsOwner = owner;
    route.from = from;
    route.transferOpen = open;
    return route;
}

uint32_t Total(const Counters& counts) {
    uint32_t sum = 0;
    for (size_t i = 0; i < transfer::kDropReasons; i++) {
        sum += counts.Get(static_cast<Drop>(i));
    }
    return sum;
}

// True when a datagram is a data packet that carries a reliable message of the given type. The layout is the
// channel's wire format: a 15-byte data header, then messages of type, channel, [id], length and payload.
bool CarriesReliable(const Bytes& datagram, uint8_t type) {
    if (datagram.size() < kDataHeaderBytes || datagram[2] != kPacketData) {
        return false;
    }
    size_t pos = kDataHeaderBytes;
    while (pos + 2 <= datagram.size()) {
        const bool reliable = datagram[pos + 1] == 1;
        const size_t lengthAt = pos + (reliable ? size_t{4} : size_t{2});
        if (lengthAt + 2 > datagram.size()) {
            return false;
        }
        if (reliable && datagram[pos] == type) {
            return true;
        }
        const size_t length = static_cast<size_t>(datagram[lengthAt] | (datagram[lengthAt + 1] << 8));
        pos = lengthAt + 2 + length;
    }
    return false;
}

// True when one of the first count prefixes of wire decodes. Each prefix is shorter than the whole message.
template <typename Decode>
bool AnyPrefixAccepted(const Bytes& wire, size_t count, Decode decode) {
    for (size_t len = 0; len < count; len++) {
        if (decode(wire.data(), len)) {
            return true;
        }
    }
    return false;
}

// Flips, cuts, extends or leaves alone one wire message, so the fuzz reaches both valid and broken shapes.
void Mutate(Bytes& wire) {
    switch (Next() % 4) {
    case 0:
        break;
    case 1:
        if (!wire.empty()) {
            const uint32_t flips = 1 + Next() % 3;
            for (uint32_t i = 0; i < flips; i++) {
                wire[Next() % wire.size()] = static_cast<unsigned char>(Next());
            }
        }
        break;
    case 2:
        wire.resize(Next() % (wire.size() + 1));
        break;
    default: {
        const uint32_t extra = 1 + Next() % 8;
        for (uint32_t i = 0; i < extra; i++) {
            wire.push_back(static_cast<unsigned char>(Next()));
        }
        break;
    }
    }
}

// Hands each streamed chunk to a receiver the way a guest would: decode the payload, then take the chunk.
Sender::EmitFn DeliverTo(Receiver& rx, uint32_t& nowMs) {
    return [&rx, &nowMs](const Bytes& payload) {
        Chunk chunk;
        return transfer::DecodeChunk(payload.data(), payload.size(), chunk) && rx.OnChunk(chunk, nowMs);
    };
}

void CheckConstants(const Track& real) {
    Check(transfer::kChunkData == 1100, "a chunk carries 1100 data bytes (decision 6)");
    Check(transfer::kTitleMax == 20, "a title is at most 20 bytes (GDX_COURSE_TITLE_MAX)");
    Check(transfer::kTransferShare == 32, "a transfer may hold 32 reliable slots, half the window");
    Check(transfer::kMaxChunks == 120, "the largest track is 120 chunks");
    Check(transfer::kDropReasons == 9, "one counter per drop reason");
    Check(transfer::ChunkCount(0) == 0 && transfer::ChunkCount(1) == 1 && transfer::ChunkCount(1100) == 1 &&
              transfer::ChunkCount(1101) == 2,
          "ChunkCount rounds up to whole chunks");
    Check(transfer::ChunkLength(1101, 0) == 1100 && transfer::ChunkLength(1101, 1) == 1 &&
              transfer::ChunkLength(1101, 2) == 0,
          "ChunkLength is a full chunk, then the remainder, then nothing past the end");
    Check(transfer::ChunkCount(track::kMaxBytes) == transfer::kMaxChunks, "the largest track has kMaxChunks chunks");
    Check(transfer::ChunkLength(track::kMaxBytes, transfer::kMaxChunks - 1) == 172,
          "the last chunk of the largest track holds 172 bytes");
    Check(transfer::ChunkCount(real.bytes.size()) > 2, "the fixture spans more than two chunks");
}

void CheckCodec(const Track& real) {
    // Offers at the edges of their range: no title, a title at the cap, the smallest and the largest track.
    const uint32_t sizes[] = {1u, static_cast<uint32_t>(track::kMaxBytes)};
    const std::string titles[] = {std::string(), std::string(transfer::kTitleMax, 'T')};
    for (uint32_t size : sizes) {
        for (const std::string& title : titles) {
            Offer in;
            in.id = real.id;
            in.size = size;
            in.title = title;
            std::vector<uint8_t> wire;
            Check(transfer::EncodeOffer(in, wire), "an Offer at the edge of its range encodes");
            Offer out;
            Check(transfer::DecodeOffer(wire.data(), wire.size(), out) && out.id == in.id && out.size == in.size &&
                      out.title == in.title,
                  "an Offer round-trips");
            std::vector<uint8_t> again;
            Check(transfer::EncodeOffer(out, again) && again == wire, "an Offer re-encodes to the same bytes");
        }
    }

    const size_t count = transfer::ChunkCount(real.bytes.size());
    for (size_t index : {size_t{0}, count - 1}) {
        const Chunk in = ChunkOf(real, index);
        std::vector<uint8_t> wire;
        Check(transfer::EncodeChunk(in.id, index, in.data.data(), in.data.size(), wire), "a chunk encodes");
        Chunk out;
        Check(transfer::DecodeChunk(wire.data(), wire.size(), out) && static_cast<size_t>(out.index) == index &&
                  out.data == in.data && out.id == in.id,
              "a chunk round-trips at its first and last place");
    }

    const std::vector<uint8_t> have = transfer::EncodeHave(real.id);
    track::TrackId idBack{};
    Check(have.size() == track::kIdBytes && transfer::DecodeHave(have.data(), have.size(), idBack) &&
              idBack == real.id,
          "Have round-trips");
    const std::vector<uint8_t> need = transfer::EncodeNeed(real.id);
    Check(need.size() == track::kIdBytes && transfer::DecodeNeed(need.data(), need.size(), idBack) &&
              idBack == real.id,
          "Need round-trips");

    for (AbortReason reason : {AbortReason::Cancelled, AbortReason::Stalled, AbortReason::Failed}) {
        std::vector<uint8_t> wire;
        Check(transfer::EncodeAbort(Abort{real.id, reason}, wire), "an Abort encodes");
        Abort out;
        Check(transfer::DecodeAbort(wire.data(), wire.size(), out) && out.id == real.id && out.reason == reason,
              "an Abort round-trips");
    }
}

void CheckOversized(const Track& real) {
    const std::vector<uint8_t> sentinel{0xAA};
    std::vector<uint8_t> out = sentinel;

    Offer tooLarge;
    tooLarge.id = real.id;
    tooLarge.size = static_cast<uint32_t>(track::kMaxBytes + 1);
    tooLarge.title = "Pack";
    Check(!transfer::EncodeOffer(tooLarge, out) && out == sentinel,
          "an Offer over the track cap is not encoded, and the output is left alone");

    Offer empty = tooLarge;
    empty.size = 0;
    Check(!transfer::EncodeOffer(empty, out) && out == sentinel, "an Offer for an empty track is not encoded");

    Offer longTitle = tooLarge;
    longTitle.size = 100;
    longTitle.title = std::string(transfer::kTitleMax + 1, 'T');
    Check(!transfer::EncodeOffer(longTitle, out) && out == sentinel, "an Offer with a title over the cap is not encoded");

    Offer decoded;
    decoded.size = 7;
    const Bytes rawTooLarge = RawOffer(real.id, static_cast<uint32_t>(track::kMaxBytes + 1), 0, "");
    Check(!transfer::DecodeOffer(rawTooLarge.data(), rawTooLarge.size(), decoded) && decoded.size == 7u,
          "an oversize Offer is refused on the wire, and the output is left alone");
    const Bytes rawEmpty = RawOffer(real.id, 0, 0, "");
    Check(!transfer::DecodeOffer(rawEmpty.data(), rawEmpty.size(), decoded), "an Offer for an empty track is refused on the wire");
    const Bytes rawLongTitle =
        RawOffer(real.id, 100, static_cast<uint8_t>(transfer::kTitleMax + 1), std::string(transfer::kTitleMax + 1, 'T'));
    Check(!transfer::DecodeOffer(rawLongTitle.data(), rawLongTitle.size(), decoded),
          "a title one byte over the cap is refused on the wire");
    const Bytes rawFine = RawOffer(real.id, 100, 4, "Test");
    Check(transfer::DecodeOffer(rawFine.data(), rawFine.size(), decoded) && decoded.size == 100u && decoded.title == "Test",
          "a control Offer decodes");

    Offer tooLargeForReceiver = tooLarge;
    Receiver rx(real.id);
    Check(!rx.OnOffer(tooLargeForReceiver, 0) && rx.Counts().Get(Drop::Malformed) == 1u,
          "a receiver refuses an oversize Offer as malformed");
    Check(rx.Buffer() == nullptr && rx.StateAt(0, 1000) == Receiver::State::Waiting,
          "and the refused Offer leaves the receiver holding nothing");
}

void CheckMalformed(const Track& real) {
    Offer sample;
    sample.id = real.id;
    sample.size = 100;
    sample.title = "Pack test";
    std::vector<uint8_t> offer;
    Check(transfer::EncodeOffer(sample, offer), "the sample Offer encodes");
    auto decodeOffer = [](const uint8_t* data, size_t len) {
        Offer o;
        return transfer::DecodeOffer(data, len, o);
    };
    Check(!AnyPrefixAccepted(offer, offer.size(), decodeOffer), "no cut-short Offer decodes");
    std::vector<uint8_t> offerPlus = offer;
    offerPlus.push_back(0);
    Check(!decodeOffer(offerPlus.data(), offerPlus.size()), "trailing bytes after an Offer are refused");

    const std::vector<uint8_t> have = transfer::EncodeHave(real.id);
    auto decodeHave = [](const uint8_t* data, size_t len) {
        track::TrackId id{};
        return transfer::DecodeHave(data, len, id);
    };
    Check(!AnyPrefixAccepted(have, have.size(), decodeHave), "no cut-short Have decodes");
    std::vector<uint8_t> havePlus = have;
    havePlus.push_back(0);
    Check(!decodeHave(havePlus.data(), havePlus.size()), "a Have with trailing bytes is refused");

    const std::vector<uint8_t> need = transfer::EncodeNeed(real.id);
    auto decodeNeed = [](const uint8_t* data, size_t len) {
        track::TrackId id{};
        return transfer::DecodeNeed(data, len, id);
    };
    Check(!AnyPrefixAccepted(need, need.size(), decodeNeed), "no cut-short Need decodes");
    std::vector<uint8_t> needPlus = need;
    needPlus.push_back(0);
    Check(!decodeNeed(needPlus.data(), needPlus.size()), "a Need with trailing bytes is refused");

    std::vector<uint8_t> abort;
    Check(transfer::EncodeAbort(Abort{real.id, AbortReason::Stalled}, abort), "the sample Abort encodes");
    auto decodeAbort = [](const uint8_t* data, size_t len) {
        Abort a;
        return transfer::DecodeAbort(data, len, a);
    };
    Check(!AnyPrefixAccepted(abort, abort.size(), decodeAbort), "no cut-short Abort decodes");
    std::vector<uint8_t> abortPlus = abort;
    abortPlus.push_back(0);
    Check(!decodeAbort(abortPlus.data(), abortPlus.size()), "an Abort with trailing bytes is refused");
    for (uint8_t code : {uint8_t{0}, uint8_t{4}, uint8_t{255}}) {
        std::vector<uint8_t> raw = IdBytes(real.id);
        raw.push_back(code);
        Check(!decodeAbort(raw.data(), raw.size()), "an unknown abort reason is refused");
    }

    const Chunk sample_ = ChunkOf(real, 1);
    std::vector<uint8_t> chunk;
    Check(transfer::EncodeChunk(sample_.id, 1, sample_.data.data(), sample_.data.size(), chunk), "the sample chunk encodes");
    auto decodeChunk = [](const uint8_t* data, size_t len) {
        Chunk c;
        return transfer::DecodeChunk(data, len, c);
    };
    // A chunk is its id and index (34 bytes) and at least one data byte, so every shorter prefix must be refused.
    Check(!AnyPrefixAccepted(chunk, 35, decodeChunk), "no chunk prefix under 35 bytes decodes");

    std::vector<uint8_t> oversize = IdBytes(real.id);
    oversize.push_back(0);
    oversize.push_back(0);
    oversize.resize(oversize.size() + transfer::kChunkData + 1, 0x11);
    Chunk scratch;
    Check(!transfer::DecodeChunk(oversize.data(), oversize.size(), scratch), "a chunk over the data limit is refused");

    Offer nullOffer;
    Chunk nullChunk;
    track::TrackId nullId{};
    Abort nullAbort;
    Check(!transfer::DecodeOffer(nullptr, 0, nullOffer) && !transfer::DecodeChunk(nullptr, 0, nullChunk) &&
              !transfer::DecodeHave(nullptr, 0, nullId) && !transfer::DecodeNeed(nullptr, 0, nullId) &&
              !transfer::DecodeAbort(nullptr, 0, nullAbort),
          "no decoder takes a null buffer");

    Chunk keep;
    keep.index = 7;
    keep.data = {1, 2, 3};
    const std::vector<uint8_t> headerOnly(chunk.begin(), chunk.begin() + 34);
    Check(!transfer::DecodeChunk(headerOnly.data(), headerOnly.size(), keep) && keep.index == 7 && keep.data.size() == 3u,
          "a refused chunk leaves its output alone");
}

void CheckFuzz(const Track& real) {
    const Chunk base = ChunkOf(real, 1);
    Bytes offerWire;
    Bytes chunkWire;
    Bytes abortWire;
    Check(transfer::EncodeOffer(real.offer, offerWire) &&
              transfer::EncodeChunk(base.id, base.index, base.data.data(), base.data.size(), chunkWire) &&
              transfer::EncodeAbort(Abort{real.id, AbortReason::Stalled}, abortWire),
          "the fuzz seeds encode");
    const Bytes haveWire = transfer::EncodeHave(real.id);
    const Bytes needWire = transfer::EncodeNeed(real.id);
    const Bytes* seeds[5] = {&offerWire, &chunkWire, &haveWire, &needWire, &abortWire};
    const char* names[5] = {"Offer", "Chunk", "Have", "Need", "Abort"};

    // Whatever a decoder accepts from mutated bytes must re-encode to exactly those bytes. Anything else would mean two
    // wire forms of one message, which a peer could use to slip past a duplicate check.
    uint32_t accepted[5] = {};
    size_t mismatches = 0;
    for (uint32_t round = 0; round < 30000; round++) {
        const uint32_t kind = round % 5;
        Bytes wire = *seeds[kind];
        Mutate(wire);
        Bytes again;
        bool ok = false;
        switch (kind) {
        case 0: {
            Offer o;
            ok = transfer::DecodeOffer(wire.data(), wire.size(), o) && transfer::EncodeOffer(o, again);
            break;
        }
        case 1: {
            Chunk c;
            ok = transfer::DecodeChunk(wire.data(), wire.size(), c) &&
                 transfer::EncodeChunk(c.id, c.index, c.data.data(), c.data.size(), again);
            break;
        }
        case 2: {
            track::TrackId id{};
            ok = transfer::DecodeHave(wire.data(), wire.size(), id);
            if (ok) {
                again = transfer::EncodeHave(id);
            }
            break;
        }
        case 3: {
            track::TrackId id{};
            ok = transfer::DecodeNeed(wire.data(), wire.size(), id);
            if (ok) {
                again = transfer::EncodeNeed(id);
            }
            break;
        }
        default: {
            Abort a;
            ok = transfer::DecodeAbort(wire.data(), wire.size(), a) && transfer::EncodeAbort(a, again);
            break;
        }
        }
        if (ok) {
            accepted[kind]++;
            if (again != wire) {
                mismatches++;
            }
        }
    }
    Check(mismatches == 0, "every message the fuzz accepts re-encodes to the same bytes");
    for (uint32_t kind = 0; kind < 5; kind++) {
        Check(accepted[kind] > 0, std::string("the fuzz accepts at least one ") + names[kind] + " message");
    }
}

void CheckAdmit() {
    const Route guestFromHost = MakeRoute(false, false, Peer::Host, true);
    const Route ownerFromHost = MakeRoute(false, true, Peer::Host, true);
    const Route hostFromGuest = MakeRoute(true, false, Peer::Guest, true);
    const Route hostFromOwner = MakeRoute(true, false, Peer::Owner, true);
    struct Row {
        Kind kind;
        Route route;
        bool allowed;
    };
    const Row rows[] = {
        {Kind::Offer, guestFromHost, true},  {Kind::Offer, hostFromGuest, false}, {Kind::Offer, hostFromOwner, true},
        {Kind::Chunk, guestFromHost, true},  {Kind::Chunk, hostFromOwner, true},  {Kind::Chunk, hostFromGuest, false},
        {Kind::Have, hostFromGuest, true},   {Kind::Have, guestFromHost, false},  {Kind::Have, hostFromOwner, false},
        {Kind::Need, hostFromGuest, true},   {Kind::Need, ownerFromHost, true},   {Kind::Need, guestFromHost, false},
        {Kind::Need, hostFromOwner, false},  {Kind::Abort, guestFromHost, true},  {Kind::Abort, hostFromOwner, true},
        {Kind::Abort, hostFromGuest, false},
    };
    size_t number = 0;
    for (const Row& row : rows) {
        number++;
        Counters counts;
        const bool got = transfer::Admit(row.kind, row.route, counts);
        const std::string label = "admit row " + std::to_string(number);
        Check(got == row.allowed, label + " is admitted or refused as decision 15 gives it");
        Check(counts.Get(Drop::WrongSender) == (row.allowed ? 0u : 1u), label + " counts a refusal as WrongSender");
        Check(counts.Get(Drop::Phase) == 0u, label + " does not count a phase refusal");
    }

    for (Kind kind : {Kind::Offer, Kind::Chunk, Kind::Have, Kind::Need, Kind::Abort}) {
        Counters counts;
        Check(!transfer::Admit(kind, MakeRoute(true, false, Peer::Guest, false), counts) &&
                  counts.Get(Drop::Phase) == 1u && counts.Get(Drop::WrongSender) == 0u,
              "nothing moves outside MACHINE and ASSETS, and the refusal is counted as Phase");
    }

    Counters counts;
    counts.Count(Drop::Malformed);
    counts.Count(Drop::Malformed);
    counts.Count(Drop::Late);
    Check(counts.Get(Drop::Malformed) == 2u && counts.Get(Drop::Late) == 1u && counts.Get(Drop::Duplicate) == 0u,
          "each drop reason keeps its own count");
}

void CheckReceiverHappyPath(const Track& real) {
    Receiver rx(real.id);
    Check(rx.OnOffer(real.offer, 100), "the Offer for the expected track is taken");
    Check(rx.Title() == real.offer.title, "the title is kept from the Offer");

    const size_t count = transfer::ChunkCount(real.bytes.size());
    uint32_t now = 100;
    bool allTaken = true;
    // The last chunk comes first, the order a relay could deliver in after a reconnect.
    for (size_t index = count; index-- > 0;) {
        now++;
        if (!rx.OnChunk(ChunkOf(real, index), now)) {
            allTaken = false;
        }
    }
    Check(allTaken, "every chunk is taken, in reverse order");
    Check(rx.Verdict() == track::Verdict::Ok, "the whole track verifies once every chunk is in");
    Check(Total(rx.Counts()) == 0u, "a clean transfer counts no drops");

    Bytes encoded;
    Check(gdx::snapshot::content(rx.Decoded(), encoded) && encoded == real.bytes,
          "the decoded track encodes back to the received bytes");
    Check(rx.Buffer() != nullptr && std::equal(real.bytes.begin(), real.bytes.end(), rx.Buffer()->Bytes()),
          "a verified transfer keeps its bytes for the relay");
    Check(rx.StateAt(now + 10000, 1000) == Receiver::State::Ok, "a finished transfer does not read as stalled");
    Check(!rx.OnChunk(ChunkOf(real, 0), now + 1) && rx.Counts().Get(Drop::Late) == 1u,
          "a chunk after the verdict is counted as late");
}

void CheckReceiverRefusals(const Track& real) {
    const size_t count = transfer::ChunkCount(real.bytes.size());
    Receiver rx(real.id);
    Check(!rx.OnChunk(ChunkOf(real, 0), 5) && rx.Counts().Get(Drop::NoOffer) == 1u,
          "a chunk before its Offer is refused as NoOffer");

    Offer other = real.offer;
    other.id[0] ^= 0xFF;
    Check(!rx.OnOffer(other, 5) && rx.Counts().Get(Drop::WrongTrack) == 1u, "an Offer for another track is refused");
    Check(!rx.OnAbort(Abort{other.id, AbortReason::Cancelled}) && rx.Counts().Get(Drop::WrongTrack) == 2u,
          "an Abort for another track is refused");
    Check(rx.OnOffer(real.offer, 100), "the Offer for the expected track is taken");
    Check(!rx.OnOffer(real.offer, 110) && rx.Counts().Get(Drop::Duplicate) == 1u, "a second Offer is a duplicate");

    Chunk stray = ChunkOf(real, 0);
    stray.id = other.id;
    Check(!rx.OnChunk(stray, 120) && rx.Counts().Get(Drop::WrongTrack) == 3u, "a chunk for another track is refused");
    Check(rx.OnChunk(ChunkOf(real, 0), 200), "the first chunk is taken");
    Check(!rx.OnChunk(ChunkOf(real, 0), 300) && rx.Counts().Get(Drop::Duplicate) == 2u,
          "a repeated chunk is a duplicate");

    Chunk past = ChunkOf(real, 0);
    past.index = static_cast<uint16_t>(count);
    Check(!rx.OnChunk(past, 400) && rx.Counts().Get(Drop::OutOfRange) == 1u,
          "a chunk index past the last chunk is out of range");
    past.index = static_cast<uint16_t>(0xFFFF);
    Check(!rx.OnChunk(past, 400) && rx.Counts().Get(Drop::OutOfRange) == 2u,
          "the largest possible chunk index is out of range");

    Chunk shortChunk = ChunkOf(real, 1);
    shortChunk.data.resize(10);
    Check(!rx.OnChunk(shortChunk, 400) && rx.Counts().Get(Drop::WrongLength) == 1u,
          "a chunk shorter than its place in the track is refused");

    Chunk empty = ChunkOf(real, 1);
    empty.data.clear();
    Check(!rx.OnChunk(empty, 400) && rx.Counts().Get(Drop::Malformed) == 1u, "a chunk with no data is malformed");
    Chunk oversize = ChunkOf(real, 1);
    oversize.data.resize(transfer::kChunkData + 1, 0);
    Check(!rx.OnChunk(oversize, 400) && rx.Counts().Get(Drop::Malformed) == 2u, "a chunk over the data limit is malformed");

    Check(rx.LastProgressMs() == 200u, "refused chunks do not count as progress");
    Check(rx.StateAt(200, 1000) == Receiver::State::Receiving, "the transfer is still receiving");
}

void CheckWrongIdentity(const Track& real) {
    Track tampered = real;
    tampered.bytes[tampered.bytes.size() / 2] ^= 0x01;
    const size_t count = transfer::ChunkCount(tampered.bytes.size());
    Receiver rx(real.id);
    Check(rx.OnOffer(real.offer, 0), "the Offer is taken");
    for (size_t index = 0; index < count; index++) {
        rx.OnChunk(ChunkOf(tampered, index), static_cast<uint32_t>(index + 1));
    }
    Check(rx.Verdict() == track::Verdict::WrongIdentity, "a tampered track fails the identity check");
    Check(rx.Buffer() == nullptr, "a failed transfer has no buffer to relay");
    Check(rx.Decoded().courseId.empty(), "a failed transfer decodes nothing");
    Check(rx.StateAt(5000, 1000) == Receiver::State::Failed, "a failed transfer stays failed");
    Check(TrackBuffer::Whole(real.id, tampered.bytes.data(), tampered.bytes.size()) == nullptr,
          "the whole-track constructor refuses the tampered bytes");
}

void CheckOwnerStopsMidway(const Track& real) {
    Check(transfer::ChunkCount(real.bytes.size()) > 3, "the fixture is long enough for the owner to stop midway");
    Receiver rx(real.id);
    Check(rx.OnOffer(real.offer, 1000), "the Offer is taken");
    Check(rx.OnChunk(ChunkOf(real, 0), 1000) && rx.OnChunk(ChunkOf(real, 1), 1000) && rx.OnChunk(ChunkOf(real, 2), 1000),
          "three chunks arrive at 1000 ms");
    Check(rx.StateAt(1100, 500) == Receiver::State::Receiving, "inside the stall window the transfer is receiving");
    Check(rx.StateAt(1520, 500) == Receiver::State::Stalled,
          "once the stall window passes with no chunk, the transfer reads as stalled");
    Check(rx.OnAbort(Abort{real.id, AbortReason::Stalled}), "the owner's abort is taken");
    Check(rx.Aborted() && rx.Cause() == AbortReason::Stalled, "the transfer records the abort and its cause");
    Check(rx.Buffer() == nullptr, "an aborted transfer has no buffer to relay");
    Check(rx.StateAt(1600, 500) == Receiver::State::Aborted, "an aborted transfer stays aborted");
    Check(!rx.OnChunk(ChunkOf(real, 3), 1600) && rx.Counts().Get(Drop::Late) == 1u, "a chunk after the abort is late");
}

void CheckPacing() {
    const Track fortyChunks = Synthetic(40 * transfer::kChunkData);
    const std::shared_ptr<TrackBuffer> full = FullBuffer(fortyChunks);
    size_t sent = 0;
    const Sender::EmitFn accept = [&sent](const Bytes&) {
        sent++;
        return true;
    };

    Sender sender(full);
    Check(sender.Pump(0, 0, accept) == 32u, "an empty window takes a full transfer share");
    Check(sender.Pump(0, 30, accept) == 2u, "30 slots in use leave room for two more chunks");
    Check(sender.Pump(0, 32, accept) == 0u, "a full share sends nothing");
    Check(sender.Next() == 34u && sent == 34u, "the sender resumes in order at chunk 34");

    Sender refused(full);
    Check(refused.Pump(0, 0, [](const Bytes&) { return false; }) == 0u && refused.Next() == 0u,
          "a refused chunk stays next");
    Check(refused.Pump(0, 0, accept) == 32u, "the refused chunk goes out once the channel accepts it");

    // A buffer still filling: the stream holds at the first chunk the buffer lacks.
    const std::shared_ptr<TrackBuffer> partial = TrackBuffer::Empty(fortyChunks.id, fortyChunks.bytes.size());
    for (size_t i = 0; i < 5; i++) {
        partial->Put(i, fortyChunks.bytes.data() + i * transfer::kChunkData,
                     transfer::ChunkLength(fortyChunks.bytes.size(), i));
    }
    Sender stream(partial);
    size_t streamed = 0;
    const Sender::EmitFn count = [&streamed](const Bytes&) {
        streamed++;
        return true;
    };
    Check(stream.Pump(0, 0, count) == 5u && stream.Next() == 5u, "a stream stops at the first chunk the buffer lacks");
    Check(stream.Pump(0, 0, count) == 0u, "nothing moves while the gap is open");
    partial->Put(5, fortyChunks.bytes.data() + 5 * transfer::kChunkData, transfer::ChunkLength(fortyChunks.bytes.size(), 5));
    Check(stream.Pump(0, 0, count) == 1u && stream.Next() == 6u, "the stream resumes when the missing chunk arrives");
}

void CheckRelay(const Track& real) {
    const std::shared_ptr<TrackBuffer> whole = TrackBuffer::Whole(real.id, real.bytes.data(), real.bytes.size());
    Check(whole != nullptr && whole->Complete(), "the owner's whole track passes the check");
    if (whole == nullptr) {
        return;
    }
    Relay relay(whole);
    Receiver first(real.id);
    Receiver second(real.id);
    Check(first.OnOffer(real.offer, 0) && second.OnOffer(real.offer, 0), "both guests take the Offer");

    relay.Need(1);
    relay.Need(2);
    relay.Have(3);
    relay.Need(3);
    Check(!relay.Streaming(3), "a guest that said Have is not streamed to");

    uint32_t now = 10;
    bool moved = true;
    while (moved) {
        moved = false;
        for (uint8_t guest : {uint8_t{1}, uint8_t{2}}) {
            Receiver& rx = guest == 1 ? first : second;
            if (relay.Pump(guest, now, 0, DeliverTo(rx, now)) > 0) {
                moved = true;
            }
        }
        now++;
    }
    Check(first.Verdict() == track::Verdict::Ok && second.Verdict() == track::Verdict::Ok,
          "both guests verify the relayed track");
    Check(first.Buffer() != nullptr && std::equal(real.bytes.begin(), real.bytes.end(), first.Buffer()->Bytes()),
          "the relayed bytes match the owner's");
    Check(!relay.Streaming(1) && !relay.Streaming(2), "a finished stream is no longer streaming");

    const Sender::EmitFn silent = [](const Bytes&) { return true; };
    Check(relay.Pump(1, now, 0, silent) == 0u, "a finished stream emits nothing");
    Check(relay.Pump(9, now, 0, silent) == 0u, "a guest the relay does not know gets nothing");
    Check(relay.Pump(3, now, 0, silent) == 0u, "a guest that said Have gets nothing");

    relay.Forget(2);
    Check(!relay.Streaming(2) && relay.Pump(2, now, 0, silent) == 0u, "a forgotten guest is no longer streamed");
    relay.Need(2);
    Check(relay.Streaming(2), "a guest that asks again is streamed from the start");
}

// The outcome of one run over the fake wire.
struct WireResult {
    bool connected = false;
    bool intact = true;        // the Offer and every chunk decoded and matched the host's bytes
    bool completed = false;    // every chunk arrived at the guest
    bool lobbyDelivered = false;
    uint32_t lobbyWaitMs = 0;  // from the lobby send to the guest's pop of it
    size_t dropped = 0;        // datagrams the fake wire removed
    uint32_t resends = 0;      // reliable messages the host sent again after their timeout
    size_t maxOutstanding = 0; // the most reliable slots the host held at once
};

// A datagram in flight. The receiving channel takes it once the clock reaches dueMs.
struct Datagram {
    uint32_t dueMs = 0;
    Bytes bytes;
};

// Runs one transfer of the buffer between a host channel and a guest channel over a fake wire with a one-way
// latency. The transfer starts once both sides are connected. With drop set, the first datagram that carries a chunk
// is removed. The lobby message goes out kLobbyDelayMs after the transfer starts, and only once the drop has happened,
// so it cannot share a datagram with the lost chunk. Its wait is measured at the guest.
WireResult RunWire(const std::shared_ptr<const TrackBuffer>& buffer, bool drop) {
    WireResult result;
    std::deque<Datagram> toGuest;
    std::deque<Datagram> toHost;
    bool dropPending = drop;
    uint32_t now = 0;

    ChannelConfig hostConfig;
    hostConfig.role = Role::Server;
    ChannelConfig guestConfig;
    guestConfig.role = Role::Client;
    guestConfig.connectionNonce = 0x51A7u;

    // The send callbacks only queue. A callback must not call back into its channel, so the wire is a plain queue.
    Channel hostChannel(hostConfig, [&](const uint8_t* data, size_t len) {
        Bytes bytes(data, data + len);
        if (dropPending && CarriesReliable(bytes, kChunkType)) {
            dropPending = false;
            result.dropped++;
            return;
        }
        toGuest.push_back(Datagram{now + kOneWayMs, std::move(bytes)});
    });
    Channel guestChannel(guestConfig, [&](const uint8_t* data, size_t len) {
        toHost.push_back(Datagram{now + kOneWayMs, Bytes(data, data + len)});
    });

    auto deliver = [&now](std::deque<Datagram>& queue, Channel& to) {
        while (!queue.empty() && queue.front().dueMs <= now) {
            Datagram datagram = std::move(queue.front());
            queue.pop_front();
            to.Receive(datagram.bytes.data(), datagram.bytes.size(), now);
        }
    };

    Check(guestChannel.Connect(now), "the guest starts the handshake");
    while (now < kHandshakeLimitMs &&
           (hostChannel.State() != LinkState::Connected || guestChannel.State() != LinkState::Connected)) {
        deliver(toHost, hostChannel);
        deliver(toGuest, guestChannel);
        hostChannel.Update(now);
        guestChannel.Update(now);
        now++;
    }
    result.connected = hostChannel.State() == LinkState::Connected && guestChannel.State() == LinkState::Connected;
    if (!result.connected) {
        return result;
    }

    const uint32_t startMs = now;
    const uint32_t deadline = startMs + kTransferLimitMs;

    Offer offer;
    offer.id = buffer->Id();
    offer.size = static_cast<uint32_t>(buffer->Size());
    offer.title = "Synthetic";
    std::vector<uint8_t> offerBytes;
    Check(transfer::EncodeOffer(offer, offerBytes), "the synthetic Offer encodes");

    transfer::Sender sender(buffer);
    const Route route = MakeRoute(false, false, Peer::Host, true);
    Counters admitted;
    std::vector<bool> got(buffer->Count(), false);
    size_t received = 0;
    bool offered = false;
    bool lobbySent = false;
    uint32_t lobbySentMs = 0;

    while (now < deadline) {
        deliver(toHost, hostChannel);
        deliver(toGuest, guestChannel);

        if (!offered) {
            hostChannel.SendReliable(kOfferType, offerBytes.data(), offerBytes.size());
            offered = true;
        }
        // The share in Pump keeps the transfer to half the window, so the lobby slot below is always free.
        sender.Pump(now, hostChannel.ReliableOutstanding(), [&hostChannel](const Bytes& payload) {
            return hostChannel.SendReliable(kChunkType, payload.data(), payload.size()) == QueueResult::Ok;
        });
        if (!lobbySent && now >= startMs + kLobbyDelayMs && (!drop || result.dropped == 1)) {
            const Bytes lobby(16, static_cast<unsigned char>(0x5A));
            hostChannel.SendReliable(kLobbyType, lobby.data(), lobby.size());
            lobbySent = true;
            lobbySentMs = now;
        }
        hostChannel.Update(now);
        guestChannel.Update(now);
        result.maxOutstanding = std::max(result.maxOutstanding, hostChannel.ReliableOutstanding());

        Message msg;
        while (guestChannel.PopMessage(msg)) {
            if (msg.type == kLobbyType) {
                result.lobbyDelivered = true;
                result.lobbyWaitMs = now - lobbySentMs;
            } else if (msg.type == kOfferType) {
                Offer heard;
                if (!transfer::Admit(Kind::Offer, route, admitted) ||
                    !transfer::DecodeOffer(msg.payload.data(), msg.payload.size(), heard) || heard.id != offer.id ||
                    heard.size != offer.size) {
                    result.intact = false;
                }
            } else if (msg.type == kChunkType) {
                Chunk heard;
                const bool decoded = transfer::DecodeChunk(msg.payload.data(), msg.payload.size(), heard);
                const uint8_t* expected = nullptr;
                size_t length = 0;
                if (!transfer::Admit(Kind::Chunk, route, admitted) || !decoded ||
                    !buffer->ChunkAt(heard.index, expected, length) || heard.data.size() != length ||
                    !std::equal(heard.data.begin(), heard.data.end(), expected)) {
                    result.intact = false;
                } else if (!got[heard.index]) {
                    got[heard.index] = true;
                    received++;
                }
            }
        }
        if (result.lobbyDelivered && received == buffer->Count()) {
            break;
        }
        now++;
    }
    result.completed = received == buffer->Count();
    result.resends = hostChannel.Stats().resends;
    result.intact = result.intact && Total(admitted) == 0u;
    return result;
}

void CheckLobbyWait() {
    const Track wire = Synthetic(kWireTrackBytes);
    const std::shared_ptr<const TrackBuffer> buffer = FullBuffer(wire);
    const WireResult calm = RunWire(buffer, false);
    const WireResult lossy = RunWire(buffer, true);

    Check(calm.connected && lossy.connected, "both runs complete the handshake");
    Check(calm.dropped == 0u && calm.completed && calm.intact, "a run without loss delivers every chunk intact");
    Check(calm.lobbyDelivered, "a run without loss delivers the lobby message");
    Check(lossy.dropped == 1u, "exactly one chunk datagram is dropped");
    Check(lossy.completed && lossy.intact, "the lossy run still delivers every chunk intact");
    Check(lossy.lobbyDelivered, "the lossy run delivers the lobby message");
    Check(lossy.resends >= 1u, "the lost chunk is sent again");
    Check(lossy.lobbyWaitMs > calm.lobbyWaitMs, "the lobby waits behind the lost chunk");
    const size_t peak = std::max(calm.maxOutstanding, lossy.maxOutstanding);
    Check(peak <= transfer::kTransferShare + 1, "the transfer keeps its share, plus one slot for the lobby");
    Check(peak <= gdx::net::kReliableWindow, "no run exceeds the reliable window");
    std::printf("lobby wait behind a lost chunk: %u ms (no loss: %u ms); max outstanding %zu of %zu window slots\n",
                lossy.lobbyWaitMs, calm.lobbyWaitMs, peak, gdx::net::kReliableWindow);
}

} // namespace

int main() {
    Track real;
    if (!BuildTrack(100, real)) {
        std::printf("FAIL: the fixture does not encode as content bytes\n");
        return 1;
    }
    CheckConstants(real);
    CheckCodec(real);
    CheckOversized(real);
    CheckMalformed(real);
    CheckFuzz(real);
    CheckAdmit();
    CheckReceiverHappyPath(real);
    CheckReceiverRefusals(real);
    CheckWrongIdentity(real);
    CheckOwnerStopsMidway(real);
    CheckPacing();
    CheckRelay(real);
    CheckLobbyWait();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
