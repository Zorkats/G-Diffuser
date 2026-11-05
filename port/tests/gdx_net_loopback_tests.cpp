// Channel over real UDP sockets on 127.0.0.1 (port/tests/gdx_net_loopback_tests.cpp).
//
// gdx_net_channel_tests proves the protocol on a simulated link. This harness proves the two layers
// fit: the channel's send callback goes through UdpSocket::SendTo, received datagrams are fed back
// through Receive(), and time comes from the steady clock. Each side accepts datagrams only from the
// other side's address, which is the routing the session layer will do.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "../net/gdx_net_channel.h"
#include "../net/gdx_net_socket.h"

namespace {

using gdx::net::Channel;
using gdx::net::ChannelConfig;
using gdx::net::CloseReason;
using gdx::net::kMaxDatagramBytes;
using gdx::net::kMaxMessagePayload;
using gdx::net::LinkState;
using gdx::net::Message;
using gdx::net::NetAddress;
using gdx::net::RecvResult;
using gdx::net::Role;
using gdx::net::QueueResult;
using gdx::net::UdpSocket;

int gChecks = 0;
int gFails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        gChecks++;                                                         \
        if (!(cond)) {                                                     \
            gFails++;                                                      \
            std::printf("    [x] line %d: %s\n", __LINE__, #cond);        \
        }                                                                  \
    } while (0)

constexpr uint32_t kLoopbackIp = 0x7F000001u;
constexpr uint8_t kStreamType = 9;

uint32_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// One end: a socket, a channel that sends through it, and the peer address it listens to.
struct End {
    UdpSocket socket;
    NetAddress peer;
    uint32_t strays = 0; // datagrams from any address but the peer's

    void Pump(Channel& channel) {
        std::vector<uint8_t> buf(kMaxDatagramBytes);
        for (;;) {
            size_t len = 0;
            NetAddress from;
            const RecvResult r = socket.RecvFrom(buf.data(), buf.size(), len, from);
            if (r != RecvResult::Ok) {
                return; // WouldBlock is the idle result; Truncated and Error drop one datagram
            }
            if (from != peer) {
                strays++;
                continue;
            }
            channel.Receive(buf.data(), len, NowMs());
        }
    }
};

// Pumps both ends until pred() holds or limitMs of wall time passes.
template <typename Pred>
bool PumpUntil(End& a, Channel& ca, End& b, Channel& cb, uint32_t limitMs, Pred pred) {
    const uint32_t start = NowMs();
    for (;;) {
        a.Pump(ca);
        b.Pump(cb);
        ca.Update(NowMs());
        cb.Update(NowMs());
        if (pred()) {
            return true;
        }
        if (NowMs() - start >= limitMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void CaseChannelOverLoopback() {
    End clientEnd;
    End serverEnd;
    CHECK(clientEnd.socket.Open(NetAddress{kLoopbackIp, 0}));
    CHECK(serverEnd.socket.Open(NetAddress{kLoopbackIp, 0}));
    if (!clientEnd.socket.IsOpen() || !serverEnd.socket.IsOpen()) {
        return;
    }
    clientEnd.peer = NetAddress{kLoopbackIp, serverEnd.socket.LocalPort()};
    serverEnd.peer = NetAddress{kLoopbackIp, clientEnd.socket.LocalPort()};

    uint32_t sendFailures = 0;
    ChannelConfig clientCfg;
    clientCfg.role = Role::Client;
    clientCfg.connectionNonce = 0x10C41B0Cu;
    ChannelConfig serverCfg;
    serverCfg.role = Role::Server;
    Channel client(clientCfg, [&](const uint8_t* d, size_t n) {
        if (clientEnd.socket.SendTo(clientEnd.peer, d, n) != gdx::net::SendResult::Ok) {
            sendFailures++;
        }
    });
    Channel server(serverCfg, [&](const uint8_t* d, size_t n) {
        if (serverEnd.socket.SendTo(serverEnd.peer, d, n) != gdx::net::SendResult::Ok) {
            sendFailures++;
        }
    });

    CHECK(client.Connect(NowMs()));
    CHECK(PumpUntil(clientEnd, client, serverEnd, server, 3000, [&] {
        return client.State() == LinkState::Connected && server.State() == LinkState::Connected;
    }));

    // 300 reliable messages, the last one the largest allowed, plus one unreliable message each way.
    constexpr uint32_t kCount = 300;
    uint32_t queued = 0;
    uint32_t inOrder = 0;
    uint32_t wrong = 0;
    bool largestIntact = false;
    bool unreliableToServer = false;
    bool unreliableToClient = false;
    const std::vector<uint8_t> largest(kMaxMessagePayload, 0x3C);
    const uint8_t ping[2] = {0xAB, 0xCD};
    CHECK(client.SendUnreliable(kStreamType, ping, sizeof(ping)) == QueueResult::Ok);
    CHECK(server.SendUnreliable(kStreamType, ping, sizeof(ping)) == QueueResult::Ok);
    Message m;
    const bool done = PumpUntil(clientEnd, client, serverEnd, server, 5000, [&] {
        while (queued < kCount) {
            uint8_t body[4] = {static_cast<uint8_t>(queued), static_cast<uint8_t>(queued >> 8), 0, 0};
            const bool last = queued + 1 == kCount;
            const QueueResult r = last ? client.SendReliable(kStreamType, largest.data(), largest.size())
                                      : client.SendReliable(kStreamType, body, sizeof(body));
            if (r != QueueResult::Ok) {
                break; // the window is full; acks will free it
            }
            queued++;
        }
        while (server.PopMessage(m)) {
            if (!m.reliable) {
                unreliableToServer = m.payload.size() == 2 && m.payload[0] == 0xAB;
            } else if (inOrder + 1 == kCount) {
                largestIntact = m.payload == largest;
                inOrder++;
            } else if (m.payload.size() == 4 && (m.payload[0] | (m.payload[1] << 8)) == static_cast<int>(inOrder)) {
                inOrder++;
            } else {
                wrong++;
            }
        }
        while (client.PopMessage(m)) {
            unreliableToClient = !m.reliable && m.payload.size() == 2 && m.payload[1] == 0xCD;
        }
        return inOrder == kCount && unreliableToServer && unreliableToClient;
    });
    CHECK(done);
    CHECK(inOrder == kCount);
    CHECK(wrong == 0);
    CHECK(largestIntact);
    CHECK(unreliableToServer);
    CHECK(unreliableToClient);
    CHECK(sendFailures == 0);
    CHECK(clientEnd.strays == 0 && serverEnd.strays == 0);
    CHECK(server.Stats().malformed == 0 && client.Stats().malformed == 0);

    client.Disconnect(NowMs());
    CHECK(client.State() == LinkState::Disconnected);
    CHECK(PumpUntil(clientEnd, client, serverEnd, server, 2000,
                    [&] { return server.State() == LinkState::Disconnected; }));
    CHECK(server.Reason() == CloseReason::Closed);
}

} // namespace

int main() {
    std::printf("=== G-Diffuser channel over loopback UDP (gdx_net_channel + gdx_net_socket) ===\n\n");
    std::printf("-- channel over 127.0.0.1\n");
    CaseChannelOverLoopback();
    std::printf("%s channel over 127.0.0.1 (%d/%d sub-checks passed)\n", gFails ? "[FAIL]" : "[PASS]",
                gChecks - gFails, gChecks);
    return gFails ? 1 : 0;
}
