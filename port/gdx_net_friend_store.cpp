// port/gdx_net_friend_store.cpp: see gdx_net_friend_store.h.
// Copyright (c) Zorkats

#include "gdx_net_friend_store.h"

#include "gdx_recovery_file.h"

#include <system_error>

namespace gdx::net::friendstore {

namespace {

// The largest valid file: the header, kMaxFriends entries with the longest name, and the checksum.
constexpr size_t kMaxFileBytes =
    7 + friends::kMaxFriends * (friends::kKeyBytes + 1 + friends::kMaxNameBytes) + 16;

} // namespace

std::filesystem::path PathUnder(const std::filesystem::path& dataRoot) {
    return dataRoot / "online" / "friends.gnfl";
}

ReadResult Read(const std::filesystem::path& file, friends::FriendList& out, friends::DecodeResult& why) {
    out = friends::FriendList();
    why = friends::DecodeResult::Ok;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return ec ? ReadResult::Unreadable : ReadResult::Missing;
    }
    recovery::Bytes bytes;
    if (!recovery::readFile(file, bytes, kMaxFileBytes)) {
        return ReadResult::Unreadable;
    }
    friends::FriendList list;
    why = friends::Decode(bytes.data(), bytes.size(), list);
    if (why != friends::DecodeResult::Ok) {
        return ReadResult::Bad;
    }
    out = std::move(list);
    return ReadResult::Ok;
}

bool SetAside(const std::filesystem::path& file) {
    std::filesystem::path aside = file;
    aside += ".bad";
    std::error_code ec;
    std::filesystem::remove(aside, ec);
    std::filesystem::rename(file, aside, ec);
    return !std::filesystem::exists(file, ec) && !ec;
}

bool Write(const std::filesystem::path& file, const friends::FriendList& list) {
    std::vector<unsigned char> bytes;
    if (!friends::Encode(list, bytes)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec || !std::filesystem::is_directory(file.parent_path(), ec)) {
        return false;
    }
    return recovery::writeFile(file, recovery::Bytes(bytes.begin(), bytes.end()));
}

} // namespace gdx::net::friendstore
