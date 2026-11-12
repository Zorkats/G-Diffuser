// port/tests/gdx_net_track_cache_tests.cpp -- the cache of downloaded custom tracks (G-NET O4.3).
// Copyright (c) Zorkats
//
// Usage: gdx_net_track_cache_tests
//
// Stores real content bytes and checks the cache's promises: a verified round trip, misses that never throw,
// a damaged file deleted on load, the caps and their least recently used order across a restart, and that
// nothing outside the managed names is ever touched. Runs in a fresh folder under the temporary directory,
// removed at the end.

#include "gdx_net_track_cache.h"

#include "gdx_net_track_pack.h"
#include "gdx_user_folders.h"

#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_course_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using gdx::snapshot::Bytes;
using gdx::snapshot::Generation;
using gdx::snapshot::Metadata;
namespace track = gdx::net::track;
using gdx::net::TrackCache;

int gChecks = 0;
int gFailures = 0;
fs::file_time_type gBase;

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

// Copied from the track pack tests: a saved track with a title, settings, and forks past 64 points.
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

// The point count changes the content, so each count is a distinct track with its own identity.
Bytes Track(int count) {
    Bytes bytes;
    gdx::snapshot::content(Fixture(count), bytes);
    return bytes;
}

track::TrackId IdOf(const Bytes& bytes) {
    return track::Identify(bytes.data(), bytes.size());
}

fs::path MakeScratch() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("gdx_track_cache_tests_" + std::to_string(static_cast<long long>(stamp)));
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

fs::path TrackFile(const fs::path& folder, const track::TrackId& id) {
    return folder / (track::ToHex(id) + ".gcp");
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::symlink_status(path, ec).type() != fs::file_type::not_found;
}

// Case-sensitive: on Windows Exists() cannot tell an upper-case name from a lower-case one.
bool ListedExactly(const fs::path& folder, const std::string& name) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string() == name) {
            return true;
        }
    }
    return false;
}

size_t EntryCount(const fs::path& folder) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        n++;
    }
    return n;
}

void WriteRaw(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

Bytes ReadRaw(const fs::path& path) {
    Bytes bytes;
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return bytes;
}

// Sets recency explicitly: file time stamps can tie within one clock tick, and the test needs a known order.
void SetAge(const fs::path& path, int slot) {
    std::error_code ec;
    fs::last_write_time(path, gBase + std::chrono::minutes(slot), ec);
}

void ExpectDroppedOnLoad(TrackCache& cache, const fs::path& file, const track::TrackId& id,
                         const std::string& label) {
    Bytes out{9};
    Check(!cache.Load(id, out) && out.empty(), label + " is a miss");
    Check(!Exists(file), label + " is deleted on load");
}

void CheckRoundTrip(const fs::path& root) {
    const fs::path folder = root / "roundtrip";
    TrackCache cache(folder);
    Check(cache.ok(), "a cache in a new absolute folder opens");
    Check(fs::is_directory(folder), "the cache folder is created when missing");

    const Bytes a = Track(10);
    const track::TrackId idA = IdOf(a);
    Check(!a.empty(), "a 10-point track gives content bytes");
    Check(!cache.Has(idA), "an empty cache has no track");

    Bytes out{1, 2, 3};
    Check(!cache.Load(idA, out) && out.empty(), "a miss returns false and clears the output");

    Check(cache.Store(idA, a), "a verified track is stored");
    Check(cache.Has(idA), "a stored track is present");
    Bytes back;
    Check(cache.Load(idA, back) && back == a, "store then load gives the same bytes");
    Bytes again;
    Check(cache.Store(idA, a) && cache.Load(idA, again) && again == a, "storing the same track again keeps it");
}

void CheckStoreRefusals(const fs::path& root) {
    const fs::path folder = root / "refusals";
    TrackCache cache(folder);
    const Bytes a = Track(10);
    const Bytes b = Track(11);
    const track::TrackId idA = IdOf(a);
    const track::TrackId idB = IdOf(b);

    Check(!cache.Store(idB, a), "bytes under another track's id are refused");

    const Bytes truncated(a.begin(), a.end() - 1);
    Check(!cache.Store(track::Identify(truncated.data(), truncated.size()), truncated),
          "a truncated track is refused even under its own hash");

    Bytes flipped = a;
    flipped[flipped.size() / 2] ^= 0x40;
    Check(!cache.Store(idA, flipped), "a flipped byte is refused");

    Bytes encoded;
    Check(gdx::snapshot::encode(Fixture(10), encoded), "the save-form generation encodes");
    Check(!cache.Store(track::Identify(encoded.data(), encoded.size()), encoded),
          "a save-form generation is not content bytes");

    const Bytes empty;
    Check(!cache.Store(track::Identify(empty.data(), 0), empty), "empty bytes are refused");

    const Bytes big(track::kMaxBytes + 1, 0);
    Check(!cache.Store(track::Identify(big.data(), big.size()), big), "bytes past the size cap are refused");

    Check(EntryCount(folder) == 0 && !Exists(TrackFile(folder, idA)) && !Exists(TrackFile(folder, idB)),
          "refused stores write nothing");
}

void CheckDamagedFileOnLoad(const fs::path& root) {
    const fs::path folder = root / "damaged";
    TrackCache cache(folder);
    const Bytes a = Track(10);
    const track::TrackId idA = IdOf(a);
    const fs::path file = TrackFile(folder, idA);

    Check(cache.Store(idA, a), "damaged case: the track is stored");
    Bytes flipped = a;
    flipped[flipped.size() / 2] ^= 0x01;
    WriteRaw(file, flipped);
    ExpectDroppedOnLoad(cache, file, idA, "a bit-flipped file");

    Check(cache.Store(idA, a), "damaged case: stored again");
    WriteRaw(file, Bytes(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(a.size() / 2)));
    ExpectDroppedOnLoad(cache, file, idA, "a truncated file");

    Check(cache.Store(idA, a), "damaged case: stored again");
    WriteRaw(file, Track(11));
    ExpectDroppedOnLoad(cache, file, idA, "another track under this name");

    Check(cache.Store(idA, a), "damaged case: stored again");
    WriteRaw(file, Bytes());
    ExpectDroppedOnLoad(cache, file, idA, "an empty file");

    Check(cache.Store(idA, a), "damaged case: stored again");
    WriteRaw(file, Bytes(track::kMaxBytes + 1, 0));
    ExpectDroppedOnLoad(cache, file, idA, "an oversized file");

    Check(cache.Store(idA, a), "damaged case: stored again");
    std::error_code ec;
    fs::remove(file, ec);
    Bytes out;
    Check(!cache.Has(idA) && !cache.Load(idA, out) && out.empty(),
          "a track removed under the cache is a miss, not an error");
}

void CheckSixtyFourCap(const fs::path& root) {
    const fs::path folder = root / "cap64";
    TrackCache cache(folder);

    std::vector<Bytes> tracks;
    std::vector<track::TrackId> ids;
    for (int count = 10; count < 75; count++) {
        tracks.push_back(Track(count));
        ids.push_back(IdOf(tracks.back()));
    }

    for (size_t i = 0; i < 64; i++) {
        Check(cache.Store(ids[i], tracks[i]), "cap case: track " + std::to_string(i) + " is stored");
    }
    for (size_t i = 0; i < 64; i++) {
        SetAge(TrackFile(folder, ids[i]), int(i));
    }
    Check(cache.Store(ids[64], tracks[64]), "cap case: the 65th track is stored");

    Check(!Exists(TrackFile(folder, ids[0])), "the least recently used track is dropped at 65");
    bool rest = true;
    for (size_t i = 1; i <= 64; i++) {
        rest = rest && Exists(TrackFile(folder, ids[i]));
    }
    Check(rest, "the other 64 tracks remain");
}

void CheckRecencyAndReopen(const fs::path& root) {
    const fs::path folder = root / "lru";
    const Bytes a = Track(10);
    const Bytes b = Track(11);
    const Bytes c = Track(12);
    const Bytes d = Track(13);
    const Bytes e = Track(14);
    const track::TrackId idA = IdOf(a);
    const track::TrackId idB = IdOf(b);
    const track::TrackId idC = IdOf(c);
    const track::TrackId idD = IdOf(d);
    const track::TrackId idE = IdOf(e);
    {
        TrackCache cache(folder, 3);
        Check(cache.Store(idA, a) && cache.Store(idB, b) && cache.Store(idC, c), "lru case: three tracks fill the cap");
        SetAge(TrackFile(folder, idA), 0);
        SetAge(TrackFile(folder, idB), 1);
        SetAge(TrackFile(folder, idC), 2);

        Bytes out;
        Check(cache.Load(idA, out) && out == a, "a load of the oldest track hits");
        Check(cache.Store(idD, d), "a fourth track is stored");
        Check(!Exists(TrackFile(folder, idB)), "a load refreshes recency, so the untouched oldest goes");
        Check(Exists(TrackFile(folder, idA)) && Exists(TrackFile(folder, idC)) && Exists(TrackFile(folder, idD)),
              "the loaded, the newer and the new tracks remain");
    }
    {
        TrackCache reopened(folder, 3);
        Check(reopened.Store(idE, e), "a reopened cache stores a fifth track");
        Check(!Exists(TrackFile(folder, idC)), "after a restart the least recently used track is still the one dropped");
        Check(Exists(TrackFile(folder, idA)) && Exists(TrackFile(folder, idD)) && Exists(TrackFile(folder, idE)),
              "the rest remain after a restart");
    }
}

void CheckByteCap(const fs::path& root) {
    const fs::path folder = root / "bytes";
    const Bytes a = Track(10);
    const Bytes b = Track(11);
    const Bytes c = Track(12);
    const track::TrackId idA = IdOf(a);
    const track::TrackId idB = IdOf(b);
    const track::TrackId idC = IdOf(c);
    {
        TrackCache wide(folder);
        Check(wide.Store(idA, a) && wide.Store(idB, b), "byte cap: two tracks fit the default cap");
    }
    SetAge(TrackFile(folder, idA), 0);
    SetAge(TrackFile(folder, idB), 1);

    // Any two of the three fit, all three do not, so exactly the oldest goes.
    const size_t cap = a.size() + b.size() + c.size() - 1;
    TrackCache capped(folder, TrackCache::kDefaultMaxFiles, cap);
    Check(capped.Store(idC, c), "byte cap: a track that fits on its own is stored");
    Check(!Exists(TrackFile(folder, idA)), "byte cap: the least recently used track is dropped");
    Check(Exists(TrackFile(folder, idB)) && Exists(TrackFile(folder, idC)),
          "byte cap: the newer tracks remain under the cap");

    TrackCache tiny(root / "tiny", TrackCache::kDefaultMaxFiles, a.size() - 1);
    Check(!tiny.Store(idA, a) && EntryCount(root / "tiny") == 0,
          "a track larger than the whole byte cap is refused without writing");
}

void CheckUnrelatedFilesSafe(const fs::path& root) {
    const fs::path folder = root / "unrelated";
    fs::create_directories(folder);

    const unsigned char abc[] = { 'a', 'b', 'c' };
    const track::TrackId idU = track::Identify(abc, 3);
    std::string upperName = track::ToHex(idU);
    std::transform(upperName.begin(), upperName.end(), upperName.begin(),
                   [](unsigned char c) { return char(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c); });
    upperName += ".gcp";
    const std::string bakName = track::ToHex(idU) + ".gcp.bak";
    WriteRaw(folder / "notes.txt", Bytes{'k', 'e', 'e', 'p'});
    WriteRaw(folder / upperName, Bytes{'n', 'o', 't', ' ', 'a', ' ', 't', 'r', 'a', 'c', 'k'});
    WriteRaw(folder / "abc.gcp", Bytes{'x'});
    WriteRaw(folder / bakName, Bytes{'y'});
    fs::create_directories(folder / "sub");
    WriteRaw(folder / "sub" / "inside.bin", Bytes{'d', 'e', 'e', 'p'});

    TrackCache cache(folder, 3);
    const Bytes t0 = Track(20);
    const Bytes t1 = Track(21);
    const Bytes t2 = Track(22);
    const Bytes t3 = Track(23);
    const Bytes t4 = Track(24);
    const std::vector<Bytes> stored = { t0, t1, t2, t3, t4 };
    for (const Bytes& t : stored) {
        Check(cache.Store(IdOf(t), t), "unrelated case: a track is stored");
    }

    int kept = 0;
    for (const Bytes& t : stored) {
        kept += Exists(TrackFile(folder, IdOf(t))) ? 1 : 0;
    }
    Check(kept == 3, "only managed tracks count toward the cap (3 of 5 kept)");
    Check(Exists(folder / "notes.txt") && Exists(folder / "abc.gcp") && Exists(folder / bakName),
          "unrelated and wrongly named files are never deleted");
    Check(ListedExactly(folder, upperName), "an upper-case name is never deleted");
    Check(ReadRaw(folder / "sub" / "inside.bin") == Bytes{'d', 'e', 'e', 'p'}, "a subfolder is left alone");

    Bytes out;
    Check(!cache.Load(idU, out) && out.empty() && ListedExactly(folder, upperName),
          "a load of the lower-case name does not delete an upper-case file that answers to it");
}

void CheckLeftoverTemp(const fs::path& root) {
    const fs::path folder = root / "temp";
    fs::create_directories(folder);
    const std::string stem = track::ToHex(IdOf(Track(10)));
    const fs::path stale = folder / (stem + ".gcp.writing-1234567-9");
    const fs::path fresh = folder / (stem + ".gcp.writing-7654321-10");
    const fs::path other = folder / "other.tmp";
    WriteRaw(stale, Bytes{1, 2, 3});
    WriteRaw(fresh, Bytes{4, 5, 6});
    WriteRaw(other, Bytes{7});
    SetAge(stale, 0);
    SetAge(other, 0);

    TrackCache cache(folder);
    Check(!Exists(stale), "a temporary file from an interrupted write is removed");
    Check(Exists(fresh), "a recent temporary file may belong to another copy and is kept");
    Check(Exists(other), "a file that is not a cache temporary is kept");

    Check(cache.Store(IdOf(Track(11)), Track(11)), "a store with a leftover present still works");
    Check(Exists(fresh), "a recent temporary file survives a store's eviction pass too");
}

void CheckRelativeRefused() {
    const fs::path relative = "gdx_track_cache_relative_probe";
    TrackCache cache(relative);
    const Bytes a = Track(10);
    Bytes out;
    Check(!cache.ok(), "a relative folder is refused");
    Check(!cache.Store(IdOf(a), a) && !cache.Load(IdOf(a), out) && !cache.Has(IdOf(a)),
          "a refused cache stores, loads and reports nothing");
    std::error_code ec;
    Check(!fs::exists(relative, ec), "a refused relative folder is not created");
}

void CheckNonRegularNeverFollowed(const fs::path& root) {
    const fs::path folder = root / "nonregular";
    TrackCache cache(folder);
    const Bytes a = Track(10);
    const track::TrackId idA = IdOf(a);
    fs::create_directories(TrackFile(folder, idA));
    Bytes out;
    Check(!cache.Has(idA) && !cache.Load(idA, out), "a directory under a managed name is a miss");
    Check(fs::is_directory(TrackFile(folder, idA)), "a directory under a managed name is left alone");

    const Bytes b = Track(11);
    const track::TrackId idB = IdOf(b);
    const fs::path target = root / "symlink_target.bin";
    WriteRaw(target, b);
    std::error_code ec;
    fs::create_symlink(target, TrackFile(folder, idB), ec);
    if (ec) {
        std::printf("SKIP: symlinks are unavailable here (%s); the symlink check did not run\n", ec.message().c_str());
        return;
    }
    Check(!cache.Has(idB) && !cache.Load(idB, out) && out.empty(),
          "a symlink under a managed name is a miss, even when its target verifies");
    Check(fs::is_symlink(TrackFile(folder, idB), ec) && Exists(target) && ReadRaw(target) == b,
          "a symlink and its target are left alone");
}

void CheckFolders(const fs::path& root) {
    const fs::path data = root / "data";
    const fs::path exports = root / "export";
    std::string errors;
    Check(gdx::EnsureUserFolders(data, exports, errors) && errors.empty(),
          "EnsureUserFolders succeeds on absolute roots");
    const fs::path tracks = data / "online" / "cache" / "tracks";
    Check(fs::is_directory(tracks), "EnsureUserFolders creates online/cache/tracks under the data root");
    TrackCache cache(tracks);
    Check(cache.ok(), "the cache opens in the folder EnsureUserFolders created");
}

} // namespace

int main() {
    gBase = fs::file_time_type::clock::now() - std::chrono::hours(1);
    const fs::path root = MakeScratch();
    Check(fs::is_directory(root), "a scratch folder is created under the temporary directory");

    CheckRoundTrip(root);
    CheckStoreRefusals(root);
    CheckDamagedFileOnLoad(root);
    CheckSixtyFourCap(root);
    CheckRecencyAndReopen(root);
    CheckByteCap(root);
    CheckUnrelatedFilesSafe(root);
    CheckLeftoverTemp(root);
    CheckRelativeRefused();
    CheckNonRegularNeverFollowed(root);
    CheckFolders(root);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
