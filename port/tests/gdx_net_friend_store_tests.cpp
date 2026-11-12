// port/tests/gdx_net_friend_store_tests.cpp: the friend list file on disk (G-NET O7.4).
// Copyright (c) Zorkats
//
// Usage: gdx_net_friend_store_tests
//
// A round trip through a folder that does not exist yet, a missing file read as the empty list, a damaged file
// refused and set aside so the next write keeps its bytes, an invalid list never written, and a file too large
// to be a list refused unread. Runs in a fresh folder under the temporary directory, removed at the end.

#include "gdx_net_friend_store.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace friends = gdx::net::friends;
namespace store = gdx::net::friendstore;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

fs::path MakeScratch() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root =
        fs::temp_directory_path() / ("gdx_friend_store_tests_" + std::to_string(static_cast<long long>(stamp)));
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

friends::Key KeyOf(unsigned char seed) {
    friends::Key key{};
    for (size_t i = 0; i < key.size(); i++) {
        key[i] = static_cast<unsigned char>(seed + i);
    }
    return key;
}

std::vector<char> Bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void CheckRoundTrip(const fs::path& root) {
    const fs::path file = store::PathUnder(root / "data");
    Check(file == root / "data" / "online" / "friends.gnfl", "the file is online/friends.gnfl under the data root");

    friends::FriendList list;
    friends::DecodeResult why = friends::DecodeResult::BadMagic;
    Check(store::Read(file, list, why) == store::ReadResult::Missing, "no file reads as Missing");
    Check(list.entries.empty() && why == friends::DecodeResult::Ok, "a missing file is the empty list");

    friends::Add(list, KeyOf(1), "Ana");
    friends::Add(list, KeyOf(50), "Bo Racer");
    Check(store::Write(file, list), "a list is written, making online/ under a data root that has none");

    friends::FriendList back;
    Check(store::Read(file, back, why) == store::ReadResult::Ok, "the written list reads back");
    Check(back.entries.size() == 2 && back.entries[0].key == KeyOf(1) && back.entries[0].name == "Ana" &&
              back.entries[1].key == KeyOf(50) && back.entries[1].name == "Bo Racer",
          "keys, names and their order survive the round trip");

    friends::Remove(back, KeyOf(1));
    Check(store::Write(file, back), "a shorter list replaces the file");
    friends::FriendList again;
    Check(store::Read(file, again, why) == store::ReadResult::Ok && again.entries.size() == 1 &&
              again.entries[0].name == "Bo Racer",
          "the replaced file holds the shorter list");
    std::error_code ec;
    size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(file.parent_path(), ec)) {
        leftovers += entry.path().filename() != "friends.gnfl" ? 1 : 0;
    }
    Check(leftovers == 0, "a write leaves no temporary file beside the list");
}

void CheckDamaged(const fs::path& root) {
    const fs::path file = store::PathUnder(root / "damaged");
    friends::FriendList list;
    friends::Add(list, KeyOf(7), "Cy");
    Check(store::Write(file, list), "a list to damage is written");
    std::vector<char> bytes = Bytes(file);
    bytes[10] = static_cast<char>(bytes[10] ^ 0x40);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    friends::FriendList read;
    friends::DecodeResult why = friends::DecodeResult::Ok;
    Check(store::Read(file, read, why) == store::ReadResult::Bad, "a flipped byte reads as Bad");
    Check(why == friends::DecodeResult::BadChecksum, "the checksum is what refuses it");
    Check(read.entries.empty(), "a Bad file gives the empty list");

    Check(store::SetAside(file), "the bad file is set aside");
    fs::path aside = file;
    aside += ".bad";
    Check(!fs::exists(file) && Bytes(aside) == bytes, "the bad bytes are kept whole at friends.gnfl.bad");

    friends::FriendList fresh;
    friends::Add(fresh, KeyOf(9), "Di");
    Check(store::Write(file, fresh), "a new list is written after the bad one was set aside");
    Check(Bytes(aside) == bytes, "writing the new list leaves the set aside bytes alone");

    // A second bad file replaces the first one set aside, so they never pile up.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "GNFL";
    }
    Check(store::Read(file, read, why) == store::ReadResult::Bad && why == friends::DecodeResult::Truncated,
          "a cut file reads as Bad, Truncated");
    Check(store::SetAside(file) && Bytes(aside) == std::vector<char>{ 'G', 'N', 'F', 'L' },
          "the newer bad file replaces the older one set aside");
}

void CheckRefusals(const fs::path& root) {
    const fs::path file = store::PathUnder(root / "refusals");
    friends::FriendList list;
    friends::Add(list, KeyOf(3), "Ed");
    Check(store::Write(file, list), "a good list is written first");
    const std::vector<char> before = Bytes(file);

    friends::FriendList twice = list;
    twice.entries.push_back(twice.entries[0]);
    Check(!store::Write(file, twice), "a list with a key twice is not written");
    friends::FriendList badName = list;
    badName.entries[0].name = " Ed";
    Check(!store::Write(file, badName), "a list with a bad name is not written");
    Check(Bytes(file) == before, "a refused list leaves the file as it was");

    // Larger than any list can be: refused without reading it.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::string big(64 * 1024, 'x');
        out << big;
    }
    friends::FriendList read;
    friends::DecodeResult why = friends::DecodeResult::Ok;
    Check(store::Read(file, read, why) == store::ReadResult::Unreadable, "a file larger than any list is Unreadable");
    Check(read.entries.empty(), "an Unreadable file gives the empty list");
}

} // namespace

int main() {
    const fs::path root = MakeScratch();
    Check(fs::is_directory(root), "a scratch folder is created under the temporary directory");

    CheckRoundTrip(root);
    CheckDamaged(root);
    CheckRefusals(root);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
