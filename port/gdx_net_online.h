// port/gdx_net_online.h: the game's side of the room directory (G-NET O5.10 to O5.12, O6.5, O7.4).
// Copyright (c) Zorkats
//
// The headless pieces (net/gdx_net_directory, net/gdx_net_listing, net/gdx_net_gather) get their transport, their
// helper threads, their random bytes and their settings here:
//   - The directory transport is the shared HTTPS helper (net/gdx_net_https).
//   - Every directory call and every system lookup runs on a detached helper thread, so neither the game thread nor
//     the lobby worker's 10 ms tick ever waits on the network.
//   - Settings are read on the game thread when a session starts, and handed in.
//
// Developer seam: GDX_NET_DEV_DIRECTORY=127.0.0.1:<port> sends every directory call to a local server over plain
// http (the HTTPS helper allows http to loopback only). Nothing else changes, so a scripted run exercises the real
// client against the Worker's own handler.

#ifndef GDX_NET_ONLINE_H
#define GDX_NET_ONLINE_H

#include "gdx_profile_keys.h"
#include "net/gdx_net_directory.h"
#include "net/gdx_net_gather.h"
#include "net/gdx_net_lobby.h"
#include "net/gdx_net_socket.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gdx::online {

// ---- Settings, read on the game thread ----

// The directory's base URLs, tried in order: the developer seam, else the gEnhancements.Online.Directory list
// (comma-separated), else the list this build was made with (GDX_NET_DIRECTORY_URLS, else the project's Worker).
std::vector<std::string> DirectoryBases();
bool DirectoryConfigured();
// Whether the developer's local test directory (GDX_NET_DEV_DIRECTORY) is set: loopback addresses may be dialed.
bool LocalTestDirectory();
// The STUN servers, "name:port" each (decision 28): gEnhancements.Online.StunServers, comma-separated.
std::vector<std::string> StunServers();
// The address the player lists in place of their own (O5.11), or none. gEnhancements.Online.Advertise; port 0
// stands for the room socket's own port.
std::optional<net::NetAddress> AdvertiseAddress();
// The setting's raw text, for the page; empty when unset.
std::string AdvertiseText();
// The active profile's signing seed (gdx_profile_state_active_seed), made the first time it is needed and then
// written to profile-keys.gpk at once: a seed that is not saved would give the profile a new friend code on the
// next launch, and its friends and bans would no longer know it. Game thread only.
int ActiveSeed(unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]);
// The active profile's friend key as 64 lower-case hex digits (decision 41): the room's host key in the directory.
// Makes the profile's seed on first use. false when no profile is active or the seed cannot be made.
bool HostKey(std::string& hex);
bool HostKeyBytes(net::lobby::Bytes32& key);

// ---- Pieces the worker and the pages share ----

net::directory::Client MakeClient(const std::vector<std::string>& bases);
void Spawn(std::function<void()> job);
void RandomBytes(uint8_t* out, size_t n);
// "ABC-DEF" for a room code value, or empty for kNoRoomCode.
std::string CodeText(uint32_t code);

// ---- A hosted room's listing, driven by the lobby worker ----

enum class ListState : uint8_t {
    Off,       // a LAN room, or no directory
    Gathering, // asking STUN servers and the gateway
    Listing,   // registering or taking over the code
    Listed,
    Failed,    // the room runs on, unlisted; text says why
};

struct ListView {
    ListState state = ListState::Off;
    std::string text;      // a line for the page: what is happening, or why it failed
    bool reachable = false;
    bool mapped = false;
    std::string publicAddress; // "203.0.113.7:24302" as STUN saw it, or empty
    std::string publicAddressLog; // the same, cut for the log (host_info::LogText)
};

struct ListOptions {
    std::vector<std::string> bases;
    std::vector<std::string> stunServers;
    std::optional<net::NetAddress> advertise;
    std::string hostKey;
    // Signs the directory writes with hostKey's secret half (CONTRACT.md §8a). With none, nothing is listed.
    net::directory::Signer signer;
    net::lobby::RoomType type = net::lobby::RoomType::Code;
    uint16_t localPort = 0;
    bool dualStack = false;
    // A successor's takeover (decision 32): the code the room carries and the secret the old host handed over.
    uint32_t takeoverCode = net::lobby::kNoRoomCode;
    net::lobby::Bytes32 takeoverSecret{};
};

// The room's seats, as the directory shows them.
struct Seats {
    uint32_t humans = 0;
    uint32_t free = 0;
};
Seats SeatsOf(const net::lobby::RoomState& room, const std::vector<net::lobby::PlayerInfo>& roster);

class RoomListing {
public:
    RoomListing(ListOptions options, net::gather::Send send);
    ~RoomListing();
    RoomListing(const RoomListing&) = delete;
    RoomListing& operator=(const RoomListing&) = delete;

    void Update(uint32_t now, const Seats& seats);
    // true when the datagram answered a STUN or port-mapping request; the room never sees it then.
    bool Receive(const net::NetAddress& from, const uint8_t* data, size_t len, uint32_t now);
    // The room ended with nobody to take it over: the record is deleted now rather than in 180 s. Call before
    // Release. A room handed to a successor, or closed silently, is not delisted.
    void Delist();
    // The room is closing: deletes the port mapping.
    void Release(uint32_t now);
    // The successor who held the takeover secret changed: the secret is replaced (listing::Listing::RotateSecret).
    // Before the room is listed there is nothing to replace: the register or takeover sets a fresh one.
    void RotateSecret(uint32_t now);
    // When the code or the takeover secret changed since the last call: the pair the room must carry now.
    bool TakeChange(uint32_t& code, net::lobby::Bytes32& secret);
    ListView View() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- A member's own reachability (decision 51) ----

// What a member of a directory room gathers on its own room socket: STUN, PCP or NAT-PMP, and its IPv6 addresses,
// the same gathering a host runs before it lists the room. The member tells the host the one flag; the host elects
// a reachable member first.
struct ReachOptions {
    std::vector<std::string> stunServers;
    std::optional<net::NetAddress> advertise; // the player's own forwarded address; port 0: the socket's
    uint16_t localPort = 0;
    bool dualStack = false;
};

class MemberReach {
public:
    MemberReach(ReachOptions options, net::gather::Send send);
    ~MemberReach();
    MemberReach(const MemberReach&) = delete;
    MemberReach& operator=(const MemberReach&) = delete;

    // Starts the gathering once the addresses are resolved, then keeps the port mapping renewed.
    void Update(uint32_t now);
    // true when the datagram answered a STUN or port-mapping request; the room never sees it then.
    bool Receive(const net::NetAddress& from, const uint8_t* data, size_t len, uint32_t now);
    // The gathering finished; Reachable is final then.
    bool Done() const;
    bool Reachable() const;
    // The member leaves the room or hosts it now: deletes the port mapping. Nothing is sent afterwards.
    void Release(uint32_t now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Joining by code: one lookup, off the worker's tick ----

class CodeLookup {
public:
    CodeLookup(std::vector<std::string> bases, uint32_t code);
    bool Done() const;
    // Valid once Done.
    net::directory::Answer<std::optional<net::directory::Record>> Result() const;

private:
    struct Slot;
    std::shared_ptr<Slot> slot_;
};

// The addresses to try for a record, in its order, that this socket can reach.
std::vector<net::NetAddress> JoinAddresses(const net::directory::Record& record, bool dualStack);

// ---- The pages' lists: Open rooms, Quick race, a friend's rooms (O6.5, O7.4) ----

enum class Ask : uint8_t { Open, Quick, Friend };

struct Lists {
    enum class Status : uint8_t { Idle, Asking, Ready, Failed };
    Status status = Status::Idle;
    Ask ask = Ask::Open;     // the request the status is about
    std::string text;        // the failure, said for the page
    uint32_t serial = 0;     // grows with each answer
    std::vector<net::directory::Record> rooms; // Open: the list; Friend: the friend's rooms
    bool quickFound = false; // Quick: whether a room was found
    net::directory::Record quick;
};

// Sends one request. Open, Quick and a friend's rooms share the 30 s cooldown (decision 38); a refused press
// says how long is left in waitMs and sends nothing. false when nothing was sent.
bool AskDirectory(Ask ask, const std::string& friendKeyHex, uint32_t now, uint32_t& waitMs);
Lists ListsSnapshot();

// ---- The friend list (O7.4) ----
//
// One list per install, at <data root>/online/friends.gnfl (gdx_net_friend_store.h). It is read on first use and
// written through on every change, under one lock, so the pages and a starting session may call these from any
// thread, except AddFriend: it reads the profile's own key, which is game thread only. A file that fails its check
// is set aside and the list starts empty; one that cannot be read shows empty, and nothing is written over it.

struct FriendEntry {
    std::string name;
    net::lobby::Bytes32 key{};
};

std::vector<FriendEntry> Friends();
std::vector<net::lobby::Bytes32> FriendKeys();

enum class FriendAdd : uint8_t {
    Added,
    Updated,  // the key was listed; its name is now the one given
    Same,     // listed already under that name: nothing written
    Full,
    BadName,
    Self,     // the active profile's own key
    NotSaved, // the file could not be written; the list is as it was
};

FriendAdd AddFriend(const net::lobby::Bytes32& key, const std::string& name);
// false when the key is not listed or the file could not be written.
bool RemoveFriend(const net::lobby::Bytes32& key);
bool IsFriend(const net::lobby::Bytes32& key);
// The name for a friend added by code, until a room shows theirs: "Friend 3", the lowest number no friend has.
std::string NewFriendName();
// The display form of a key: 56 characters in 14 groups of 4.
std::string FriendCodeOf(const net::lobby::Bytes32& key);

} // namespace gdx::online

#endif // GDX_NET_ONLINE_H
