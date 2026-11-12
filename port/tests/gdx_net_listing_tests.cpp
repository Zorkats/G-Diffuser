// port/tests/gdx_net_listing_tests.cpp: a hosted room's directory listing: register, heartbeat, takeover (G-NET O5.10).
// Copyright (c) Zorkats
//
// The listing talks to a fake directory that applies the CONTRACT.md §5 rules to the real client's requests: a live
// record per code, a takeover hash, code_taken, not_found and bad_secret. The fake can drop an answer after applying
// the write, which is the ambiguous case the listing has to settle.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "net/gdx_net_directory.h"
#include "net/gdx_net_listing.h"

#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond)

namespace {

namespace dir = gdx::net::directory;
namespace ls = gdx::net::listing;
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

const std::string kHostKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kBase = "https://dir.example";

// The fake directory checks no signature, so a fixed one stands in; the directory tests sign for real.
dir::Client SignedClient(std::vector<std::string> bases, dir::Transport transport) {
    dir::Client client(std::move(bases), std::move(transport));
    client.SetSigner([](std::string_view, dir::Signature& out) {
        out.fill(0x5A);
        return true;
    });
    return client;
}

std::string Json(int ttl) {
    return "{\"ttl\":" + std::to_string(ttl) + "}";
}

std::string ErrorBody(const char* token) {
    return std::string("{\"error\":\"") + token + "\"}";
}

// The directory's rules, as far as a listing can see them (CONTRACT.md §5.1 to §5.3).
struct FakeDirectory {
    struct Row {
        std::string hash;
        uint32_t expiresAt = 0;
        uint32_t humans = 0;
    };
    std::map<std::string, Row> rows;
    uint32_t now = 0;
    std::vector<std::string> calls; // "register ABC234", "heartbeat ABC234", ...
    int dropAfterApply = 0;          // answers lost after the write was applied
    int lostBeforeApply = 0;         // requests lost before they arrive
    int rateLimited = 0;             // 429s to give, with Retry-After 40
    int internal = 0;                // 500 internal, nothing applied
    int badSignature = 0;            // 403 bad_signature, nothing applied
    bool sawSig = true;              // every body so far ended with a sig

    bool Live(const std::string& code) const {
        auto it = rows.find(code);
        return it != rows.end() && static_cast<int32_t>(it->second.expiresAt - now) > 0;
    }

    dir::Response Answer(int status, const std::string& body) {
        dir::Response r;
        r.ok = true;
        r.status = status;
        r.body = body;
        return r;
    }

    dir::Response Handle(const dir::Request& request) {
        if (lostBeforeApply > 0) {
            lostBeforeApply--;
            return dir::Response();
        }
        if (rateLimited > 0) {
            rateLimited--;
            dir::Response r = Answer(429, ErrorBody("rate_limited"));
            r.retryAfter = "40";
            return r;
        }
        if (internal > 0) {
            internal--;
            return Answer(500, ErrorBody("internal"));
        }
        dir::JsonValue body;
        if (!dir::ParseStrictJson(request.body, body) || body.kind != dir::JsonValue::Kind::Object) {
            return Answer(400, ErrorBody("bad_json"));
        }
        sawSig = sawSig && !body.keys.empty() && body.keys.back() == "sig";
        if (badSignature > 0) {
            badSignature--;
            return Answer(403, ErrorBody("bad_signature"));
        }
        const std::string path = request.url.substr(kBase.size());
        const dir::JsonValue* humans = body.Find("humans");
        const uint32_t count = humans != nullptr ? static_cast<uint32_t>(humans->number) : 0;
        dir::Response answer;
        if (path == "/v1/rooms") {
            const std::string code = body.Find("code")->text;
            calls.push_back("register " + code);
            if (Live(code)) {
                return Answer(409, ErrorBody("code_taken"));
            }
            rows[code] = Row{ body.Find("takeoverHash")->text, now + 180000, count };
            answer = Answer(201, Json(180));
        } else {
            const size_t slash = path.find('/', 10);
            const std::string code = path.substr(10, slash - 10);
            const std::string op = path.substr(slash + 1);
            calls.push_back(op + " " + code);
            if (!Live(code)) {
                return Answer(404, ErrorBody("not_found"));
            }
            dir::Secret secret{};
            if (!dir::ParseSecretHex(body.Find("secret")->text, secret) ||
                dir::TakeoverHashOf(secret) != rows[code].hash) {
                return Answer(403, ErrorBody("bad_secret"));
            }
            if (op == "delist") {
                rows.erase(code);
                return Answer(200, Json(0));
            }
            rows[code].expiresAt = now + 180000;
            rows[code].humans = count;
            if (op == "takeover") {
                rows[code].hash = body.Find("nextHash")->text;
            }
            answer = Answer(200, Json(180));
        }
        if (dropAfterApply > 0) {
            dropAfterApply--;
            return dir::Response();
        }
        return answer;
    }
};

// A listing on a fake directory. Jobs run when the test says, so a call can be held in flight.
struct Rig {
    FakeDirectory fake;
    std::deque<std::function<void()>> jobs;
    bool inline_ = true;
    uint32_t seed = 1;
    std::function<dir::Response(const dir::Request&)> override; // replaces the fake when set
    ls::Listing listing;

    explicit Rig(uint32_t firstSeed = 1)
        : seed(firstSeed), listing(SignedClient({ kBase },
                              [this](const dir::Request& r) { return override ? override(r) : fake.Handle(r); }),
                  [this](std::function<void()> job) {
                      if (inline_) {
                          job();
                      } else {
                          jobs.push_back(std::move(job));
                      }
                  },
                  [this](uint8_t* out, size_t n) {
                      for (size_t i = 0; i < n; i++) {
                          seed = seed * 1103515245u + 12345u;
                          out[i] = static_cast<uint8_t>(seed >> 16);
                      }
                  }) {}

    // Advances both clocks and lets the listing act.
    void At(uint32_t now) {
        fake.now = now;
        listing.Update(now);
    }
};

dir::Draft MakeDraft(uint32_t humans) {
    dir::Draft draft;
    draft.type = dir::RoomType::Code;
    draft.humans = humans;
    draft.freeSeats = 4;
    draft.reachable = true;
    draft.protocol = 7;
    draft.hostKey = kHostKey;
    draft.endpoints.push_back(ls::EndpointOf(NetAddress::FromV4(0xCB007107u, 24302), dir::How::Stun));
    return draft;
}

void TestCodesAndEndpoints() {
    std::printf("codes and endpoints\n");
    CHECK(ls::DirectoryCode(0) == "000000");
    CHECK(ls::DirectoryCode((1u << 30) - 1) == "ZZZZZZ");
    CHECK(ls::DirectoryCode(1u << 30).empty());
    for (uint32_t value : { 0u, 1u, 12345u, 777777u, (1u << 30) - 1 }) {
        uint32_t back = 0;
        CHECK(dir::IsCode(ls::DirectoryCode(value)));
        CHECK(ls::CodeValue(ls::DirectoryCode(value), back) && back == value);
    }
    uint32_t typed = 0;
    CHECK(ls::CodeValue("abc-2l4", typed) && ls::DirectoryCode(typed) == "ABC214");
    CHECK(!ls::CodeValue("ABCDE", typed) && !ls::CodeValue("ABCDEU", typed));

    const dir::Endpoint v4 = ls::EndpointOf(NetAddress::FromV4(0xCB007107u, 24302), dir::How::Upnp);
    CHECK(v4.family == dir::Family::V4 && v4.addr == "203.0.113.7" && v4.port == 24302 && v4.how == dir::How::Upnp);
    NetAddress back;
    CHECK(ls::AddressOf(v4, back) && back == NetAddress::FromV4(0xCB007107u, 24302));

    NetAddress six;
    CHECK(gdx::net::ParseAddress("[2001:db8::1]:5000", six));
    const dir::Endpoint v6 = ls::EndpointOf(six, dir::How::Ipv6);
    CHECK(v6.family == dir::Family::V6 && v6.addr == "2001:db8::1" && v6.port == 5000 && dir::IsIpv6(v6.addr));
    CHECK(ls::AddressOf(v6, back) && back == six);

    dir::Endpoint wrong = v4;
    wrong.family = dir::Family::V6;
    CHECK(!ls::AddressOf(wrong, back));
    wrong = v4;
    wrong.port = 0;
    CHECK(!ls::AddressOf(wrong, back));
    wrong = v4;
    wrong.addr = "203.0.113";
    CHECK(!ls::AddressOf(wrong, back));
}

void TestDialable() {
    std::printf("addresses a joiner may send to\n");
    const auto v4 = [](uint32_t ip) { return NetAddress::FromV4(ip, 24302); };
    CHECK(ls::Dialable(v4(0xCB007107u), false));  // 203.0.113.7
    CHECK(ls::Dialable(v4(0xC0A8010Au), false));  // 192.168.1.10: a host on the same network
    CHECK(ls::Dialable(v4(0x0A000001u), false));  // 10.0.0.1
    CHECK(!ls::Dialable(v4(0x7F000001u), false)); // loopback
    CHECK(ls::Dialable(v4(0x7F000001u), true));   // loopback, with a local test directory
    CHECK(!ls::Dialable(v4(0x00000000u), true));  // unspecified
    CHECK(!ls::Dialable(v4(0xFFFFFFFFu), true));  // broadcast
    CHECK(!ls::Dialable(v4(0xE0000001u), true));  // multicast
    CHECK(!ls::Dialable(v4(0xF0000001u), true));  // reserved
    CHECK(!ls::Dialable(v4(0xA9FE0101u), true));  // link-local
    CHECK(!ls::Dialable(NetAddress::FromV4(0xCB007107u, 0), false));
    NetAddress six;
    CHECK(gdx::net::ParseAddress("[2001:db8::1]:5000", six) && ls::Dialable(six, false));
    CHECK(gdx::net::ParseAddress("[::1]:5000", six) && !ls::Dialable(six, false) && ls::Dialable(six, true));
    CHECK(gdx::net::ParseAddress("[::]:5000", six) && !ls::Dialable(six, true));
    CHECK(gdx::net::ParseAddress("[ff02::1]:5000", six) && !ls::Dialable(six, true));
    CHECK(gdx::net::ParseAddress("[fe80::1]:5000", six) && !ls::Dialable(six, true));
    CHECK(gdx::net::ParseAddress("[fd00::1]:5000", six) && ls::Dialable(six, false)); // unique local stays
}

void TestRegisterAndHeartbeat() {
    std::printf("register and heartbeat\n");
    Rig r;
    CHECK(r.listing.state() == ls::State::Idle);
    r.listing.Register(MakeDraft(1), 0);
    CHECK(r.listing.state() == ls::State::Working);
    r.At(10);
    CHECK(r.listing.state() == ls::State::Listed);
    const std::string code = ls::DirectoryCode(r.listing.code());
    CHECK(r.fake.calls.size() == 1 && r.fake.calls[0] == "register " + code);
    CHECK(r.fake.rows[code].hash == dir::TakeoverHashOf(r.listing.secret()));
    const uint32_t serial = r.listing.serial();

    // No heartbeat before its time; the new draft goes with the next one.
    r.listing.SetDraft(MakeDraft(3));
    r.At(ls::kHeartbeatMs - 1);
    CHECK(r.fake.calls.size() == 1);
    r.At(ls::kHeartbeatMs + 10);
    r.At(ls::kHeartbeatMs + 20);
    CHECK(r.fake.calls.size() == 2 && r.fake.calls[1] == "heartbeat " + code);
    CHECK(r.fake.rows[code].humans == 3);
    CHECK(r.listing.state() == ls::State::Listed && r.listing.serial() == serial);

    // Kept alive well past one record life.
    for (uint32_t t = 2; t <= 6; t++) {
        r.At(t * ls::kHeartbeatMs + 100);
        r.At(t * ls::kHeartbeatMs + 200);
    }
    CHECK(r.fake.Live(code) && r.listing.state() == ls::State::Listed);
}

void TestCodeTaken() {
    std::printf("code taken\n");
    // The first code this seed draws, learnt from a twin rig with the same seed.
    Rig twin;
    twin.listing.Register(MakeDraft(1), 0);
    twin.At(0);
    const std::string first = ls::DirectoryCode(twin.listing.code());

    Rig r;
    r.fake.rows[first] = FakeDirectory::Row{ std::string(64, 'a'), 10000000, 2 };
    r.listing.Register(MakeDraft(1), 0);
    const uint32_t serialAtStart = r.listing.serial();
    r.At(10);
    CHECK(r.listing.state() == ls::State::Listed);
    CHECK(ls::DirectoryCode(r.listing.code()) != first);
    CHECK(r.fake.calls.size() == 2 && r.fake.calls[0] == "register " + first);
    CHECK(r.listing.serial() > serialAtStart); // the room hears of the new code

    // Every code taken: the listing gives up after kCodeTries registers.
    Rig full;
    full.override = [&full](const dir::Request& request) {
        full.fake.calls.push_back(request.url);
        return full.fake.Answer(409, ErrorBody("code_taken"));
    };
    full.listing.Register(MakeDraft(1), 0);
    full.At(0);
    CHECK(full.listing.state() == ls::State::Failed && full.listing.error() == dir::Error::CodeTaken);
    CHECK(full.fake.calls.size() == static_cast<size_t>(ls::kCodeTries));
}

void TestAmbiguousRegister() {
    std::printf("register applied, answer lost\n");
    Rig r;
    r.fake.dropAfterApply = 1;
    r.listing.Register(MakeDraft(1), 0);
    r.At(0);
    const std::string code = ls::DirectoryCode(r.listing.code());
    CHECK(r.listing.state() == ls::State::Working && r.listing.error() == dir::Error::Unreachable);
    CHECK(r.fake.Live(code));
    // The retry hears code_taken for its own record, asks with a heartbeat, and keeps the code.
    r.At(ls::kRetryMs);
    r.At(ls::kRetryMs + 1);
    CHECK(r.listing.state() == ls::State::Listed);
    CHECK(ls::DirectoryCode(r.listing.code()) == code);
    CHECK(r.fake.calls.size() == 3 && r.fake.calls[1] == "register " + code && r.fake.calls[2] == "heartbeat " + code);

    // A register lost before it arrived is simply sent again.
    Rig lost;
    lost.fake.lostBeforeApply = 1;
    lost.listing.Register(MakeDraft(1), 0);
    lost.At(0);
    CHECK(lost.listing.state() == ls::State::Working);
    lost.At(ls::kRetryMs);
    CHECK(lost.listing.state() == ls::State::Listed && lost.fake.calls.size() == 1);
}

void TestRateLimitAndInternal() {
    std::printf("rate limit and internal\n");
    Rig r;
    r.fake.rateLimited = 1;
    r.listing.Register(MakeDraft(1), 0);
    r.At(0);
    CHECK(r.listing.state() == ls::State::Working);
    r.At(39999);
    CHECK(r.listing.state() == ls::State::Working); // Retry-After 40 s is longer than kRetryMs
    r.At(40000);
    CHECK(r.listing.state() == ls::State::Listed);

    // A heartbeat that meets a 500 is retried; the record lives on.
    const std::string code = ls::DirectoryCode(r.listing.code());
    r.fake.internal = 1;
    r.At(40000 + ls::kHeartbeatMs);
    CHECK(r.listing.error() == dir::Error::Internal && r.fake.calls.size() == 1);
    r.At(40000 + ls::kHeartbeatMs + ls::kRetryMs);
    CHECK(r.listing.state() == ls::State::Listed && r.fake.calls.back() == "heartbeat " + code);
}

void TestRecordExpired() {
    std::printf("record expired while unreachable\n");
    Rig r;
    r.listing.Register(MakeDraft(1), 0);
    r.At(0);
    const std::string code = ls::DirectoryCode(r.listing.code());
    const dir::Secret secret = r.listing.secret();
    // The directory is unreachable for longer than a record lives.
    r.fake.lostBeforeApply = 100;
    uint32_t t = ls::kHeartbeatMs;
    while (t < 200000) {
        r.At(t);
        t += ls::kRetryMs;
    }
    CHECK(!r.fake.Live(code));
    r.fake.lostBeforeApply = 0;
    r.At(t);
    r.At(t + 1);
    CHECK(r.listing.state() == ls::State::Listed && ls::DirectoryCode(r.listing.code()) == code);
    CHECK(r.listing.secret() == secret);
    CHECK(r.fake.calls.back() == "register " + code);
}

void TestTakeover() {
    std::printf("takeover by the next host\n");
    Rig old;
    old.listing.Register(MakeDraft(2), 0);
    old.At(0);
    const uint32_t code = old.listing.code();
    const std::string text = ls::DirectoryCode(code);

    // The successor shares the directory: its listing runs against the same fake.
    Rig next(2);
    next.fake.rows = old.fake.rows;
    next.fake.now = 30000;
    next.listing.Takeover(code, old.listing.secret(), MakeDraft(1), 30000);
    next.At(30000);
    CHECK(next.listing.state() == ls::State::Listed && next.listing.code() == code);
    CHECK(next.listing.secret() != old.listing.secret());
    CHECK(next.fake.rows[text].hash == dir::TakeoverHashOf(next.listing.secret()));

    // The old host's secret is dead: a heartbeat from it fails for good, which is its signal to step down.
    old.fake.rows = next.fake.rows;
    old.At(ls::kHeartbeatMs);
    CHECK(old.listing.state() == ls::State::Failed && old.listing.error() == dir::Error::BadSecret);

    // A wrong secret is refused.
    Rig thief(3);
    thief.fake.rows = next.fake.rows;
    thief.fake.now = 30000;
    thief.listing.Takeover(code, dir::Secret{}, MakeDraft(1), 30000);
    thief.At(30000);
    CHECK(thief.listing.state() == ls::State::Failed && thief.listing.error() == dir::Error::BadSecret);
    CHECK(thief.fake.rows[text].hash == dir::TakeoverHashOf(next.listing.secret()));
}

void TestTakeoverEdges() {
    std::printf("takeover edges\n");
    Rig old;
    old.listing.Register(MakeDraft(2), 0);
    old.At(0);
    const uint32_t code = old.listing.code();
    const std::string text = ls::DirectoryCode(code);

    // Applied, answer lost: the retry meets bad_secret and finds the new secret already in place.
    Rig next(2);
    next.fake.rows = old.fake.rows;
    next.fake.dropAfterApply = 1;
    next.listing.Takeover(code, old.listing.secret(), MakeDraft(1), 0);
    const uint32_t serial = next.listing.serial();
    next.At(0);
    CHECK(next.listing.state() == ls::State::Working);
    next.At(ls::kRetryMs);
    next.At(ls::kRetryMs + 1);
    CHECK(next.listing.state() == ls::State::Listed);
    CHECK(next.fake.rows[text].hash == dir::TakeoverHashOf(next.listing.secret()));
    CHECK(next.listing.serial() > serial);
    CHECK(next.fake.calls.size() == 3 && next.fake.calls[1] == "takeover " + text && next.fake.calls[2] == "heartbeat " + text);

    // The record ran out before the takeover: the successor registers the same code under its new secret.
    Rig late(4);
    late.fake.rows = old.fake.rows;
    late.fake.now = 200000;
    late.listing.Takeover(code, old.listing.secret(), MakeDraft(1), 200000);
    late.At(200000);
    CHECK(late.listing.state() == ls::State::Listed && late.listing.code() == code);
    CHECK(late.listing.secret() != old.listing.secret());
    CHECK(late.fake.rows[text].hash == dir::TakeoverHashOf(late.listing.secret()));
    CHECK(late.fake.calls.size() == 2 && late.fake.calls[1] == "register " + text);
}

void TestRotateSecret() {
    std::printf("the secret rotates when its holder stops being the successor\n");
    Rig r;
    r.listing.Register(MakeDraft(2), 0);
    r.At(0);
    const uint32_t code = r.listing.code();
    const std::string text = ls::DirectoryCode(code);
    const dir::Secret leaked = r.listing.secret();
    const uint32_t serial = r.listing.serial();
    r.listing.RotateSecret(10);
    CHECK(r.listing.state() == ls::State::Listed); // the code stays on show meanwhile
    r.At(10);
    CHECK(r.listing.state() == ls::State::Listed && r.listing.code() == code);
    CHECK(r.listing.secret() != leaked && r.listing.serial() > serial);
    CHECK(r.fake.rows[text].hash == dir::TakeoverHashOf(r.listing.secret()));
    CHECK(r.fake.calls.size() == 2 && r.fake.calls[1] == "takeover " + text);

    // The old successor can neither take the code nor refresh it.
    Rig thief(3);
    thief.fake.rows = r.fake.rows;
    thief.fake.now = 20;
    thief.listing.Takeover(code, leaked, MakeDraft(1), 20);
    thief.At(20);
    CHECK(thief.listing.state() == ls::State::Failed && thief.listing.error() == dir::Error::BadSecret);
    CHECK(thief.fake.rows[text].hash == dir::TakeoverHashOf(r.listing.secret()));

    // The heartbeats go on under the new secret.
    r.At(10 + ls::kHeartbeatMs);
    CHECK(r.listing.state() == ls::State::Listed && r.fake.calls.back() == "heartbeat " + text);

    // Asked while the register is out: the rotation waits for its answer.
    Rig held(5);
    held.inline_ = false;
    held.listing.Register(MakeDraft(1), 0);
    held.At(0);
    held.listing.RotateSecret(1);
    CHECK(held.jobs.size() == 1);
    held.jobs.front()();
    held.jobs.pop_front();
    const dir::Secret first = held.listing.secret();
    held.At(2);
    CHECK(held.jobs.size() == 1 && held.listing.state() == ls::State::Listed);
    held.jobs.front()();
    held.jobs.pop_front();
    held.At(3);
    CHECK(held.listing.secret() != first);
    CHECK(held.fake.calls.size() == 2 && held.fake.calls[1].rfind("takeover ", 0) == 0);

    // Asked while a lost register waits for its retry: the retry goes first, then the rotation.
    Rig lost(6);
    lost.fake.lostBeforeApply = 1;
    lost.listing.Register(MakeDraft(1), 0);
    lost.At(0);
    CHECK(lost.listing.state() == ls::State::Working);
    lost.listing.RotateSecret(1);
    lost.At(1);
    CHECK(lost.fake.calls.empty());
    lost.At(ls::kRetryMs);
    CHECK(lost.listing.state() == ls::State::Listed);
    CHECK(lost.fake.calls.size() == 2 && lost.fake.calls[1].rfind("takeover ", 0) == 0);
    CHECK(lost.fake.rows.begin()->second.hash == dir::TakeoverHashOf(lost.listing.secret()));
}

void TestInFlight() {
    std::printf("a call in flight\n");
    Rig r;
    r.inline_ = false;
    r.listing.Register(MakeDraft(1), 0);
    r.At(5);
    CHECK(r.listing.state() == ls::State::Working && r.jobs.size() == 1);
    CHECK(r.fake.calls.empty());
    r.At(100000); // nothing new is sent while one call is out
    CHECK(r.jobs.size() == 1);
    r.jobs.front()();
    r.jobs.pop_front();
    r.At(100001);
    CHECK(r.listing.state() == ls::State::Listed);
    // The heartbeat is timed from the answer, not from the send.
    r.At(100001 + ls::kHeartbeatMs - 1);
    CHECK(r.jobs.empty());
    r.At(100001 + ls::kHeartbeatMs);
    CHECK(r.jobs.size() == 1);

    // A new register drops the answer of the call still out.
    r.listing.Register(MakeDraft(1), 200000);
    CHECK(r.jobs.size() == 2);
    r.jobs.front()(); // the old heartbeat finishes into a slot nobody reads
    r.jobs.pop_front();
    r.At(200001);
    CHECK(r.listing.state() == ls::State::Working);
    r.jobs.front()();
    r.jobs.pop_front();
    r.At(200002);
    CHECK(r.listing.state() == ls::State::Listed);
}

void TestBadConfig() {
    std::printf("bad config and invalid drafts\n");
    Rig r;
    dir::Draft draft = MakeDraft(1);
    draft.endpoints.clear();
    r.listing.Register(draft, 0);
    r.At(0);
    CHECK(r.listing.state() == ls::State::Failed && r.listing.error() == dir::Error::Invalid);
    CHECK(r.fake.calls.empty());

    ls::Listing http(SignedClient({ "http://dir.example" }, [](const dir::Request&) { return dir::Response(); }),
                     [](std::function<void()> job) { job(); }, [](uint8_t* out, size_t n) {
                         for (size_t i = 0; i < n; i++) {
                             out[i] = 0x42;
                         }
                     });
    http.Register(MakeDraft(1), 0);
    http.Update(0);
    CHECK(http.state() == ls::State::Failed && http.error() == dir::Error::BadConfig);
}

// A host that ends its room with nobody to take it deletes the record (CONTRACT.md §5.8, decision 54), and a refused
// signature fails the listing like any refusal it cannot fix (§8a).
void TestDelistAndSignature() {
    std::printf("delist and signed writes\n");
    Rig r;
    r.listing.Register(MakeDraft(1), 0);
    r.At(0);
    CHECK(r.listing.state() == ls::State::Listed);
    const std::string code = ls::DirectoryCode(r.listing.code());
    CHECK(r.fake.Live(code));
    r.listing.Delist();
    CHECK(r.listing.state() == ls::State::Idle);
    CHECK(!r.fake.Live(code));
    CHECK(r.fake.calls.size() == 2 && r.fake.calls[1] == "delist " + code);
    CHECK(r.fake.sawSig);
    // Nothing more goes out: no heartbeat is due, and a second delist has nothing to delete.
    r.At(ls::kHeartbeatMs + 1);
    r.listing.Delist();
    CHECK(r.fake.calls.size() == 2);

    Rig idle;
    idle.listing.Delist();
    CHECK(idle.fake.calls.empty());

    Rig refused;
    refused.fake.badSignature = 1;
    refused.listing.Register(MakeDraft(1), 0);
    refused.At(0);
    CHECK(refused.listing.state() == ls::State::Failed && refused.listing.error() == dir::Error::BadSignature);
    refused.listing.Delist();
    CHECK(refused.fake.calls.empty());  // a failed listing holds no record
}

} // namespace

int main() {
    TestCodesAndEndpoints();
    TestDialable();
    TestRegisterAndHeartbeat();
    TestCodeTaken();
    TestAmbiguousRegister();
    TestRateLimitAndInternal();
    TestRecordExpired();
    TestTakeover();
    TestTakeoverEdges();
    TestRotateSecret();
    TestInFlight();
    TestBadConfig();
    TestDelistAndSignature();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
