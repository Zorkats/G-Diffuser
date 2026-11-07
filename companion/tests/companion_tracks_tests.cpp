// Copyright (c) Zorkats
//
// Headless tests for the Tracks workspace: a synthetic Course Edit payload is sorted by the height it
// needs, the .gdxc header matches the game's import format, exports never land without passing, and a
// file that is not a disk image is refused. No game data is used. Links gdx_companion_host only.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gdx_companion_paths.h"
#include "gdx_companion_tracks.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint32_t U32(const std::vector<uint8_t>& bytes, size_t at) {
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

// A host-order payload with three control points, the highest at highest_y, as
// port/tests/gdx_foreign_track_tests.c builds it.
std::vector<uint8_t> MakePayload(float highest_y) {
    GdxForeignCourseData course;
    std::memset(&course, 0, sizeof(course));
    course.creatorId = GDX_FOREIGN_CREATOR_NINTENDO;
    course.controlPointCount = 3;
    course.venue = 2;
    course.bgm = 5;
    course.controlPoint[0].pos.x = 1000.0f;
    course.controlPoint[1].pos.z = 500.0f;
    course.controlPoint[2].pos.y = highest_y;
    course.bankAngle[2] = -20;
    int invalid = 0;
    course.checksum = gdx_foreign_track_checksum(&course, GDX_FOREIGN_TRACK_EXTENDED_MAX_Y, &invalid);
    std::vector<uint8_t> payload(GDX_FOREIGN_TRACK_PAYLOAD_SIZE, 0);
    std::memcpy(payload.data(), &course, sizeof(course));
    return payload;
}

void Inspection() {
    std::vector<uint8_t> low = MakePayload(4000.0f);
    TrackInspection stock = InspectTrackPayload(low.data(), low.size(), false);
    Check(stock.fit == TrackFit::Stock && stock.stock_errors == 0 && stock.payload.size() == low.size(),
          "a track under 5000 fits the stock height");
    Check(stock.venue == 2 && stock.bgm == 5 && stock.control_points == 3, "venue, music and points are read");

    std::vector<uint8_t> high = MakePayload(6000.0f);
    TrackInspection extended = InspectTrackPayload(high.data(), high.size(), false);
    Check(extended.fit == TrackFit::NeedsExtended && extended.stock_errors == GDX_FOREIGN_TRACK_ERR_GEOMETRY &&
              extended.extended_errors == 0 && !extended.payload.empty(),
          "a track at 6000 needs Extended course height");

    std::vector<uint8_t> bad = low;
    bad[0] = 7;  // creator
    TrackInspection refused = InspectTrackPayload(bad.data(), bad.size(), false);
    Check(refused.fit == TrackFit::Refused && refused.payload.empty() &&
              (refused.extended_errors & GDX_FOREIGN_TRACK_ERR_CREATOR) != 0,
          "a track not made in Course Edit is refused");
    Check(InspectTrackPayload(low.data(), 100, false).stock_errors == GDX_FOREIGN_TRACK_ERR_BAD_SIZE,
          "a short payload is refused by size");

    const std::vector<std::string> lines = DescribeTrackErrors(GDX_FOREIGN_TRACK_ERR_CREATOR | (1u << 30));
    Check(lines.size() == 2, "each error bit, known or not, gets a line");
}

void Gdxc() {
    const std::vector<uint8_t> payload = MakePayload(100.0f);
    std::vector<uint8_t> out;
    std::string error;
    Check(EncodeGdxc("MY TRACK", "CRSD", payload, out, error), "a track encodes");
    Check(out.size() == 0x41 + payload.size() && std::memcmp(out.data(), "GXC1", 4) == 0, "GXC1 header, 0x41 bytes");
    Check(U32(out, 0x04) == 1 && U32(out, 0x08) == 1 && std::memcmp(out.data() + 0x0C, "CRSD", 4) == 0,
          "version 1, type track, fourcc CRSD");
    Check(std::memcmp(out.data() + 0x10, "MY TRACK", 8) == 0 && out[0x24] == 0, "the name is stored, terminated");
    Check(U32(out, 0x25) == 0 && U32(out, 0x29) == 0xC830, "flags 0, size 0xC830");
    Check(U32(out, 0x2D) == Crc32(payload.data(), payload.size()), "the CRC covers the payload");
    bool zero = true;
    for (size_t i = 0x31; i < 0x41; ++i) zero = zero && out[i] == 0;
    Check(zero && std::memcmp(out.data() + 0x41, payload.data(), payload.size()) == 0,
          "16 reserved zero bytes, then the payload");
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Check(Crc32(check, sizeof(check)) == 0xCBF43926u, "CRC-32 matches the zlib check value");

    Check(!EncodeGdxc("", "CRSD", payload, out, error), "an empty name is refused");
    Check(!EncodeGdxc(std::string(21, 'A'), "CRSD", payload, out, error), "a 21-byte name is refused");
    Check(EncodeGdxc(std::string(20, 'A'), "CRSE", payload, out, error), "a 20-byte name is allowed");
    Check(!EncodeGdxc("A", "MCHN", payload, out, error), "a machine is never exported");
    Check(!EncodeGdxc("A", "CRSD", std::vector<uint8_t>(10), out, error), "a wrong payload size is refused");
}

void Export(const fs::path& base) {
    ImageTrack track;
    track.name = "Big/Loop:1";
    track.display_name = track.name;
    track.extension = "CRSD";
    track.inspection = InspectTrackPayload(MakePayload(100.0f).data(), GDX_FOREIGN_TRACK_PAYLOAD_SIZE, false);
    const fs::path game = base / "game";
    const fs::path path = DefaultExportPath(game, track);
    Check(path == game / "exports" / "Big_Loop_1.gdxc", "unsafe name bytes become '_' in exports/");
    ImageTrack blank = track;
    blank.name = "...";
    Check(DefaultExportPath(game, blank).filename() == "track.gdxc", "a name of dots falls back to track");

    std::string error;
    Check(!ExportTrack(track, path, false, error), "the export folder must exist");
    fs::create_directories(game / "exports");
    Check(ExportTrack(track, path, false, error), "a passing track exports");
    const std::vector<uint8_t> written = ReadBytes(path);
    Check(written.size() == 0x41 + GDX_FOREIGN_TRACK_PAYLOAD_SIZE && !fs::exists(fs::path(path) += ".tmp"),
          "the export is complete and its temporary file is gone");
    Check(!ExportTrack(track, path, false, error), "an existing export is kept without Replace");
    Check(ExportTrack(track, path, true, error), "Replace overwrites the export");
    Check(!ExportTrack(track, game / "exports" / "x.bin", false, error), "the export must end in .gdxc");
    ImageTrack refused = track;
    refused.inspection.fit = TrackFit::Refused;
    Check(!ExportTrack(refused, game / "exports" / "r.gdxc", false, error) && !fs::exists(game / "exports" / "r.gdxc"),
          "a refused track is never written");
}

void Images(const fs::path& base) {
    const fs::path fake = base / "fake.ndd";
    {
        std::ofstream out(fake, std::ios::binary);
        out << std::string(4096, 'x');
    }
    ImageListing listing;
    std::string error;
    Check(!ReadDiskImage(fake, listing, error) && !error.empty(), "a file that is not a disk image is refused");
    Check(!ReadDiskImage(base / "missing.ndd", listing, error), "a missing image is refused");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_tracks_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    Inspection();
    Gdxc();
    Export(base);
    Images(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
