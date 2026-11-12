/* Standalone unit test for port/net/gdx_net_socket.cpp, the UDP socket under the online layer.
 *
 * Every socket binds 127.0.0.1 on an ephemeral port, so no traffic leaves the machine and no
 * firewall rule is needed. The exchange test waits for each datagram before it sends the next, so
 * the receive queue never fills and the result does not depend on loopback buffer sizes.
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

#include "../net/gdx_net_socket.h"

namespace net = gdx::net;
using Clock = std::chrono::steady_clock;

static int gChecks = 0;
static int gFails = 0;

// Records one check. `what` names the check in the failure line, which matters inside loops.
static void Check(bool ok, const char* what, int line) {
    gChecks++;
    if (!ok) {
        gFails++;
        std::printf("    [x] line %d: %s\n", line, what);
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)

static net::NetAddress Loopback(uint16_t port) {
    return net::NetAddress::FromV4(0x7F000001u, port);
}

// [::1]:port
static net::NetAddress Loopback6(uint16_t port) {
    net::NetAddress addr;
    addr.ip[15] = 1;
    addr.port = port;
    return addr;
}

static Clock::time_point Deadline() {
    return Clock::now() + std::chrono::seconds(5);
}

// Polls until a datagram arrives or the deadline passes. The 1 ms sleep keeps the poll from
// spinning a core while the datagram is in flight.
static net::RecvResult WaitRecv(net::UdpSocket& sock, void* buf, std::size_t cap, std::size_t& len,
                                net::NetAddress& from, Clock::time_point deadline) {
    for (;;) {
        const net::RecvResult r = sock.RecvFrom(buf, cap, len, from);
        if (r != net::RecvResult::WouldBlock || Clock::now() >= deadline) {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void CaseAddressRoundTrip() {
    const char* samples[] = { "192.168.1.20:24302", "0.0.0.0:0", "127.0.0.1:1", "10.0.0.7:80",
                              "255.255.255.255:65535" };
    for (const char* text : samples) {
        net::NetAddress addr;
        CHECK(net::ParseAddress(text, addr));
        CHECK(net::FormatAddress(addr) == text);
        net::NetAddress again;
        CHECK(net::ParseAddress(net::FormatAddress(addr).c_str(), again) && again == addr);
    }

    net::NetAddress a;
    net::NetAddress b;
    CHECK(net::ParseAddress("1.2.3.4:5678", a));
    CHECK(net::ParseAddress("1.2.3.4:5678", b));
    CHECK(a == b && !(a != b));
    CHECK(net::ParseAddress("1.2.3.4:5679", b));
    CHECK(a != b && !(a == b));
    CHECK(a.IsV4() && a.Ipv4() == 0x01020304u && a.port == 5678);  // host byte order, not network order
}

static void CaseAddressRejects() {
    const char* bad[] = {
        "", "1.2.3.4", "1.2.3.4:", ":80", "hello", "localhost:80",
        "1.2.3.4:65536", "1.2.3.4:99999", "1.2.3.4:123456", "1.2.3.4:080", "1.2.3.4:-1",
        "1.2.3.4:80x", "1.2.3.4:80 ", " 1.2.3.4:80", "1.2.3.4:80:90",
        "1.2.3.256:80", "1.2.3:80", "1.2.3.4.5:80", "1.2.3.4.:80", "1..3.4:80", "1.2.3.04:80", "-1.2.3.4:80",
    };
    for (const char* text : bad) {
        // The sentinel shows that a rejected parse leaves the output untouched.
        net::NetAddress addr = net::NetAddress::FromV4(0xDEADBEEFu, 4242);
        const bool rejected = !net::ParseAddress(text, addr);
        Check(rejected && addr == net::NetAddress::FromV4(0xDEADBEEFu, 4242), text, __LINE__);
    }
    net::NetAddress addr;
    CHECK(!net::ParseAddress(nullptr, addr));
}

// O5.3: IPv6 text in brackets, written back in the RFC 5952 form; an IPv4 address is held mapped.
static void CaseIpv6AddressText() {
    const char* canonical[] = { "[::1]:24302", "[::]:0", "[2001:db8::1]:80", "[fe80::1:2:3:4]:1",
                                "[2001:db8:0:1:1:1:1:1]:7", "[1:2:3:4:5:6:7:8]:65535", "[2001:db8::1:0:0:1]:5" };
    for (const char* text : canonical) {
        net::NetAddress addr;
        const bool ok = net::ParseAddress(text, addr) && net::FormatAddress(addr) == text && !addr.IsV4();
        Check(ok, text, __LINE__);
    }
    // Other spellings of the same address come back canonical: lower case, no leading zeros, the longest run
    // of zero groups (the first on a tie) as "::", and a single zero group kept.
    const char* spelled[][2] = {
        { "[2001:DB8::A]:1", "[2001:db8::a]:1" },
        { "[2001:0db8:0000:0000:0000:0000:0000:0001]:2", "[2001:db8::1]:2" },
        { "[2001:db8:0:0:1:0:0:1]:5", "[2001:db8::1:0:0:1]:5" },
        { "[2001:db8:0:1:0:0:0:1]:6", "[2001:db8:0:1::1]:6" },
        { "[0:0:0:0:0:0:0:0]:3", "[::]:3" },
    };
    for (const auto& pair : spelled) {
        net::NetAddress addr;
        Check(net::ParseAddress(pair[0], addr) && net::FormatAddress(addr) == pair[1], pair[0], __LINE__);
    }
    // The mapped form of an IPv4 address is the IPv4 address.
    net::NetAddress mapped;
    CHECK(net::ParseAddress("[::ffff:102:304]:9", mapped));
    CHECK(mapped.IsV4() && mapped == net::NetAddress::FromV4(0x01020304u, 9));
    CHECK(net::FormatAddress(mapped) == "1.2.3.4:9");
    CHECK(Loopback6(1).Ipv4() == 0 && !Loopback6(1).IsV4());

    const char* bad[] = { "[::1]", "::1:80", "[::1:80", "[::1]80", "[1::2::3]:1", "[12345::]:1", "[::1]:65536",
                          "[g::]:1", "[1:2:3:4:5:6:7:8:9]:1", "[1:2:3:4:5:6:7::8]:1", "[1:2:3:4:5:6:7]:1",
                          "[:1::]:1", "[1::2:]:1", "[]:1", "[::ffff:1.2.3.4]:1", "[fe80::1%1]:1" };
    for (const char* text : bad) {
        net::NetAddress addr = Loopback(4242);
        Check(!net::ParseAddress(text, addr) && addr == Loopback(4242), text, __LINE__);
    }
}

static void CaseEphemeralBind() {
    net::UdpSocket sock;
    CHECK(!sock.IsOpen() && sock.LocalPort() == 0);
    CHECK(sock.Open(Loopback(0)));
    CHECK(sock.IsOpen() && sock.LocalPort() != 0);
    CHECK(!sock.Open(Loopback(0)));  // already open: refused, and the bound socket is kept
    CHECK(sock.LocalPort() != 0);

    // A fixed port that is already bound must be refused, not shared.
    net::UdpSocket clash;
    CHECK(!clash.Open(Loopback(sock.LocalPort())));
    CHECK(!clash.IsOpen());

    net::NetAddress parsed;
    CHECK(net::ParseAddress("127.0.0.1:0", parsed));
    net::UdpSocket fromText;
    CHECK(fromText.Open(parsed) && fromText.LocalPort() != 0);
}

static void CaseEmptyReceive() {
    net::UdpSocket sock;
    CHECK(sock.Open(Loopback(0)));
    unsigned char buf[16];
    std::size_t len = 99;
    net::NetAddress from;
    const Clock::time_point start = Clock::now();
    const net::RecvResult r = sock.RecvFrom(buf, sizeof(buf), len, from);
    const Clock::duration waited = Clock::now() - start;
    CHECK(r == net::RecvResult::WouldBlock);
    CHECK(len == 0);
    CHECK(waited < std::chrono::seconds(1));  // "immediately": far below any real wait
}

static void CaseClosedRefuses() {
    net::UdpSocket sock;
    const unsigned char byte = 1;
    unsigned char buf[4];
    std::size_t len = 7;
    net::NetAddress from;
    CHECK(sock.SendTo(Loopback(9), &byte, 1) == net::SendResult::Error);
    CHECK(sock.RecvFrom(buf, sizeof(buf), len, from) == net::RecvResult::Error);
    CHECK(len == 0);
    sock.Close();  // closing a closed socket is harmless
    CHECK(!sock.IsOpen());
}

// Payload i is 0 to 1200 bytes long, and its bytes depend on i and position. A datagram that
// lands at the wrong socket or is cut short therefore fails the content check.
static std::size_t PayloadSize(int i) {
    return static_cast<std::size_t>((i * 131) % 1201);
}

static unsigned char PayloadByte(int i, std::size_t pos) {
    return static_cast<unsigned char>(i * 7 + static_cast<int>(pos) * 3 + 1);
}

static void CaseExchange() {
    constexpr int kCount = 100;
    net::UdpSocket a;
    net::UdpSocket b;
    CHECK(a.Open(Loopback(0)));
    CHECK(b.Open(Loopback(0)));
    const net::NetAddress addrA = Loopback(a.LocalPort());
    const net::NetAddress addrB = Loopback(b.LocalPort());
    const Clock::time_point deadline = Deadline();

    // Even datagrams go A to B and odd ones go B to A. Each is received before the next is sent.
    for (int i = 0; i < kCount; i++) {
        const bool fromA = (i % 2) == 0;
        net::UdpSocket& sender = fromA ? a : b;
        net::UdpSocket& receiver = fromA ? b : a;
        const net::NetAddress& senderAddr = fromA ? addrA : addrB;
        const net::NetAddress& destAddr = fromA ? addrB : addrA;

        unsigned char payload[1201];
        const std::size_t size = PayloadSize(i);
        for (std::size_t pos = 0; pos < size; pos++) {
            payload[pos] = PayloadByte(i, pos);
        }
        net::SendResult sent = sender.SendTo(destAddr, payload, size);
        while (sent == net::SendResult::WouldBlock && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sent = sender.SendTo(destAddr, payload, size);
        }
        Check(sent == net::SendResult::Ok, "datagram handed to the OS", __LINE__);
        if (sent != net::SendResult::Ok) {
            std::printf("    datagram %d was not sent\n", i);
            break;
        }

        unsigned char got[2048];
        std::size_t gotLen = 0;
        net::NetAddress from;
        const net::RecvResult r = WaitRecv(receiver, got, sizeof(got), gotLen, from, deadline);
        const bool intact = r == net::RecvResult::Ok && gotLen == size && from == senderAddr &&
                            (size == 0 || std::memcmp(got, payload, size) == 0);
        Check(intact, "datagram arrives with its size, sender and bytes intact", __LINE__);
        if (!intact) {
            std::printf("    datagram %d (%zu bytes) failed\n", i, size);
            break;
        }
    }
}

// Sends one datagram and waits for it, checking the bytes and the sender the receiver sees.
static bool OneDatagram(net::UdpSocket& sender, net::UdpSocket& receiver, const net::NetAddress& to,
                        const net::NetAddress& expectFrom, unsigned char tag) {
    const unsigned char payload[3] = { 'O', '5', tag };
    if (sender.SendTo(to, payload, sizeof(payload)) != net::SendResult::Ok) {
        return false;
    }
    unsigned char got[16];
    std::size_t len = 0;
    net::NetAddress from;
    const net::RecvResult r = WaitRecv(receiver, got, sizeof(got), len, from, Deadline());
    return r == net::RecvResult::Ok && len == sizeof(payload) && std::memcmp(got, payload, len) == 0 &&
           from == expectFrom;
}

// O5.3: two sockets on [::1] exchange datagrams both ways.
static void CaseIpv6Loopback() {
    net::UdpSocket a;
    net::UdpSocket b;
    CHECK(a.Open(Loopback6(0)) && a.Ipv6() && !a.DualStack());
    CHECK(b.Open(Loopback6(0)));
    CHECK(OneDatagram(a, b, Loopback6(b.LocalPort()), Loopback6(a.LocalPort()), 1));
    CHECK(OneDatagram(b, a, Loopback6(a.LocalPort()), Loopback6(b.LocalPort()), 2));
    // An IPv6-only socket does not reach an IPv4 peer.
    net::UdpSocket v4;
    CHECK(v4.Open(Loopback(0)));
    const unsigned char byte = 0;
    CHECK(a.SendTo(Loopback(v4.LocalPort()), &byte, 1) == net::SendResult::Error);
}

// O5.3: one socket bound to "::" talks to an IPv4 peer and an IPv6 peer, and sees the IPv4 one as the mapped
// address that FromV4 builds. An IPv4 socket refuses an IPv6 destination.
static void CaseDualStack() {
    net::UdpSocket dual;
    CHECK(dual.Open(net::NetAddress()));
    CHECK(dual.IsOpen() && dual.DualStack() && dual.Ipv6() && dual.LocalPort() != 0);
    net::UdpSocket v4;
    net::UdpSocket v6;
    CHECK(v4.Open(Loopback(0)) && !v4.Ipv6() && !v4.DualStack());
    CHECK(v6.Open(Loopback6(0)));
    CHECK(OneDatagram(v4, dual, Loopback(dual.LocalPort()), Loopback(v4.LocalPort()), 4));
    CHECK(OneDatagram(dual, v4, Loopback(v4.LocalPort()), Loopback(dual.LocalPort()), 5));
    CHECK(OneDatagram(v6, dual, Loopback6(dual.LocalPort()), Loopback6(v6.LocalPort()), 6));
    CHECK(OneDatagram(dual, v6, Loopback6(v6.LocalPort()), Loopback6(dual.LocalPort()), 7));
    const unsigned char byte = 0;
    CHECK(v4.SendTo(Loopback6(v6.LocalPort()), &byte, 1) == net::SendResult::Error);
    // The port is taken for both families: an IPv4 socket cannot bind it beside the dual-stack one.
    net::UdpSocket clash;
    CHECK(!clash.Open(net::NetAddress::FromV4(0, dual.LocalPort())));
}

static void CaseOversizeDropped() {
    net::UdpSocket sender;
    net::UdpSocket receiver;
    CHECK(sender.Open(Loopback(0)));
    CHECK(receiver.Open(Loopback(0)));
    const net::NetAddress to = Loopback(receiver.LocalPort());
    const Clock::time_point deadline = Deadline();

    unsigned char big[1000];
    std::memset(big, 0xAB, sizeof(big));
    CHECK(sender.SendTo(to, big, sizeof(big)) == net::SendResult::Ok);

    // The window is 512 bytes with 16 guard bytes after it. Any copy past the window would
    // overwrite the guard, and the check below would catch it.
    unsigned char window[512 + 16];
    std::memset(window, 0x55, sizeof(window));
    std::size_t len = 99;
    net::NetAddress from;
    CHECK(WaitRecv(receiver, window, 512, len, from, deadline) == net::RecvResult::Truncated);
    CHECK(len == 0);
    bool guardIntact = true;
    for (std::size_t pos = 512; pos < sizeof(window); pos++) {
        guardIntact = guardIntact && window[pos] == 0x55;
    }
    CHECK(guardIntact);

    // The oversize datagram was dropped rather than left at the head of the queue, so the next
    // receive returns the small datagram sent after it.
    const unsigned char small[3] = { 1, 2, 3 };
    CHECK(sender.SendTo(to, small, sizeof(small)) == net::SendResult::Ok);
    std::size_t smallLen = 0;
    net::NetAddress smallFrom;
    CHECK(WaitRecv(receiver, window, 512, smallLen, smallFrom, deadline) == net::RecvResult::Ok);
    CHECK(smallLen == 3 && window[0] == 1 && window[2] == 3);
    CHECK(smallFrom == Loopback(sender.LocalPort()));
}

static void CaseSendToClosedPort() {
    net::UdpSocket sender;
    CHECK(sender.Open(Loopback(0)));

    // Learn a port that nothing is bound to by opening and closing a socket on it.
    net::UdpSocket dead;
    CHECK(dead.Open(Loopback(0)));
    const net::NetAddress deadAddr = Loopback(dead.LocalPort());
    dead.Close();

    const unsigned char ping[4] = { 9, 9, 9, 9 };
    CHECK(sender.SendTo(deadAddr, ping, sizeof(ping)) == net::SendResult::Ok);

    // Windows may report the ICMP port-unreachable on a later receive. Polling for a while lets
    // it arrive, and no poll may surface it as an Error.
    unsigned char buf[64];
    std::size_t len = 0;
    net::NetAddress from;
    bool sawError = false;
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (sender.RecvFrom(buf, sizeof(buf), len, from) == net::RecvResult::Error) {
            sawError = true;
        }
    }
    CHECK(!sawError);

    // Traffic sent afterwards still gets through.
    net::UdpSocket peer;
    CHECK(peer.Open(Loopback(0)));
    const unsigned char hello[5] = { 'h', 'e', 'l', 'l', 'o' };
    CHECK(peer.SendTo(Loopback(sender.LocalPort()), hello, sizeof(hello)) == net::SendResult::Ok);
    CHECK(WaitRecv(sender, buf, sizeof(buf), len, from, Deadline()) == net::RecvResult::Ok);
    CHECK(len == sizeof(hello) && std::memcmp(buf, hello, sizeof(hello)) == 0);
    CHECK(from == Loopback(peer.LocalPort()));
}

static void CaseReopenCycles() {
    net::UdpSocket peer;
    CHECK(peer.Open(Loopback(0)));
    net::UdpSocket sock;
    for (int cycle = 0; cycle < 2; cycle++) {
        CHECK(sock.Open(Loopback(0)));
        CHECK(sock.IsOpen() && sock.LocalPort() != 0);

        const unsigned char tag = static_cast<unsigned char>(0x10 + cycle);
        CHECK(peer.SendTo(Loopback(sock.LocalPort()), &tag, 1) == net::SendResult::Ok);
        unsigned char buf[4] = { 0, 0, 0, 0 };
        std::size_t len = 0;
        net::NetAddress from;
        CHECK(WaitRecv(sock, buf, sizeof(buf), len, from, Deadline()) == net::RecvResult::Ok);
        CHECK(len == 1 && buf[0] == tag);

        sock.Close();
        CHECK(!sock.IsOpen() && sock.LocalPort() == 0);
    }
}

static void CaseMoveSemantics() {
    static_assert(!std::is_copy_constructible<net::UdpSocket>::value, "UdpSocket must not be copyable");
    static_assert(!std::is_copy_assignable<net::UdpSocket>::value, "UdpSocket must not be copy-assignable");
    static_assert(std::is_move_constructible<net::UdpSocket>::value, "UdpSocket must be movable");
    static_assert(std::is_move_assignable<net::UdpSocket>::value, "UdpSocket must be move-assignable");

    net::UdpSocket a;
    CHECK(a.Open(Loopback(0)));
    const uint16_t port = a.LocalPort();
    net::UdpSocket b(std::move(a));
    CHECK(b.IsOpen() && b.LocalPort() == port);
    CHECK(!a.IsOpen() && a.LocalPort() == 0);

    // The moved-to socket owns the handle, so a datagram sent to the port reaches it.
    net::UdpSocket peer;
    CHECK(peer.Open(Loopback(0)));
    const unsigned char tag = 0x42;
    CHECK(peer.SendTo(Loopback(port), &tag, 1) == net::SendResult::Ok);
    unsigned char buf[4] = { 0, 0, 0, 0 };
    std::size_t len = 0;
    net::NetAddress from;
    CHECK(WaitRecv(b, buf, sizeof(buf), len, from, Deadline()) == net::RecvResult::Ok);
    CHECK(len == 1 && buf[0] == tag);

    // Move-assigning over an open socket closes the handle it held, so that port is free again.
    net::UdpSocket c;
    CHECK(c.Open(Loopback(0)));
    const uint16_t oldPort = c.LocalPort();
    c = std::move(b);
    CHECK(c.IsOpen() && c.LocalPort() == port);
    CHECK(!b.IsOpen());
    net::UdpSocket reuse;
    CHECK(reuse.Open(Loopback(oldPort)));
}

int main() {
    struct {
        const char* name;
        void (*fn)();
    } cases[] = {
        { "address parse and format round trip", CaseAddressRoundTrip },
        { "malformed addresses rejected", CaseAddressRejects },
        { "IPv6 address text (O5.3)", CaseIpv6AddressText },
        { "ephemeral bind on 127.0.0.1", CaseEphemeralBind },
        { "empty receive returns WouldBlock", CaseEmptyReceive },
        { "closed socket refuses I/O", CaseClosedRefuses },
        { "100 datagrams exchanged, sizes 0 to 1200", CaseExchange },
        { "two sockets on [::1] (O5.3)", CaseIpv6Loopback },
        { "a dual-stack socket reaches IPv4 and IPv6 (O5.3)", CaseDualStack },
        { "oversize datagram dropped, no overflow", CaseOversizeDropped },
        { "send to a closed port leaves receives working", CaseSendToClosedPort },
        { "open and close cycles", CaseReopenCycles },
        { "move semantics", CaseMoveSemantics },
    };
    const int numCases = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int failedCases = 0;

    std::printf("=== G-Diffuser UDP socket harness (port/net/gdx_net_socket.cpp) ===\n\n");
    for (int i = 0; i < numCases; i++) {
        const int failsBefore = gFails;
        const int checksBefore = gChecks;
        std::printf("-- %s\n", cases[i].name);
        cases[i].fn();
        if (gFails > failsBefore) {
            failedCases++;
            std::printf("[FAIL] %s (%d/%d sub-checks failed)\n\n", cases[i].name, gFails - failsBefore,
                        gChecks - checksBefore);
        } else {
            std::printf("[PASS] %s (%d sub-checks)\n\n", cases[i].name, gChecks - checksBefore);
        }
    }
    std::printf("=== Summary: %d/%d cases passed (%d/%d sub-checks passed) ===\n", numCases - failedCases,
                numCases, gChecks - gFails, gChecks);
    return failedCases ? 1 : 0;
}
