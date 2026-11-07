// port/gdx_lan_lobby.cpp -- see gdx_lan_lobby.h.
//
// One worker thread per session. The worker owns the socket and the session object outright; the ImGui
// thread only queues commands and copies the published View, both under one mutex. The runtime lives on
// the heap and is never destroyed, so an exit path that skips Shutdown() cannot hit a joinable
// std::thread in a static destructor.

#include "gdx_lan_lobby.h"

#include "port_log.h"

#include <atomic>
#include <chrono>
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
constexpr int kMaxReadsPerTick = 256;

// What the worker should run.
enum class Kind : uint8_t { Browse, Host, Join };

struct Command {
    enum class Type : uint8_t { Ready, Chat, Kick } type = Type::Ready;
    bool ready = false;
    std::string text;
    uint8_t id = 0;
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

void Publish(Mode mode, const std::string& status, uint16_t port, const std::string& hostText, uint8_t localId,
             const std::vector<PlayerInfo>& roster, const std::deque<ShownLine>* chat, uint32_t chatSerial,
             const std::vector<LanBrowser::FoundRoom>* rooms) {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    View& v = rt.view;
    v.mode = mode;
    v.status = status;
    v.port = port;
    v.hostText = hostText;
    v.localId = localId;
    v.roster = roster;
    if (chat != nullptr) {
        v.chat.assign(chat->begin(), chat->end());
    } else {
        v.chat.clear();
    }
    v.chatSerial = chatSerial;
    if (rooms != nullptr) {
        v.rooms = *rooms;
    } else {
        v.rooms.clear();
    }
}

std::deque<Command> TakeCommands() {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    std::deque<Command> taken;
    taken.swap(rt.queue);
    return taken;
}

// The worker. socket is already open; everything else is built here and never leaves this thread.
void Run(Kind kind, std::shared_ptr<UdpSocket> socket, uint16_t port, LocalProfile me, NetAddress hostAddr) {
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
        host = std::make_unique<LobbyHost>(build, me, port, send);
    } else {
        client = std::make_unique<LobbyClient>(build, me, hostAddr, RandomNonce(), send);
        client->Start(start);
    }

    std::vector<uint8_t> buffer(net::kMaxDatagramBytes);
    uint32_t chatSerial = 0;
    size_t lastChatSize = 0;
    std::string lastChatBack;
    const std::string hostText = kind == Kind::Join ? net::FormatAddress(hostAddr) : std::string();
    const std::vector<PlayerInfo> noRoster;

    while (!rt.stop.load()) {
        const uint32_t now = NowMs();
        for (const Command& cmd : TakeCommands()) {
            if (cmd.type == Command::Type::Ready) {
                if (host) {
                    host->SetReady(cmd.ready);
                } else if (client) {
                    client->SetReady(cmd.ready);
                }
            } else if (cmd.type == Command::Type::Chat) {
                if (host) {
                    host->Chat(cmd.text);
                } else if (client) {
                    client->Chat(cmd.text);
                }
            } else if (cmd.type == Command::Type::Kick && host) {
                host->Kick(cmd.id, now);
            }
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
            if (browser) {
                browser->Receive(from, buffer.data(), len, now);
            } else if (host) {
                host->Receive(from, buffer.data(), len, now);
            } else if (client) {
                client->Receive(from, buffer.data(), len, now);
            }
        }

        if (browser) {
            browser->Update(now);
            Publish(Mode::Browsing, std::string(), port, std::string(), kSystemId, noRoster, nullptr, 0,
                    &browser->Rooms());
        } else if (host) {
            host->Update(now);
            const std::deque<ShownLine>& log = host->ChatLog();
            if (log.size() != lastChatSize || (!log.empty() && log.back().text != lastChatBack)) {
                chatSerial++;
                lastChatSize = log.size();
                lastChatBack = log.empty() ? std::string() : log.back().text;
            }
            Publish(Mode::Hosting, std::string(), port, std::string(), kHostId, host->Roster(), &log, chatSerial,
                    nullptr);
        } else if (client) {
            client->Update(now);
            const std::deque<ShownLine>& log = client->ChatLog();
            if (log.size() != lastChatSize || (!log.empty() && log.back().text != lastChatBack)) {
                chatSerial++;
                lastChatSize = log.size();
                lastChatBack = log.empty() ? std::string() : log.back().text;
            }
            const LobbyClient::State state = client->GetState();
            if (state == LobbyClient::State::Closed) {
                gdx_port_logf("[lan] left %s: %s\n", hostText.c_str(), client->CloseText().c_str());
                Publish(Mode::Closed, client->CloseText(), hostAddr.port, hostText, kSystemId, noRoster, nullptr, 0,
                        nullptr);
                socket->Close();
                return;
            }
            Publish(state == LobbyClient::State::Joined ? Mode::Joined : Mode::Joining, std::string(),
                    hostAddr.port, hostText, client->LocalId(), client->Roster(), &log, chatSerial, nullptr);
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

bool Launch(Kind kind, uint16_t bindPort, uint16_t port, const LocalProfile& me, const NetAddress& hostAddr,
            std::string& error) {
    StopWorker();
    auto socket = std::make_shared<UdpSocket>();
    if (!socket->Open(Addr(0, bindPort))) {
        error = bindPort != 0 ? "Port " + std::to_string(bindPort) +
                                    " is in use, maybe by another copy of G-Diffuser. Pick another port."
                              : "Could not open a network socket.";
        Publish(Mode::Closed, error, port, std::string(), kSystemId, {}, nullptr, 0, nullptr);
        return false;
    }
    if (kind == Kind::Browse && !socket->SetBroadcast(true)) {
        gdx_port_logf("[lan] broadcast refused; searching this computer only\n");
    }
    gdx_port_logf("[lan] %s on port %u\n", kind == Kind::Browse ? "browse" : kind == Kind::Host ? "host" : "join",
                  static_cast<unsigned>(socket->LocalPort()));
    Publish(kind == Kind::Browse ? Mode::Browsing : kind == Kind::Host ? Mode::Hosting : Mode::Joining,
            std::string(), port, kind == Kind::Join ? net::FormatAddress(hostAddr) : std::string(),
            kind == Kind::Host ? kHostId : kSystemId, {}, nullptr, 0, nullptr);
    Rt().worker = std::thread(Run, kind, socket, port, me, hostAddr);
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
    Launch(Kind::Browse, 0, port, LocalProfile(), NetAddress(), error);
}

bool StartHost(uint16_t port, const LocalProfile& me, std::string& error) {
    return Launch(Kind::Host, port, port, me, NetAddress(), error);
}

void StartJoin(const NetAddress& host, const LocalProfile& me) {
    std::string error;
    Launch(Kind::Join, 0, host.port, me, host, error);
}

void SetReady(bool ready) {
    Command cmd;
    cmd.type = Command::Type::Ready;
    cmd.ready = ready;
    Push(std::move(cmd));
}

void Chat(const std::string& text) {
    Command cmd;
    cmd.type = Command::Type::Chat;
    cmd.text = text;
    Push(std::move(cmd));
}

void Kick(uint8_t id) {
    Command cmd;
    cmd.type = Command::Type::Kick;
    cmd.id = id;
    Push(std::move(cmd));
}

void Stop() {
    StopWorker();
    Publish(Mode::Idle, std::string(), 0, std::string(), kSystemId, {}, nullptr, 0, nullptr);
}

void Dismiss() {
    Stop();
}

View Snapshot() {
    Runtime& rt = Rt();
    std::lock_guard<std::mutex> guard(rt.lock);
    return rt.view;
}

void Shutdown() {
    StopWorker();
}

} // namespace gdx::lan
