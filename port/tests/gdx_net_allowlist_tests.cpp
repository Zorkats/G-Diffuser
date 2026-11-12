// port/tests/gdx_net_allowlist_tests.cpp -- the signed stable allowlist: format, signature, sequence, trust window,
// the gate and the reject text (G-NET O5.9).
// Copyright (c) Zorkats
//
// Usage: gdx_net_allowlist_tests [fuzz rounds per kind, default 3000]
//
// The keys come from fixed seeds inside this file and are throwaway. Each row of the ODD 9.1 and plan decision 30
// matrix has its own check, then the framing, the sequence rule, the state blob, and a seeded fuzz of the decoder.

#include "gdx_net_allowlist.h"

#include "monocypher-ed25519.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

namespace allow = gdx::net::allowlist;
using Bytes = std::vector<uint8_t>;
using Verdict = allow::Verdict;
using Refusal = allow::Refusal;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void ExpectVerdict(Verdict got, Verdict want, const std::string& what) {
    Check(got == want, what + ": got " + allow::VerdictName(got));
}

void ExpectRefusal(Refusal got, Refusal want, const std::string& what) {
    Check(got == want, what + ": got " + allow::RefusalName(got));
}

// Throwaway keys. Nothing here is a release key.
struct Keys {
    allow::PublicKey pub{};
    allow::SecretKey sec{};
};

Keys MakeKeys(uint8_t salt) {
    allow::Seed seed{};
    for (size_t i = 0; i < seed.size(); i++) {
        seed[i] = static_cast<uint8_t>(salt + 7 * i);
    }
    Keys keys;
    allow::KeyPairFromSeed(seed, keys.pub, keys.sec);
    return keys;
}

// The list the owner would publish: 1.3.0 current, 1.2.1 still allowed, 1.2.0 yanked.
allow::List Fixture() {
    allow::List list;
    list.protocol = 3;
    list.sequence = 10;
    list.issued = 1762905600ull;
    list.current = "1.3.0";
    list.allowed.push_back("1.3.0");
    list.allowed.push_back("1.2.1");
    allow::Yank yank;
    yank.tag = "1.2.0";
    yank.reason = "Desyncs in the race intro";
    list.yanked.push_back(yank);
    return list;
}

Bytes Signed(const allow::List& list, const Keys& keys) {
    Bytes out;
    allow::Sign(list, keys.sec, out);
    return out;
}

bool SameList(const allow::List& a, const allow::List& b) {
    if (a.protocol != b.protocol || a.sequence != b.sequence || a.issued != b.issued || a.graceUntil != b.graceUntil ||
        a.current != b.current || a.allowed != b.allowed || a.yanked.size() != b.yanked.size()) {
        return false;
    }
    for (size_t i = 0; i < a.yanked.size(); i++) {
        if (a.yanked[i].tag != b.yanked[i].tag || a.yanked[i].reason != b.yanked[i].reason) {
            return false;
        }
    }
    return true;
}

void CheckRoundTrip(const Keys& trusted) {
    const allow::List fixture = Fixture();
    const Bytes bytes = Signed(fixture, trusted);
    Check(bytes.size() > 5 && bytes[0] == 'G' && bytes[1] == 'N' && bytes[2] == 'A' && bytes[3] == 'L' &&
              bytes[4] == 1,
          "the bytes open with GNAL and format version 1");

    allow::List decoded;
    ExpectVerdict(allow::Decode(bytes, trusted.pub, decoded), Verdict::Ok, "a good signature decodes");
    Check(SameList(decoded, fixture), "the decoded fields equal the signed fields");
    Bytes again;
    Check(allow::Sign(decoded, trusted.sec, again) && again == bytes, "signing the decoded list gives the same bytes");

    allow::State state;
    ExpectVerdict(allow::Accept(bytes, trusted.pub, 1000, state), Verdict::Ok, "a good list is accepted");
    ExpectRefusal(allow::Admit(state, "1.3.0", "stable", 3, 1000, 0), Refusal::Allowed, "the current release is admitted");
    ExpectRefusal(allow::Admit(state, "1.2.1", "stable", 3, 1000, 0), Refusal::Allowed,
                  "a listed older stable release is admitted when no deadline is set");
}

void CheckSigning(const Keys& trusted) {
    // Each row breaks one rule. Sign must refuse it and write nothing.
    struct Broken {
        const char* what;
        allow::List list;
    };
    std::vector<Broken> broken;
    auto add = [&broken](const char* what, const allow::List& list) { broken.push_back(Broken{what, list}); };

    {
        allow::List l = Fixture();
        l.current = "1.3 0";
        add("a space in the current tag", l);
    }
    {
        allow::List l = Fixture();
        l.current = "";
        add("an empty current tag", l);
    }
    {
        allow::List l = Fixture();
        l.current = "1.9.9";
        add("a current tag that is not in allowed", l);
    }
    {
        allow::List l = Fixture();
        l.allowed.clear();
        add("an empty allowed list", l);
    }
    {
        allow::List l = Fixture();
        l.current = std::string(49, 'a');
        l.allowed.clear();
        l.allowed.push_back(l.current);
        add("a 49-byte tag", l);
    }
    {
        allow::List l = Fixture();
        l.allowed[1] = std::string("1.2.1") + char(0x01);
        add("a control byte in an allowed tag", l);
    }
    {
        allow::List l = Fixture();
        l.allowed[1] = std::string("1.2.1") + char(0xC3);
        add("a byte above ASCII in an allowed tag", l);
    }
    {
        allow::List l = Fixture();
        l.allowed.push_back("1.3.0");
        add("a repeated allowed tag", l);
    }
    {
        allow::List l = Fixture();
        l.yanked[0].reason = std::string(121, 'r');
        add("a 121-byte reason", l);
    }
    {
        allow::List l = Fixture();
        l.yanked[0].reason = std::string("bad\tword");
        add("a tab in a reason", l);
    }
    {
        allow::List l = Fixture();
        l.yanked[0].reason = std::string("bad") + char(0x7F);
        add("a DEL byte in a reason", l);
    }
    {
        allow::List l = Fixture();
        allow::Yank again;
        again.tag = "1.2.0";
        again.reason = "second";
        l.yanked.push_back(again);
        add("a repeated yanked tag", l);
    }
    {
        allow::List l = Fixture();
        l.yanked[0].tag = "1.2 0";
        add("a space in a yanked tag", l);
    }
    {
        allow::List l = Fixture();
        l.allowed.clear();
        for (int i = 0; i <= 255; i++) {
            l.allowed.push_back("t" + std::to_string(i));
        }
        l.current = "t0";
        add("256 allowed tags", l);
    }

    for (const Broken& row : broken) {
        Bytes out;
        const bool ok = allow::Sign(row.list, trusted.sec, out);
        Check(!ok && out.empty() && allow::CheckList(row.list) != nullptr,
              std::string("Sign refuses ") + row.what);
    }

    // The limits themselves are allowed: 48 and 120 bytes, an empty reason, 255 allowed tags.
    allow::List edge = Fixture();
    edge.current = std::string(48, 'x');
    edge.allowed.clear();
    edge.allowed.push_back(edge.current);
    edge.yanked[0].tag = "1.2.0";
    edge.yanked[0].reason = std::string(120, 'r');
    Bytes edgeBytes;
    Check(allow::Sign(edge, trusted.sec, edgeBytes), "a 48-byte tag and a 120-byte reason sign");
    allow::List edgeBack;
    ExpectVerdict(allow::Decode(edgeBytes, trusted.pub, edgeBack), Verdict::Ok, "a 48-byte tag and a 120-byte reason decode");

    allow::List empty = Fixture();
    allow::Yank blank;
    blank.tag = "1.1.0";
    blank.reason = "";
    empty.yanked.push_back(blank);
    Bytes emptyBytes;
    Check(allow::Sign(empty, trusted.sec, emptyBytes), "an empty reason signs");
    allow::List emptyBack;
    ExpectVerdict(allow::Decode(emptyBytes, trusted.pub, emptyBack), Verdict::Ok, "an empty reason decodes");

    allow::List full = Fixture();
    full.allowed.clear();
    for (int i = 0; i < 255; i++) {
        full.allowed.push_back("t" + std::to_string(i));
    }
    full.current = "t0";
    Bytes fullBytes;
    Check(allow::Sign(full, trusted.sec, fullBytes), "255 allowed tags sign");
    allow::List fullBack;
    ExpectVerdict(allow::Decode(fullBytes, trusted.pub, fullBack), Verdict::Ok, "255 allowed tags decode");
    Check(fullBack.allowed.size() == 255, "all 255 allowed tags come back");
}

void CheckSignature(const Keys& trusted, const Keys& other) {
    const Bytes good = Signed(Fixture(), trusted);
    Bytes foreign;
    allow::Sign(Fixture(), other.sec, foreign);
    allow::List out;
    out.current = "KEEP";
    ExpectVerdict(allow::Decode(foreign, trusted.pub, out), Verdict::BadSignature,
                  "a list signed by another key is refused");
    Check(out.current == "KEEP", "a refused list leaves the output alone");

    // One flipped byte anywhere breaks the signature, or breaks the structure before the signature is reached.
    for (size_t i = 0; i < good.size(); i++) {
        Bytes flipped = good;
        flipped[i] ^= 0x01;
        const std::string where = "a flipped byte at " + std::to_string(i);
        const Verdict verdict = allow::Decode(flipped, trusted.pub, out);
        Check(verdict != Verdict::Ok, where + " is refused");
        if (i >= good.size() - allow::kSignatureBytes) {
            ExpectVerdict(verdict, Verdict::BadSignature, where + " in the signature");
        }
        allow::State state;
        Check(allow::Accept(flipped, trusted.pub, 1000, state) != Verdict::Ok && !state.held,
              where + " leaves the state alone");
    }
    Check(out.current == "KEEP", "no refused flip wrote the output");

    Bytes badMagic = good;
    badMagic[0] ^= 0x20;
    ExpectVerdict(allow::Decode(badMagic, trusted.pub, out), Verdict::Malformed, "a wrong magic is malformed");
    Bytes badVersion = good;
    badVersion[4] = 2;
    ExpectVerdict(allow::Decode(badVersion, trusted.pub, out), Verdict::Malformed, "format version 2 is malformed");
}

void CheckFraming(const Keys& trusted) {
    allow::List scratch;
    const Bytes good = Signed(Fixture(), trusted);
    for (size_t length = 0; length < good.size(); length++) {
        Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
        allow::List out;
        out.current = "KEEP";
        const Verdict verdict = allow::Decode(cut, trusted.pub, out);
        Check(verdict == Verdict::Malformed && out.current == "KEEP",
              "a list cut to " + std::to_string(length) + " bytes is malformed");
    }

    Bytes plusOne = good;
    plusOne.push_back(0);
    ExpectVerdict(allow::Decode(plusOne, trusted.pub, scratch), Verdict::Malformed,
                  "one trailing byte is malformed");
    Bytes plusSignature = good;
    plusSignature.resize(good.size() + allow::kSignatureBytes, 0);
    ExpectVerdict(allow::Decode(plusSignature, trusted.pub, scratch), Verdict::Malformed,
                  "64 trailing bytes are malformed");

    Bytes huge(allow::kMaxBytes + 1, 0);
    ExpectVerdict(allow::Decode(huge, trusted.pub, scratch), Verdict::Malformed,
                  "a buffer past the size cap is malformed");

    // The current tag's length byte claims more than the buffer holds.
    Bytes shortCurrent = good;
    shortCurrent[33] = 0xFF;
    ExpectVerdict(allow::Decode(shortCurrent, trusted.pub, scratch), Verdict::Malformed,
                  "a length byte past the end is malformed");
    // The allowed count claims more tags than are present. The walk then runs into the signature, so the result
    // is refused either way; the test does not depend on which verdict.
    Bytes countLie = good;
    countLie[39] = 9;
    Check(allow::Decode(countLie, trusted.pub, scratch) != Verdict::Ok,
          "a count that claims more tags than present is refused");
}

void CheckSequence(const Keys& trusted) {
    allow::List older = Fixture();
    older.sequence = 9;
    allow::List newer = Fixture();
    newer.sequence = 11;
    allow::List sameSequence = Fixture();
    sameSequence.yanked.clear();
    const Bytes ten = Signed(Fixture(), trusted);

    allow::State state;
    ExpectVerdict(allow::Accept(ten, trusted.pub, 1000, state), Verdict::Ok, "sequence 10 is accepted");
    Check(state.highestSequence == 10 && state.held && state.acceptedMonoMs == 1000,
          "the state keeps sequence 10 and its time");

    ExpectVerdict(allow::Accept(Signed(older, trusted), trusted.pub, 2000, state), Verdict::Replayed,
                  "an older list is refused");
    Check(state.list.sequence == 10 && state.acceptedMonoMs == 1000, "a refused older list leaves the held list alone");

    ExpectVerdict(allow::Accept(Signed(sameSequence, trusted), trusted.pub, 2000, state), Verdict::Replayed,
                  "a different list under the held sequence is refused");
    Check(state.acceptedMonoMs == 1000, "a refused list under the held sequence leaves the trust window alone");

    ExpectVerdict(allow::Accept(ten, trusted.pub, 3000, state), Verdict::Ok, "the same list again is a refresh");
    Check(state.acceptedMonoMs == 3000, "a refresh restarts the trust window");

    ExpectVerdict(allow::Accept(Signed(newer, trusted), trusted.pub, 4000, state), Verdict::Ok,
                  "a higher sequence is accepted");
    Check(state.highestSequence == 11 && state.acceptedMonoMs == 4000, "the state moves to sequence 11");

    ExpectVerdict(allow::Accept(ten, trusted.pub, 5000, state), Verdict::Replayed,
                  "sequence 10 is refused once 11 was accepted");
}

void CheckGate(const Keys& trusted) {
    const uint64_t t0 = 1000;
    const Bytes good = Signed(Fixture(), trusted);
    const std::string current = "1.3.0";

    allow::State none;
    Check(!allow::Trusted(none, t0), "no list held means no trust");
    ExpectRefusal(allow::Admit(none, current, "stable", 3, t0, 0), Refusal::NoList,
                  "without a list, public rooms are refused");
    Check(allow::RejectCopy(Refusal::NoList, none, current, "stable") ==
              "Public rooms need the latest stable release list, which this copy has not fetched yet. "
              "Offline and LAN still work.",
          "the NoList reject text");

    allow::State state;
    ExpectVerdict(allow::Accept(good, trusted.pub, t0, state), Verdict::Ok, "the list is accepted for the gate checks");
    Check(allow::Trusted(state, t0) && allow::Trusted(state, t0 + allow::kTrustMs - 1),
          "trusted from acceptance until 24 h");
    ExpectRefusal(allow::Admit(state, current, "stable", 3, t0 + allow::kTrustMs - 1, 0), Refusal::Allowed,
                  "trusted just inside 24 h");
    ExpectRefusal(allow::Admit(state, current, "stable", 3, t0 + allow::kTrustMs, 0), Refusal::Stale,
                  "stale at 24 h with no refresh");
    Check(allow::RejectCopy(Refusal::Stale, state, current, "stable") ==
              "Public rooms need a fresh check of the latest stable release, which has not succeeded for 24 hours. "
              "Offline and LAN still work.",
          "the Stale reject text");
    ExpectRefusal(allow::Admit(state, "1.2.0", "stable", 3, t0 + allow::kTrustMs + 5, 0), Refusal::Stale,
                  "a stale list refuses even a yanked build as stale first");

    const uint64_t refreshAt = t0 + allow::kTrustMs + 5000;
    ExpectVerdict(allow::Accept(good, trusted.pub, refreshAt, state), Verdict::Ok,
                  "a refresh of the same list is accepted after 24 h");
    const uint64_t now = refreshAt + 1000;
    ExpectRefusal(allow::Admit(state, current, "stable", 3, now, 0), Refusal::Allowed,
                  "a refresh restores trust");
    Check(!allow::Trusted(state, refreshAt - 1), "a monotonic clock that goes backwards is not trusted");

    ExpectRefusal(allow::Admit(state, "1.2.1", "stable", 2, now, 0), Refusal::ProtocolMismatch,
                  "an older protocol id is refused");
    Check(allow::RejectCopy(Refusal::ProtocolMismatch, state, "1.2.1", "stable") ==
              "This build (1.2.1) uses a different network protocol. Public rooms require 1.3.0. "
              "Offline and LAN still work.",
          "the ProtocolMismatch reject text");

    // Only the stable channel is admitted. Nightly, dev and an unknown channel are refused whatever the list says.
    ExpectRefusal(allow::Admit(state, current, "nightly", 3, now, 0), Refusal::Prerelease, "a nightly channel is refused");
    ExpectRefusal(allow::Admit(state, current, "dev", 3, now, 0), Refusal::Prerelease, "a dev channel is refused");
    ExpectRefusal(allow::Admit(state, current, "beta", 3, now, 0), Refusal::Prerelease, "an unknown channel is refused");
    ExpectRefusal(allow::Admit(state, current, "", 3, now, 0), Refusal::Prerelease, "an empty channel is refused");
    ExpectRefusal(allow::Admit(state, current, "Stable", 3, now, 0), Refusal::Prerelease,
                  "the channel name is case sensitive");

    const std::string nightlyTag = "1.4.0-nightly.20261108.a1b2c3d";
    ExpectRefusal(allow::Admit(state, nightlyTag, "nightly", 3, now, 0), Refusal::Prerelease,
                  "a nightly build is refused from public rooms");
    Check(allow::RejectCopy(Refusal::Prerelease, state, nightlyTag, "nightly") ==
              "This is a nightly (1.4.0-nightly.20261108.a1b2c3d). Public rooms require 1.3.0. "
              "Offline and LAN still work.",
          "the nightly reject text names the channel and what still works");
    Check(allow::RejectCopy(Refusal::Prerelease, state, "1.4.0-dev.7", "dev") ==
              "This is a development build (1.4.0-dev.7). Public rooms require 1.3.0. Offline and LAN still work.",
          "the dev reject text");
    Check(allow::RejectCopy(Refusal::Prerelease, state, "2.0.0", "beta") ==
              "This build (2.0.0) is not a stable release. Public rooms require 1.3.0. Offline and LAN still work.",
          "the unknown channel reject text");

    ExpectRefusal(allow::Admit(state, "1.2.0", "stable", 3, now, 0), Refusal::Yanked,
                  "a yanked build is refused");
    Check(allow::RejectCopy(Refusal::Yanked, state, "1.2.0", "stable") ==
              "This build (1.2.0) was pulled: Desyncs in the race intro. Public rooms require 1.3.0. "
              "Offline and LAN still work.",
          "the yanked reject text carries the reason");

    ExpectRefusal(allow::Admit(state, "1.3.0-custom", "stable", 3, now, 0), Refusal::NotListed,
                  "a self-compiled tag that is not listed is refused");
    Check(allow::RejectCopy(Refusal::NotListed, state, "1.3.0-custom", "stable") ==
              "This build (1.3.0-custom) is not on the list of allowed stable releases. Public rooms require 1.3.0. "
              "Offline and LAN still work.",
          "the NotListed reject text");
    ExpectRefusal(allow::Admit(state, "1.1.0", "stable", 3, now, 0), Refusal::NotListed,
                  "an older tag that is not on the list is refused");
    ExpectRefusal(allow::Admit(state, "1.2.1", "stable", 3, now, 0), Refusal::Allowed,
                  "a listed older stable release is admitted");
    Check(allow::RejectCopy(Refusal::Allowed, state, current, "stable").empty(), "an admitted build gets no reject text");

    // Grace: an older stable build is admitted until grace_until, the current build never depends on it, and a yank
    // holds inside grace.
    allow::List graced = Fixture();
    graced.sequence = 11;
    graced.graceUntil = 1800000000ull;
    allow::State withGrace;
    ExpectVerdict(allow::Accept(Signed(graced, trusted), trusted.pub, now, withGrace), Verdict::Ok,
                  "a list with a grace deadline is accepted");
    ExpectRefusal(allow::Admit(withGrace, "1.2.1", "stable", 3, now, 1799999999ull), Refusal::Allowed,
                  "an older stable build is admitted before grace_until");
    ExpectRefusal(allow::Admit(withGrace, "1.2.1", "stable", 3, now, 1800000000ull), Refusal::NotListed,
                  "an older stable build is refused at grace_until");
    ExpectRefusal(allow::Admit(withGrace, current, "stable", 3, now, 1800000000ull), Refusal::Allowed,
                  "the current release does not depend on grace");
    ExpectRefusal(allow::Admit(withGrace, "1.2.0", "stable", 3, now, 1799999999ull), Refusal::Yanked,
                  "a yanked build stays refused inside grace");
    ExpectRefusal(allow::Admit(withGrace, current, "stable", 3, now - 1, 0), Refusal::Stale,
                  "the monotonic clock running backwards makes the list stale");
}

void CheckStateBlob(const Keys& trusted) {
    allow::State held;
    held.highestSequence = 0x0102030405060708ull;
    const allow::StateBlob blob = allow::EncodeState(held);
    Check(blob[0] == 0x08 && blob[7] == 0x01, "the state blob is the sequence, little-endian");
    const allow::State back = allow::DecodeState(blob);
    Check(back.highestSequence == held.highestSequence && !back.held,
          "DecodeState restores the sequence and holds no list");

    // A restored copy refuses what it has passed, and needs a fresh list before public rooms open.
    allow::State ten;
    ten.highestSequence = 10;
    allow::State restored = allow::DecodeState(allow::EncodeState(ten));
    ExpectRefusal(allow::Admit(restored, "1.3.0", "stable", 3, 1000, 0), Refusal::NoList,
                  "a restored copy has no list until it fetches one");
    allow::List nine = Fixture();
    nine.sequence = 9;
    ExpectVerdict(allow::Accept(Signed(nine, trusted), trusted.pub, 1000, restored), Verdict::Replayed,
                  "a list below the restored sequence is refused");
    ExpectVerdict(allow::Accept(Signed(Fixture(), trusted), trusted.pub, 1000, restored), Verdict::Ok,
                  "the restored sequence accepts its own list again");
}

void CheckKeys() {
    allow::Seed seed{};
    for (size_t i = 0; i < seed.size(); i++) {
        seed[i] = static_cast<uint8_t>(0x33 + i);
    }
    allow::PublicKey p1{};
    allow::PublicKey p2{};
    allow::SecretKey s1{};
    allow::SecretKey s2{};
    allow::KeyPairFromSeed(seed, p1, s1);
    allow::KeyPairFromSeed(seed, p2, s2);
    Check(p1 == p2 && s1 == s2, "a seed gives the same key pair every time");
    Check(std::equal(p1.begin(), p1.end(), s1.begin() + 32), "the public key sits in secret key bytes 32..63");
}

uint64_t gRng = 0x9E3779B97F4A7C15ULL;

uint32_t Next() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return static_cast<uint32_t>(gRng >> 16);
}

// Edits a buffer the way a hostile peer might: flips, overwrites, cuts, grows, inserts and deletes. Half the
// edits land in the first 80 bytes, where the lengths and counts are.
void Mutate(Bytes& m) {
    const int edits = 1 + static_cast<int>(Next() % 6);
    for (int e = 0; e < edits && !m.empty(); e++) {
        const size_t at = (Next() & 1) ? Next() % std::min<size_t>(m.size(), 80) : Next() % m.size();
        const auto where = m.begin() + static_cast<std::ptrdiff_t>(at);
        switch (Next() % 7) {
            case 0:
                m[at] = static_cast<uint8_t>(m[at] ^ (1u << (Next() % 8)));
                break;
            case 1:
                m[at] = static_cast<uint8_t>(Next());
                break;
            case 2:
                m[at] = static_cast<uint8_t>((Next() & 1) ? 0xFF : 0x00);
                break;
            case 3:
                m.resize(at);
                break;
            case 4:
                m.insert(where, static_cast<size_t>(1 + Next() % 16), static_cast<uint8_t>(Next()));
                break;
            case 5:
                m.erase(where, m.begin() + static_cast<std::ptrdiff_t>(std::min(m.size(), at + 1 + Next() % 32)));
                break;
            default:
                m.resize(m.size() + 1 + Next() % 64, static_cast<uint8_t>(Next()));
                break;
        }
    }
}

// Mutated bytes under the original signature. None may pass, since the signature covers every byte.
void FuzzRaw(const Bytes& good, const Keys& keys, int rounds, std::map<std::string, int>& tally) {
    for (int round = 0; round < rounds; round++) {
        Bytes m = good;
        Mutate(m);
        if (m == good) {
            continue;
        }
        allow::List list;
        const Verdict verdict = allow::Decode(m, keys.pub, list);
        tally[allow::VerdictName(verdict)]++;
        Check(verdict != Verdict::Ok, "a mutated buffer under the original signature is refused");
    }
}

// Mutated bodies signed again with the test key, so the fault reaches the field checks rather than the signature.
// Ok must mean the buffer re-encodes to exactly the same bytes.
void FuzzResigned(const Bytes& good, const Keys& keys, int rounds, std::map<std::string, int>& tally) {
    for (int round = 0; round < rounds; round++) {
        Bytes body(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() - allow::kSignatureBytes));
        Mutate(body);
        if (body.empty()) {
            continue;
        }
        allow::Signature signature{};
        crypto_ed25519_sign(signature.data(), keys.sec.data(), body.data(), body.size());
        Bytes resigned = body;
        resigned.insert(resigned.end(), signature.begin(), signature.end());

        allow::List list;
        const Verdict verdict = allow::Decode(resigned, keys.pub, list);
        tally[allow::VerdictName(verdict)]++;
        if (verdict == Verdict::Ok) {
            Bytes again;
            Check(allow::Sign(list, keys.sec, again) && again == resigned,
                  "a re-signed body that decodes encodes back to the same bytes");
        } else {
            ExpectVerdict(verdict, Verdict::Malformed, "a re-signed body with a broken field is malformed");
        }
    }
}

// Random bytes behind a valid head. Nothing decodes, since no random run carries a valid signature.
void FuzzNoise(const Keys& keys, int rounds, std::map<std::string, int>& tally) {
    for (int round = 0; round < rounds; round++) {
        Bytes noise(static_cast<size_t>(Next() % 4096), uint8_t{0});
        for (uint8_t& b : noise) {
            b = static_cast<uint8_t>(Next());
        }
        if (noise.size() >= 5) {
            noise[0] = 'G';
            noise[1] = 'N';
            noise[2] = 'A';
            noise[3] = 'L';
            noise[4] = allow::kFormatVersion;
        }
        allow::List list;
        const Verdict verdict = allow::Decode(noise, keys.pub, list);
        tally[allow::VerdictName(verdict)]++;
        Check(verdict != Verdict::Ok, "noise behind the head does not decode");
    }
}

void CheckFuzz(const Keys& trusted, int rounds) {
    const Bytes good = Signed(Fixture(), trusted);
    std::map<std::string, int> tally;
    FuzzRaw(good, trusted, rounds, tally);
    FuzzResigned(good, trusted, rounds, tally);
    FuzzNoise(trusted, rounds / 4, tally);
    std::printf("fuzz verdicts:");
    for (const auto& [name, n] : tally) {
        std::printf(" %s %d,", name.c_str(), n);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3000;
    const Keys trusted = MakeKeys(0x5A);
    const Keys other = MakeKeys(0xA5);
    CheckRoundTrip(trusted);
    CheckSigning(trusted);
    CheckSignature(trusted, other);
    CheckFraming(trusted);
    CheckSequence(trusted);
    CheckGate(trusted);
    CheckStateBlob(trusted);
    CheckKeys();
    CheckFuzz(trusted, rounds);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
