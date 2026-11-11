// O2 connection channel (port/net/gdx_net_channel.h): handshake, reliable and unreliable messages,
// keepalive and timeout, over a datagram link that the caller supplies.
//
// The channel never opens a socket and never reads a clock. The owner passes a send callback, feeds
// received datagrams to Receive(), and calls Update() with the current time in milliseconds. That is
// what lets the tests run a simulated lossy link on a fake clock, and it keeps OS code in
// gdx_net_socket.*. The send callback must not call back into the channel. The channel is not
// thread-safe: the thread that owns it must also make every call.

#ifndef GDX_NET_CHANNEL_H
#define GDX_NET_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace gdx::net {

// Largest datagram sent or accepted, in both directions. 1200 stays under the 1280-byte IPv6 minimum
// MTU after the IPv6 and UDP headers, and leaves room for tunnels such as VPNs.
constexpr size_t kMaxDatagramBytes = 1200;

// Messages are never fragmented, so one message must fit in one datagram beside the 15-byte packet
// header and the 6-byte reliable message header. O2 traffic is small (inputs, votes, lobby state).
// Large content (tracks up to 1 MB, icons up to 16 KB) is chunked by the session layer, which also
// owns its hash check. Fragmenting here would need a reassembly buffer to bound and fuzz, for no
// gain in O2.
constexpr size_t kMaxMessagePayload = 1179;

// Caps. Each one bounds memory and every loop over it. A send that hits a cap returns QueueFull.
// A receive that hits the inbox cap drops an unreliable message (counted), or closes the link with
// Overflow when the message was reliable, because a reliable message cannot be dropped silently.
constexpr size_t kReliableWindow = 64;   // ids a sender may have unacked, and the reorder buffer size
constexpr size_t kInboxCap = 128;        // delivered messages waiting for PopMessage
constexpr size_t kUnreliableOutCap = 64; // unreliable messages waiting for the next Update

// 2 since the room protocol of milestone O3 (gdx_net_lobby.h). A peer on version 1 is refused with
// VersionMismatch.
constexpr uint16_t kProtocolVersion = 2;

enum class Role : uint8_t {
    Client,
    Server
};

enum class LinkState : uint8_t {
    Disconnected,
    Connecting,
    Connected
};

// Why the link is down. These numbers are wire values in refuse and disconnect packets. Never
// renumber them, because an older peer would then misread them.
enum class CloseReason : uint8_t {
    None = 0,
    Closed = 1,          // Disconnect() on either side
    Timeout = 2,         // no valid packet for timeoutMs, or the handshake was never answered
    VersionMismatch = 3, // the server refused a hello with a different protocol version
    Busy = 4,            // the server already has a connected peer
    Overflow = 5         // a cap was hit that the peer cannot recover from
};

enum class QueueResult : uint8_t {
    Ok,
    NotConnected,
    TooLarge,  // payload above kMaxMessagePayload
    QueueFull, // a cap is full; retry after Update() has run and acks have arrived
    Invalid    // null data with a nonzero length
};

const char* CloseReasonName(CloseReason reason);
const char* QueueResultName(QueueResult result);

struct Message {
    uint8_t type = 0; // application-defined; the channel never interprets it
    bool reliable = false;
    std::vector<uint8_t> payload;
};

struct ChannelConfig {
    Role role = Role::Client;
    // Client only. The caller supplies a fresh random value, so tests stay deterministic. Connect()
    // always uses this value, so build a new Channel for each connection attempt: reusing one would
    // let stale packets from the earlier attempt pass the nonce check. The nonce rejects stray and
    // stale packets. It is not authentication: identity lives in the profile key (design doc section 10).
    uint32_t connectionNonce = 0;
    // Advertised in hello and required of the peer. Tests change it to force a refusal.
    uint16_t version = kProtocolVersion;
    // The first packet sequence and the first message id. Both peers must use the same value. Tests
    // start near 65535 to exercise wraparound. Production can keep the default.
    uint16_t initialSequence = 0;
    uint32_t timeoutMs = 10000;  // silence before the link is dropped
    uint32_t keepaliveMs = 1000; // idle time before an empty packet keeps the peer's timer alive
};

struct ChannelStats {
    uint32_t packetsIn = 0;
    uint32_t packetsOut = 0;
    uint32_t malformed = 0;     // bad length, unknown type, bad channel byte, oversize field
    uint32_t badMagic = 0;      // first two bytes are not the protocol magic
    uint32_t wrongNonce = 0;    // right magic, but the connection nonce does not match
    uint32_t ignored = 0;       // valid packet that means nothing in the current state
    uint32_t duplicates = 0;    // repeated packets, and reliable messages already delivered
    uint32_t outOfWindow = 0;   // reliable ids too far ahead for the reorder buffer
    uint32_t resends = 0;       // reliable messages sent again after their RTO
    uint32_t overflowDrops = 0; // unreliable messages dropped because the inbox was full
    uint32_t rttMs = 0;         // smoothed round trip in milliseconds; 0 until the first sample
};

class Channel {
public:
    using SendFn = std::function<void(const uint8_t* data, size_t len)>;

    Channel(const ChannelConfig& config, SendFn send);

    // Client only. Starts the handshake with config.connectionNonce. Returns false when the role or
    // the current state does not allow a new connection.
    bool Connect(uint32_t nowMs);

    // Sends a disconnect packet carrying reason Closed, then drops the link.
    void Disconnect(uint32_t nowMs);

    // Consumes one received datagram. Every length is checked before it is read. A packet that fails
    // any length or field check, or carries a reliable id beyond the window, is dropped whole with no
    // ack and no state change, so the sender keeps its reliable messages and resends.
    void Receive(const uint8_t* data, size_t len, uint32_t nowMs);

    // Runs the timers: handshake retries, timeout, resends, acks and keepalive. Call it once per frame.
    void Update(uint32_t nowMs);

    QueueResult SendReliable(uint8_t type, const uint8_t* data, size_t len);
    QueueResult SendUnreliable(uint8_t type, const uint8_t* data, size_t len);

    // Pops the oldest delivered message. Reliable messages arrive exactly once, in send order.
    // Unreliable messages arrive in the order they land, at most once each.
    bool PopMessage(Message& out);

    LinkState State() const {
        return state_;
    }

    CloseReason Reason() const {
        return reason_;
    }

    const ChannelStats& Stats() const {
        return stats_;
    }

private:
    // A reliable message waiting for an ack. lastSeq is the packet it most recently rode in. An ack for
    // that packet clears it. An ack for an older packet does not, which costs at most one extra resend.
    struct Pending {
        uint16_t id = 0;
        Message msg;
        bool sent = false;
        uint32_t sentMs = 0;
        uint16_t lastSeq = 0;
    };

    // A packet that carried messages, kept for ack matching and RTT. Empty keepalives are not kept:
    // the peer acks them only on its next packet, so their round trip would be inflated.
    struct InFlight {
        uint16_t seq = 0;
        uint32_t sentMs = 0;
    };

    // One reorder-buffer entry. The slot index is id % kReliableWindow.
    struct Slot {
        bool used = false;
        uint16_t id = 0;
        Message msg;
    };

    // A packet being built.
    struct Outgoing {
        std::vector<uint8_t> bytes;
        uint16_t seq = 0;
        size_t msgs = 0;
    };

    // A parsed message that still points into the datagram being received.
    struct WireMessage {
        uint8_t type = 0;
        bool reliable = false;
        uint16_t id = 0;
        size_t length = 0;
        const uint8_t* payload = nullptr;
    };

    void OnHello(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs);
    void OnAccept(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs);
    void OnRefuse(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs);
    void OnDisconnect(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs);
    void OnData(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs);

    static bool ParseData(const uint8_t* body, size_t len, uint16_t& seq, uint16_t& ack, uint32_t& bits,
                          std::vector<WireMessage>& out);
    static Message MakeMessage(const WireMessage& m);
    static void AppendMessage(Outgoing& out, bool reliable, uint16_t id, const Message& msg);

    void ResetConnection();
    void Close(CloseReason reason, bool notifyPeer, uint32_t nowMs);
    void SendHello(uint32_t nowMs);
    void SendControl(uint8_t type, uint32_t nonce, const std::vector<uint8_t>& body, uint32_t nowMs);
    void Emit(const std::vector<uint8_t>& bytes, uint32_t nowMs);
    void BeginData(Outgoing& out);
    void Place(Outgoing& out, size_t need, uint32_t nowMs);
    void Ship(Outgoing& out, uint32_t nowMs);
    void FlushOutbound(uint32_t nowMs);

    bool RecordRemoteSeq(uint16_t seq);
    void ProcessAcks(uint16_t ack, uint32_t bits, uint32_t nowMs);
    void AckSeq(uint16_t seq);
    bool AcceptReliable(const WireMessage& m, uint32_t nowMs);
    bool DrainSlots(uint32_t nowMs);
    void AddRttSample(uint32_t sampleMs);
    uint32_t RtoMs() const;

    ChannelConfig config_;
    SendFn send_;
    LinkState state_ = LinkState::Disconnected;
    CloseReason reason_ = CloseReason::None;
    uint32_t nonce_ = 0;          // the connection nonce: ours (client) or the accepted hello's (server)
    uint32_t connectStartMs_ = 0; // client handshake started, for the handshake timeout
    uint32_t lastHelloMs_ = 0;    // last hello sent, for retries
    uint32_t lastRecvMs_ = 0;     // last valid packet from the peer, for the silence timeout
    uint32_t lastSendMs_ = 0;     // last datagram sent, for keepalive

    // Sender side.
    uint16_t nextSeq_ = 0;
    uint16_t nextMsgId_ = 0;
    std::deque<Pending> sendQueue_;
    std::deque<Message> unreliableOut_;
    std::deque<InFlight> inFlight_;
    bool rttValid_ = false;
    int32_t srttMs_ = 0;
    int32_t rttVarMs_ = 0;

    // Receiver side.
    uint16_t remoteSeq_ = 0;  // highest packet sequence seen from the peer
    uint32_t recvBits_ = 0;   // bit i set: packet remoteSeq_ - 1 - i was received
    bool ackPending_ = false; // a packet with messages arrived and has not been acked yet
    uint16_t nextRecvId_ = 0; // next reliable id to deliver
    std::vector<Slot> slots_;
    std::deque<Message> inbox_;

    ChannelStats stats_;
};

} // namespace gdx::net

#endif // GDX_NET_CHANNEL_H
