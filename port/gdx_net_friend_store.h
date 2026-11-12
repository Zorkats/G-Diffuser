// port/gdx_net_friend_store.h: the friend list file on disk (G-NET O7.4).
// Copyright (c) Zorkats
//
// The bytes are those of net/gdx_net_friends.h; this module only moves them to and from one file, through the
// recovery writer, so a crash mid-write leaves the old list or the new one and never half of each. The game keeps
// the file at <data root>/online/friends.gnfl.
//
// A file that fails its check is moved aside to <file>.bad before anything replaces it: the player's list is
// never overwritten because one byte of it went bad, and the old bytes stay for whoever wants to look.

#pragma once

#include "net/gdx_net_friends.h"

#include <filesystem>

namespace gdx::net::friendstore {

// The file's name under the data root.
std::filesystem::path PathUnder(const std::filesystem::path& dataRoot);

enum class ReadResult {
    Ok,
    Missing,    // no file: an empty list
    Unreadable, // the file is there and could not be read, or is larger than any list
    Bad,        // the bytes failed Decode; why says how
};

// Reads the whole file. out is the empty list unless the result is Ok.
ReadResult Read(const std::filesystem::path& file, friends::FriendList& out, friends::DecodeResult& why);

// Moves a bad file to <file>.bad, replacing an older one there. true when nothing is left at file.
bool SetAside(const std::filesystem::path& file);

// Writes the list, making the directory above the file when it is missing. false, with the old file unchanged,
// when the list is not valid or the write failed.
bool Write(const std::filesystem::path& file, const friends::FriendList& list);

} // namespace gdx::net::friendstore
