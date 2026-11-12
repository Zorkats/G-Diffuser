// port/tests/gdx_net_track_flow_tests.cpp: a room's custom track flow, host and clients back to back (G-NET O4.6).
// Copyright (c) Zorkats
//
// Usage: gdx_net_track_flow_tests
//
// One HostTrackFlow and up to three ClientTrackFlow objects pass their messages through in-memory queues, with
// every time a nowMs argument. The track is a real saved course with forks, several chunks long, so every
// receiver runs the whole-track check. Each case starts from fresh flows and fresh stores.

#include "gdx_net_track_flow.h"

#include "gdx_net_track_pack.h"

#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace lobby = gdx::net::lobby;
namespace track = gdx::net::track;
namespace transfer = gdx::net::transfer;
using gdx::snapshot::Bytes;
using gdx::snapshot::Generation;
using gdx::snapshot::Metadata;
using lobby::ClientTrackFlow;
using lobby::HostTrackFlow;
using lobby::Outgoing;
using lobby::RoomNews;
using lobby::TrackChoice;
using lobby::TrackStore;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

uint32_t Binding(const Generation& g) {
    return gdx_course_registry_crc32(reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
}

// The transfer tests' fixture: a title, per-track settings and forks past 64 points.
Generation Fixture(int count) {
    Generation g;
    g.disk = 0x1020304050607080ULL;
    g.directory = 3;
    g.courseId = "FLOW TEST.CRSD";
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
    store(Metadata::Title, gdx_course_title_encode("Flow test", Binding(g), buffer.data(), buffer.size()));
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

struct Track {
    std::vector<uint8_t> bytes;
    track::TrackId id{};
};

Track gTrack;
Track gOther;

bool BuildTrack(int count, Track& out) {
    Bytes content;
    if (!gdx::snapshot::content(Fixture(count), content)) {
        return false;
    }
    out.bytes.assign(content.begin(), content.end());
    out.id = track::Identify(out.bytes.data(), out.bytes.size());
    return true;
}

TrackChoice Custom(const Track& t) {
    TrackChoice c;
    c.track = lobby::kTrackCustom;
    std::copy(t.id.begin(), t.id.end(), c.custom.id.begin());
    c.custom.title = "Flow test";
    return c;
}

TrackChoice Stock(uint8_t index) {
    TrackChoice c;
    c.track = index;
    return c;
}

// One copy's disk: the cache and its own tracks in one map.
struct Disk {
    std::map<track::TrackId, std::vector<uint8_t>> tracks;
    int stores = 0;

    TrackStore Store() {
        TrackStore s;
        s.load = [this](const track::TrackId& id, std::vector<uint8_t>& bytes) {
            auto it = tracks.find(id);
            if (it == tracks.end()) {
                return false;
            }
            bytes = it->second;
            return true;
        };
        s.store = [this](const track::TrackId& id, const std::vector<uint8_t>& bytes) {
            tracks[id] = bytes;
            stores++;
        };
        return s;
    }
};

struct Wire {
    uint8_t from = 0;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

uint8_t AbortReasonOf(const Wire& w) {
    transfer::Abort a;
    return transfer::DecodeAbort(w.payload.data(), w.payload.size(), a) ? uint8_t(a.reason) : 0xFF;
}

// A host with guests 1 to 3, each optional. The room's track and owner are set by the case.
class Room {
public:
    explicit Room(std::vector<uint8_t> guests) : guests_(std::move(guests)), host_(disks_[lobby::kHostId].Store()) {
        for (uint8_t g : guests_) {
            clients_[g] = std::make_unique<ClientTrackFlow>(disks_[g].Store());
        }
    }

    Disk& DiskOf(uint8_t id) {
        return disks_[id];
    }

    HostTrackFlow& Host() {
        return host_;
    }

    ClientTrackFlow& Client(uint8_t id) {
        return *clients_.at(id);
    }

    // Recreates the flows so a disk filled after construction is what they load from.
    void Reset() {
        host_ = HostTrackFlow(disks_[lobby::kHostId].Store());
        for (uint8_t g : guests_) {
            clients_[g] = std::make_unique<ClientTrackFlow>(disks_[g].Store());
        }
    }

    void Leave(uint8_t id) {
        guests_.erase(std::remove(guests_.begin(), guests_.end(), id), guests_.end());
        clients_.erase(id);
        toClient.erase(id);
    }

    // A new player given the id, with an empty disk.
    void Join(uint8_t id) {
        disks_[id] = Disk();
        guests_.push_back(id);
        clients_[id] = std::make_unique<ClientTrackFlow>(disks_[id].Store());
    }

    // One tick of every copy. Returns true when any message moved.
    bool Step(uint32_t nowMs) {
        bool moved = false;
        std::vector<Outgoing> out;
        RoomNews news;
        host_.Sync(track, owner, race, open, guests_, nowMs, out, news);
        moved |= Route(out);
        Take(news);
        for (auto& [id, client] : clients_) {
            client->Sync(track, owner == id, race, open, nowMs);
        }
        for (auto& [id, client] : clients_) {
            std::deque<Wire>& q = toClient[id];
            while (!q.empty()) {
                Wire w = std::move(q.front());
                q.pop_front();
                std::vector<Outgoing> replies;
                if (!client->Receive(w.type, w.payload.data(), w.payload.size(), nowMs, replies)) {
                    clientRefused[id]++;
                }
                for (Outgoing& r : replies) {
                    toHost.push_back({ id, r.type, std::move(r.payload) });
                }
                moved = true;
            }
        }
        while (!toHost.empty()) {
            Wire w = std::move(toHost.front());
            toHost.pop_front();
            if (w.type == lobby::kMsgTrackChunk && tamperUpload) {
                w.payload.back() ^= 0x5A;
                tamperUpload = false;
            }
            std::vector<Outgoing> replies;
            RoomNews n;
            if (!host_.Receive(w.from, w.type, w.payload.data(), w.payload.size(), nowMs, replies, n)) {
                hostRefused++;
            }
            moved |= Route(replies);
            Take(n);
            moved = true;
        }
        for (uint8_t g : guests_) {
            moved |= host_.Pump(g, 0, nowMs, [this, g](const std::vector<uint8_t>& p) {
                toClient[g].push_back({ lobby::kHostId, lobby::kMsgTrackChunk, p });
                sentChunks[g]++;
                return true;
            }) > 0;
        }
        for (auto& [id, client] : clients_) {
            const uint8_t from = id;
            moved |= client->Pump(0, nowMs, [this, from](const std::vector<uint8_t>& p) {
                toHost.push_back({ from, lobby::kMsgTrackChunk, p });
                return true;
            }) > 0;
        }
        return moved;
    }

    // Steps until nothing moves for a few ticks or the limit passes. Returns the time it stopped.
    uint32_t Run(uint32_t startMs, uint32_t limitMs = 30000) {
        uint32_t now = startMs;
        int quiet = 0;
        while (now - startMs < limitMs && quiet < 50) {
            quiet = Step(now) ? 0 : quiet + 1;
            now += 10;
        }
        return now;
    }

    bool Route(std::vector<Outgoing>& out) {
        for (Outgoing& o : out) {
            log.push_back({ o.to, o.type, o.payload });
            if (clients_.count(o.to) != 0) {
                toClient[o.to].push_back({ lobby::kHostId, o.type, std::move(o.payload) });
            }
        }
        const bool any = !out.empty();
        out.clear();
        return any;
    }

    void Take(const RoomNews& n) {
        for (uint8_t id : n.held) {
            held.insert(id);
        }
        failed = failed || n.failed;
    }

    size_t Sent(uint8_t to, uint8_t type) const {
        return size_t(std::count_if(log.begin(), log.end(), [&](const Wire& w) { return w.from == to && w.type == type; }));
    }

    TrackChoice track = Stock(0);
    uint8_t owner = lobby::kSystemId;
    uint16_t race = 1;
    bool open = true;
    bool tamperUpload = false;

    std::set<uint8_t> held;
    bool failed = false;
    int hostRefused = 0;
    std::map<uint8_t, int> clientRefused;
    std::map<uint8_t, size_t> sentChunks;
    std::vector<Wire> log; // what the host sent, from = the addressee
    std::map<uint8_t, std::deque<Wire>> toClient;
    std::deque<Wire> toHost;

private:
    std::map<uint8_t, Disk> disks_;
    std::vector<uint8_t> guests_;
    HostTrackFlow host_;
    std::map<uint8_t, std::unique_ptr<ClientTrackFlow>> clients_;
};

bool SameBytes(const std::shared_ptr<const transfer::TrackBuffer>& b, const Track& t) {
    return b != nullptr && b->Size() == t.bytes.size() && std::memcmp(b->Bytes(), t.bytes.data(), t.bytes.size()) == 0;
}

// The host owns the track; two guests lack it and download it from the host.
void TestHostOwned() {
    Room room({ 1, 2 });
    room.DiskOf(lobby::kHostId).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = lobby::kHostId;
    room.Run(0);
    Check(room.held.count(lobby::kHostId) == 1, "host-owned: the host holds its own track at once");
    Check(room.held.count(1) == 1 && room.held.count(2) == 1, "host-owned: both guests report Have");
    Check(!room.failed, "host-owned: nothing failed");
    Check(room.Sent(1, lobby::kMsgTrackOffer) == 1 && room.Sent(2, lobby::kMsgTrackOffer) == 1,
          "host-owned: one Offer per guest");
    Check(room.Sent(1, lobby::kMsgTrackNeed) == 0, "host-owned: the host never asks anyone");
    Check(SameBytes(room.Client(1).Held(), gTrack) && SameBytes(room.Client(2).Held(), gTrack),
          "host-owned: each guest's race track is the owner's bytes");
    Check(room.DiskOf(1).stores == 1 && room.DiskOf(2).stores == 1, "host-owned: each guest keeps it once");
    Check(room.DiskOf(lobby::kHostId).stores == 0, "host-owned: the host does not store its own track again");
    Check(room.hostRefused == 0 && room.clientRefused[1] == 0 && room.clientRefused[2] == 0,
          "host-owned: no message refused");
    const lobby::TrackProgress p = room.Client(1).Progress();
    Check(p.active && p.held && !p.failed && p.chunks == p.of && p.of > 1, "host-owned: guest progress is whole");
}

// Guest 1 owns the track. The host asks it, keeps the upload and relays it to guest 2 while it fills.
void TestGuestOwnedRelay() {
    Room room({ 1, 2 });
    room.DiskOf(1).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = 1;
    room.Run(0);
    Check(room.Sent(1, lobby::kMsgTrackNeed) == 1, "relay: the host asks the owner once");
    Check(room.Sent(1, lobby::kMsgTrackOffer) == 0, "relay: the owner is never offered its own track");
    Check(room.Sent(2, lobby::kMsgTrackOffer) == 1, "relay: the other guest is offered once");
    Check(room.held.count(lobby::kHostId) == 1, "relay: the host holds the upload");
    Check(room.held.count(2) == 1, "relay: the other guest holds the relayed copy");
    Check(room.held.count(1) == 0, "relay: the owner never says Have to a Need");
    Check(!room.failed, "relay: nothing failed");
    Check(SameBytes(room.Host().Held(), gTrack), "relay: the host's race track is the owner's bytes");
    Check(SameBytes(room.Client(2).Held(), gTrack), "relay: the guest's race track is the owner's bytes");
    Check(room.DiskOf(lobby::kHostId).stores == 1, "relay: the host keeps the upload in its cache");
    Check(room.hostRefused == 0 && room.clientRefused[2] == 0, "relay: no message refused");
}

// The room is closed: nothing moves until it opens.
void TestClosedPhase() {
    Room room({ 1, 2 });
    room.DiskOf(1).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = 1;
    room.open = false;
    room.Run(0, 500);
    Check(room.log.empty(), "closed: the host sends nothing before MACHINE");
    room.open = true;
    room.Run(1000);
    Check(room.held.count(2) == 1, "closed: the transfer runs once the room opens");
}

// The owner's upload is changed on the wire: the host's check fails and the room falls back.
void TestTamperedUpload() {
    Room room({ 1, 2 });
    room.DiskOf(1).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = 1;
    room.tamperUpload = true;
    room.Run(0);
    Check(room.failed, "tampered: the host reports the failure to the room");
    Check(room.held.count(lobby::kHostId) == 0 && room.held.count(2) == 0, "tampered: nobody holds the track");
    Check(room.Host().Held() == nullptr && room.Client(2).Held() == nullptr,
          "tampered: no copy races on it");
    Check(room.DiskOf(lobby::kHostId).stores == 0 && room.DiskOf(2).stores == 0, "tampered: nothing is kept");
    bool abortedFailed = false;
    for (const Wire& w : room.log) {
        if (w.from == 2 && w.type == lobby::kMsgTrackAbort) {
            abortedFailed = AbortReasonOf(w) == uint8_t(transfer::AbortReason::Failed);
        }
    }
    Check(abortedFailed, "tampered: the guest's stream is aborted as Failed");
    Check(room.Host().Progress().failed, "tampered: the host's progress shows the failure");
}

// The owner's copy cannot load the track it named: it answers the Need with an Abort and the room falls back.
void TestOwnerLacksTrack() {
    Room room({ 1, 2 });
    room.track = Custom(gTrack);
    room.owner = 1;
    room.Run(0);
    Check(room.failed, "owner lacks it: the room hears the failure");
    Check(room.Sent(2, lobby::kMsgTrackOffer) == 0, "owner lacks it: nobody else is offered a track");
}

// The host named a track it cannot load: failure at once, with no message sent.
void TestHostOwnerLacksTrack() {
    Room room({ 1 });
    room.track = Custom(gTrack);
    room.owner = lobby::kHostId;
    room.Step(0);
    Check(room.failed, "host owner lacks it: failure on the first tick");
    Check(room.log.empty(), "host owner lacks it: nothing is sent");
}

// A guest that kept the track from an earlier room answers the Offer with Have, and nothing streams to it.
void TestCachedGuest() {
    Room room({ 1, 2 });
    room.DiskOf(lobby::kHostId).tracks[gTrack.id] = gTrack.bytes;
    room.DiskOf(2).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = lobby::kHostId;
    room.Run(0);
    Check(room.held.count(2) == 1, "cached: the guest says Have");
    Check(room.sentChunks[2] == 0, "cached: no chunk is sent to it");
    Check(room.sentChunks[1] > 0, "cached: the other guest still downloads");
    Check(room.DiskOf(2).stores == 0, "cached: the guest does not store it again");
}

// The room's track changes mid-transfer: every stream is cancelled, and the new track starts clean.
void TestCancel() {
    Room room({ 1, 2 });
    room.DiskOf(1).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = 1;
    room.Step(0);  // Need to the owner
    room.Step(10); // the owner's Offer reaches the host
    room.Step(20); // the host offers guest 2
    Check(room.Sent(2, lobby::kMsgTrackOffer) == 1, "cancel: guest 2 was offered before the change");
    room.track = Stock(3);
    room.owner = lobby::kSystemId;
    room.Step(30);
    size_t cancelled = 0;
    for (const Wire& w : room.log) {
        if (w.type == lobby::kMsgTrackAbort && AbortReasonOf(w) == uint8_t(transfer::AbortReason::Cancelled)) {
            cancelled++;
        }
    }
    Check(cancelled == 2, "cancel: the owner's upload and guest 2's stream are both cancelled");
    Check(!room.Host().Progress().active && !room.Client(2).Progress().active, "cancel: no flow stays active");
    room.Run(40);
    Check(room.held.empty() && !room.failed, "cancel: a stock track holds nothing and fails nothing");
    Check(room.Client(1).Progress().held == false, "cancel: the owner's flow resets with the stock track");
}

// The same custom track in the next race is a new transfer: the race index is part of the identity of a run.
void TestNextRace() {
    Room room({ 1 });
    room.DiskOf(lobby::kHostId).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = lobby::kHostId;
    room.Run(0);
    Check(room.held.count(1) == 1, "next race: the guest held it in the first race");
    room.held.clear();
    room.race = 2;
    room.Run(1000);
    Check(room.Sent(1, lobby::kMsgTrackOffer) == 2, "next race: the guest is offered again");
    Check(room.held.count(1) == 1 && room.sentChunks[1] > 0, "next race: it answers Have from its cache");
    Check(room.DiskOf(1).stores == 1, "next race: the second race stores nothing new");
}

// A guest that left is forgotten, and a new player given its id is offered the track afresh.
void TestLeaveAndRejoin() {
    Room room({ 1, 2 });
    room.DiskOf(lobby::kHostId).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = lobby::kHostId;
    room.Step(0);
    room.Leave(2);
    room.Run(10);
    Check(room.held.count(1) == 1, "leave: the guest that stayed still downloads");
    Check(room.Sent(2, lobby::kMsgTrackOffer) == 1, "leave: the guest that left had one Offer");
    room.Join(2);
    const uint32_t end = room.Run(1000);
    Check(room.Sent(2, lobby::kMsgTrackOffer) == 2, "rejoin: the new player with that id is offered afresh");
    Check(room.held.count(2) == 1 && SameBytes(room.Client(2).Held(), gTrack), "rejoin: and downloads the track");
    Check(end < 1000 + 30000, "rejoin: the transfer ends before the limit");
}

// Messages the flows must refuse, with the reason counted.
void TestRefusals() {
    Room room({ 1, 2 });
    room.DiskOf(1).tracks[gTrack.id] = gTrack.bytes;
    room.Reset();
    room.track = Custom(gTrack);
    room.owner = 1;
    room.Step(0);
    HostTrackFlow& host = room.Host();
    std::vector<Outgoing> out;
    RoomNews news;

    // An Offer from a guest that is not the owner.
    transfer::Offer offer;
    offer.id = gTrack.id;
    offer.size = uint32_t(gTrack.bytes.size());
    offer.title = "Flow test";
    std::vector<uint8_t> bytes;
    Check(transfer::EncodeOffer(offer, bytes), "refusals: offer encodes");
    const uint32_t wrongSender = host.Counts().Get(transfer::Drop::WrongSender);
    Check(!host.Receive(2, lobby::kMsgTrackOffer, bytes.data(), bytes.size(), 5, out, news),
          "refusals: a non-owner's Offer is refused");
    Check(host.Counts().Get(transfer::Drop::WrongSender) == wrongSender + 1, "refusals: counted as WrongSender");

    // A Need from a guest that was never offered.
    const std::vector<uint8_t> need = transfer::EncodeNeed(gTrack.id);
    Check(!host.Receive(2, lobby::kMsgTrackNeed, need.data(), need.size(), 5, out, news),
          "refusals: a Need before any Offer is refused");

    // A message naming another track.
    const std::vector<uint8_t> otherHave = transfer::EncodeHave(gOther.id);
    const uint32_t wrongTrack = host.Counts().Get(transfer::Drop::WrongTrack);
    Check(!host.Receive(2, lobby::kMsgTrackHave, otherHave.data(), otherHave.size(), 5, out, news),
          "refusals: a Have for another track is refused");
    Check(host.Counts().Get(transfer::Drop::WrongTrack) == wrongTrack + 1, "refusals: counted as WrongTrack");
    Check(news.held.empty(), "refusals: no refused message reaches the room");

    // A truncated payload.
    const uint32_t malformed = host.Counts().Get(transfer::Drop::Malformed);
    Check(!host.Receive(2, lobby::kMsgTrackHave, need.data(), need.size() - 1, 5, out, news),
          "refusals: a truncated Have is refused");
    Check(host.Counts().Get(transfer::Drop::Malformed) == malformed + 1, "refusals: counted as Malformed");

    // A type outside 26 to 30.
    Check(!host.Receive(2, lobby::kMsgTrackAbort + 1, need.data(), need.size(), 5, out, news),
          "refusals: an unknown type is refused");

    // The guest that is not the owner refuses the host's Need.
    std::vector<Outgoing> replies;
    ClientTrackFlow& guest = room.Client(2);
    guest.Sync(room.track, false, room.race, true, 5);
    Check(!guest.Receive(lobby::kMsgTrackNeed, need.data(), need.size(), 5, replies),
          "refusals: a non-owner refuses the host's Need");
    Check(replies.empty(), "refusals: and sends nothing back");
    Check(out.empty(), "refusals: no refused message makes the host send");

    // Nothing moves on a stock track.
    room.track = Stock(4);
    room.Step(100);
    Check(!host.Receive(1, lobby::kMsgTrackOffer, bytes.data(), bytes.size(), 110, out, news),
          "refusals: an Offer after the track went stock is refused");
}

} // namespace

int main() {
    if (!BuildTrack(90, gTrack) || !BuildTrack(70, gOther)) {
        std::printf("FAIL: fixture tracks do not build\n0 checks, 1 failed\n");
        return 1;
    }
    Check(gTrack.bytes.size() > transfer::kChunkData * 2, "fixture: the track spans several chunks");
    TestHostOwned();
    TestGuestOwnedRelay();
    TestClosedPhase();
    TestTamperedUpload();
    TestOwnerLacksTrack();
    TestHostOwnerLacksTrack();
    TestCachedGuest();
    TestCancel();
    TestNextRace();
    TestLeaveAndRejoin();
    TestRefusals();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
