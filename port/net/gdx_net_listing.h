// port/net/gdx_net_listing.h: a hosted room's place in the room directory (G-NET O5.10 and O5.11).
// Copyright (c) Zorkats
//
// A host registers its room under a fresh code and keeps the record alive with a heartbeat. When the host is lost,
// its successor takes the same code over with the secret the old host handed it (plan decision 32), and sets a new
// secret for its own successor.
//
// Headless: no socket, no clock, no thread. Every directory call is a blocking HTTPS exchange, so the caller passes
// a Spawn that runs it elsewhere (the game: a helper thread; the tests: inline). The answer is picked up by the next
// Update on the caller's thread. Nothing else crosses threads.
//
// Ambiguous writes. A write that ends in Internal or Unreachable may have been applied (CONTRACT.md §6). A register
// retried after one can answer code_taken for our own record, and a takeover retried after one can answer
// bad_secret because the old secret already died. Both cases ask the directory with a heartbeat under the secret
// the write would have set before giving the code up.

#pragma once

#include "gdx_net_directory.h"
#include "gdx_net_socket.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gdx::net::listing {

constexpr uint32_t kHeartbeatMs = 60000; // three heartbeats in each 180 s record life (CONTRACT.md §5)
constexpr uint32_t kRetryMs = 15000;     // after a call that may work later: no answer, or a 5xx
constexpr int kCodeTries = 5;            // fresh codes tried on code_taken before the listing gives up

// Runs a job away from the caller's thread. The job owns everything it touches.
using Spawn = std::function<void(std::function<void()>)>;
// Fills out with n bytes from the caller's CSPRNG.
using Random = std::function<void(uint8_t* out, size_t n)>;

// The directory's six-character code for a room code value (codes::FormatRoomCode without the dash), or empty
// when the value is out of range.
std::string DirectoryCode(uint32_t value);
// The value of a directory code. false when text is not one.
bool CodeValue(const std::string& text, uint32_t& value);

// An endpoint for a socket address, in the directory's address form (CONTRACT.md §3).
directory::Endpoint EndpointOf(const NetAddress& address, directory::How how);
// The socket address of an endpoint. false when its address does not parse.
bool AddressOf(const directory::Endpoint& endpoint, NetAddress& out);
// Whether a joiner may send to a listed address (DialableIp, gdx_net_address.h). A record is anyone's to write,
// so an address no host can be reached at is refused rather than sent hellos.
bool Dialable(const NetAddress& address, bool allowLoopback);

enum class State : uint8_t {
    Idle,    // nothing asked yet
    Working, // a call is out, or one waits for its retry
    Listed,  // the record is live under code()
    Failed,  // the directory refused for good; error() says why. The room runs on, unlisted
};

class Listing {
public:
    Listing(directory::Client client, Spawn spawn, Random random);

    // A new room: draws a code and a secret, and registers them.
    void Register(const directory::Draft& draft, uint32_t now);
    // The room's next host, after the old one is lost: proves secret on code, and sets a new secret. code is the
    // room code value the room carries (lobby::RoomState::code).
    void Takeover(uint32_t code, const directory::Secret& secret, const directory::Draft& draft, uint32_t now);
    // A member who held the secret is no longer the successor: a takeover on this host's own record swaps the
    // secret, so the old holder cannot move the code or refresh it. The room stays Listed meanwhile; a register
    // or takeover still out finishes first.
    void RotateSecret(uint32_t now);
    // The room's state for the next heartbeat, which keeps its time: free seats change too often to send each one.
    void SetDraft(const directory::Draft& draft);
    // Collects a call's answer and sends the next call that is due.
    void Update(uint32_t now);
    // The room has ended with no successor: asks the directory to delete the record now (CONTRACT.md §5.8), rather
    // than leave a dead room listed for up to 180 s. Fire and forget: nothing reads the answer, and the listing is
    // Idle after it. A host that hands the room over never calls this. Sent under the secret held now, so it loses
    // to a takeover still out; the record then runs out as before.
    void Delist();

    State state() const { return state_; }
    directory::Error error() const { return error_; }
    // The code value while Working or Listed; 0xFFFFFFFF (lobby::kNoRoomCode) before the first draw.
    uint32_t code() const { return code_; }
    // The secret that takes this record over now. The room hands it to its successor.
    const directory::Secret& secret() const { return secret_; }
    // Grows whenever code() or secret() changes, so the caller knows to hand the new pair to the room.
    uint32_t serial() const { return serial_; }

private:
    enum class Call : uint8_t { None, Register, Heartbeat, Takeover };
    struct Slot;

    void Send(Call call, uint32_t now);
    void Answered(Call call, const directory::Answer<uint32_t>& answer, uint32_t now);
    void NewCode();
    void Fail(directory::Error error);
    void Later(uint32_t now, uint32_t ms, Call call);

    directory::Client client_;
    Spawn spawn_;
    Random random_;
    directory::Draft draft_;
    State state_ = State::Idle;
    directory::Error error_ = directory::Error::None;
    uint32_t code_ = 0xFFFFFFFFu;
    directory::Secret secret_{};
    directory::Secret next_{};   // Takeover: the secret the takeover sets
    bool ambiguous_ = false;     // the last write may have been applied without an answer
    bool probing_ = false;       // a heartbeat asks whether a code_taken record is our own
    bool rotate_ = false;        // RotateSecret waits for the call in flight
    int codeTries_ = 0;
    uint32_t serial_ = 0;
    Call due_ = Call::None;      // the call to send at dueAt_
    uint32_t dueAt_ = 0;
    Call out_ = Call::None;      // the call in flight
    std::shared_ptr<Slot> slot_; // its answer, filled by the job
};

} // namespace gdx::net::listing
