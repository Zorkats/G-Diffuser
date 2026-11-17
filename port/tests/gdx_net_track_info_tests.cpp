// port/tests/gdx_net_track_info_tests.cpp: the creator names and song of a room's custom track (protocol 15).
// Copyright (c) Zorkats
//
// Usage: gdx_net_track_info_tests
//
// The Info codec's round trips and refusals, then one HostInfoFlow and up to three ClientInfoFlow objects passing
// their messages through in-memory queues. The meta bytes are opaque here: the flows carry them, the game decodes.

#include "gdx_net_track_info.h"

#include "gdx_net_transfer.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace lobby = gdx::net::lobby;
namespace track = gdx::net::track;
namespace transfer = gdx::net::transfer;
using lobby::ClientInfoFlow;
using lobby::HostInfoFlow;
using lobby::InfoReady;
using lobby::InfoSource;
using lobby::Outgoing;
using lobby::TrackChoice;
using lobby::TrackInfo;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

track::TrackId IdOf(uint8_t seed) {
    track::TrackId id{};
    for (size_t i = 0; i < id.size(); i++) {
        id[i] = uint8_t(seed + i * 3);
    }
    return id;
}

TrackInfo InfoOf(const track::TrackId& id, bool song) {
    TrackInfo info;
    info.track = id;
    info.meta = { 'G', 'X', 'M', '1', 1, 2, 3, 4, 5 };
    info.hasSong = song;
    if (song) {
        info.song = IdOf(0x80);
        info.songSize = 42191447;
    }
    return info;
}

bool Same(const TrackInfo& a, const TrackInfo& b) {
    return a.track == b.track && a.meta == b.meta && a.hasSong == b.hasSong && a.song == b.song &&
           a.songSize == b.songSize;
}

TrackChoice Custom(const track::TrackId& id) {
    TrackChoice c;
    c.track = lobby::kTrackCustom;
    c.custom.id = id;
    c.custom.title = "Info test";
    return c;
}

void TestCodec() {
    const track::TrackId id = IdOf(7);
    std::vector<uint8_t> bytes = lobby::EncodeInfoNeed(id);
    track::TrackId back{};
    Check(bytes.size() == 32 && lobby::DecodeInfoNeed(bytes.data(), bytes.size(), back) && back == id,
          "codec: InfoNeed round trip");
    Check(!lobby::DecodeInfoNeed(bytes.data(), 31, back), "codec: a short InfoNeed is refused");

    for (bool song : { false, true }) {
        const TrackInfo info = InfoOf(id, song);
        Check(lobby::EncodeInfo(info, bytes), "codec: Info encodes");
        Check(bytes.size() == 32 + 2 + info.meta.size() + 1 + (song ? 36 : 0), "codec: Info is as long as its layout");
        TrackInfo out;
        Check(lobby::DecodeInfo(bytes.data(), bytes.size(), out) && Same(out, info), "codec: Info round trip");
        std::vector<uint8_t> longer = bytes;
        longer.push_back(0);
        Check(!lobby::DecodeInfo(longer.data(), longer.size(), out), "codec: trailing bytes are refused");
        Check(!lobby::DecodeInfo(bytes.data(), bytes.size() - 1, out), "codec: a cut Info is refused");
    }

    TrackInfo empty;
    empty.track = id;
    Check(lobby::EncodeInfo(empty, bytes) && bytes.size() == 35, "codec: no names and no song is 35 bytes");

    TrackInfo big = InfoOf(id, false);
    big.meta.assign(lobby::kInfoMetaMax + 1, 0);
    Check(!lobby::EncodeInfo(big, bytes), "codec: a meta block over 512 bytes is not sent");
    big.meta.assign(lobby::kInfoMetaMax, 0);
    Check(lobby::EncodeInfo(big, bytes), "codec: a 512-byte meta block is sent");
    bytes[32] = 0x01; // meta length 513
    bytes[33] = 0x02;
    TrackInfo out;
    Check(!lobby::DecodeInfo(bytes.data(), bytes.size(), out), "codec: a meta length over 512 is refused");

    TrackInfo song = InfoOf(id, true);
    song.songSize = 0;
    Check(!lobby::EncodeInfo(song, bytes), "codec: an empty song is not sent");
    song.songSize = uint32_t(transfer::kSongMaxBytes + 1);
    Check(!lobby::EncodeInfo(song, bytes), "codec: a song over 64 MB is not sent");
    song.songSize = uint32_t(transfer::kSongMaxBytes);
    Check(lobby::EncodeInfo(song, bytes), "codec: a 64 MB song is sent");
    bytes[bytes.size() - 1] = 0x7F; // size far over the limit
    Check(!lobby::DecodeInfo(bytes.data(), bytes.size(), out), "codec: a received size over 64 MB is refused");

    TrackInfo flag = InfoOf(id, false);
    Check(lobby::EncodeInfo(flag, bytes), "codec: Info without song encodes");
    bytes.back() = 2;
    Check(!lobby::DecodeInfo(bytes.data(), bytes.size(), out), "codec: a song flag other than 0 or 1 is refused");
}

// One copy's own tracks: what its InfoSource answers.
struct Own {
    std::map<track::TrackId, TrackInfo> ready;
    std::map<track::TrackId, bool> pending;

    InfoSource Source() {
        return [this](const track::TrackId& id, TrackInfo& out) {
            if (pending.count(id) != 0 && pending[id]) {
                return InfoReady::Pending;
            }
            auto it = ready.find(id);
            if (it == ready.end()) {
                return InfoReady::Unknown;
            }
            out = it->second;
            return InfoReady::Ready;
        };
    }
};

struct Wire {
    uint8_t peer = 0; // the sender to the host, the addressee from it
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

class Room {
public:
    explicit Room(std::vector<uint8_t> guests) : guests_(std::move(guests)) {
        Reset();
    }

    Own& OwnOf(uint8_t id) {
        return own_[id];
    }

    void Reset() {
        host_ = HostInfoFlow(own_[lobby::kHostId].Source());
        clients_.clear();
        for (uint8_t g : guests_) {
            clients_[g] = std::make_unique<ClientInfoFlow>(own_[g].Source());
        }
    }

    void Join(uint8_t id) {
        guests_.push_back(id);
        clients_[id] = std::make_unique<ClientInfoFlow>(own_[id].Source());
    }

    HostInfoFlow& Host() {
        return host_;
    }

    ClientInfoFlow& Client(uint8_t id) {
        return *clients_.at(id);
    }

    void Step() {
        std::vector<Outgoing> out;
        host_.Sync(track, owner, race, guests_, out);
        Route(out);
        for (auto& [id, client] : clients_) {
            std::vector<Outgoing> replies;
            client->Sync(track, race, replies);
            for (Outgoing& r : replies) {
                toHost.push_back({ id, r.type, std::move(r.payload) });
            }
            std::deque<Wire>& q = toClient[id];
            while (!q.empty()) {
                Wire w = std::move(q.front());
                q.pop_front();
                replies.clear();
                if (!client->Receive(w.type, w.payload.data(), w.payload.size(), replies)) {
                    clientRefused[id]++;
                }
                for (Outgoing& r : replies) {
                    toHost.push_back({ id, r.type, std::move(r.payload) });
                }
            }
        }
        while (!toHost.empty()) {
            Wire w = std::move(toHost.front());
            toHost.pop_front();
            sentToHost.push_back(w);
            if (!host_.Receive(w.peer, w.type, w.payload.data(), w.payload.size())) {
                hostRefused++;
            }
        }
    }

    void Run(int ticks = 10) {
        for (int i = 0; i < ticks; i++) {
            Step();
        }
    }

    size_t Sent(uint8_t to, uint8_t type) const {
        size_t n = 0;
        for (const Wire& w : log) {
            n += (w.peer == to && w.type == type) ? 1 : 0;
        }
        return n;
    }

    TrackChoice track;
    uint8_t owner = lobby::kSystemId;
    uint16_t race = 1;
    int hostRefused = 0;
    std::map<uint8_t, int> clientRefused;
    std::vector<Wire> log;        // what the host sent
    std::vector<Wire> sentToHost; // what the guests sent
    std::map<uint8_t, std::deque<Wire>> toClient;
    std::deque<Wire> toHost;

private:
    void Route(std::vector<Outgoing>& out) {
        for (Outgoing& o : out) {
            log.push_back({ o.to, o.type, o.payload });
            if (clients_.count(o.to) != 0) {
                toClient[o.to].push_back({ lobby::kHostId, o.type, std::move(o.payload) });
            }
        }
        out.clear();
    }

    std::map<uint8_t, Own> own_;
    std::vector<uint8_t> guests_;
    HostInfoFlow host_;
    std::map<uint8_t, std::unique_ptr<ClientInfoFlow>> clients_;
};

// The host owns the track: no InfoNeed, and every guest gets the host's Info once.
void TestHostOwner() {
    const track::TrackId id = IdOf(1);
    Room room({ 1, 2 });
    room.OwnOf(lobby::kHostId).ready[id] = InfoOf(id, true);
    room.Reset();
    room.track = Custom(id);
    room.owner = lobby::kHostId;
    room.Run();
    Check(room.Sent(1, lobby::kMsgTrackInfoNeed) == 0 && room.Sent(2, lobby::kMsgTrackInfoNeed) == 0,
          "host owner: the host asks nobody");
    Check(room.Sent(1, lobby::kMsgTrackInfo) == 1 && room.Sent(2, lobby::kMsgTrackInfo) == 1,
          "host owner: each guest gets the Info once");
    Check(room.Host().Info() != nullptr && Same(*room.Host().Info(), InfoOf(id, true)), "host owner: the host knows it");
    Check(room.Client(1).Info() != nullptr && Same(*room.Client(1).Info(), InfoOf(id, true)) &&
              room.Client(2).Info() != nullptr && Same(*room.Client(2).Info(), InfoOf(id, true)),
          "host owner: both guests know it");
    Check(room.hostRefused == 0 && room.clientRefused[1] == 0 && room.clientRefused[2] == 0,
          "host owner: no message refused");
}

// A guest owns the track and its song is still being hashed: the host asks once, the answer waits for the hash,
// and the other guest gets the Info from the host. The owner is never sent its own Info.
void TestGuestOwnerPending() {
    const track::TrackId id = IdOf(2);
    Room room({ 1, 2 });
    room.OwnOf(1).ready[id] = InfoOf(id, true);
    room.OwnOf(1).pending[id] = true;
    room.Reset();
    room.track = Custom(id);
    room.owner = 1;
    room.Run();
    Check(room.Sent(1, lobby::kMsgTrackInfoNeed) == 1, "pending: the host asks the owner once");
    Check(room.sentToHost.empty(), "pending: the owner does not answer before its hash ends");
    Check(room.Host().Info() == nullptr && room.Client(2).Info() == nullptr, "pending: nobody knows the Info yet");
    room.OwnOf(1).pending[id] = false;
    room.Run();
    Check(room.sentToHost.size() == 1 && room.sentToHost[0].type == lobby::kMsgTrackInfo,
          "pending: the owner answers once the hash ends");
    Check(room.Host().Info() != nullptr && Same(*room.Host().Info(), InfoOf(id, true)), "pending: the host keeps it");
    Check(room.Sent(2, lobby::kMsgTrackInfo) == 1, "pending: the other guest gets it from the host");
    Check(room.Sent(1, lobby::kMsgTrackInfo) == 0, "pending: the owner is not sent its own Info");
    Check(room.Client(1).Info() != nullptr && room.Client(2).Info() != nullptr &&
              Same(*room.Client(2).Info(), InfoOf(id, true)),
          "pending: both guests know it");
    Check(room.hostRefused == 0 && room.clientRefused[2] == 0, "pending: no message refused");
}

// The host already held the track (the case Info exists for): it still asks the owner, and a latecomer is sent
// the Info when it joins.
void TestLatecomer() {
    const track::TrackId id = IdOf(3);
    Room room({ 1 });
    room.OwnOf(1).ready[id] = InfoOf(id, false);
    room.Reset();
    room.track = Custom(id);
    room.owner = 1;
    room.Run();
    Check(room.Host().Info() != nullptr && !room.Host().Info()->hasSong, "latecomer: the host knows a songless Info");
    room.Join(3);
    room.Run();
    Check(room.Sent(3, lobby::kMsgTrackInfo) == 1, "latecomer: the new guest gets the Info");
    Check(room.Client(3).Info() != nullptr && Same(*room.Client(3).Info(), InfoOf(id, false)),
          "latecomer: the new guest knows it");
}

// Info from a guest the host did not ask, or for another track, is refused. A track that is not the owner's
// gets no answer.
void TestRefusals() {
    const track::TrackId id = IdOf(4);
    Room room({ 1, 2 });
    room.OwnOf(1).ready[id] = InfoOf(id, true);
    room.OwnOf(1).pending[id] = true; // the owner holds its answer, so the host is still waiting below
    room.Reset();
    room.track = Custom(id);
    room.owner = 1;
    room.Step();
    Check(room.Host().Info() == nullptr, "refusals: the host waits for the owner");
    std::vector<uint8_t> bytes;
    lobby::EncodeInfo(InfoOf(id, false), bytes);
    Check(!room.Host().Receive(2, lobby::kMsgTrackInfo, bytes.data(), bytes.size()),
          "refusals: Info from a guest the host did not ask is refused");
    lobby::EncodeInfo(InfoOf(IdOf(9), false), bytes);
    Check(!room.Host().Receive(1, lobby::kMsgTrackInfo, bytes.data(), bytes.size()),
          "refusals: Info for another track is refused");
    room.OwnOf(1).pending[id] = false;
    room.Run();
    Check(room.Host().Info() != nullptr && room.Host().Info()->hasSong, "refusals: the owner's real Info still lands");
    lobby::EncodeInfo(InfoOf(id, false), bytes);
    Check(!room.Host().Receive(1, lobby::kMsgTrackInfo, bytes.data(), bytes.size()),
          "refusals: a second Info from the owner is refused");

    // Guest 2 is named owner but does not have the track: it never answers, and nobody learns an Info.
    const track::TrackId other = IdOf(5);
    Room lost({ 1, 2 });
    lost.track = Custom(other);
    lost.owner = 2;
    lost.Run();
    Check(lost.Sent(2, lobby::kMsgTrackInfoNeed) == 1 && lost.sentToHost.empty(),
          "refusals: an owner without the track does not answer");
    Check(lost.Host().Info() == nullptr && lost.Client(1).Info() == nullptr, "refusals: nobody learns an Info");
}

// The room moves to a stock track or the next race: the Info is dropped and asked again for the new race.
void TestReset() {
    const track::TrackId id = IdOf(6);
    Room room({ 1, 2 });
    room.OwnOf(1).ready[id] = InfoOf(id, true);
    room.Reset();
    room.track = Custom(id);
    room.owner = 1;
    room.Run();
    Check(room.Client(2).Info() != nullptr, "reset: the first race knows the Info");
    room.track = TrackChoice();
    room.Run(2);
    Check(room.Host().Info() == nullptr && room.Client(2).Info() == nullptr, "reset: a stock track drops it");
    room.track = Custom(id);
    room.race = 2;
    room.Run();
    Check(room.Sent(1, lobby::kMsgTrackInfoNeed) == 2, "reset: the next race asks the owner again");
    Check(room.Client(2).Info() != nullptr, "reset: the next race knows it again");
}

} // namespace

int main() {
    TestCodec();
    TestHostOwner();
    TestGuestOwnerPending();
    TestLatecomer();
    TestRefusals();
    TestReset();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
