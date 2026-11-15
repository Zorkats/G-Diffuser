// port/net/gdx_net_directory.h: room directory client, headless: rules, encoders, reader, failover (G-NET O5.5).
// Copyright (c) Zorkats
//
// The directory is a Cloudflare Worker the owner deploys (CONTRACT.md §1). This module opens no socket. The caller
// passes a Transport that performs one HTTPS request and reports what came back. Nothing here reads a clock: the
// directory reads its own, and no room call carries a time (CONTRACT.md §5). A relay ticket carries the expiry its
// host chose, which the caller passes in (CONTRACT.md §5.9).
//
// Every rule is cited to its CONTRACT.md section. A rule changes in the contract first. The tests in
// port/tests/gdx_net_directory_tests.cpp check the vectors of CONTRACT.md §9.
//
// Every write is signed (CONTRACT.md §8a). The caller passes a Signer that signs with the secret half of the draft's
// host key, so this module never holds a secret key. A write with no signer is refused before anything is sent.
//
// Failover, per call, through the base URLs in order. A transport failure moves on to the next base, for reads and
// writes alike. A 5xx moves on for a read only. A write may have been applied before the error (CONTRACT.md §6), and
// a retry would then answer code_taken or bad_secret for a write that succeeded. A 4xx answer, and any 2xx answer, is
// final. Relay credentials (CONTRACT.md §5.9) are a POST that changes no room, so they fail over like a read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gdx::net::directory {

// Limits (CONTRACT.md §2, §3, §5, §7).
constexpr size_t kBodyMax = 16384;
constexpr uint32_t kTtlSeconds = 180;
constexpr size_t kListMax = 10;
constexpr size_t kEndpointsMax = 8;
constexpr uint32_t kHumansMax = 12;
constexpr uint32_t kProtocolMax = 65535;
constexpr size_t kJsonDepthMax = 8;
constexpr uint64_t kJsonIntegerMax = 9007199254740991ull;

// The wire names of the enums are in the encoders and the reader. The numbers are not wire values.
enum class RoomType : uint8_t { Code, Open, Friends };
enum class Family : uint8_t { V4, V6 };
enum class How : uint8_t { Manual, Upnp, Natpmp, Pcp, Stun, Ipv6 };

struct Endpoint {
    Family family = Family::V4;
    std::string addr;
    uint16_t port = 0;
    How how = How::Manual;
};

// The room's state, as the host sends it with every register, heartbeat and takeover (CONTRACT.md §3, Draft).
struct Draft {
    RoomType type = RoomType::Open;
    uint32_t humans = 0;
    uint32_t freeSeats = 0;
    bool reachable = false;
    uint16_t protocol = 0;
    std::string hostKey;
    std::vector<Endpoint> endpoints;
};

// What the directory returns: the draft, the code and the seconds left (CONTRACT.md §3, Record).
struct Record {
    std::string code;
    Draft draft;
    uint32_t ttl = 0;
};

// Relay credentials for Cloudflare's TURN service (CONTRACT.md §5.9).
constexpr uint32_t kTurnTtlSeconds = 7200;
constexpr size_t kTurnServersMax = 8;
constexpr size_t kTurnTextMax = 512;

struct TurnGrant {
    std::string username;
    std::string password;
    uint32_t ttl = 0;
    std::vector<std::string> servers; // "host:port", UDP, in the directory's order
};

// A server of a grant, "host:port" (CONTRACT.md §5.9): a host of 1 to 253 letters, digits, '.' and '-', not starting
// with either of the last two, and a port of 1 to 65535 with no leading zero. On failure host and port are left alone.
bool SplitTurnServer(std::string_view text, std::string& host, uint16_t& port);

// The takeover secret: 32 random bytes from the host's CSPRNG (CONTRACT.md §8). The caller creates it.
using Secret = std::array<uint8_t, 32>;

// An Ed25519 signature (RFC 8032) over SigningMessage (CONTRACT.md §8a).
using Signature = std::array<uint8_t, 64>;

// Signs message with the secret half of the host key the write names: the draft's hostKey on register, heartbeat
// and takeover, the record's on delist. Returns false when it cannot, and the write is then not sent.
using Signer = std::function<bool(std::string_view message, Signature& out)>;

// The first line of every signed message (CONTRACT.md §8a).
constexpr std::string_view kSignLabel = "G-NET directory v1";

// Relay tickets (CONTRACT.md §5.9). The host signs one for its live room under its own first line, so a ticket can
// never stand for a directory write. It expires kTicketLifetimeSeconds out, and the host sends a new one every
// kTicketRefreshSeconds, so a member always holds one the directory takes (it allows 300 s past the expiry).
constexpr std::string_view kRelayLabel = "G-NET relay v1";
constexpr uint32_t kTicketLifetimeSeconds = 600;
constexpr uint32_t kTicketRefreshSeconds = 300;

struct RelayTicket {
    std::string code;
    uint64_t expires = 0; // Unix time in seconds
    Signature sig{};
};

// The ticket's body before sig: {"code":"...","expires":N}. Refuses a code that fails §3 and an expiry outside the
// §2 integer range. On refusal out is left alone.
bool EncodeRelayTicket(std::string_view code, uint64_t expires, std::string& out);

// The bytes a ticket's signature covers: kRelayLabel, "\nPOST /v1/turn\n", then the body EncodeRelayTicket wrote.
std::string RelaySigningMessage(std::string_view unsignedBody);

// Signs a ticket with the host's signer, the one its directory writes use. false, and out left alone, when the
// ticket cannot be encoded or the signer refuses.
bool SignRelayTicket(std::string_view code, uint64_t expires, const Signer& signer, RelayTicket& out);

// The friend mailbox (CONTRACT.md §5.10, §5.11). A send is signed by the sender's friend key and a collect by the
// inbox's, both under kFriendLabel, so neither can stand for a room write or a relay ticket. The client signs a
// collect that expires kCollectLifetimeSeconds out; the directory takes it inside the relay ticket's window.
constexpr std::string_view kFriendLabel = "G-NET friends v1";
constexpr uint32_t kFriendTtlSeconds = 1209600; // 14 days
constexpr size_t kFriendInboxMax = 20;
constexpr size_t kFriendNameMax = 16;
constexpr uint32_t kCollectLifetimeSeconds = 600;

// One request a collect returned: the sender's friend key (§3) and the name they sent.
struct FriendRequestEntry {
    std::string fromKey;
    std::string name;
};

// A name a friend request may carry (CONTRACT.md §3): 1 to kFriendNameMax printable ASCII bytes, no space at
// either end. The profile name rule, so every profile name passes.
bool IsFriendName(std::string_view text);

// The send's body before sig: {"fromKey":"...","name":"..."}, with a quote or backslash in the name escaped as §2
// allows. Refuses a key or name that fails §3. On refusal out is left alone.
bool EncodeFriendRequest(std::string_view fromKey, std::string_view name, std::string& out);

// The collect's body before sig: {"expires":N}. Refuses an expiry outside the §2 integer range.
bool EncodeFriendCollect(uint64_t expires, std::string& out);

// The bytes a mailbox signature covers: kFriendLabel, "\nPOST ", the path, a line feed, then the body.
std::string FriendSigningMessage(std::string_view path, std::string_view unsignedBody);

struct RegisterRequest {
    std::string code;
    // TakeoverHashOf(the secret the host keeps). Sent on register only (CONTRACT.md §4).
    std::string takeoverHash;
    Draft draft;
};

struct HeartbeatRequest {
    std::string code;
    Secret secret{};
    Draft draft;
};

struct TakeoverRequest {
    std::string code;
    // The secret that is valid now. It stops working on success (CONTRACT.md §5.3).
    Secret secret{};
    // TakeoverHashOf(the successor's secret), which the successor will need for its own takeover (CONTRACT.md §8).
    std::string nextHash;
    Draft draft;
};

// The host closes its room (CONTRACT.md §5.8). Signed by the record's host key.
struct DelistRequest {
    std::string code;
    Secret secret{};
};

// A parsed value in the strict grammar of CONTRACT.md §2. An object keeps its members in order. The parser refuses a
// repeated key, so keys[i] names values[i] and no name appears twice.
struct JsonValue {
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    uint64_t number = 0;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    // The member with this name, or null. Only an Object has members.
    const JsonValue* Find(std::string_view key) const;
};

// Validation, CONTRACT.md §3. Each check is the rule the directory applies, so a value that passes here is one the
// directory accepts.
bool IsCode(std::string_view text);
bool IsHostKey(std::string_view text);
bool IsIpv4(std::string_view text);
bool IsIpv6(std::string_view text);
bool DraftValid(const Draft& draft);

// Secrets and takeover hashes, CONTRACT.md §8. The hash is over the 32 raw bytes, not the hex text (§3).
std::string SecretHex(const Secret& secret);
bool ParseSecretHex(std::string_view text, Secret& out);
std::string TakeoverHashOf(const Secret& secret);

// The strict reader of CONTRACT.md §2: the grammar the directory sends, and nothing looser. On failure out is left
// alone. It does not check the body cap; the client does, before it parses.
bool ParseStrictJson(std::string_view text, JsonValue& out);

// The encoders refuse what the directory would refuse, so a request the client sends is never answered bad_field.
// Keys are written in the order of CONTRACT.md §4 and §5. On refusal out is left alone.
bool EncodeRegister(const RegisterRequest& request, std::string& out);
bool EncodeHeartbeat(const HeartbeatRequest& request, std::string& out);
bool EncodeTakeover(const TakeoverRequest& request, std::string& out);
bool EncodeDelist(const DelistRequest& request, std::string& out);

// The bytes a write's signature covers (CONTRACT.md §8a): kSignLabel, a line feed, "POST ", the path, a line feed,
// then the body an encoder wrote, before its sig.
std::string SigningMessage(std::string_view path, std::string_view unsignedBody);

// Appends ,"sig":"<128 hex>" as the body's last member. The body is one an encoder wrote, so it ends with '}'.
// Returns false, and leaves body alone, when it does not.
bool AppendSignature(std::string& body, const Signature& signature);

// One exchange as the client asks for it. contentType is set on POST only and is application/json (CONTRACT.md §2).
struct Request {
    std::string method;
    std::string url;
    std::string contentType;
    std::string body;
};

// What the transport got back. ok is false when no HTTP answer arrived (connect, TLS, timeout). Then status and body
// are ignored. retryAfter is the raw Retry-After header, or empty.
struct Response {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string retryAfter;
};

// Performs one exchange. It must not throw: a failure is reported through Response::ok.
using Transport = std::function<Response(const Request&)>;

// Every outcome a call can report. The names after Malformed are the directory's error tokens (CONTRACT.md §6).
enum class Error : uint8_t {
    None,
    BadConfig,        // no base URL, or one that is not https:// (CONTRACT.md §2). Nothing was sent
    Invalid,          // the caller's input breaks CONTRACT.md §2 or §3, or a write has no signer. Nothing was sent
    Unreachable,      // no base gave a usable answer. A write that ends here may still have been applied
    Malformed,        // an answer broke the contract: a body outside the grammar, or a status its token does not carry
    BadJson,
    BadField,
    BadCode,
    BadHostKey,
    BadQuery,
    UnsupportedMedia,
    TooLarge,
    BadSecret,
    BadSignature,     // the sig did not verify, or a heartbeat named another key than the record's (CONTRACT.md §8a)
    StaleTicket,      // a relay ticket's expiry is too far past or ahead of the directory's clock (CONTRACT.md §5.9)
    NotFound,
    NoRoute,
    MethodNotAllowed,
    CodeTaken,
    InboxFull,        // 409: the recipient's inbox holds kFriendInboxMax requests (CONTRACT.md §5.10)
    RateLimited,
    Internal,         // a write may already be applied (CONTRACT.md §6): report it, never retry it elsewhere
    TurnUpstream,     // 502: Cloudflare gave the directory no relay credentials (CONTRACT.md §5.9); try again later
    TurnUnavailable,  // 503: the directory has no relay configured; play on without one
};

// The token for logs and tests: "code_taken", "unreachable" and so on.
const char* ErrorName(Error error);

// A call's result. value is the call's success payload, and is empty on any error. retryAfterSeconds is set on
// RateLimited: the whole seconds to wait, from Retry-After (CONTRACT.md §6). It is 0 otherwise.
template <typename T>
struct Answer {
    Error error = Error::None;
    uint32_t retryAfterSeconds = 0;
    T value{};
};

class Client {
public:
    // bases are tried in order for every call. Each must start with https:// and must not end with a slash.
    Client(std::vector<std::string> bases, Transport transport);

    // The signer every write uses (CONTRACT.md §8a). Reads need none.
    void SetSigner(Signer signer);

    // Value: the ttl, which is 180 (CONTRACT.md §5.1).
    Answer<uint32_t> Register(const RegisterRequest& request) const;
    // Value: the ttl (CONTRACT.md §5.2).
    Answer<uint32_t> Heartbeat(const HeartbeatRequest& request) const;
    // Value: the ttl (CONTRACT.md §5.3).
    Answer<uint32_t> Takeover(const TakeoverRequest& request) const;
    // Value: 0, the ttl of a deleted record (CONTRACT.md §5.8).
    Answer<uint32_t> Delist(const DelistRequest& request) const;
    // Value: the room. NotFound when no live room has the code (CONTRACT.md §5.4).
    Answer<std::optional<Record>> Lookup(std::string_view code) const;
    // Value: up to kListMax rooms, most humans first (CONTRACT.md §5.5).
    Answer<std::vector<Record>> OpenRooms(uint16_t protocol) const;
    // Value: empty when no reachable Open room has a seat. That is an answer, not an error (CONTRACT.md §5.6).
    Answer<std::optional<Record>> QuickRace(uint16_t protocol) const;
    // Value: the rooms the key hosts, of any type (CONTRACT.md §5.7).
    Answer<std::vector<Record>> HostRooms(std::string_view hostKey, uint16_t protocol) const;
    // Value: credentials for the relay (CONTRACT.md §5.9), asked with the room host's ticket. A base that answers 502
    // or 503 is passed over for the next; when none gives credentials, the error is the last of those two any base
    // gave, else Unreachable. Invalid, with nothing sent, when the ticket cannot be encoded.
    Answer<TurnGrant> TurnCredentials(const RelayTicket& ticket) const;
    // Value: the request's lifetime, kFriendTtlSeconds (CONTRACT.md §5.10). The signer must sign with fromKey's
    // secret half. A send changes the directory, so it fails over as a write does.
    Answer<uint32_t> SendFriendRequest(std::string_view toKey, std::string_view fromKey, std::string_view name) const;
    // Value: the requests sent to key, oldest first, which the directory deletes as it answers (CONTRACT.md §5.11).
    // The signer must sign with key's secret half. expires is Unix seconds, chosen by the caller. A collect deletes,
    // so it fails over as a write does: a 5xx may have emptied the inbox.
    Answer<std::vector<FriendRequestEntry>> CollectFriendRequests(std::string_view key, uint64_t expires) const;

private:
    // Signs body for path under label and sends it as a write. Invalid when there is no signer or it refuses.
    template <typename T>
    Answer<T> Write(std::string_view label, const std::string& path, std::string body, int expected,
                    bool (*read)(const JsonValue&, T&)) const;

    std::vector<std::string> bases_;
    Transport transport_;
    Signer signer_;
};

}  // namespace gdx::net::directory
