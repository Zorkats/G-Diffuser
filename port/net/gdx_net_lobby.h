// port/net/gdx_net_lobby.h: the O2 LAN lobby wire format, LAN discovery datagrams and the chat filter.
//
// Pure: no socket, no clock, no game header. gdx_net_lobby_session.* moves these bytes over the reliable
// channel (gdx_net_channel.h); this file only builds and checks them. Every decoder reads lengths before
// bytes, refuses anything short, long or out of range, and refuses trailing bytes, so a decoder either
// returns a whole valid message or false and leaves its output unchanged.
//
// Lobby messages ride the reliable channel as Message{type, payload}. All integers are little-endian.
// A string is u8 length + bytes.
//
//   1 Join      client to host  build, name, u32 colour (0x00RRGGBB), u8 machine
//   2 Welcome   host to client  u8 player id
//   3 Reject    host to client  u8 RejectReason; the host drops the link right after
//   4 Roster    host to all     u8 count, count x { u8 id, u8 flags (1 host, 2 ready), u32 colour, u8 machine, name }
//   5 Ready     client to host  u8 0 or 1
//   6 Chat      client to host  u16 length + text
//   7 ChatLine  host to all     u8 from id (kSystemId for room notices), u16 length + text
//
// Discovery datagrams travel outside the channel, on the same socket. They start with "GDXL", which the
// channel never sends (its packets start with "GD" and a type byte from 0 to 4), so the host tells them
// apart by the first four bytes.
//
//   Probe   "GDXL" u8 1, u16 kLobbyVersion, u32 nonce
//   Room    "GDXL" u8 2, u16 kLobbyVersion, u32 nonce (the probe's), u16 port, u8 players, u8 cap,
//           u8 open (0 or 1), build, host name
//
// Names follow the profile rule (gdx_profile_name_ok): 1 to kNameMax printable ASCII bytes, no space at
// either end. Chat is printable ASCII only, at most kChatMax bytes after SanitizeChat.

#ifndef GDX_NET_LOBBY_H
#define GDX_NET_LOBBY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx::net::lobby {

constexpr uint16_t kDefaultPort = 24302;
constexpr uint16_t kLobbyVersion = 1;
// Humans per room, the host included (owner decision D1).
constexpr size_t kMaxPlayers = 8;
constexpr size_t kNameMax = 16;
constexpr size_t kBuildMax = 64;
constexpr size_t kChatMax = 120;
// The from id of a room notice ("Ana joined").
constexpr uint8_t kSystemId = 0xFF;
// The host is always player 0.
constexpr uint8_t kHostId = 0;

enum MessageType : uint8_t {
    kMsgJoin = 1,
    kMsgWelcome = 2,
    kMsgReject = 3,
    kMsgRoster = 4,
    kMsgReady = 5,
    kMsgChat = 6,
    kMsgChatLine = 7,
};

// Wire values. Never renumber them.
enum class RejectReason : uint8_t {
    Build = 1,      // a different G-Diffuser build
    Full = 2,       // kMaxPlayers already in
    BadProfile = 3, // the name or machine breaks the rules
    Kicked = 4,     // the host removed this player
};

const char* RejectReasonText(RejectReason reason);

struct PlayerInfo {
    uint8_t id = 0;
    bool host = false;
    bool ready = false;
    uint32_t colour = 0;
    uint8_t machine = 0xFF;
    std::string name;

    bool operator==(const PlayerInfo& o) const {
        return id == o.id && host == o.host && ready == o.ready && colour == o.colour && machine == o.machine &&
               name == o.name;
    }
};

struct JoinRequest {
    std::string build;
    std::string name;
    uint32_t colour = 0;
    uint8_t machine = 0xFF;
};

struct ChatLine {
    uint8_t from = kSystemId;
    std::string text;
};

struct RoomInfo {
    uint16_t port = 0;
    uint8_t players = 0;
    uint8_t cap = 0;
    bool open = false;
    std::string build;
    std::string hostName;
};

// The profile name rule. Machine is a roster index below 30, or 0xFF for none.
bool NameOk(const std::string& name);
bool MachineOk(uint8_t machine);
bool ColourOk(uint32_t colour);

// Keeps printable ASCII, turns tabs and line breaks into spaces, folds runs of spaces into one, trims both
// ends and cuts to kChatMax. Returns "" when nothing printable is left.
std::string SanitizeChat(const std::string& raw);

// Masks blocked words with '*', ignoring case and common digit swaps (0 for o, 1 for i, 3 for e, 4 for a,
// 5 for s, 7 for t, @ for a, $ for s). A built-in list covers common English profanity and slurs. extra
// adds the player's own words, one per entry: a plain word matches at the start of a word, a word written
// with a leading '*' matches inside words as well ("*fuck" catches "motherfucker"), and a word with a
// trailing '|' matches only as a whole word ("ass|"). A match masks the whole word it sits in ("shitty").
// Entries that are empty or hold anything but letters, digits and those marks are ignored. Text is
// printable ASCII, as SanitizeChat leaves it.
std::string FilterChat(const std::string& text, const std::vector<std::string>& extra);

// Encoders. A value that breaks its rule is a caller bug: the encoder still returns bytes, and the decoder on
// the far side refuses them.
std::vector<uint8_t> EncodeJoin(const JoinRequest& join);
std::vector<uint8_t> EncodeWelcome(uint8_t id);
std::vector<uint8_t> EncodeReject(RejectReason reason);
std::vector<uint8_t> EncodeRoster(const std::vector<PlayerInfo>& roster);
std::vector<uint8_t> EncodeReady(bool ready);
std::vector<uint8_t> EncodeChat(const std::string& text);
std::vector<uint8_t> EncodeChatLine(const ChatLine& line);

bool DecodeJoin(const uint8_t* data, size_t len, JoinRequest& out);
bool DecodeWelcome(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeReject(const uint8_t* data, size_t len, RejectReason& out);
// Ids are unique and below kMaxPlayers, exactly one entry is the host and it is id 0, at most kMaxPlayers.
bool DecodeRoster(const uint8_t* data, size_t len, std::vector<PlayerInfo>& out);
bool DecodeReady(const uint8_t* data, size_t len, bool& out);
// The text must already be sanitized: SanitizeChat(text) == text and not empty.
bool DecodeChat(const uint8_t* data, size_t len, std::string& out);
bool DecodeChatLine(const uint8_t* data, size_t len, ChatLine& out);

// Discovery.
bool IsDiscovery(const uint8_t* data, size_t len);
std::vector<uint8_t> EncodeProbe(uint32_t nonce);
std::vector<uint8_t> EncodeRoom(uint32_t nonce, const RoomInfo& room);
bool DecodeProbe(const uint8_t* data, size_t len, uint32_t& nonce);
bool DecodeRoom(const uint8_t* data, size_t len, uint32_t& nonce, RoomInfo& out);

} // namespace gdx::net::lobby

#endif // GDX_NET_LOBBY_H
