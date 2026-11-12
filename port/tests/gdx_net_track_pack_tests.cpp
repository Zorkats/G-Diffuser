// port/tests/gdx_net_track_pack_tests.cpp -- a custom track's identity and the receiver's check (G-NET O4.1).
// Copyright (c) Zorkats
//
// Usage: gdx_net_track_pack_tests [fuzz rounds per track, default 3000]
//
// Builds content bytes for a short and a long (forked) track, then checks every verdict of Verify, that a
// failed check leaves its output alone, and a seeded fuzz: mutated buffers whose hash is made to match, so
// every one reaches the snapshot decoder as network bytes would.

#include "gdx_net_track_pack.h"

#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using gdx::snapshot::Bytes;
using gdx::snapshot::Generation;
using gdx::snapshot::Metadata;
namespace track = gdx::net::track;

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

// A saved track as the snapshot tests build one: a title and per-track settings, and forks past 64 points.
Generation Fixture(int count) {
    Generation g;
    g.disk = 0x1020304050607080ULL;
    g.directory = 3;
    g.courseId = "PACK TEST.CRSD";
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
    store(Metadata::Title, gdx_course_title_encode("Pack test", Binding(g), buffer.data(), buffer.size()));
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

track::Verdict VerifyBytes(const Bytes& bytes, const track::TrackId& id, Generation* out = nullptr) {
    return track::Verify(bytes.data(), bytes.size(), id, out);
}

void CheckIdentityAndHex() {
    const unsigned char abc[] = { 'a', 'b', 'c' };
    const std::string abcHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    Check(track::ToHex(track::Identify(abc, 3)) == abcHex, "Identify is SHA-256 (FIPS 180-2 \"abc\")");
    Check(track::ToHex(track::Identify(nullptr, 0)) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "Identify of no bytes is SHA-256 of the empty string");

    track::TrackId id{};
    Check(track::FromHex(abcHex, id) && track::ToHex(id) == abcHex, "hex reads back");
    std::string upper = abcHex;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    Check(track::FromHex(upper, id) && track::ToHex(id) == abcHex, "hex reads upper case");
    const track::TrackId kept = id;
    Check(!track::FromHex(abcHex.substr(1), id), "63 digits are refused");
    Check(!track::FromHex(abcHex + "0", id), "65 digits are refused");
    std::string bad = abcHex;
    bad[10] = 'g';
    Check(!track::FromHex(bad, id), "a non-hex digit is refused");
    bad[10] = ' ';
    Check(!track::FromHex(bad, id), "a space is refused");
    Check(id == kept, "a refused hex leaves the id alone");
}

void CheckVerdicts(int count, const Bytes& bytes) {
    const std::string label = std::to_string(count) + "-point track: ";
    const track::TrackId id = track::Identify(bytes.data(), bytes.size());

    Generation out;
    Check(VerifyBytes(bytes, id, &out) == track::Verdict::Ok, label + "its own content bytes verify");
    Check(out.disk == 1 && out.directory == 0 && out.courseId == "CONTENT.CRSD" && out.pointCount == count,
          label + "the verified track carries the content identity");
    Bytes again;
    Check(gdx::snapshot::content(out, again) && again == bytes, label + "the verified track is its own content");
    Check(VerifyBytes(bytes, id) == track::Verdict::Ok, label + "a null output is allowed");

    Generation untouched;
    untouched.courseId = "UNTOUCHED";
    auto unchanged = [&]() { return untouched.courseId == "UNTOUCHED" && untouched.base.empty(); };

    Check(track::Verify(nullptr, 0, id, &untouched) == track::Verdict::Empty, label + "no bytes are empty");
    Check(track::Verify(bytes.data(), 0, id, &untouched) == track::Verdict::Empty, label + "length 0 is empty");

    Bytes oversized(track::kMaxBytes + 1, 0);
    std::copy(bytes.begin(), bytes.end(), oversized.begin());
    Check(VerifyBytes(oversized, track::Identify(oversized.data(), oversized.size()), &untouched) ==
              track::Verdict::Oversized,
          label + "one byte past the cap is oversized, even with a matching identity");

    Bytes corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x40;
    Check(VerifyBytes(corrupt, id, &untouched) == track::Verdict::WrongIdentity,
          label + "a flipped bit fails the identity");
    const Bytes truncated(bytes.begin(), bytes.end() - 1);
    Check(VerifyBytes(truncated, id, &untouched) == track::Verdict::WrongIdentity,
          label + "a truncated buffer fails the identity");
    track::TrackId other = id;
    other[31] ^= 1;
    Check(VerifyBytes(bytes, other, &untouched) == track::Verdict::WrongIdentity,
          label + "the right bytes under another identity fail");

    // A sender that names its own bad bytes passes the hash and must be stopped by the decoder.
    Check(VerifyBytes(truncated, track::Identify(truncated.data(), truncated.size()), &untouched) ==
              track::Verdict::Undecodable,
          label + "a truncated buffer under its own identity does not decode");
    Bytes badMagic = bytes;
    badMagic[0] ^= 0x20;
    Check(VerifyBytes(badMagic, track::Identify(badMagic.data(), badMagic.size()), &untouched) ==
              track::Verdict::Undecodable,
          label + "a wrong magic does not decode");
    Bytes badPayload = bytes;
    badPayload[bytes.size() - 1] ^= 0x01;
    Check(VerifyBytes(badPayload, track::Identify(badPayload.data(), badPayload.size()), &untouched) ==
              track::Verdict::Undecodable,
          label + "a changed payload byte fails the generation's own CRC");

    // A whole, valid generation that is not content bytes: the save as stored, and the seal.
    Generation stored = Fixture(count);
    Bytes encoded;
    Check(gdx::snapshot::encode(stored, encoded), label + "the stored form encodes");
    Check(VerifyBytes(encoded, track::Identify(encoded.data(), encoded.size()), &untouched) ==
              track::Verdict::NotCanonical,
          label + "a generation bound to its slot is not content bytes");
    Generation sealed = stored;
    Check(gdx::snapshot::seal(sealed) && gdx::snapshot::rebind(sealed, 1, 0, "CONTENT.CRSD") &&
              gdx::snapshot::encode(sealed, encoded),
          label + "a sealed generation under the content name encodes");
    Check(VerifyBytes(encoded, track::Identify(encoded.data(), encoded.size()), &untouched) ==
              track::Verdict::NotCanonical,
          label + "a sealed generation is not content bytes");
    Check(unchanged(), label + "no failed check wrote its output");
}

uint64_t gRng = 0x9E3779B97F4A7C15ULL;
uint32_t Next() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return uint32_t(gRng >> 16);
}

// Mutated network bytes with a matching hash, so each one is decoded. Ok must mean the buffer is still
// exactly the content bytes of the track it decodes to.
void Fuzz(const Bytes& bytes, int rounds, std::map<std::string, int>& tally) {
    for (int round = 0; round < rounds; round++) {
        Bytes m = bytes;
        const int edits = 1 + int(Next() % 6);
        for (int e = 0; e < edits && !m.empty(); e++) {
            // Half the edits land in the header, where the lengths and counts are.
            const size_t at = (Next() & 1) ? Next() % std::min<size_t>(m.size(), 96) : Next() % m.size();
            switch (Next() % 7) {
                case 0:
                    m[at] ^= uint8_t(1u << (Next() % 8));
                    break;
                case 1:
                    m[at] = uint8_t(Next());
                    break;
                case 2:
                    m[at] = (Next() & 1) ? 0xFF : 0x00;
                    break;
                case 3:
                    m.resize(at);
                    break;
                case 4:
                    m.insert(m.begin() + long(at), size_t(1 + Next() % 16), uint8_t(Next()));
                    break;
                case 5:
                    m.erase(m.begin() + long(at), m.begin() + long(std::min(m.size(), at + 1 + Next() % 32)));
                    break;
                default:
                    m.resize(m.size() + 1 + Next() % 64, uint8_t(Next()));
                    break;
            }
        }
        Generation out;
        const auto verdict = track::Verify(m.data(), m.size(), track::Identify(m.data(), m.size()), &out);
        tally[track::VerdictName(verdict)]++;
        if (verdict == track::Verdict::Ok) {
            Bytes again;
            Check(gdx::snapshot::content(out, again) && again == m, "a fuzzed buffer that verifies is canonical");
        } else {
            Check(verdict != track::Verdict::WrongIdentity, "a fuzzed buffer under its own hash passes the hash");
        }
        // The decoder alone, as a peer could reach it with any bytes.
        Generation direct;
        (void)gdx::snapshot::decode(m, direct);
    }
    // Pure noise behind a valid magic.
    for (int round = 0; round < rounds / 4; round++) {
        Bytes noise(size_t(Next() % 4096), 0);
        for (auto& b : noise) {
            b = uint8_t(Next());
        }
        if (noise.size() >= 4) {
            std::memcpy(noise.data(), bytes.data(), 4);
        }
        const auto verdict = track::Verify(noise.data(), noise.size(), track::Identify(noise.data(), noise.size()),
                                           nullptr);
        tally[track::VerdictName(verdict)]++;
        Check(noise.empty() ? verdict == track::Verdict::Empty : verdict == track::Verdict::Undecodable,
              "noise behind the magic does not decode");
    }
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3000;
    CheckIdentityAndHex();
    std::map<std::string, int> tally;
    for (const int count : { 10, 100 }) {
        Bytes bytes;
        Check(gdx::snapshot::content(Fixture(count), bytes) && !bytes.empty(),
              std::to_string(count) + "-point fixture gives content bytes");
        if (bytes.empty()) {
            continue;
        }
        std::printf("%d-point track: %zu content bytes, id %s\n", count, bytes.size(),
                    track::ToHex(track::Identify(bytes.data(), bytes.size())).c_str());
        CheckVerdicts(count, bytes);
        Fuzz(bytes, rounds, tally);
    }
    std::printf("fuzz verdicts:");
    for (const auto& [name, n] : tally) {
        std::printf(" %s %d,", name.c_str(), n);
    }
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
