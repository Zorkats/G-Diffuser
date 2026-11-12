// port/tests/gdx_net_host_info_tests.cpp: which local addresses a room lists, and how the log writes an address
// (G-NET decision 59).
// Copyright (c) Zorkats
//
// PreferTemporary and LogText are pure; the addresses are built here. LocalAddresses runs once against this machine,
// checked only for its shape, and nothing it returns is printed.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "net/gdx_net_host_info.h"

#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond)

namespace {

namespace hi = gdx::net::host_info;
using gdx::net::NetAddress;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

NetAddress V6(std::initializer_list<uint16_t> groups, uint16_t port = 0) {
    NetAddress a;
    size_t i = 0;
    for (uint16_t g : groups) {
        a.ip[i++] = static_cast<uint8_t>(g >> 8);
        a.ip[i++] = static_cast<uint8_t>(g);
    }
    a.port = port;
    return a;
}

hi::LocalAddress Local(const NetAddress& a, bool temporary = false, bool deprecated = false) {
    hi::LocalAddress l;
    l.address = a;
    l.temporary = temporary;
    l.deprecated = deprecated;
    return l;
}

const NetAddress kLan = NetAddress::FromV4(0xC0A80114u, 0);                          // 192.168.1.20
const NetAddress kStable = V6({ 0x2a02, 0x1234, 0x5678, 0x9abc, 0x1111, 0x2222, 0x3333, 0x4444 });
const NetAddress kTemp = V6({ 0x2a02, 0x1234, 0x5678, 0x9abc, 0xaaaa, 0xbbbb, 0xcccc, 0xdddd });
const NetAddress kOldTemp = V6({ 0x2a02, 0x1234, 0x5678, 0x9abc, 0x5555, 0x6666, 0x7777, 0x8888 });
const NetAddress kLinkLocal = V6({ 0xfe80, 0, 0, 0, 0x1111, 0x2222, 0x3333, 0x4444 });
const NetAddress kUla = V6({ 0xfd00, 0x1, 0x2, 0x3, 0, 0, 0, 1 });

bool Has(const std::vector<NetAddress>& list, const NetAddress& a) {
    return std::find(list.begin(), list.end(), a) != list.end();
}

void TestTemporaryWins() {
    std::printf("a temporary global address hides the stable one\n");
    const std::vector<NetAddress> kept =
        hi::PreferTemporary({ Local(kLan), Local(kStable), Local(kTemp, true), Local(kLinkLocal), Local(kUla) });
    CHECK(kept.size() == 4);
    CHECK(Has(kept, kLan) && Has(kept, kTemp) && Has(kept, kLinkLocal) && Has(kept, kUla));
    CHECK(!Has(kept, kStable));
    CHECK(kept[0] == kLan && kept[1] == kTemp); // in order
}

void TestStableAlone() {
    std::printf("no temporary address: the stable one stays\n");
    const std::vector<NetAddress> kept = hi::PreferTemporary({ Local(kLan), Local(kStable) });
    CHECK(kept.size() == 2 && Has(kept, kStable));
}

void TestDeprecated() {
    std::printf("deprecated addresses go, and do not hide the stable one\n");
    std::vector<NetAddress> kept = hi::PreferTemporary({ Local(kStable), Local(kOldTemp, true, true) });
    CHECK(kept.size() == 1 && kept[0] == kStable);
    kept = hi::PreferTemporary({ Local(kStable), Local(kOldTemp, true, true), Local(kTemp, true) });
    CHECK(kept.size() == 1 && kept[0] == kTemp);
    kept = hi::PreferTemporary({ Local(kStable, false, true) });
    CHECK(kept.empty());
    // IPv4 has no such state; a flag set on one by mistake does not drop it.
    kept = hi::PreferTemporary({ Local(kLan, false, true) });
    CHECK(kept.size() == 1);
}

void TestRepeats() {
    std::printf("repeats are listed once\n");
    const std::vector<NetAddress> kept = hi::PreferTemporary({ Local(kTemp, true), Local(kLan), Local(kTemp, true) });
    CHECK(kept.size() == 2);
}

void TestLogText() {
    std::printf("log text cuts public addresses\n");
    CHECK(hi::LogText(NetAddress::FromV4(0xCB007107u, 24302)) == "203.0.113.0:24302 (/24)");
    CHECK(hi::LogText(NetAddress::FromV4(0xC0A80114u, 24302)) == "192.168.1.20:24302");
    CHECK(hi::LogText(NetAddress::FromV4(0x0A000005u, 1)) == "10.0.0.5:1");
    CHECK(hi::LogText(NetAddress::FromV4(0xAC1F0001u, 1)) == "172.31.0.1:1");
    CHECK(hi::LogText(NetAddress::FromV4(0xAC200001u, 1)) == "172.32.0.0:1 (/24)"); // just past 172.16/12
    CHECK(hi::LogText(NetAddress::FromV4(0x64400102u, 1)) == "100.64.1.2:1");     // shared, carrier-grade NAT
    CHECK(hi::LogText(NetAddress::FromV4(0x64800102u, 1)) == "100.128.1.0:1 (/24)");
    CHECK(hi::LogText(NetAddress::FromV4(0x7F000001u, 1)) == "127.0.0.1:1");
    CHECK(hi::LogText(NetAddress::FromV4(0xA9FE0101u, 1)) == "169.254.1.1:1");
    CHECK(hi::LogText(V6({ 0x2a02, 0x1234, 0x5678, 0x9abc, 1, 2, 3, 4 }, 24302)) == "[2a02:1234:5678::]:24302 (/48)");
    CHECK(hi::LogText(V6({ 0x2001, 0xdb8, 0x1, 0x2, 0, 0, 0, 1 }, 7)) == "[2001:db8:1::]:7 (/48)");
    CHECK(hi::LogText(V6({ 0xfe80, 0, 0, 0, 0x1111, 0x2222, 0x3333, 0x4444 }, 7)) ==
          "[fe80::1111:2222:3333:4444]:7");
    CHECK(hi::LogText(V6({ 0xfd00, 0x1, 0x2, 0x3, 0, 0, 0, 1 }, 7)) == "[fd00:1:2:3::1]:7");
}

void TestIfInet6() {
    std::printf("/proc/net/if_inet6 lines\n");
    const std::string text = "2a021234567890bc1111222233334444 02 40 00 00     eth0\n"
                             "2a021234567890bcaaaabbbbccccdddd 02 40 00 01     eth0\n"
                             "2a021234567890bc5555666677778888 02 40 00 21     eth0\n"
                             "fe800000000000001111222233334444 02 40 20 80     eth0\n"
                             "00000000000000000000000000000001 01 80 10 80       lo\n"
                             "2a02123456789xbc5555666677778888 02 40 00 01     eth0\n" // not hex
                             "2a021234 02 40 00 01 eth0\n"                               // short
                             "2a021234567890bc5555666677778889 02 40 00\n";               // no flags
    const std::vector<hi::LocalAddress> list = hi::ParseIfInet6(text);
    CHECK(list.size() == 5);
    if (list.size() == 5) {
        CHECK(list[0].address == V6({ 0x2a02, 0x1234, 0x5678, 0x90bc, 0x1111, 0x2222, 0x3333, 0x4444 }));
        CHECK(!list[0].temporary && !list[0].deprecated);
        CHECK(list[1].temporary && !list[1].deprecated);
        CHECK(list[2].temporary && list[2].deprecated);
        CHECK(!list[3].temporary && !list[3].deprecated); // 0x80 is IFA_F_PERMANENT
        const std::vector<NetAddress> kept = hi::PreferTemporary(list);
        CHECK(kept.size() == 3 && kept[0] == list[1].address && kept[1] == list[3].address);
    }
    CHECK(hi::ParseIfInet6("").empty());
}

void TestThisMachine() {
    std::printf("this machine's addresses: ports 0, no repeats\n");
    const std::vector<NetAddress> local = hi::LocalAddresses();
    bool ports = true;
    bool repeats = false;
    for (size_t i = 0; i < local.size(); i++) {
        ports = ports && local[i].port == 0;
        for (size_t j = i + 1; j < local.size(); j++) {
            repeats = repeats || local[i] == local[j];
        }
    }
    CHECK(ports && !repeats);
    const size_t global = static_cast<size_t>(std::count_if(local.begin(), local.end(), [](const NetAddress& a) {
        return !a.IsV4() && (a.ip[0] & 0xE0u) == 0x20u;
    }));
    std::printf("  %zu address(es), %zu global IPv6\n", local.size(), global);
}

} // namespace

int main() {
    TestTemporaryWins();
    TestStableAlone();
    TestDeprecated();
    TestRepeats();
    TestLogText();
    TestIfInet6();
    TestThisMachine();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
