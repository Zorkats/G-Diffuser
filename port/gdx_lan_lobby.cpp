// port/gdx_lan_lobby.cpp -- see gdx_lan_lobby.h.
//
// One worker thread per session. The worker owns the socket and the session object outright; the ImGui
// thread only queues commands and copies the published View, both under one mutex. The runtime lives on
// the heap and is never destroyed, so an exit path that skips Shutdown() cannot hit a joinable
// std::thread in a static destructor.
//
// The poses of a race do not go through that mutex. The game thread and the worker exchange them through
// the two boxes of net/gdx_net_race.h, which have a lock of their own: each tick the worker takes what
// the game queued and hands it to the session, then gives the game what the room sent.
//
// A host who leaves the room (D28.5) leaves its worker running: the room runs the race to its results and
// then goes to the next host. From the moment the host leaves, that worker takes no command and publishes no
// view, so the next session can start beside it; StopWorker moves it aside as the leaver.

#include "gdx_lan_lobby.h"

#include "gdx_course_state.h"
#include "gdx_extract_launch.h"
#include "gdx_hackmods.h"
#include "gdx_net_machine_select.h"
#include "gdx_net_race_room.h"
#include "gdx_net_track_cache.h"
#include "gdx_profile_keys.h"
#include "gdx_profile_state.h"
#include "net/gdx_net_codes.h"
#include "net/gdx_net_friends.h"
#include "net/gdx_net_host_info.h"
#include "net/gdx_net_race.h"
#include "net/gdx_net_track_pack.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"

#include "port_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#ifndef GDX_VERSION_STRING
#define GDX_VERSION_STRING "dev"
#endif
#ifndef GDX_BUILD_COMMIT
#define GDX_BUILD_COMMIT ""
#endif

namespace gdx::lan {

namespace {

using net::NetAddress;
using net::RecvResult;
using net::UdpSocket;
using namespace net::lobby;

constexpr uint32_t kTickMs = 10;
const char* const kPortCvar = "gEnhancements.Online.LanPort";
constexpr int kMaxReadsPerTick = 256;

// What the worker should run.
enum class Kind : uint8_t { Browse, Host, Join };

// The online half of a session (gdx_net_online.h), read on the ImGui thread at launch: the worker never reads a
// setting or the profile store.
// The profile seed the join proof is signed with (O7.3), read on the game thread when a session starts. The
// bytes are wiped when the last session holding them ends. The profile store holds the same seed for the life of
// the process, so this adds no place a secret outlives its use.
struct JoinSeed {
    unsigned char bytes[net::friends::kSeedBytes] = {};
    JoinSeed() = default;
    JoinSeed(const JoinSeed&) = delete;
    JoinSeed& operator=(const JoinSeed&) = delete;
    ~JoinSeed() {
        gdx_profile_keys_wipe(bytes, sizeof(bytes));
    }
};

struct OnlineSetup {
    RoomType type = RoomType::Lan;      // Host: the room's type; any but Lan is listed
    std::vector<std::string> bases;     // the directory; empty: nothing is listed or looked up
    std::vector<std::string> stun;
    std::optional<NetAddress> advertise;
    std::string hostKey;                // empty: no listing, so a promoted host does not take the code over
    uint32_t joinCode = kNoRoomCode;    // Join: ask the directory for this room first
    std::vector<NetAddress> candidates; // Join: the room's addresses, tried in order
    bool byRecord = false;              // Join: candidates came from the directory; none left closes the session
    // O7.3. identity is this copy's friend key, zero without a profile seed; seed signs the join proof, and with
    // none a Join carries no key. friends are the keys a Friends room this copy hosts admits.
    Bytes32 identity{};
    std::shared_ptr<const JoinSeed> seed;
    std::vector<Bytes32> friends;
    std::optional<Bytes32> expectHost; // Join: the host key the directory gave for the room
    bool localTest = false;            // a local test directory is set: a successor on loopback may be dialed
    bool directLinks = true;           // gEnhancements.Online.DirectLinks (decision 67)
};

struct Command {
    enum class Type : uint8_t {
        Ready,
        Vote,
        Pick,
        Lock,
        Emote,
        RaceNext,
        StartSeries,
        SkipLottery,
        EndSeries,
        RacerCap,
        CustomTracks,
        Kick,
        Ban,
        GridReady,
        Rules,
        Finish,
        Standing,
        HostLeft,
        VoteCustom,
        PickCustom,
        CpuFill,
        CpuTuning
    } type = Type::Ready;
    bool flag = false; // Ready, RaceNext, CustomTracks, CpuFill, CpuTuning (track hazards)
    uint16_t aggression = kAiAggressionDefault; // CpuTuning
    uint8_t value = 0; // a track, a preset message, a racer cap or a player id
    LockRequest lock;
    ReadyToRace ready;
    RaceRules rules;
    FinishReport finish;
    std::vector<uint8_t> order; // Standing: player ids, first to last
    uint16_t race = 0;          // HostLeft: the race the host's game left
    CustomTrack custom;         // VoteCustom, PickCustom
};

// What the ImGui thread tells one worker. handOver and nextName are guarded by the runtime's lock.
struct WorkerCtl {
    std::atomic<bool> stop{ false };
    // Stop without a goodbye: the game is closing, which counts as a crash (decision 17), so the members
    // find the next host through the silence instead of being told the room closed.
    std::atomic<bool> silent{ false };
    std::atomic<bool> done{ false }; // the worker returned
    bool handOver = false;           // the host left the room; the worker runs on until the room is handed over
    std::string nextName;            // while it does: the player the room goes to
};

struct Runtime {
    std::mutex lock;
    View view;                  // guarded by lock
    std::deque<Command> queue;  // guarded by lock
    std::thread worker;         // started and joined on the ImGui thread only
    std::shared_ptr<WorkerCtl> ctl = std::make_shared<WorkerCtl>(); // the worker's; replaced on the ImGui thread
    std::thread leaver;         // a left room's worker (D28.5), ImGui thread only
    std::shared_ptr<WorkerCtl> leaverCtl;
    std::vector<RoomMember> lastRoom; // guarded by lock; see LastRoom()
    // A session started since lastRoom was filled: the first view with another member replaces the list, so a
    // join that is refused, or a room nobody else enters, keeps the players of the room before. Under lock.
    bool lastRoomStale = false;
};

constexpr size_t kLastRoomMax = 32;

// Adds the members of a published view to the last room. Under the runtime's lock.
void RememberMembers(Runtime& rt, const View& v, const Bytes32& own) {
    for (const MemberKey& member : v.keys) {
        if (member.key == own) {
            continue;
        }
        const auto player = std::find_if(v.roster.begin(), v.roster.end(),
                                         [&](const PlayerInfo& p) { return p.id == member.id; });
        if (player == v.roster.end()) {
            continue;
        }
        if (rt.lastRoomStale) {
            rt.lastRoom.clear();
            rt.lastRoomStale = false;
        }
        const auto known = std::find_if(rt.lastRoom.begin(), rt.lastRoom.end(),
                                        [&](const RoomMember& m) { return m.key == member.key; });
        if (known != rt.lastRoom.end()) {
            known->name = player->name;
        } else if (rt.lastRoom.size() < kLastRoomMax) {
            rt.lastRoom.push_back(RoomMember{ player->name, member.key });
        }
    }
}

// The directory signer for a hosted room (CONTRACT.md §8a): the friend key is the host key. None with no seed.
net::directory::Signer DirectorySignerFor(const OnlineSetup& online) {
    if (!online.seed) {
        return net::directory::Signer();
    }
    return [seed = online.seed](std::string_view message, net::directory::Signature& out) {
        net::friends::Signature signature{};
        if (!net::friends::DirectorySign(seed->bytes, reinterpret_cast<const unsigned char*>(message.data()),
                                         message.size(), signature)) {
            return false;
        }
        std::copy(signature.begin(), signature.end(), out.begin());
        return true;
    };
}

// The join proof signer for a session (O7.3), or none when the profile has no seed.
JoinSigner SignerFor(const OnlineSetup& online) {
    if (!online.seed) {
        return JoinSigner();
    }
    return [seed = online.seed, identity = online.identity](const JoinNonce& nonce, Bytes32& key,
                                                            std::array<uint8_t, 64>& proof) {
        net::friends::Key host{};
        net::friends::Nonce bytes{};
        net::friends::Signature signature{};
        std::copy(nonce.hostKey.begin(), nonce.hostKey.end(), host.begin());
        std::copy(nonce.nonce.begin(), nonce.nonce.end(), bytes.begin());
        net::friends::JoinProofSign(seed->bytes, nonce.roomId, host, bytes, signature);
        key = identity;
        std::copy(signature.begin(), signature.end(), proof.begin());
        return true;
    };
}

// The host key of a directory record, when it is one.
std::optional<Bytes32> RecordHostKey(const net::directory::Record& record) {
    Bytes32 key{};
    const std::string& text = record.draft.hostKey;
    if (!net::directory::IsHostKey(text) || !net::directory::ParseSecretHex(text, key)) {
        return std::nullopt;
    }
    return key;
}

Runtime& Rt() {
    static Runtime* runtime = new Runtime(); // never destroyed, see the file comment
    return *runtime;
}

uint32_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t RandomNonce() {
    std::random_device device;
    uint32_t value = 0;
    while (value == 0) {
        value = device();
    }
    return value;
}

// 32 bytes from the OS source (std::random_device, as RandomNonce): a room's lottery key (O6.4) or a
// directory takeover secret (O5.10).
Bytes32 RandomKey() {
    std::random_device device;
    Bytes32 key{};
    for (size_t i = 0; i < key.size(); i += 4) {
        const uint32_t word = device();
        for (size_t b = 0; b < 4; b++) {
            key[i + b] = static_cast<uint8_t>(word >> (8 * b));
        }
    }
    return key;
}

NetAddress Addr(uint32_t ip, uint16_t port) {
    return NetAddress::FromV4(ip, port);
}

// Joins the worker of a left room once it is done, or at once with force: the room then goes to the next host
// through the silence, as after a crash. ImGui thread only.
void ReapLeaver(bool force) {
    Runtime& rt = Rt();
    if (!rt.leaver.joinable() || (!force && !rt.leaverCtl->done.load())) {
        return;
    }
    rt.leaverCtl->silent.store(true);
    rt.leaverCtl->stop.store(true);
    rt.leaver.join();
    rt.leaverCtl.reset();
}

// Ends the worker of the last session, if any. A host's worker whose room is being handed over is moved aside
// instead, unless silent: then it stops like any other, without a goodbye. ImGui thread only.
void StopWorker(bool silent = false) {
    Runtime& rt = Rt();
    ReapLeaver(false);
    if (rt.worker.joinable()) {
        bool handOver = false;
        {
            std::lock_guard<std::mutex> guard(rt.lock);
            handOver = rt.ctl->handOver;
        }
        if (handOver && !silent) {
            ReapLeaver(true); // one left room at a time: an older one still running goes as in a crash
            rt.leaver = std::move(rt.worker);
            rt.leaverCtl = rt.ctl;
        } else {
            rt.ctl->silent.store(silent);
            rt.ctl->stop.store(true);
            rt.worker.join();
        }
    }
    std::lock_guard<std::mutex> guard(rt.lock);
    rt.ctl = std::make_shared<WorkerCtl>();
    rt.queue.clear();
}

View MakeView(Mode mode, const std::string& status, uint16_t port, const std::string& hostText, uint8_t localId) {
    View v;
    v.mode = mode;
    v.status = status;
    v.port = port;
    v.hostText = hostText;
    v.localId = localId;
    return v;
}

void Publish(View view) {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    rt.view = std::move(view);
}

// The commands queued for the worker, and whether the host left the room: both read under one lock, so every
// command the game queued before it left is in the list.
std::deque<Command> TakeCommands(const WorkerCtl& ctl, bool& handOver) {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    std::deque<Command> taken;
    taken.swap(rt.queue);
    handOver = ctl.handOver;
    return taken;
}

// L2 (decision 46): the host's upload over each race, from the links' byte counters, so a run of real copies
// can be set beside the model of gdx_net_upload_tests.
struct RaceUpload {
    bool racing = false;
    LobbyHost::UploadTotals at;
    uint32_t sinceMs = 0;
};

void LogRaceUpload(const LobbyHost& host, bool racing, const std::vector<PlayerInfo>& roster, uint32_t now,
                   RaceUpload& upload) {
    if (racing == upload.racing) {
        return;
    }
    upload.racing = racing;
    const LobbyHost::UploadTotals totals = host.Upload();
    if (racing) {
        upload.at = totals;
        upload.sinceMs = now;
        return;
    }
    const double seconds = (now - upload.sinceMs) / 1000.0;
    if (seconds <= 0.0) {
        return;
    }
    size_t watchers = 0;
    for (const PlayerInfo& player : roster) {
        watchers += player.watcher ? 1 : 0;
    }
    const double kbit = static_cast<double>(totals.bytes - upload.at.bytes) * 8.0 / 1000.0 / seconds;
    const double datagrams = static_cast<double>(totals.datagrams - upload.at.datagrams) / seconds;
    const double links = static_cast<double>(totals.links > 0 ? totals.links : 1);
    gdx_port_logf("[lan] upload over the race: %.1f s, %zu links (%zu humans, %zu watching), %.1f kbit/s from the "
                  "host, %.1f per link, %.1f datagrams/s per link%s\n",
                  seconds, totals.links, roster.size(), watchers, kbit, kbit / links, datagrams / links,
                  totals.links != upload.at.links ? " (links changed during the race)" : "");
}

// Decision 66: how each other racer's poses reached this member over the race, logged when it ends. A pose
// counts for the path it came by first; the age is the host clock on arrival minus the host clock at its tick.
void LogRacePaths(const LobbyClient& client, bool racing, bool& wasRacing) {
    if (racing == wasRacing) {
        return;
    }
    wasRacing = racing;
    Grid grid;
    const PosePathStats& stats = client.PathStats();
    if (racing || !client.RaceGrid(grid) || stats.raceIndex != grid.raceIndex) {
        return;
    }
    const auto mean = [&](size_t slot, size_t path) {
        return stats.aged[slot][path] > 0 ? static_cast<long long>(stats.ageSumMs[slot][path] / stats.aged[slot][path])
                                          : -1LL;
    };
    for (size_t slot = 0; slot < grid.humans.size() && slot < kGridSlots; slot++) {
        if (grid.humans[slot].id == client.LocalId()) {
            continue;
        }
        const auto& first = stats.first[slot];
        gdx_port_logf("[lan] race %u poses of slot %zu (player %u): first by host %u, direct %u, relay %u; mean age "
                      "host %lld ms, direct %lld ms, relay %lld ms (-1: none)\n",
                      static_cast<unsigned>(grid.raceIndex), slot, static_cast<unsigned>(grid.humans[slot].id),
                      first[kPathHost], first[kPathDirect], first[kPathRelay], mean(slot, kPathHost),
                      mean(slot, kPathDirect), mean(slot, kPathRelay));
    }
    const net::links::LinkStats& link = client.Links().Stats();
    gdx_port_logf("[lan] direct links: %zu peers, %u punches and %u pose datagrams sent, %u refused; poses taken "
                  "by LAN %u, public %u, relay %u\n",
                  client.Links().PeerCount(), link.punchesSent, link.posesSent, link.refused,
                  link.posesReceived[static_cast<size_t>(net::links::PathKind::Lan)],
                  link.posesReceived[static_cast<size_t>(net::links::PathKind::Public)],
                  link.posesReceived[static_cast<size_t>(net::links::PathKind::Relay)]);
}

// Decisions 67 and 68: what the host may tell the other members about reaching this copy. With direct links off
// that is nothing; on, this copy's addresses on its own networks with the room socket's port, IPv4 first. The
// host gives those only to a member behind the same public address.
PathsInfo OwnPaths(bool direct, uint16_t port) {
    PathsInfo paths;
    paths.direct = direct;
    if (!direct) {
        return paths;
    }
    std::vector<NetAddress> all = net::host_info::LocalAddresses();
    std::stable_partition(all.begin(), all.end(), [](const NetAddress& a) { return net::IsMappedV4(a.ip); });
    for (NetAddress address : all) {
        if (paths.lan.size() >= kPeerLanMax) {
            break;
        }
        address.port = port;
        paths.lan.push_back(net::links::EndpointOf(address));
    }
    return paths;
}

void RunHostCommand(LobbyHost& host, const Command& cmd, uint32_t now) {
    switch (cmd.type) {
    case Command::Type::Ready:
        host.SetReady(cmd.flag, now);
        break;
    case Command::Type::Vote:
        host.Vote(cmd.value, now);
        break;
    case Command::Type::Pick:
        host.Pick(cmd.value, now);
        break;
    case Command::Type::VoteCustom:
        host.Vote(TrackChoice{ kTrackCustom, cmd.custom }, now);
        break;
    case Command::Type::PickCustom:
        host.Pick(TrackChoice{ kTrackCustom, cmd.custom }, now);
        break;
    case Command::Type::Lock:
        host.Lock(cmd.lock, now);
        break;
    case Command::Type::Emote:
        host.Emote(cmd.value, now);
        break;
    case Command::Type::RaceNext:
        break; // the host always holds a race seat
    case Command::Type::StartSeries:
        host.Start(now);
        break;
    case Command::Type::SkipLottery:
        host.SkipLottery(now);
        break;
    case Command::Type::EndSeries:
        host.EndSeries(now);
        break;
    case Command::Type::RacerCap:
        host.SetRacerCap(cmd.value, now);
        break;
    case Command::Type::CustomTracks:
        host.SetCustomTracks(cmd.flag, now);
        break;
    case Command::Type::CpuFill: {
        CpuOptions cpu = host.Cpu();
        cpu.fill = cmd.flag;
        host.SetCpuOptions(cpu, now);
        break;
    }
    case Command::Type::CpuTuning: {
        CpuOptions cpu = host.Cpu();
        cpu.aiAggression = cmd.aggression;
        cpu.trackHazards = cmd.flag;
        host.SetCpuOptions(cpu, now);
        break;
    }
    case Command::Type::Kick:
        host.Kick(cmd.value, now);
        break;
    case Command::Type::Ban:
        host.Ban(cmd.value, now);
        break;
    case Command::Type::GridReady:
        host.GridReady(cmd.ready, now);
        break;
    case Command::Type::Rules:
        host.SetRules(cmd.rules);
        break;
    case Command::Type::Finish:
        host.Finish(cmd.finish, now);
        break;
    case Command::Type::Standing:
        host.SetStanding(cmd.order);
        break;
    case Command::Type::HostLeft:
        // Said of one race: by the time it is run the room may be on the next.
        if (host.Room(now).raceIndex == cmd.race) {
            host.HostLeftRace(now);
        }
        break;
    }
}

void RunClientCommand(LobbyClient& client, const Command& cmd, uint32_t now) {
    switch (cmd.type) {
    case Command::Type::Ready:
        client.SetReady(cmd.flag);
        break;
    case Command::Type::Vote:
        client.Vote(cmd.value);
        break;
    case Command::Type::Pick:
        client.Pick(cmd.value);
        break;
    case Command::Type::VoteCustom:
        client.Vote(TrackChoice{ kTrackCustom, cmd.custom });
        break;
    case Command::Type::PickCustom:
        client.Pick(TrackChoice{ kTrackCustom, cmd.custom });
        break;
    case Command::Type::Lock:
        client.Lock(cmd.lock);
        break;
    case Command::Type::Emote:
        client.Emote(cmd.value, now);
        break;
    case Command::Type::RaceNext:
        client.RaceNext(cmd.flag);
        break;
    case Command::Type::GridReady:
        client.GridReady(cmd.ready);
        break;
    case Command::Type::Finish:
        client.Finish(cmd.finish);
        break;
    default:
        break; // the host's controls
    }
}

// What the game queued since the last tick leaves with this one. host or client, whichever is running.
void SendRacePoses(LobbyHost* host, LobbyClient* client) {
    GdxNetSlotPose taken[GDX_NET_RACE_OUT_CAPACITY];
    unsigned int raceIndex = 0;
    const int count = gdx_net_race_take_outgoing(&raceIndex, taken, GDX_NET_RACE_OUT_CAPACITY);
    if (count <= 0) {
        return;
    }
    std::vector<StatePose> poses(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const GdxNetPose& from = taken[i].pose;
        StatePose& to = poses[static_cast<size_t>(i)];
        to.slot = static_cast<uint8_t>(taken[i].slot);
        to.tick = from.tick;
        to.timeMs = from.timeMs;
        for (size_t k = 0; k < 3; k++) {
            to.pos[k] = from.pos[k];
            to.vel[k] = from.vel[k];
            to.forward[k] = from.forward[k];
            to.up[k] = from.up[k];
        }
        // A lap outside what the message carries would lose the whole pose; the nearest one keeps it.
        to.lap = static_cast<int16_t>(from.lap < kStateLapMin ? kStateLapMin
                                                             : from.lap > kStateLapMax ? kStateLapMax : from.lap);
        to.flags = from.flags;
        // The same for an attack state the game should never have.
        to.attack = static_cast<uint8_t>(from.attack < 0 || from.attack > kStateAttackMax ? 0 : from.attack);
    }
    if (host != nullptr) {
        host->SendPoses(static_cast<uint16_t>(raceIndex), poses);
    } else if (client != nullptr) {
        client->SendPoses(static_cast<uint16_t>(raceIndex), poses);
    }
}

// What the room sent goes to the game, which takes it in on its next tick.
void DeliverRacePoses(LobbyHost* host, LobbyClient* client) {
    std::vector<StatePose> poses;
    uint16_t raceIndex = 0;
    if (host != nullptr) {
        host->TakePoses(raceIndex, poses);
    } else if (client != nullptr) {
        client->TakePoses(raceIndex, poses);
    }
    for (const StatePose& from : poses) {
        GdxNetPose to = {};
        to.tick = from.tick;
        to.timeMs = from.timeMs;
        for (size_t k = 0; k < 3; k++) {
            to.pos[k] = from.pos[k];
            to.vel[k] = from.vel[k];
            to.forward[k] = from.forward[k];
            to.up[k] = from.up[k];
        }
        to.lap = from.lap;
        to.flags = from.flags;
        to.attack = from.attack;
        gdx_net_race_deliver(raceIndex, from.slot, &to);
    }
}

// This copy's own tracks, as the course select read them for a ballot or a pick (gdx_net_own_tracks.h). Kept
// from the game thread, read by the worker's store.
struct OwnTracks {
    std::mutex lock;
    std::deque<std::pair<net::track::TrackId, std::vector<uint8_t>>> tracks; // oldest first
};

constexpr size_t kOwnTracksMax = 6;

OwnTracks& Own() {
    static OwnTracks* own = new OwnTracks(); // never destroyed, as the runtime
    return *own;
}

bool LoadOwnTrack(const net::track::TrackId& id, std::vector<uint8_t>& bytes) {
    OwnTracks& own = Own();
    std::lock_guard<std::mutex> guard(own.lock);
    for (const auto& kept : own.tracks) {
        if (kept.first == id) {
            bytes = kept.second;
            return true;
        }
    }
    return false;
}

// The worker. socket is already open; everything else is built here and never leaves this thread.
void Run(Kind kind, std::shared_ptr<UdpSocket> socket, uint16_t port, LocalProfile me, NetAddress hostAddr,
         Seat seat, OnlineSetup online, std::shared_ptr<WorkerCtl> ctl) {
    Runtime& rt = Rt();
    struct MarkDone {
        WorkerCtl& ctl;
        ~MarkDone() {
            ctl.done.store(true);
        }
    } markDone{ *ctl };
    // A view goes out only while this copy is in the room: once the host left, the view is the next session's.
    const Bytes32 ownKey = online.identity;
    const auto publish = [&rt, &ctl, ownKey](View v) {
        std::lock_guard<std::mutex> guard(rt.lock);
        if (!ctl->handOver) {
            RememberMembers(rt, v, ownKey);
            rt.view = std::move(v);
        }
    };
    bool leaving = false; // the host left the room and this worker runs it on in the background (D28.5)
    const DatagramSend send = [socket](const NetAddress& to, const uint8_t* data, size_t len) {
        socket->SendTo(to, data, len);
    };
    const std::string build = BuildId();
    std::unique_ptr<LanBrowser> browser;
    std::unique_ptr<LobbyHost> host;
    std::unique_ptr<LobbyClient> client;
    const uint32_t start = NowMs();

    // This copy's own tracks first, then the downloaded ones, which are read and written on this thread only.
    auto cache = std::make_shared<net::TrackCache>(std::filesystem::path(Ship::Context::GetAppDirectoryPath()) /
                                                   "online" / "cache" / "tracks");
    TrackStore store;
    store.load = [cache](const net::track::TrackId& id, std::vector<uint8_t>& bytes) {
        return LoadOwnTrack(id, bytes) || (cache->ok() && cache->Load(id, bytes));
    };
    if (cache->ok()) {
        store.store = [cache](const net::track::TrackId& id, const std::vector<uint8_t>& bytes) {
            if (!cache->Store(id, bytes)) {
                gdx_port_logf("[lan] a verified track could not be kept in the cache\n");
            }
        };
    }

    // A directory room's listing (O5.10), on whichever copy hosts it: the first host registers a code, a promoted
    // one takes the code over with the secret the old host handed it.
    std::unique_ptr<online::RoomListing> listing;
    online::ListState loggedListState = online::ListState::Off;
    uint32_t listingCode = kNoRoomCode; // the listing's code; the room carries it only while the room is listed
    const auto startListing = [&](RoomType type, uint32_t takeoverCode, const Bytes32& takeoverSecret) {
        listingCode = kNoRoomCode;
        if (type == RoomType::Lan || online.bases.empty() || online.hostKey.empty()) {
            gdx_port_logf("[lan] this %s room is not listed: %s\n", RoomTypeName(type),
                          type == RoomType::Lan       ? "a LAN room"
                          : online.bases.empty()      ? "no room directory is set"
                                                      : "this player has no friend key");
            if (host && type != RoomType::Lan) {
                host->SetRoomCode(kNoRoomCode); // a promoted host keeps the old code, which nobody renews now
            }
            return;
        }
        online::ListOptions options;
        options.bases = online.bases;
        options.stunServers = online.stun;
        options.advertise = online.advertise;
        options.hostKey = online.hostKey;
        options.signer = DirectorySignerFor(online);
        options.type = type;
        options.localPort = socket->LocalPort();
        options.dualStack = socket->DualStack();
        options.takeoverCode = takeoverCode;
        options.takeoverSecret = takeoverSecret;
        listing = std::make_unique<online::RoomListing>(std::move(options), send);
        loggedListState = online::ListState::Off;
    };
    // Decision 51: a member of a directory room gathers on its own socket and tells the host whether it can be
    // reached, so the host elects a reachable successor first. It starts once the room's first `room` names the
    // type, keeps its port mapping through a rejoin, and is released when the member hosts or leaves.
    std::unique_ptr<online::MemberReach> reach;
    bool reachLogged = false;
    const auto releaseReach = [&](uint32_t now) {
        if (reach) {
            reach->Release(now);
            reach.reset();
        }
    };
    // O7.3: every client signs its Join with the profile's friend key, and one that joins a directory room checks
    // the host's key against the one the directory gave. The directory takes a record only when its key signed the
    // write (decision 53), so a host cannot list a room under another player's key.
    // livePaths is ownPaths plus the relay address while this copy has one.
    const PathsInfo ownPaths = OwnPaths(online.directLinks, socket->LocalPort());
    PathsInfo livePaths = ownPaths;
    gdx_port_logf("[lan] direct links %s, %zu LAN addresses to offer\n", online.directLinks ? "on" : "off",
                  ownPaths.lan.size());
    // Decisions 69, 70, 72: a member of a directory room allocates a TURN relay address on its socket, so a member
    // it cannot reach directly still gets its poses by the shorter way. Like reach, it starts once the room's
    // type is known, lasts through a rejoin, and is released when the member hosts or leaves.
    std::unique_ptr<online::MemberRelay> relay;
    std::vector<NetAddress> relayPeers;
    const auto releaseRelay = [&]() {
        if (relay) {
            relay->Release();
            relay.reset();
        }
        relayPeers.clear();
        livePaths = ownPaths;
    };
    const auto makeClient = [&](const NetAddress& addr) {
        auto made = std::make_unique<LobbyClient>(build, me, seat, addr, RandomNonce(), send, store);
        made->SetSigner(SignerFor(online));
        made->AllowLoopback(online.localTest);
        made->SetPaths(livePaths);
        made->SetRelaySend([&relay](const NetAddress& to, const uint8_t* data, size_t len) {
            if (relay) {
                relay->SendTo(to, data, len);
            }
        });
        if (online.expectHost) {
            made->ExpectHostKey(*online.expectHost);
        }
        return made;
    };
    const auto setUpHost = [&](LobbyHost& made) {
        made.SetIdentity(online.identity);
        made.SetFriends(online.friends);
    };
    // Joining by code: the lookup runs first, then each address of the room in turn.
    std::unique_ptr<online::CodeLookup> lookup;
    const std::string joinCode = online::CodeText(online.joinCode);
    size_t candidate = 0;
    if (!socket->DualStack()) {
        online.candidates.erase(std::remove_if(online.candidates.begin(), online.candidates.end(),
                                               [](const NetAddress& a) { return !a.IsV4(); }),
                                online.candidates.end());
    }

    if (kind == Kind::Browse) {
        std::vector<NetAddress> targets = { Addr(0xFFFFFFFFu, port), Addr(0x7F000001u, port) };
        if (port != kDefaultPort) {
            targets.push_back(Addr(0xFFFFFFFFu, kDefaultPort));
            targets.push_back(Addr(0x7F000001u, kDefaultPort));
        }
        browser = std::make_unique<LanBrowser>(targets, RandomNonce(), send);
    } else if (kind == Kind::Host) {
        RoomConfig config;
        config.roomId = RandomNonce(); // seeds the track draws, so two rooms never draw alike
        config.lotteryKey = RandomKey(); // an Open room commits to each lottery with it (decision 39)
        config.type = online.type;
        // Developer switch: GDX_NET_DEV_ROOM_TYPE=open hosts this LAN room as an Open room, so a scripted run
        // exercises the committed lottery without the directory.
        if (const char* type = std::getenv("GDX_NET_DEV_ROOM_TYPE");
            type != nullptr && std::strcmp(type, "open") == 0 && config.type == RoomType::Lan) {
            config.type = RoomType::Open;
            gdx_port_logf("[lan] GDX_NET_DEV_ROOM_TYPE: hosting an Open room\n");
        }
        host = std::make_unique<LobbyHost>(build, me, port, config, send, store);
        setUpHost(*host);
        if (online.type != RoomType::Lan) {
            startListing(online.type, kNoRoomCode, Bytes32{});
        }
    } else if (online.joinCode != kNoRoomCode) {
        lookup = std::make_unique<online::CodeLookup>(online.bases, online.joinCode);
        gdx_port_logf("[lan] asking the room directory for %s\n", joinCode.c_str());
    } else {
        if (online.byRecord && online.candidates.empty()) {
            publish(MakeView(Mode::Closed, "The room has no address this copy can reach.", 0, std::string(),
                             kSystemId));
            socket->Close();
            return;
        }
        if (!online.candidates.empty()) {
            hostAddr = online.candidates[0];
        }
        client = makeClient(hostAddr);
        client->Start(start);
    }

    std::vector<uint8_t> buffer(net::kMaxDatagramBytes);
    uint32_t lineSerial = 0;
    size_t lastLineCount = 0;
    std::string lastLineBack;
    std::string hostText = client ? net::FormatAddress(hostAddr) : std::string();

    // The host is gone (D28.4): the successor hosts the room on this same socket, which is the address the
    // old host announced for it; any other member comes back to the successor. false when there is nothing
    // to follow and the session is over.
    const auto followHandover = [&](uint32_t now) {
        const HandoverPlan plan = client->NextHost();
        if (plan.kind == HandoverPlan::Kind::Promote) {
            // Decision 32: the directory code goes with the room. Read before the client goes.
            const RoomState was = client->Room(now);
            const Bytes32 secret = client->TakeoverSecret();
            std::optional<RoomMachine> machine = RoomMachine::Promote(client->Mirror(now), plan.id, now, RandomKey());
            if (!machine.has_value()) {
                return false;
            }
            port = socket->LocalPort();
            host = std::make_unique<LobbyHost>(build, me, port, std::move(*machine), client->HandoverTable(), plan.id,
                                               send, store);
            setUpHost(*host);
            host->SetMirrorKeys(client->Keys()); // the members' keys, so a ban still finds them
            host->SetBans(client->Bans());       // decision 52: the old host's bans last as long as the room
            releaseReach(now);                   // the listing gathers again, and maps the port for the room
            releaseRelay();                      // a host relays poses itself and has no member links
            gdx_port_logf("[lan] %s is gone; hosting the room on port %u\n", net::host_info::LogText(hostAddr).c_str(),
                          static_cast<unsigned>(port));
            client.reset();
            hostText.clear();
            if (was.type != RoomType::Lan) {
                // No secret: the old host never listed the room, so this one registers it afresh.
                startListing(was.type, IsZero32(secret) ? kNoRoomCode : was.code, secret);
            }
            return true;
        }
        if (plan.kind == HandoverPlan::Kind::Rejoin) {
            // A Rejoin is proved by its token, not a key: the successor's key is not the one the directory gave.
            auto next = std::make_unique<LobbyClient>(build, me, seat, plan.address, RandomNonce(), send, store);
            next->SetSigner(SignerFor(online));
            next->AllowLoopback(online.localTest);
            next->SetPaths(livePaths);
            next->SetRelaySend([&relay](const NetAddress& to, const uint8_t* data, size_t len) {
                if (relay) {
                    relay->SendTo(to, data, len);
                }
            });
            relayPeers.clear(); // the peers stay bound until the new host's Peers name them again
            if (!next->SetRejoin(plan)) {
                return false;
            }
            next->Start(now);
            gdx_port_logf("[lan] %s is gone; going back to the room at %s\n", net::host_info::LogText(hostAddr).c_str(),
                          net::host_info::LogText(plan.address).c_str());
            hostAddr = plan.address;
            hostText = net::FormatAddress(hostAddr);
            client = std::move(next);
            // The old host's other addresses lead to a host that is gone: a rejoin that fails must close with
            // its own reason, not fall back to them.
            online.candidates.clear();
            candidate = 0;
            return true;
        }
        return false;
    };

    // Copies the log into the view and bumps the serial when a line was added. The size alone misses a new
    // line once the log is at its cap, hence the look at the last one.
    const auto takeLines = [&](View& v, const std::deque<ShownLine>& log) {
        if (log.size() != lastLineCount || (!log.empty() && log.back().text != lastLineBack)) {
            lineSerial++;
            lastLineCount = log.size();
            lastLineBack = log.empty() ? std::string() : log.back().text;
        }
        v.lines.assign(log.begin(), log.end());
        v.lineSerial = lineSerial;
    };
    RaceUpload upload;
    bool memberRacing = false;            // the race phase as LogRacePaths last saw it
    uint32_t loggedLottery = 0xFFFFFFFFu; // the race whose lottery a member last logged

    while (!ctl->stop.load()) {
        const uint32_t now = NowMs();
        if (lookup) {
            if (!lookup->Done()) {
                View v = MakeView(Mode::Joining, std::string(), 0, std::string(), kSystemId);
                v.joinCode = joinCode;
                publish(std::move(v));
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                continue;
            }
            const auto answer = lookup->Result();
            lookup.reset();
            std::string closed;
            if (answer.error == net::directory::Error::NotFound) {
                closed = "No room has the code " + joinCode + ".";
            } else if (answer.error != net::directory::Error::None || !answer.value) {
                closed = answer.error == net::directory::Error::Unreachable
                             ? "The room directory did not answer. Try again in a moment."
                             : std::string("The room directory refused the code: ") +
                                   net::directory::ErrorName(answer.error) + ".";
            } else {
                online.candidates = online::JoinAddresses(*answer.value, socket->DualStack());
                online.expectHost = RecordHostKey(*answer.value);
                if (online.candidates.empty()) {
                    closed = "The room " + joinCode + " has no address this copy can reach.";
                }
            }
            if (!closed.empty()) {
                gdx_port_logf("[lan] join by code: %s\n", closed.c_str());
                View v = MakeView(Mode::Closed, closed, 0, std::string(), kSystemId);
                v.joinCode = joinCode;
                publish(std::move(v));
                socket->Close();
                return;
            }
            gdx_port_logf("[lan] %s has %zu address(es); trying %s\n", joinCode.c_str(), online.candidates.size(),
                          net::host_info::LogText(online.candidates[0]).c_str());
            hostAddr = online.candidates[0];
            hostText = net::FormatAddress(hostAddr);
            client = makeClient(hostAddr);
            client->Start(now);
        }
        if (!leaving) {
            bool handOver = false;
            for (const Command& cmd : TakeCommands(*ctl, handOver)) {
                if (host) {
                    RunHostCommand(*host, cmd, now);
                } else if (client) {
                    RunClientCommand(*client, cmd, now);
                }
            }
            if (handOver && !host) {
                break; // only a host hands a room over (gdx_lan_room_hand_over)
            }
            if (handOver) {
                leaving = true;
                host->Leave(now);
                gdx_port_logf("[lan] left the room; it goes on until it is handed over\n");
            }
        }
        if (!leaving) {
            SendRacePoses(host.get(), client.get());
        }
        for (int i = 0; i < kMaxReadsPerTick; i++) {
            size_t len = 0;
            NetAddress from;
            const RecvResult got = socket->RecvFrom(buffer.data(), buffer.size(), len, from);
            if (got == RecvResult::WouldBlock || got == RecvResult::Error) {
                break;
            }
            if (got != RecvResult::Ok) {
                continue; // a truncated datagram is never ours: ours fit kMaxDatagramBytes
            }
            if (listing && listing->Receive(from, buffer.data(), len, now)) {
                continue; // a STUN or port-mapping answer, never a room peer
            }
            if (reach && reach->Receive(from, buffer.data(), len, now)) {
                continue;
            }
            // After listing and reach: the STUN and TURN servers may share an address, and each takes only the
            // answers to its own requests.
            if (relay && relay->Receive(from, buffer.data(), len, now)) {
                continue;
            }
            if (browser) {
                browser->Receive(from, buffer.data(), len, now);
            } else if (host) {
                host->Receive(from, buffer.data(), len, now);
            } else if (client) {
                client->Receive(from, buffer.data(), len, now);
            }
        }

        if (host) {
            host->Update(now);
        } else if (client) {
            client->Update(now);
        }
        if (client && !host) {
            if (!reach && client->GetState() == LobbyClient::State::Joined &&
                client->Room(now).type != RoomType::Lan) {
                online::ReachOptions options;
                options.stunServers = online.stun;
                options.advertise = online.advertise;
                options.localPort = socket->LocalPort();
                options.dualStack = socket->DualStack();
                reach = std::make_unique<online::MemberReach>(std::move(options), send);
            }
            if (reach) {
                reach->Update(now);
                if (reach->Done()) {
                    client->SetReachable(reach->Reachable());
                    if (!reachLogged) {
                        reachLogged = true;
                        gdx_port_logf("[lan] reachable from outside: %s\n", reach->Reachable() ? "yes" : "no");
                    }
                }
            }
            if (!relay && !online.bases.empty() && client->GetState() == LobbyClient::State::Joined &&
                client->Room(now).type != RoomType::Lan) {
                online::RelayOptions options;
                options.bases = online.bases;
                relay = std::make_unique<online::MemberRelay>(
                    std::move(options), send,
                    [&client](const NetAddress& peer, const uint8_t* data, size_t len, uint32_t at) {
                        if (client) {
                            client->ReceiveRelay(peer, data, len, at);
                        }
                    });
            }
            if (relay) {
                relay->Update(now);
                const std::optional<NetAddress> relayed = relay->Relayed();
                const PeerEndpoint want = relayed ? net::links::EndpointOf(*relayed) : PeerEndpoint();
                if (!(want == livePaths.relay)) {
                    livePaths.relay = want;
                    client->SetPaths(livePaths);
                    gdx_port_logf("[lan] relay address %s\n", relayed ? "offered to the room" : "withdrawn");
                }
                std::vector<NetAddress> peers = client->Links().RelayAddresses();
                if (peers != relayPeers) {
                    relay->SetPeers(peers, now);
                    relayPeers = std::move(peers);
                }
            }
        }
        if (host && listing) {
            if (host->TakeSecretExposed()) {
                listing->RotateSecret(now);
            }
            listing->Update(now, online::SeatsOf(host->Room(now), host->Roster()));
            uint32_t code = kNoRoomCode;
            Bytes32 secret{};
            if (listing->TakeChange(code, secret)) {
                listingCode = code;
                host->SetTakeoverSecret(secret);
            }
            const online::ListView lv = listing->View();
            // A code the directory has not accepted yet, or no longer keeps, is not shown to anyone.
            const uint32_t shown = lv.state == online::ListState::Listed ? listingCode : kNoRoomCode;
            if (host->Room(now).code != shown) {
                host->SetRoomCode(shown);
            }
            if (lv.state != loggedListState) {
                loggedListState = lv.state;
                const bool listed = lv.state == online::ListState::Listed;
                gdx_port_logf("[lan] room listing: %s%s%s\n", lv.text.c_str(), listed ? " as " : "",
                              listed ? online::CodeText(host->Room(now).code).c_str() : "");
                if (listed || lv.state == online::ListState::Failed) {
                    gdx_port_logf("[lan] reachable %s, port mapped %s, public address %s\n",
                                  lv.reachable ? "yes" : "no", lv.mapped ? "yes" : "no",
                                  lv.publicAddressLog.empty() ? "unknown" : lv.publicAddressLog.c_str());
                }
            }
        }
        if (!leaving) {
            DeliverRacePoses(host.get(), client.get());
        }

        if (browser) {
            browser->Update(now);
            View v = MakeView(Mode::Browsing, std::string(), port, std::string(), kSystemId);
            v.rooms = browser->Rooms();
            publish(std::move(v));
        } else if (host && leaving) {
            if (host->Closed()) {
                gdx_port_logf("[lan] %s\n", host->CloseText().c_str());
                if (listing) {
                    if (!host->HandedOver()) {
                        listing->Delist(); // nobody was left to host it
                    }
                    listing->Release(now);
                }
                socket->Close();
                return;
            }
            std::string next;
            for (const PlayerInfo& player : host->Roster()) {
                if (player.id == host->Successor()) {
                    next = player.name;
                }
            }
            std::lock_guard<std::mutex> guard(rt.lock);
            ctl->nextName = next;
        } else if (host) {
            if (host->Closed()) {
                // A promoted host nobody came back to.
                gdx_port_logf("[lan] closed the room: %s\n", host->CloseText().c_str());
                publish(MakeView(Mode::Closed, host->CloseText(), port, std::string(), kSystemId));
                if (listing) {
                    listing->Delist();
                    listing->Release(now);
                }
                socket->Close();
                return;
            }
            View v = MakeView(Mode::Hosting, std::string(), port, std::string(), kHostId);
            v.roster = host->Roster();
            v.room = host->Room(now);
            LogRaceUpload(*host, v.room.phase == Phase::Racing, v.roster, now, upload);
            v.votes = host->Votes();
            v.lottery = host->Lottery();
            v.picked = host->LastPicked();
            v.grid = host->RaceGrid();
            v.haveGrid = !v.grid.humans.empty();
            v.haveStart = host->StartSent(v.start);
            v.results = host->Results();
            v.haveResults = v.results.final || !v.results.entries.empty();
            v.trackProgress = host->Track();
            v.raceTrack = host->RaceTrack();
            v.clockReady = true; // the room runs on this copy's clock
            if (listing) {
                v.listing = listing->View();
            }
            v.keys = host->Keys();
            takeLines(v, host->Lines());
            publish(std::move(v));
        } else if (client) {
            const LobbyClient::State state = client->GetState();
            if (state == LobbyClient::State::Closed && followHandover(now)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                continue;
            }
            if (state == LobbyClient::State::Closed && !client->EverConnected() &&
                candidate + 1 < online.candidates.size()) {
                // The room never answered here: try its next address.
                candidate++;
                gdx_port_logf("[lan] no answer at %s; trying %s\n", net::host_info::LogText(hostAddr).c_str(),
                              net::host_info::LogText(online.candidates[candidate]).c_str());
                hostAddr = online.candidates[candidate];
                hostText = net::FormatAddress(hostAddr);
                client = makeClient(hostAddr);
                client->Start(now);
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                continue;
            }
            if (state == LobbyClient::State::Closed) {
                gdx_port_logf("[lan] left %s: %s\n", net::host_info::LogText(hostAddr).c_str(),
                              client->CloseText().c_str());
                View closedView = MakeView(Mode::Closed, client->CloseText(), hostAddr.port, hostText, kSystemId);
                closedView.joinCode = joinCode;
                publish(std::move(closedView));
                releaseReach(now);
                releaseRelay();
                socket->Close();
                return;
            }
            View v = MakeView(state == LobbyClient::State::Joined ? Mode::Joined : Mode::Joining, std::string(),
                              hostAddr.port, hostText, client->LocalId());
            v.joinCode = joinCode;
            v.roster = client->Roster();
            v.room = client->Room(now);
            v.votes = client->Votes();
            v.lottery = client->Lottery();
            v.lotteryChecked = client->LotteryChecked();
            LogRacePaths(*client, v.room.phase == Phase::Racing, memberRacing);
            if (v.room.phase == Phase::Lottery && loggedLottery != v.room.raceIndex) {
                loggedLottery = v.room.raceIndex;
                gdx_port_logf("[lan] race %u lottery: %zu ballots, %s, %s\n", static_cast<unsigned>(v.room.raceIndex),
                              v.lottery.ballots.size(),
                              IsZero32(v.lottery.salt) ? "drawn from public values" : "salt revealed",
                              v.lotteryChecked ? "checked" : "CHECK FAILED");
            }
            v.picked = client->LastPicked();
            v.haveGrid = client->RaceGrid(v.grid);
            v.haveStart = client->StartSent(v.start);
            v.haveResults = client->Results(v.results);
            v.trackProgress = client->Track();
            v.raceTrack = client->RaceTrack();
            v.clockReady = client->Clock().Ready();
            v.clockOffsetMs = client->Clock().OffsetMs();
            v.keys = client->Keys();
            takeLines(v, client->Lines());
            publish(std::move(v));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    }

    const uint32_t now = NowMs();
    releaseReach(now);
    releaseRelay();
    if (listing) {
        // A host that closes the room ends it, so its record goes too. A silent stop or a handover leaves the record
        // for the successor to take over; it runs out in 180 s otherwise.
        if (host && !ctl->silent.load() && !host->HandedOver()) {
            listing->Delist();
        }
        listing->Release(now);
    }
    if (host) {
        if (!ctl->silent.load()) {
            host->Close(now);
        }
    } else if (client) {
        client->Leave(now);
    }
    socket->Close();
}

void Push(Command cmd) {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    if (rt.queue.size() < 64) {
        rt.queue.push_back(std::move(cmd));
    }
}

void PushType(Command::Type type, uint8_t value = 0, bool flag = false) {
    Command cmd;
    cmd.type = type;
    cmd.value = value;
    cmd.flag = flag;
    Push(std::move(cmd));
}

// The ROM hack latched at boot: its name and the SHA-256 of its archive, read once. The stock game is an
// empty name and a zero hash. False when the archive cannot be read or its name cannot travel.
bool LocalHack(HackIdentity& out) {
    static std::mutex lock;
    static bool done = false;
    static bool ok = false;
    static HackIdentity cached;
    std::lock_guard<std::mutex> guard(lock);
    if (!done) {
        done = true;
        cached = HackIdentity();
        cached.name = GdxHackModsActiveName();
        if (cached.name.empty()) {
            ok = true;
        } else {
            const std::string path = GdxHackModsLatchActivePath();
            gdx::net::track::TrackId hash{};
            ok = !path.empty() && gdx::net::track::FromHex(GdxExtractFileSha256(path.c_str()), hash) &&
                 HackOk(cached);
            std::copy(hash.begin(), hash.end(), cached.hash.begin());
        }
    }
    out = cached;
    return ok;
}

bool Launch(Kind kind, uint16_t bindPort, uint16_t port, const LocalProfile& me, const NetAddress& hostAddr,
            Seat seat, const OnlineSetup& online, std::string& error) {
    StopWorker();
    // Decision 47: a ROM hack changes courses and machines under the same names, so a room races one
    // game. The join carries the hack this copy runs, and the host lets in only the same hack.
    LocalProfile local = me;
    if (kind != Kind::Browse && !LocalHack(local.hack)) {
        error = "The ROM hack in use could not be read, so a room cannot check that everyone runs it. Select "
                "it again in Workshop - ROM Hacks and restart.";
        gdx_port_logf("[lan] %s refused: ROM hack %s has no identity\n", kind == Kind::Host ? "host" : "join",
                      GdxHackModsActiveName().c_str());
        Publish(MakeView(Mode::Closed, error, port, std::string(), kSystemId));
        return false;
    }
    auto socket = std::make_shared<UdpSocket>();
    // Browsing broadcasts, which only IPv4 has. A host or a member opens one dual-stack socket (O5.3), so an
    // IPv6 member or successor is reached on the same port; the socket falls back to IPv4 where the OS has no
    // IPv6.
    NetAddress bindAddr = Addr(0, bindPort);
    if (kind != Kind::Browse) {
        bindAddr = NetAddress();
        bindAddr.port = bindPort;
    }
    if (!socket->Open(bindAddr)) {
        const bool leaverRuns = Rt().leaver.joinable() && !Rt().leaverCtl->done.load();
        error = bindPort != 0 && leaverRuns
                    ? "Your last room still runs on port " + std::to_string(bindPort) +
                          " until its race is over. Host again after that, or pick another port."
                : bindPort != 0 ? "Port " + std::to_string(bindPort) +
                                      " is in use, maybe by another copy of G-Diffuser. Pick another port."
                                : "Could not open a network socket.";
        Publish(MakeView(Mode::Closed, error, port, std::string(), kSystemId));
        return false;
    }
    if (kind == Kind::Browse && !socket->SetBroadcast(true)) {
        gdx_port_logf("[lan] broadcast refused; searching this computer only\n");
    }
    if (kind != Kind::Browse) {
        gdx_port_logf("[lan] socket is %s\n", socket->DualStack() ? "dual-stack" : "IPv4 only");
    }
    gdx_port_logf("[lan] %s on port %u\n", kind == Kind::Browse ? "browse" : kind == Kind::Host ? "host" : "join",
                  static_cast<unsigned>(socket->LocalPort()));
    const bool byAddress = kind == Kind::Join && online.joinCode == kNoRoomCode && online.candidates.empty();
    View first = MakeView(kind == Kind::Browse ? Mode::Browsing : kind == Kind::Host ? Mode::Hosting : Mode::Joining,
                          std::string(), port, byAddress ? net::FormatAddress(hostAddr) : std::string(),
                          kind == Kind::Host ? kHostId : kSystemId);
    first.joinCode = online::CodeText(online.joinCode);
    Publish(std::move(first));
    if (kind != Kind::Browse) {
        std::lock_guard<std::mutex> guard(Rt().lock);
        Rt().lastRoomStale = true;
    }
    Rt().worker = std::thread(Run, kind, socket, port, local, hostAddr, seat, online, Rt().ctl);
    return true;
}

} // namespace

std::string BuildId() {
    std::string id = GDX_VERSION_STRING;
    const std::string commit = GDX_BUILD_COMMIT;
    if (!commit.empty()) {
        id += "+" + commit;
    }
    if (id.size() > kBuildMax) {
        id.resize(kBuildMax);
    }
    return id;
}

namespace {

// The directory settings and the host key, for a session that may list a room: a host of any type but Lan, and
// a member who may be promoted to host a directory room. needKey makes the profile's seed when it has none; a
// LAN session only uses a seed that is already there.
OnlineSetup ReadOnlineSetup(bool needKey) {
    gdx_course_boot(); // the key lives in the profile stores, read on the process's first call that needs one
    OnlineSetup setup;
    setup.bases = online::DirectoryBases();
    setup.stun = online::StunServers();
    setup.advertise = online::AdvertiseAddress();
    setup.localTest = online::LocalTestDirectory();
    setup.directLinks = online::DirectLinks();
    if (needKey || gdx_profile_state_active_has_seed() != 0) {
        auto seed = std::make_shared<JoinSeed>();
        if (online::ActiveSeed(seed->bytes) == GDX_PROFILE_OK && online::HostKey(setup.hostKey) &&
            online::HostKeyBytes(setup.identity)) {
            setup.seed = std::move(seed);
            setup.friends = online::FriendKeys();
        } else {
            setup.hostKey.clear();
            setup.identity = Bytes32{};
        }
    }
    return setup;
}

} // namespace

void StartBrowse(uint16_t port) {
    std::string error;
    Launch(Kind::Browse, 0, port, LocalProfile(), NetAddress(), Seat::Race, OnlineSetup(), error);
}

bool StartHost(uint16_t port, const LocalProfile& me, std::string& error) {
    return StartHost(port, me, RoomType::Lan, error);
}

bool StartHost(uint16_t port, const LocalProfile& me, RoomType type, std::string& error) {
    OnlineSetup setup = ReadOnlineSetup(type != RoomType::Lan);
    setup.type = type;
    if (type != RoomType::Lan && setup.bases.empty()) {
        error = "No room directory is set, so a room can only be hosted for this network.";
        Publish(MakeView(Mode::Closed, error, port, std::string(), kSystemId));
        return false;
    }
    if (type != RoomType::Lan && setup.hostKey.empty()) {
        error = "Pick a player profile first: the room directory lists a room under its host's key.";
        Publish(MakeView(Mode::Closed, error, port, std::string(), kSystemId));
        return false;
    }
    return Launch(Kind::Host, port, port, me, NetAddress(), Seat::Race, setup, error);
}

void StartJoin(const NetAddress& host, const LocalProfile& me, Seat seat) {
    std::string error;
    Launch(Kind::Join, 0, host.port, me, host, seat, ReadOnlineSetup(false), error);
}

void StartJoinCode(uint32_t code, const LocalProfile& me, Seat seat) {
    OnlineSetup setup = ReadOnlineSetup(true); // a member may be promoted and take the code over
    if (setup.bases.empty()) {
        StopWorker();
        Publish(MakeView(Mode::Closed, "No room directory is set, so a code cannot be looked up.", 0, std::string(),
                         kSystemId));
        return;
    }
    setup.joinCode = code;
    std::string error;
    Launch(Kind::Join, 0, 0, me, NetAddress(), seat, setup, error);
}

void StartJoinRecord(const net::directory::Record& record, const LocalProfile& me, Seat seat) {
    OnlineSetup setup = ReadOnlineSetup(true);
    setup.candidates = online::JoinAddresses(record, true); // the worker drops IPv6 on an IPv4-only socket
    setup.byRecord = true;
    setup.expectHost = RecordHostKey(record);
    std::string error;
    const uint16_t port = setup.candidates.empty() ? 0 : setup.candidates[0].port;
    Launch(Kind::Join, 0, port, me, NetAddress(), seat, setup, error);
}

void SetReady(bool ready) {
    PushType(Command::Type::Ready, 0, ready);
}

void Vote(uint8_t track) {
    PushType(Command::Type::Vote, track);
}

void Pick(uint8_t track) {
    PushType(Command::Type::Pick, track);
}

void VoteCustom(const CustomTrack& track) {
    Command cmd;
    cmd.type = Command::Type::VoteCustom;
    cmd.custom = track;
    Push(std::move(cmd));
}

void PickCustom(const CustomTrack& track) {
    Command cmd;
    cmd.type = Command::Type::PickCustom;
    cmd.custom = track;
    Push(std::move(cmd));
}

void KeepOwnTrack(const net::track::TrackId& id, std::vector<uint8_t> bytes) {
    OwnTracks& own = Own();
    std::lock_guard<std::mutex> guard(own.lock);
    for (auto it = own.tracks.begin(); it != own.tracks.end(); ++it) {
        if (it->first == id) {
            own.tracks.erase(it);
            break;
        }
    }
    own.tracks.emplace_back(id, std::move(bytes));
    while (own.tracks.size() > kOwnTracksMax) {
        own.tracks.pop_front();
    }
}

void Lock(const LockRequest& lock) {
    Command cmd;
    cmd.type = Command::Type::Lock;
    cmd.lock = lock;
    Push(std::move(cmd));
}

void Emote(uint8_t emote) {
    PushType(Command::Type::Emote, emote);
}

void RaceNext(bool raceNext) {
    PushType(Command::Type::RaceNext, 0, raceNext);
}

void StartSeries() {
    PushType(Command::Type::StartSeries);
}

void SkipLottery() {
    PushType(Command::Type::SkipLottery);
}

void EndSeries() {
    PushType(Command::Type::EndSeries);
}

void SetRacerCap(uint8_t cap) {
    PushType(Command::Type::RacerCap, cap);
}

void SetCustomTracks(bool on) {
    PushType(Command::Type::CustomTracks, 0, on);
}

void SetCpuFill(bool on) {
    PushType(Command::Type::CpuFill, 0, on);
}

void Kick(uint8_t id) {
    PushType(Command::Type::Kick, id);
}

void Ban(uint8_t id) {
    PushType(Command::Type::Ban, id);
}

std::vector<RoomMember> LastRoom() {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    return rt.lastRoom;
}

namespace {

// Each mute keeps who held the id when it was set, so a handover that gives the id to someone else (the successor
// becomes id 0) ends the mute instead of moving it.
struct MuteBook {
    std::bitset<256> on;
    std::array<std::string, 256> who; // name and key; empty until the next view fills it in
};

MuteBook& Mutes() {
    static MuteBook mutes;
    return mutes;
}

std::string Identity(const View& view, const PlayerInfo& player) {
    std::string out = player.name;
    out.push_back('\n');
    for (const net::lobby::MemberKey& member : view.keys) {
        if (member.id == player.id) {
            out.append(reinterpret_cast<const char*>(member.key.data()), member.key.size());
        }
    }
    return out;
}

} // namespace

bool Muted(uint8_t id) {
    return Mutes().on.test(id);
}

void SetMuted(uint8_t id, bool muted) {
    MuteBook& book = Mutes();
    book.on.set(id, muted);
    book.who[id].clear();
}

void ForgetMutesOutside(const View& view) {
    MuteBook& book = Mutes();
    std::bitset<256> kept;
    for (const PlayerInfo& player : view.roster) {
        if (!book.on.test(player.id)) {
            continue;
        }
        const std::string who = Identity(view, player);
        if (book.who[player.id].empty()) {
            book.who[player.id] = who;
        }
        if (book.who[player.id] == who && player.id != view.localId) {
            kept.set(player.id);
        }
    }
    book.on = kept;
}

void Stop() {
    StopWorker();
    Publish(View());
}

void Dismiss() {
    Stop();
}

View Snapshot() {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    return rt.view;
}

void Read(const std::function<void(const View&)>& read) {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    read(rt.view);
}

bool ActiveProfile(LocalProfile& out) {
    // The profiles are read with the other stores on the first call of the process that needs one, and the
    // room screen can be the first.
    gdx_course_boot();
    const GdxProfileStore* store = gdx_profile_state_store();
    if (store->count <= 0 || store->active < 0 || store->active >= store->count) {
        return false;
    }
    const GdxProfile& p = store->profiles[store->active];
    out.name = p.name;
    out.colour = p.colour & 0xFFFFFFu;
    out.machine = (p.preferredMachine >= 0 && p.preferredMachine < GDX_PROFILE_ROSTER_COUNT)
                      ? static_cast<uint8_t>(p.preferredMachine)
                      : static_cast<uint8_t>(0xFF);
    return NameOk(out.name);
}

uint16_t PortSetting() {
    const int port = CVarGetInteger(kPortCvar, kDefaultPort);
    return static_cast<uint16_t>((port >= 1024 && port <= 65535) ? port : kDefaultPort);
}

void SavePortSetting(uint16_t port) {
    if (port >= 1024) {
        CVarSetInteger(kPortCvar, port);
        CVarSave();
    }
}

// The game is closing: a room this copy hosts, or one it left that still runs here, goes as in a crash, so the
// members carry on under the next host (decision 17).
void Shutdown() {
    StopWorker(true);
    ReapLeaver(true);
}

void DevFrame() {
    enum class Dev : uint8_t { Unread, Off, Host, Join };
    static Dev dev = Dev::Unread;
    static uint16_t port = 0;
    static uint32_t frame = 0;
    static bool hosted = false;
    static uint32_t tableLogged = 0; // the race index after the last table written to the log
    // GDX_NET_DEV_CODE_FILE=<path>: the host hosts a Code room and writes its code there once listed; the
    // joiner waits for the file and joins by that code, through the directory (O5.10).
    static std::string codeFile;
    static bool codeWritten = false;

    if (dev == Dev::Unread) {
        dev = Dev::Off;
        const char* env = std::getenv("GDX_NET_DEV");
        if (env != nullptr) {
            const bool host = std::strncmp(env, "host:", 5) == 0;
            const bool join = std::strncmp(env, "join:", 5) == 0;
            const long value = (host || join) ? std::strtol(env + 5, nullptr, 10) : 0;
            if (value >= 1024 && value <= 65535) {
                port = static_cast<uint16_t>(value);
                dev = host ? Dev::Host : Dev::Join;
                gdx_port_logf("[lan] GDX_NET_DEV: %s on port %u\n", host ? "host" : "join", static_cast<unsigned>(port));
                if (const char* file = std::getenv("GDX_NET_DEV_CODE_FILE"); file != nullptr && file[0] != 0) {
                    codeFile = file;
                    gdx_port_logf("[lan] GDX_NET_DEV_CODE_FILE: a Code room, its code in %s\n", file);
                }
            }
        }
    }
    if (dev == Dev::Off || (frame++ % 30) != 0) {
        return; // twice a second is plenty
    }

    LocalProfile me;
    me.name = dev == Dev::Host ? "DEV HOST" : "DEV JOIN";
    me.colour = dev == Dev::Host ? 0x3060FFu : 0xFF6030u;
    me.machine = dev == Dev::Host ? 0 : 5;

    const View view = Snapshot();
    if (view.haveResults && view.results.final && view.results.raceIndex + 1u > tableLogged) {
        tableLogged = view.results.raceIndex + 1u;
        gdx_port_logf("[lan] race %u results, %u rows\n", static_cast<unsigned>(view.results.raceIndex),
                      static_cast<unsigned>(view.results.entries.size()));
        for (size_t place = 0; place < view.results.entries.size(); place++) {
            const ResultEntry& entry = view.results.entries[place];
            unsigned series = 0;
            for (const PlayerInfo& player : view.roster) {
                series = player.id == entry.id ? player.points : series;
            }
            gdx_port_logf("[lan]   place %u: player %u kind %u time %u ms, %u points, series %u\n",
                          static_cast<unsigned>(place + 1), static_cast<unsigned>(entry.id),
                          static_cast<unsigned>(entry.kind), static_cast<unsigned>(entry.timeMs),
                          static_cast<unsigned>(entry.points), series);
        }
    }
    if (dev == Dev::Host && !hosted) {
        LocalProfile active;
        if (!codeFile.empty() && !ActiveProfile(active)) {
            return; // a Code room is listed under the active profile's key: wait until a profile is there
        }
        std::string error;
        hosted = true; // one try: a taken port is said in the log, not retried
        if (!StartHost(port, me, codeFile.empty() ? RoomType::Lan : RoomType::Code, error)) {
            gdx_port_logf("[lan] GDX_NET_DEV: %s\n", error.c_str());
        }
        return;
    }
    if (dev == Dev::Host && !codeFile.empty() && !codeWritten && view.mode == Mode::Hosting &&
        view.room.code != kNoRoomCode) {
        codeWritten = true;
        const std::string text = online::CodeText(view.room.code);
        if (FILE* f = std::fopen(codeFile.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
        gdx_port_logf("[lan] GDX_NET_DEV_CODE_FILE: wrote %s\n", text.c_str());
    }
    if (dev == Dev::Join && !codeFile.empty() && (view.mode == Mode::Idle || view.mode == Mode::Closed)) {
        char text[16] = {};
        if (FILE* f = std::fopen(codeFile.c_str(), "rb")) {
            const size_t n = std::fread(text, 1, sizeof(text) - 1, f);
            std::fclose(f);
            text[n] = 0;
        }
        uint32_t code = 0;
        if (net::codes::ParseRoomCode(text, code)) {
            gdx_port_logf("[lan] GDX_NET_DEV: joining by code %s\n", text);
            StartJoinCode(code, me, Seat::Race);
        }
        return;
    }
    if (dev == Dev::Join && (view.mode == Mode::Idle || view.mode == Mode::Closed)) {
        StartJoin(Addr(0x7F000001u, port), me, Seat::Race); // again until the host is up
        return;
    }
    if ((view.mode != Mode::Hosting && view.mode != Mode::Joined) || view.room.phase != Phase::Lobby) {
        return;
    }
    for (const PlayerInfo& player : view.roster) {
        if (player.id == view.localId && !player.ready) {
            SetReady(true);
        }
    }
}

} // namespace gdx::lan

// The lock-in screen's view of the room (gdx_net_machine_select.h). The game thread calls these every tick
// while the screen is up, so they copy a few numbers under the lock and never the whole view.
extern "C" void gdx_lan_lock_in_room(GdxNetLockInRoom* out) {
    using namespace gdx::lan;
    *out = GdxNetLockInRoom{};
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if (view.mode != Mode::Hosting && view.mode != Mode::Joined) {
        return;
    }
    out->inRoom = 1;
    out->phaseMachine = view.room.phase == gdx::net::lobby::Phase::Machine ? 1 : 0;
    out->raceIndex = view.room.raceIndex;
    out->leftMs = view.room.phaseLeftMs;
    out->lockedCount = view.room.locked;
    out->racers = view.room.racers;
    out->track = view.room.track;
    if (view.room.track == gdx::net::lobby::kTrackCustom) {
        const std::string& title = view.room.custom.title;
        const size_t length = (std::min)(title.size(), sizeof(out->title) - 1);
        std::copy(title.begin(), title.begin() + static_cast<std::ptrdiff_t>(length), out->title);
    }
    for (const gdx::net::lobby::PlayerInfo& player : view.roster) {
        if (player.id == view.localId) {
            out->entrant = (player.inRace && !player.watcher) ? 1 : 0;
            out->locked = player.locked ? 1 : 0;
            break;
        }
    }
}

extern "C" void gdx_lan_lock_in_send(int machine, int skin, int enginePercent) {
    gdx::net::lobby::LockRequest lock;
    lock.machine = static_cast<uint8_t>(machine);
    lock.skin = static_cast<uint8_t>(skin);
    lock.engine = static_cast<uint8_t>(enginePercent);
    // An edited machine travels as its record, so every copy races what this player built (decision 10).
    // A record the host would refuse is left home: the player races the stock machine instead.
    if (gdx_net_race_local_record(machine, lock.record.data()) != 0) {
        lock.hasRecord = gdx::net::lobby::MachineRecordOk(lock.record);
        if (!lock.hasRecord) {
            gdx_port_logf("[lan] machine %d: its Create Machine record is out of range, racing stock\n", machine);
            lock.record = gdx::net::lobby::MachineRecord{};
        }
    }
    gdx::lan::Lock(lock);
}

// ---- The room, as an online race sees it (gdx_net_race_room.h). Game thread. ----

extern "C" int CVarGetInteger(const char* name, int defaultValue);     // libultraship consolevariablebridge.h
extern "C" float CVarGetFloat(const char* name, float defaultValue); // libultraship consolevariablebridge.h

extern "C" void gdx_lan_race_room(GdxNetRaceRoom* out) {
    using namespace gdx::lan;
    using namespace gdx::net::lobby;
    *out = GdxNetRaceRoom{};
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if (view.mode != Mode::Hosting && view.mode != Mode::Joined) {
        return;
    }
    out->inRoom = 1;
    out->host = view.mode == Mode::Hosting ? 1 : 0;
    out->phase = view.room.phase == Phase::GridSync    ? GDX_NET_RACE_PHASE_LOADING
                 : view.room.phase == Phase::Countdown ? GDX_NET_RACE_PHASE_COUNTDOWN
                 : view.room.phase == Phase::Racing    ? GDX_NET_RACE_PHASE_RACING
                                                       : GDX_NET_RACE_PHASE_NONE;
    out->raceIndex = view.room.raceIndex;
    out->track = view.room.track;
    out->cpusOut = view.room.cpusOut ? 1 : 0;
    for (const PlayerInfo& player : view.roster) {
        if (player.id == view.localId) {
            out->onGrid = (player.inRace && !player.watcher) ? 1 : 0;
            break;
        }
    }

    const Grid& grid = view.grid;
    if (!view.haveGrid || grid.raceIndex != view.room.raceIndex || grid.humans.empty() ||
        grid.humans.size() > GDX_NET_RACE_HUMANS) {
        return;
    }
    int self = -1;
    for (size_t i = 0; i < grid.humans.size(); i++) {
        out->machine[i] = grid.humans[i].machine;
        out->skin[i] = grid.humans[i].skin;
        out->hasRecord[i] = grid.humans[i].hasRecord ? 1 : 0;
        std::copy(grid.humans[i].record.begin(), grid.humans[i].record.end(), out->record[i]);
        if (grid.humans[i].id == view.localId) {
            self = static_cast<int>(i);
        }
    }
    if (self < 0) {
        return; // a watcher: the grid is someone else's race
    }
    out->haveGrid = 1;
    out->seed = grid.seed;
    out->humans = static_cast<int>(grid.humans.size());
    out->cpus = grid.cpus;
    out->self = self;
    out->laps = grid.rules.laps;
    out->ignoreTrackSettings = grid.rules.ignoreTrackSettings ? 1 : 0;
    out->lavaNoKill = grid.rules.lavaNoKill ? 1 : 0;
    out->magnetRange = grid.rules.magnetRange;
    out->magnetStrength = grid.rules.magnetStrength;
    out->boostFrames = static_cast<int>(grid.rules.boostFrames);
}

extern "C" int gdx_lan_race_track(unsigned char* id, unsigned char* out, unsigned long capacity, unsigned long* length) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if ((view.mode != Mode::Hosting && view.mode != Mode::Joined) ||
        view.room.track != gdx::net::lobby::kTrackCustom || !view.raceTrack || !view.raceTrack->Complete() ||
        view.raceTrack->Id() != view.room.custom.id || view.raceTrack->Size() > capacity) {
        return 0;
    }
    std::copy(view.raceTrack->Id().begin(), view.raceTrack->Id().end(), id);
    std::copy(view.raceTrack->Bytes(), view.raceTrack->Bytes() + view.raceTrack->Size(), out);
    *length = static_cast<unsigned long>(view.raceTrack->Size());
    return 1;
}

extern "C" int gdx_lan_race_elapsed(unsigned raceIndex, int* elapsedMs) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if ((view.mode != Mode::Hosting && view.mode != Mode::Joined) || !view.haveStart || !view.clockReady ||
        view.start.raceIndex != raceIndex) {
        return 0;
    }
    // Read now, not at the worker's last tick: that one is up to 10 ms old.
    *elapsedMs = static_cast<int>(static_cast<int32_t>(NowMs() + view.clockOffsetMs - view.start.startAtMs));
    return 1;
}

extern "C" int gdx_lan_race_clock(unsigned* hostMs) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if ((view.mode != Mode::Hosting && view.mode != Mode::Joined) || !view.clockReady) {
        return 0;
    }
    *hostMs = NowMs() + view.clockOffsetMs; // read now, as gdx_lan_race_elapsed does
    return 1;
}

extern "C" void gdx_lan_race_ready(unsigned raceIndex, const unsigned char* digest, unsigned loadMs) {
    using namespace gdx::lan;
    Command cmd;
    cmd.type = Command::Type::GridReady;
    cmd.ready.raceIndex = static_cast<uint16_t>(raceIndex);
    std::memcpy(cmd.ready.digest.data(), digest, cmd.ready.digest.size());
    cmd.ready.loadMs = static_cast<uint16_t>(loadMs > 0xFFFFu ? 0xFFFFu : loadMs);
    Push(std::move(cmd));
}

static_assert(gdx::net::lobby::kMachineRecordBytes == GDX_NET_RACE_RECORD_BYTES &&
                  GDX_NET_RACE_RECORD_BYTES == GDX_NET_MACHINE_RECORD_BYTES,
              "a machine record has one size on the wire, in the room and in the race");
static_assert(static_cast<int>(gdx::net::lobby::ResultKind::Finished) == GDX_NET_RACE_RESULT_FINISHED &&
                  static_cast<int>(gdx::net::lobby::ResultKind::Unfinished) == GDX_NET_RACE_RESULT_UNFINISHED &&
                  static_cast<int>(gdx::net::lobby::ResultKind::Retired) == GDX_NET_RACE_RESULT_RETIRED,
              "gdx_net_race_room.h names the result kinds by number");

extern "C" void gdx_lan_race_finish(unsigned raceIndex, int retired, unsigned timeMs) {
    using namespace gdx::lan;
    using namespace gdx::net::lobby;
    Command cmd;
    cmd.type = Command::Type::Finish;
    cmd.finish.raceIndex = static_cast<uint16_t>(raceIndex);
    if (retired) {
        cmd.finish.kind = ResultKind::Retired;
    } else {
        cmd.finish.kind = ResultKind::Finished;
        cmd.finish.timeMs = timeMs < 1 ? 1 : timeMs > kRaceTimeMaxMs ? kRaceTimeMaxMs : timeMs;
    }
    Push(std::move(cmd));
}

extern "C" void gdx_lan_race_standing(unsigned raceIndex, const unsigned char* humans, int count) {
    using namespace gdx::lan;
    Command cmd;
    cmd.type = Command::Type::Standing;
    {
        Runtime& rt = Rt();
        std::lock_guard<std::mutex> guard(rt.lock);
        const View& view = rt.view;
        if (view.mode != Mode::Hosting || !view.haveGrid || view.grid.raceIndex != raceIndex) {
            return;
        }
        for (int i = 0; i < count; i++) {
            if (humans[i] < view.grid.humans.size()) {
                cmd.order.push_back(view.grid.humans[humans[i]].id);
            }
        }
    }
    Push(std::move(cmd));
}

extern "C" void gdx_lan_race_host_left(unsigned raceIndex) {
    using namespace gdx::lan;
    Command cmd;
    cmd.type = Command::Type::HostLeft;
    cmd.race = static_cast<uint16_t>(raceIndex);
    Push(std::move(cmd));
}

extern "C" void gdx_lan_room_hand_over(void) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    if (rt.view.mode != Mode::Hosting) {
        return;
    }
    rt.ctl->handOver = true;
    rt.view = View();
}

extern "C" int gdx_lan_room_handing_over(char* next, int size) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const WorkerCtl* ctl = rt.ctl->handOver ? rt.ctl.get() : rt.leaverCtl.get();
    if (ctl == nullptr || !ctl->handOver || ctl->done.load()) {
        return 0;
    }
    if (next != nullptr && size > 0) {
        const size_t n = (std::min)(ctl->nextName.size(), static_cast<size_t>(size - 1));
        std::memcpy(next, ctl->nextName.data(), n);
        next[n] = '\0';
    }
    return 1;
}

extern "C" int gdx_lan_race_results(unsigned raceIndex, GdxNetRaceResults* out) {
    using namespace gdx::lan;
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    const View& view = rt.view;
    if ((view.mode != Mode::Hosting && view.mode != Mode::Joined) || !view.haveResults ||
        view.results.raceIndex != raceIndex || !view.haveGrid || view.grid.raceIndex != raceIndex ||
        view.grid.humans.size() > GDX_NET_RACE_HUMANS) {
        return 0;
    }
    *out = GdxNetRaceResults{};
    out->humans = static_cast<int>(view.grid.humans.size());
    for (size_t i = 0; i < view.grid.humans.size(); i++) {
        out->kind[i] = GDX_NET_RACE_RESULT_NONE;
        for (const gdx::net::lobby::ResultEntry& entry : view.results.entries) {
            if (entry.id == view.grid.humans[i].id) {
                out->kind[i] = static_cast<unsigned char>(entry.kind);
                out->timeMs[i] = entry.timeMs;
                break;
            }
        }
    }
    return 1;
}

extern "C" unsigned gdx_lan_now_ms(void) {
    return gdx::lan::NowMs();
}

extern "C" void gdx_lan_race_capture_rules(void) {
    using namespace gdx::lan;
    using namespace gdx::net::lobby;
    // The sliders clamp to these limits and racer.c clamps again; a hand-edited config must not make the
    // room refuse the rules.
    const auto slider = [](const char* name, float fallback, float low, float high) {
        const float value = CVarGetFloat(name, fallback);
        if (!(value >= low)) {
            return value < low ? low : fallback; // a NaN gets the default
        }
        return value > high ? high : value;
    };
    Command cmd;
    cmd.type = Command::Type::Rules;
    cmd.rules.ignoreTrackSettings = CVarGetInteger("gEnhancements.Gameplay.IgnoreTrackSettings", 0) != 0;
    cmd.rules.lavaNoKill = CVarGetInteger("gEnhancements.Gameplay.LavaNoKill", 0) != 0;
    cmd.rules.magnetRange = slider("gEnhancements.Gameplay.MagnetPullRange", 80.0f, kMagnetRangeMin, kMagnetRangeMax);
    cmd.rules.magnetStrength =
        slider("gEnhancements.Gameplay.MagnetPullStrength", 1.7f, kMagnetStrengthMin, kMagnetStrengthMax);
    // BoostDuration.cpp treats 0 or less as stock and clamps the top the same way.
    const int boost = CVarGetInteger("gEnhancements.Tuning.BoostDuration", 0);
    cmd.rules.boostFrames = boost <= 0 ? 0u : boost > static_cast<int>(kBoostFramesMax) ? kBoostFramesMax
                                                                                        : static_cast<uint32_t>(boost);
    Push(std::move(cmd));
    // The CPU-only options (decision 24). Only the host's copy runs CPUs: a client's worker drops this
    // command with the other host controls (RunClientCommand).
    Command cpu;
    cpu.type = Command::Type::CpuTuning;
    const int aggression = CVarGetInteger("gEnhancements.Gameplay.AiAggression", kAiAggressionDefault);
    cpu.aggression = aggression < kAiAggressionMin   ? kAiAggressionMin
                     : aggression > kAiAggressionMax ? kAiAggressionMax
                                                     : static_cast<uint16_t>(aggression);
    cpu.flag = CVarGetInteger("gEnhancements.Gameplay.CpuTrackHazards", 0) != 0;
    Push(std::move(cpu));
}
