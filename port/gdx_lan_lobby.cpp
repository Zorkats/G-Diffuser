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

#include "gdx_lan_lobby.h"

#include "gdx_course_state.h"
#include "gdx_net_machine_select.h"
#include "gdx_net_race_room.h"
#include "gdx_profile_state.h"
#include "net/gdx_net_race.h"

#include "libultraship/bridge/consolevariablebridge.h"

#include "port_log.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
        Kick,
        GridReady,
        Rules,
        Finish,
        Standing,
        HostLeft
    } type = Type::Ready;
    bool flag = false; // Ready, RaceNext
    uint8_t value = 0; // a track, a preset message, a racer cap or a player id
    LockRequest lock;
    ReadyToRace ready;
    RaceRules rules;
    FinishReport finish;
    std::vector<uint8_t> order; // Standing: player ids, first to last
    uint16_t race = 0;          // HostLeft: the race the host's game left
};

struct Runtime {
    std::mutex lock;
    View view;                  // guarded by lock
    std::deque<Command> queue;  // guarded by lock
    std::thread worker;         // started and joined on the ImGui thread only
    std::atomic<bool> stop{ false };
};

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

NetAddress Addr(uint32_t ip, uint16_t port) {
    NetAddress a;
    a.ip = ip;
    a.port = port;
    return a;
}

// Joins the worker of the last session, if any. ImGui thread only.
void StopWorker() {
    Runtime& rt = Rt();
    if (rt.worker.joinable()) {
        rt.stop.store(true);
        rt.worker.join();
    }
    rt.stop.store(false);
    std::lock_guard<std::mutex> guard(rt.lock);
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

std::deque<Command> TakeCommands() {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    std::deque<Command> taken;
    taken.swap(rt.queue);
    return taken;
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
    case Command::Type::Kick:
        host.Kick(cmd.value, now);
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

// The worker. socket is already open; everything else is built here and never leaves this thread.
void Run(Kind kind, std::shared_ptr<UdpSocket> socket, uint16_t port, LocalProfile me, NetAddress hostAddr,
         Seat seat) {
    Runtime& rt = Rt();
    const DatagramSend send = [socket](const NetAddress& to, const uint8_t* data, size_t len) {
        socket->SendTo(to, data, len);
    };
    const std::string build = BuildId();
    std::unique_ptr<LanBrowser> browser;
    std::unique_ptr<LobbyHost> host;
    std::unique_ptr<LobbyClient> client;
    const uint32_t start = NowMs();

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
        host = std::make_unique<LobbyHost>(build, me, port, config, send);
    } else {
        client = std::make_unique<LobbyClient>(build, me, seat, hostAddr, RandomNonce(), send);
        client->Start(start);
    }

    std::vector<uint8_t> buffer(net::kMaxDatagramBytes);
    uint32_t lineSerial = 0;
    size_t lastLineCount = 0;
    std::string lastLineBack;
    const std::string hostText = kind == Kind::Join ? net::FormatAddress(hostAddr) : std::string();

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

    while (!rt.stop.load()) {
        const uint32_t now = NowMs();
        for (const Command& cmd : TakeCommands()) {
            if (host) {
                RunHostCommand(*host, cmd, now);
            } else if (client) {
                RunClientCommand(*client, cmd, now);
            }
        }
        SendRacePoses(host.get(), client.get());
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
        DeliverRacePoses(host.get(), client.get());

        if (browser) {
            browser->Update(now);
            View v = MakeView(Mode::Browsing, std::string(), port, std::string(), kSystemId);
            v.rooms = browser->Rooms();
            Publish(std::move(v));
        } else if (host) {
            View v = MakeView(Mode::Hosting, std::string(), port, std::string(), kHostId);
            v.roster = host->Roster();
            v.room = host->Room(now);
            v.votes = host->Votes();
            v.lottery = host->Lottery();
            v.picked = host->LastPicked();
            v.grid = host->RaceGrid();
            v.haveGrid = !v.grid.humans.empty();
            v.haveStart = host->StartSent(v.start);
            v.results = host->Results();
            v.haveResults = v.results.final || !v.results.entries.empty();
            v.clockReady = true; // the room runs on this copy's clock
            takeLines(v, host->Lines());
            Publish(std::move(v));
        } else if (client) {
            const LobbyClient::State state = client->GetState();
            if (state == LobbyClient::State::Closed) {
                gdx_port_logf("[lan] left %s: %s\n", hostText.c_str(), client->CloseText().c_str());
                Publish(MakeView(Mode::Closed, client->CloseText(), hostAddr.port, hostText, kSystemId));
                socket->Close();
                return;
            }
            View v = MakeView(state == LobbyClient::State::Joined ? Mode::Joined : Mode::Joining, std::string(),
                              hostAddr.port, hostText, client->LocalId());
            v.roster = client->Roster();
            v.room = client->Room(now);
            v.votes = client->Votes();
            v.lottery = client->Lottery();
            v.lotteryChecked = client->LotteryChecked();
            v.picked = client->LastPicked();
            v.haveGrid = client->RaceGrid(v.grid);
            v.haveStart = client->StartSent(v.start);
            v.haveResults = client->Results(v.results);
            v.clockReady = client->Clock().Ready();
            v.clockOffsetMs = client->Clock().OffsetMs();
            takeLines(v, client->Lines());
            Publish(std::move(v));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    }

    const uint32_t now = NowMs();
    if (host) {
        host->Close(now);
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

bool Launch(Kind kind, uint16_t bindPort, uint16_t port, const LocalProfile& me, const NetAddress& hostAddr,
            Seat seat, std::string& error) {
    StopWorker();
    auto socket = std::make_shared<UdpSocket>();
    if (!socket->Open(Addr(0, bindPort))) {
        error = bindPort != 0 ? "Port " + std::to_string(bindPort) +
                                    " is in use, maybe by another copy of G-Diffuser. Pick another port."
                              : "Could not open a network socket.";
        Publish(MakeView(Mode::Closed, error, port, std::string(), kSystemId));
        return false;
    }
    if (kind == Kind::Browse && !socket->SetBroadcast(true)) {
        gdx_port_logf("[lan] broadcast refused; searching this computer only\n");
    }
    gdx_port_logf("[lan] %s on port %u\n", kind == Kind::Browse ? "browse" : kind == Kind::Host ? "host" : "join",
                  static_cast<unsigned>(socket->LocalPort()));
    Publish(MakeView(kind == Kind::Browse ? Mode::Browsing : kind == Kind::Host ? Mode::Hosting : Mode::Joining,
                     std::string(), port, kind == Kind::Join ? net::FormatAddress(hostAddr) : std::string(),
                     kind == Kind::Host ? kHostId : kSystemId));
    Rt().worker = std::thread(Run, kind, socket, port, me, hostAddr, seat);
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

void StartBrowse(uint16_t port) {
    std::string error;
    Launch(Kind::Browse, 0, port, LocalProfile(), NetAddress(), Seat::Race, error);
}

bool StartHost(uint16_t port, const LocalProfile& me, std::string& error) {
    return Launch(Kind::Host, port, port, me, NetAddress(), Seat::Race, error);
}

void StartJoin(const NetAddress& host, const LocalProfile& me, Seat seat) {
    std::string error;
    Launch(Kind::Join, 0, host.port, me, host, seat, error);
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

void Kick(uint8_t id) {
    PushType(Command::Type::Kick, id);
}

namespace {

std::bitset<256>& Mutes() {
    static std::bitset<256> mutes;
    return mutes;
}

} // namespace

bool Muted(uint8_t id) {
    return Mutes().test(id);
}

void SetMuted(uint8_t id, bool muted) {
    Mutes().set(id, muted);
}

void ForgetMutesOutside(const View& view) {
    std::bitset<256> present;
    for (const PlayerInfo& player : view.roster) {
        present.set(player.id);
    }
    Mutes() &= present;
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

void Shutdown() {
    StopWorker();
}

void DevFrame() {
    enum class Dev : uint8_t { Unread, Off, Host, Join };
    static Dev dev = Dev::Unread;
    static uint16_t port = 0;
    static uint32_t frame = 0;
    static bool hosted = false;
    static uint32_t tableLogged = 0; // the race index after the last table written to the log

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
        std::string error;
        hosted = true; // one try: a taken port is said in the log, not retried
        if (!StartHost(port, me, error)) {
            gdx_port_logf("[lan] GDX_NET_DEV: %s\n", error.c_str());
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
    Push(std::move(cmd));
}
