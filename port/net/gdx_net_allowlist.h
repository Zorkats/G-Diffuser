// port/net/gdx_net_allowlist.h -- the signed stable allowlist that gates public rooms (G-NET O5.9).
// Copyright (c) Zorkats
//
// The owner signs a list with an Ed25519 key. The game ships only the public half, so a copy can tell a list the
// owner published from one that someone else made, and it refuses any list older than one it has already passed
// (plan decision 30). A list names the stable release that public rooms require, the older builds that may still
// join while listed, and the yanked builds with the reason to show (ODD 9.1).
//
// Pure: no file, no socket, no timer. The caller fetches the bytes, keeps the state blob, and passes in the
// monotonic and Unix times. The gate covers public rooms only: the Open, Code and Friends-only rooms that the
// directory lists (ODD 3.3). LAN, direct connect and offline play never call Admit. Their only check is the
// protocol id, which the join handshake compares, so nothing in this file runs on those paths.
//
// Format. Integers are little-endian. A Str8 is a u8 length followed by that many bytes.
//
//   "GNAL"                                              4 bytes, magic
//   u8                                                  format version, 1
//   u32                                                 protocol id
//   u64                                                 sequence, raised by every new list
//   u64                                                 issued, Unix seconds, informational only
//   u64                                                 grace_until, Unix seconds, or 0 for no deadline
//   Str8                                                current tag
//   u8 count, then count x Str8                         allowed tags
//   u8 count, then count x { Str8 tag, Str8 reason }    yanked builds
//   64 bytes                                            Ed25519 signature over every byte before it
//
// A tag is 1 to 48 printable ASCII bytes with no space, since the owner's text format splits on spaces. A reason is
// 0 to 120 printable ASCII bytes. The current tag must be in allowed. A yanked tag is refused even when it is also
// allowed. Decoding walks the lengths only far enough to find the signature, checks the signature, and then reads
// and checks the fields, so no field is trusted before it is signed.
//
// Trust and replay. A copy keeps the highest sequence it has accepted, in an 8-byte state blob the caller stores,
// and refuses a list below it. A list with the same sequence and the same signature is a refresh: it restarts the
// trust window. A held list is trusted for kTrustMs of monotonic time after the last accepted copy. The blob
// survives a launch and the list does not, so after a launch the copy fetches a list before public rooms open.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx::net::allowlist {

constexpr uint8_t kFormatVersion = 1;
constexpr size_t kTagMax = 48;
constexpr size_t kReasonMax = 120;
constexpr size_t kMaxCount = 255;        // a u8 count
constexpr size_t kMaxBytes = 65536;      // larger buffers are refused before they are scanned
constexpr size_t kSeedBytes = 32;
constexpr size_t kPublicKeyBytes = 32;
constexpr size_t kSecretKeyBytes = 64;
constexpr size_t kSignatureBytes = 64;
constexpr size_t kStateBytes = 8;
// The trust window of plan decision 30 and ODD 9.1: 24 hours on the monotonic clock.
constexpr uint64_t kTrustMs = 24ull * 60ull * 60ull * 1000ull;

using Seed = std::array<uint8_t, kSeedBytes>;
using PublicKey = std::array<uint8_t, kPublicKeyBytes>;
using SecretKey = std::array<uint8_t, kSecretKeyBytes>;
using Signature = std::array<uint8_t, kSignatureBytes>;
using StateBlob = std::array<uint8_t, kStateBytes>;

struct Yank {
    std::string tag;
    std::string reason;
};

struct List {
    uint32_t protocol = 0;
    uint64_t sequence = 0;
    uint64_t issued = 0;      // Unix seconds, informational only
    uint64_t graceUntil = 0;  // Unix seconds, 0 for no deadline
    std::string current;
    std::vector<std::string> allowed;
    std::vector<Yank> yanked;
};

// What a copy remembers about the lists it has accepted.
struct State {
    uint64_t highestSequence = 0;  // carried across launches by EncodeState and DecodeState
    bool held = false;             // a list was accepted in this run; the list itself is not persisted
    uint64_t acceptedMonoMs = 0;   // monotonic time of the last accepted copy
    Signature heldSignature{};     // the signature of the held list; equal sequence and signature is a refresh
    List list;
};

enum class Verdict { Ok, BadSignature, Malformed, Replayed };

// Why a public room refuses this build. Allowed means the room may be joined.
enum class Refusal { Allowed, NoList, Stale, ProtocolMismatch, Prerelease, Yanked, NotListed };

const char* VerdictName(Verdict verdict);
const char* RefusalName(Refusal refusal);

// Derives the key pair from a seed. The seed is not changed. Monocypher keeps the public key in secret bytes 32..63.
void KeyPairFromSeed(const Seed& seed, PublicKey& publicKey, SecretKey& secretKey);

// Null when the list may be signed or accepted, else the first rule it breaks. Sign and Decode both use it.
const char* CheckList(const List& list);

// Writes the signed bytes. Returns false, and writes nothing, when CheckList refuses the list.
bool Sign(const List& list, const SecretKey& secretKey, std::vector<uint8_t>& out);

// Checks the structure, then the signature, then the fields. out is written only on Ok.
Verdict Decode(const std::vector<uint8_t>& bytes, const PublicKey& publicKey, List& out);

// Decode, then the sequence rule. On Ok the state holds the list and the monotonic time it was accepted.
// Replayed: a sequence below the highest one seen, or the held sequence with a different signature.
Verdict Accept(const std::vector<uint8_t>& bytes, const PublicKey& publicKey, uint64_t nowMonoMs, State& state);

// True for kTrustMs after the last accepted copy. A monotonic clock that goes backwards is not trusted.
bool Trusted(const State& state, uint64_t nowMonoMs);

// The gate, for public rooms only. The first failing check is the refusal, in this order:
//   channel      ownChannel must be "stable". Nightly, dev and any unknown channel are Prerelease, whatever the list says.
//   list         a list must be held (NoList) and trusted (Stale).
//   protocol     ownProtocol must equal the list's protocol (ProtocolMismatch).
//   yanked       ownTag must not be on yanked[] (Yanked; RejectCopy shows the reason).
//   current      ownTag equal to current is Allowed.
//   older stable ownTag on allowed[] is Allowed while grace_until is 0 or nowUnixSeconds is before it.
//   otherwise    NotListed.
// nowUnixSeconds is read for grace_until alone: it is the copy's wall clock, the only clock read here. A skewed
// clock moves the grace end by the skew. It cannot admit a yanked build, or a list outside its trust window.
// Not settled by ODD 9.1: whether an older tag ends when the owner drops it from allowed[] before grace_until. This
// gate ends it at whichever comes first, because the spec describes both.
Refusal Admit(const State& state, const std::string& ownTag, const std::string& ownChannel, uint32_t ownProtocol,
              uint64_t nowMonoMs, uint64_t nowUnixSeconds);

// The reject text of ODD 9.1, in English: it names the build and what public rooms require, and says that offline
// and LAN still work. Empty for Allowed. A native screen may translate by Refusal, but the wording lives here.
std::string RejectCopy(Refusal refusal, const State& state, const std::string& ownTag, const std::string& ownChannel);

StateBlob EncodeState(const State& state);
// The blob is not signed. It guards against an old list being copied back by accident, not against someone who
// edits the file. A copy that cannot read its blob starts again from sequence 0.
State DecodeState(const StateBlob& blob);

}  // namespace gdx::net::allowlist
