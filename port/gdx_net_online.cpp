// port/gdx_net_online.cpp: see gdx_net_online.h.
// Copyright (c) Zorkats

#include "gdx_net_online.h"

#include "gdx_course_state.h"
#include "gdx_net_friend_store.h"

#include "gdx_profile_keys.h"
#include "gdx_profile_state.h"
#include "net/gdx_net_channel.h"
#include "net/gdx_net_codes.h"
#include "net/gdx_net_friends.h"
#include "net/gdx_net_host_info.h"
#include "net/gdx_net_https.h"
#include "net/gdx_net_listing.h"
#include "net/gdx_net_open_rooms.h"
#include "net/gdx_net_turn.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"

#include "port_log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#ifndef GDX_VERSION_STRING
#define GDX_VERSION_STRING "dev"
#endif

namespace gdx::online {

namespace {

namespace directory = net::directory;
namespace lobby = net::lobby;

// The developer seam's stand-in base. .invalid never resolves (RFC 6761), so a build that somehow sent it out
// would reach nobody.
constexpr const char* kDevBase = "https://directory.dev.invalid";
constexpr uint16_t kStunPort = 3478;
constexpr int kDirectoryTimeoutMs = 10000;
constexpr const char* kDefaultStun = "stun.cloudflare.com:3478";
// The directory this build uses when the player has set none (decision 49): GDX_NET_DIRECTORY_URLS when the build
// defines it, else the project's own deployment (the gnet-directory Worker).
#ifdef GDX_NET_DIRECTORY_URLS
constexpr const char* kDefaultDirectory = GDX_NET_DIRECTORY_URLS;
#else
constexpr const char* kDefaultDirectory = "https://gnet-directory.zorkats.workers.dev";
#endif

std::string Trim(const std::string& text) {
    size_t a = 0;
    size_t b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) {
        ++a;
    }
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) {
        --b;
    }
    return text.substr(a, b - a);
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            comma = text.size();
        }
        std::string item = Trim(text.substr(start, comma - start));
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        start = comma + 1;
    }
    return out;
}

// "127.0.0.1:port" or "localhost:port", port 1 to 65535. The only form the seam takes: the HTTPS helper allows
// plain http to these two hosts and no other.
bool DevDirectory(std::string& origin) {
    const char* env = std::getenv("GDX_NET_DEV_DIRECTORY");
    if (env == nullptr) {
        return false;
    }
    const std::string text = Trim(env);
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if ((host != "127.0.0.1" && host != "localhost") || port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const unsigned long value = std::strtoul(port.c_str(), nullptr, 10);
    if (value == 0 || value > 65535) {
        return false;
    }
    origin = "http://" + host + ":" + port;
    return true;
}

} // namespace

bool LocalTestDirectory() {
    std::string origin;
    return DevDirectory(origin);
}

bool DirectLinks() {
    return CVarGetInteger("gEnhancements.Online.DirectLinks", 1) != 0;
}

namespace {

std::string UserAgent() {
    char text[96];
    std::snprintf(text, sizeof(text), "G-Diffuser/%s", GDX_VERSION_STRING);
    return text;
}

directory::Response Exchange(const directory::Request& request, const std::string& devOrigin) {
    net::https::Request out;
    out.url = request.url;
    if (!devOrigin.empty() && out.url.compare(0, std::strlen(kDevBase), kDevBase) == 0) {
        out.url = devOrigin + out.url.substr(std::strlen(kDevBase));
    }
    out.headers.push_back("Accept: application/json");
    if (!request.contentType.empty()) {
        out.headers.push_back("Content-Type: " + request.contentType);
    }
    out.userAgent = UserAgent();
    out.body = request.body;
    out.timeoutMs = kDirectoryTimeoutMs;
    out.maxBodyBytes = directory::kBodyMax;
    out.keepErrorBody = true;
    const net::https::Response got =
        request.method == "POST" ? net::https::Post(out) : net::https::Get(out);

    directory::Response response;
    // An HTTP answer arrived: a 2xx, or another status with its body kept. TooLarge with a status is an answer
    // whose body broke the cap; the client reads the empty body as malformed, which is what it is.
    const bool answered = got.status != 0 && (got.error == net::https::Error::None ||
                                              got.error == net::https::Error::Status ||
                                              got.error == net::https::Error::TooLarge);
    response.ok = answered;
    if (answered) {
        response.status = got.status;
        response.body = got.body;
        response.retryAfter = got.retryAfter;
    }
    return response;
}

directory::RoomType DirectoryType(lobby::RoomType type) {
    switch (type) {
        case lobby::RoomType::Open:
            return directory::RoomType::Open;
        case lobby::RoomType::Friends:
            return directory::RoomType::Friends;
        default:
            return directory::RoomType::Code;
    }
}

std::string HexOf(const uint8_t* bytes, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 15];
    }
    return out;
}

std::string FailureText(directory::Error error) {
    switch (error) {
        case directory::Error::BadConfig:
            return "No room directory is set";
        case directory::Error::Unreachable:
            return "The room directory did not answer";
        case directory::Error::CodeTaken:
            return "No free room code was found";
        case directory::Error::BadSecret:
            return "Another host took over the room's code";
        case directory::Error::NotFound:
            return "The room directory lost the room";
        case directory::Error::RateLimited:
            return "The room directory is busy";
        case directory::Error::Invalid:
            return "The room cannot be listed as it is";
        default:
            return std::string("The room directory refused: ") + directory::ErrorName(error);
    }
}

} // namespace

// ---- Settings ----

std::vector<std::string> DirectoryBases() {
    std::string origin;
    if (DevDirectory(origin)) {
        return { kDevBase };
    }
    std::vector<std::string> bases = SplitList(CVarGetString("gEnhancements.Online.Directory", ""));
    if (bases.empty()) {
        bases = SplitList(kDefaultDirectory);
    }
    // A trailing slash would make every path "//rooms"; the client refuses it, so take it off here.
    for (std::string& base : bases) {
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
    }
    bases.erase(std::remove(bases.begin(), bases.end(), std::string()), bases.end());
    return bases;
}

bool DirectoryConfigured() {
    return !DirectoryBases().empty();
}

std::vector<std::string> StunServers() {
    std::vector<std::string> servers = SplitList(CVarGetString("gEnhancements.Online.StunServers", kDefaultStun));
    if (servers.empty()) {
        servers.push_back(kDefaultStun);
    }
    return servers;
}

std::string AdvertiseText() {
    // Developer seam: copies on one computer list 127.0.0.1:<port>, so a scripted run does not depend on the
    // router sending a public address back inside.
    if (const char* dev = std::getenv("GDX_NET_DEV_ADVERTISE"); dev != nullptr && dev[0] != 0) {
        return Trim(dev);
    }
    return Trim(CVarGetString("gEnhancements.Online.Advertise", ""));
}

std::optional<net::NetAddress> AdvertiseAddress() {
    const std::string text = AdvertiseText();
    net::NetAddress address;
    if (text.empty() || !net::ParseAddress(text.c_str(), address)) {
        return std::nullopt;
    }
    return address;
}

// A seed made here and not yet in profile-keys.gpk: the flush that writes it was refused while a recovery or
// snapshot write ran, so the next call tries again.
static std::atomic<bool> gSeedUnsaved{ false };

int ActiveSeed(unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES]) {
    gdx_course_boot(); // the seeds are read from profile-keys.gpk first, so an existing one is never replaced
    const unsigned long before = gdx_profile_state_keys_revision();
    const int rc = gdx_profile_state_active_seed(seed);
    if (rc != GDX_PROFILE_OK) {
        return rc;
    }
    if (gdx_profile_state_keys_revision() != before || gSeedUnsaved.load()) {
        const bool saved = gdx_course_state_flush() >= 0;
        gSeedUnsaved.store(!saved);
        if (!saved) {
            gdx_port_logf("[lan] the profile key is not saved yet; it is tried again on the next use\n");
        }
    }
    return rc;
}

bool HostKeyBytes(lobby::Bytes32& key) {
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    if (ActiveSeed(seed) != GDX_PROFILE_OK) {
        std::memset(seed, 0, sizeof(seed));
        return false;
    }
    net::friends::Key friendKey{};
    net::friends::DeriveFriendKey(seed, friendKey);
    std::memset(seed, 0, sizeof(seed));
    std::memcpy(key.data(), friendKey.data(), key.size());
    return true;
}

bool HostKey(std::string& hex) {
    lobby::Bytes32 key{};
    if (!HostKeyBytes(key)) {
        return false;
    }
    hex = HexOf(key.data(), key.size());
    return true;
}

// ---- Shared pieces ----

directory::Client MakeClient(const std::vector<std::string>& bases) {
    std::string origin;
    if (!DevDirectory(origin)) {
        origin.clear();
    }
    return directory::Client(bases, [origin](const directory::Request& request) { return Exchange(request, origin); });
}

void Spawn(std::function<void()> job) {
    std::thread(std::move(job)).detach();
}

void RandomBytes(uint8_t* out, size_t n) {
    static std::mutex lock;
    static std::random_device device;
    std::lock_guard<std::mutex> guard(lock);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(device() & 0xFF);
    }
}

std::string CodeText(uint32_t code) {
    if (code == lobby::kNoRoomCode || code >= lobby::kRoomCodeValues) {
        return std::string();
    }
    return net::codes::FormatRoomCode(code);
}

Seats SeatsOf(const lobby::RoomState& room, const std::vector<lobby::PlayerInfo>& roster) {
    uint32_t humans = 0;
    for (const lobby::PlayerInfo& player : roster) {
        if (!player.watcher) {
            ++humans;
        }
    }
    humans = (std::min)(humans, directory::kHumansMax);
    const uint32_t cap = (std::min)(static_cast<uint32_t>(room.racerCap), directory::kHumansMax);
    Seats seats;
    seats.humans = humans;
    seats.free = cap > humans ? cap - humans : 0;
    return seats;
}

// ---- RoomListing ----

namespace {

// The host_info answers, gathered on a helper thread.
struct InfoSlot {
    std::mutex lock;
    bool done = false;
    bool abandoned = false; // released before the gathering started
    net::gather::Inputs inputs;
};

// Every UPnP call runs on one helper thread, in the order asked. A member that becomes the host deletes its mapping
// and maps the port again for the listing; the deletion must not reach the router after the new mapping. The queue
// is never freed: a detached helper may still run it while the process exits.
struct UpnpQueue {
    std::mutex lock;
    std::deque<std::function<void()>> jobs;
    bool running = false;
};

UpnpQueue& Upnp() {
    static UpnpQueue* queue = new UpnpQueue();
    return *queue;
}

void PostUpnp(std::function<void()> job) {
    UpnpQueue& q = Upnp();
    std::lock_guard<std::mutex> guard(q.lock);
    q.jobs.push_back(std::move(job));
    if (q.running) {
        return;
    }
    q.running = true;
    Spawn([&q]() {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard<std::mutex> guard(q.lock);
                if (q.jobs.empty()) {
                    q.running = false;
                    return;
                }
                next = std::move(q.jobs.front());
                q.jobs.pop_front();
            }
            next();
        }
    });
}

// Decision 58: maps the room port through UPnP before the gathering starts, and gives the gatherer the hooks that
// renew and delete the mapping through the queue. Blocks the calling helper thread until the router answers.
void MapUpnp(net::gather::Inputs& inputs, uint16_t port) {
    auto promise = std::make_shared<std::promise<std::optional<net::host_info::UpnpMapping>>>();
    std::future<std::optional<net::host_info::UpnpMapping>> answer = promise->get_future();
    PostUpnp([promise, port]() { promise->set_value(net::host_info::UpnpMap(port, net::gather::kMappingSeconds)); });
    std::optional<net::host_info::UpnpMapping> mapping = answer.get();
    if (!mapping) {
        return;
    }
    gdx_port_logf("[online] UPnP: the router maps the room port (lease %u s)\n",
                  static_cast<unsigned>(mapping->leaseSeconds));
    auto held = std::make_shared<const net::host_info::UpnpMapping>(std::move(*mapping));
    inputs.upnpExternal = held->external;
    inputs.upnpLeaseSeconds = held->leaseSeconds;
    inputs.upnpRenew = [held]() { PostUpnp([held]() { net::host_info::UpnpRenew(*held); }); };
    inputs.upnpRelease = [held]() { PostUpnp([held]() { net::host_info::UpnpUnmap(*held); }); };
}

// The listing or the member went away before its gathering started. A UPnP mapping already made is deleted now; one
// still being made is deleted when it lands.
void AbandonInfo(InfoSlot& slot) {
    std::function<void()> release;
    {
        std::lock_guard<std::mutex> guard(slot.lock);
        slot.abandoned = true;
        if (slot.done) {
            release = std::move(slot.inputs.upnpRelease);
            slot.inputs.upnpRelease = nullptr;
        }
    }
    if (release) {
        release();
    }
}

// Resolves what a gathering needs off the worker's tick: the STUN servers, the gateway and the address toward it,
// the local addresses, and a UPnP mapping. The lookups block, so they run on their own thread and fill the slot when
// done. A player who advertises their own forwarded address asks the router for nothing.
void StartHostInfo(const std::shared_ptr<InfoSlot>& slot, std::vector<std::string> servers, uint16_t port,
                   bool dualStack, std::optional<net::NetAddress> advertise) {
    if (advertise && advertise->port == 0) {
        advertise->port = port; // port 0: the room socket's own
    }
    Spawn([slot, servers = std::move(servers), port, dualStack, advertise]() {
        net::gather::Inputs inputs;
        for (const std::string& server : servers) {
            for (const net::NetAddress& a : net::host_info::Resolve(server, kStunPort)) {
                inputs.stunServers.push_back(a);
            }
        }
        net::NetAddress gateway;
        if (net::host_info::DefaultGateway(gateway)) {
            inputs.gateway = gateway;
            net::NetAddress local;
            if (net::host_info::LocalAddressToward(gateway, local)) {
                inputs.localToGateway = local;
            }
        }
        inputs.localAddresses = net::host_info::LocalAddresses();
        inputs.localPort = port;
        inputs.dualStack = dualStack;
        inputs.advertise = advertise;
        if (!advertise && port != 0) {
            MapUpnp(inputs, port);
        }
        std::function<void()> release;
        {
            std::lock_guard<std::mutex> guard(slot->lock);
            if (slot->abandoned) {
                release = std::move(inputs.upnpRelease);
            } else {
                slot->inputs = std::move(inputs);
                slot->done = true;
            }
        }
        if (release) {
            release();
        }
    });
}

} // namespace

struct RoomListing::Impl {
    enum class Stage : uint8_t { Info, Gather, List, Failed };

    ListOptions options;
    net::gather::Send send;
    Stage stage = Stage::Info;
    std::shared_ptr<InfoSlot> info = std::make_shared<InfoSlot>();
    std::optional<net::gather::Gatherer> gatherer;
    std::optional<net::listing::Listing> listing;
    directory::Draft draft;
    std::string failure;
    uint32_t seenSerial = 0;
    bool released = false;

    void Fail(std::string text) {
        stage = Stage::Failed;
        failure = std::move(text);
    }
};

RoomListing::RoomListing(ListOptions options, net::gather::Send send) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->send = std::move(send);
    if (impl_->options.bases.empty()) {
        impl_->Fail(FailureText(directory::Error::BadConfig));
        return;
    }
    if (!directory::IsHostKey(impl_->options.hostKey) || !impl_->options.signer) {
        impl_->Fail("No player profile is active");
        return;
    }
    StartHostInfo(impl_->info, impl_->options.stunServers, impl_->options.localPort, impl_->options.dualStack,
                  impl_->options.advertise);
}

RoomListing::~RoomListing() = default;

void RoomListing::Update(uint32_t now, const Seats& seats) {
    Impl& m = *impl_;
    if (m.released) {
        return;
    }
    if (m.stage == Impl::Stage::Info) {
        std::lock_guard<std::mutex> guard(m.info->lock);
        if (!m.info->done) {
            return;
        }
        m.gatherer.emplace(std::move(m.info->inputs), m.send, RandomBytes);
        m.gatherer->Start(now);
        m.stage = Impl::Stage::Gather;
    }
    if (m.gatherer) {
        m.gatherer->Update(now); // the mapping's renewal runs for the room's life
    }
    m.draft.type = DirectoryType(m.options.type);
    m.draft.humans = seats.humans;
    m.draft.freeSeats = seats.free;
    m.draft.protocol = net::kProtocolVersion;
    m.draft.hostKey = m.options.hostKey;
    if (m.stage == Impl::Stage::Gather) {
        if (!m.gatherer->Done(now)) {
            return;
        }
        m.draft.endpoints = m.gatherer->Endpoints();
        m.draft.reachable = m.gatherer->Reachable();
        if (m.draft.endpoints.empty()) {
            m.Fail("No address to list the room under");
            return;
        }
        if (!directory::DraftValid(m.draft)) {
            m.Fail(FailureText(directory::Error::Invalid));
            return;
        }
        directory::Client client = MakeClient(m.options.bases);
        client.SetSigner(m.options.signer);
        m.listing.emplace(std::move(client), Spawn, RandomBytes);
        if (m.options.takeoverCode != lobby::kNoRoomCode) {
            directory::Secret secret{};
            std::memcpy(secret.data(), m.options.takeoverSecret.data(), secret.size());
            m.listing->Takeover(m.options.takeoverCode, secret, m.draft, now);
        } else {
            m.listing->Register(m.draft, now);
        }
        m.stage = Impl::Stage::List;
    }
    if (m.stage == Impl::Stage::List) {
        m.listing->SetDraft(m.draft);
        m.listing->Update(now);
        if (m.listing->state() == net::listing::State::Failed) {
            m.Fail(FailureText(m.listing->error()));
        }
    }
}

bool RoomListing::Receive(const net::NetAddress& from, const uint8_t* data, size_t len, uint32_t now) {
    return impl_->gatherer && impl_->gatherer->Receive(from, data, len, now);
}

void RoomListing::Delist() {
    if (impl_->listing && !impl_->released) {
        impl_->listing->Delist();
    }
}

void RoomListing::Release(uint32_t now) {
    if (impl_->released) {
        return;
    }
    if (impl_->gatherer) {
        impl_->gatherer->Release(now);
    } else {
        AbandonInfo(*impl_->info);
    }
    impl_->released = true;
}

void RoomListing::RotateSecret(uint32_t now) {
    if (impl_->listing && !impl_->released) {
        impl_->listing->RotateSecret(now);
    }
}

bool RoomListing::TakeChange(uint32_t& code, lobby::Bytes32& secret) {
    Impl& m = *impl_;
    if (!m.listing || m.listing->serial() == m.seenSerial) {
        return false;
    }
    m.seenSerial = m.listing->serial();
    code = m.listing->code();
    std::memcpy(secret.data(), m.listing->secret().data(), secret.size());
    return true;
}

ListView RoomListing::View() const {
    const Impl& m = *impl_;
    ListView view;
    switch (m.stage) {
        case Impl::Stage::Info:
        case Impl::Stage::Gather:
            view.state = ListState::Gathering;
            view.text = "Finding the address of this host";
            break;
        case Impl::Stage::List:
            if (m.listing->state() == net::listing::State::Listed) {
                view.state = ListState::Listed;
                view.text = "Listed";
            } else {
                view.state = ListState::Listing;
                view.text = "Listing the room";
            }
            break;
        case Impl::Stage::Failed:
            view.state = ListState::Failed;
            view.text = m.failure;
            break;
    }
    if (m.gatherer) {
        view.reachable = m.gatherer->Reachable();
        view.mapped = m.gatherer->Mapped();
        if (std::optional<net::NetAddress> a = m.gatherer->PublicAddress()) {
            view.publicAddress = net::FormatAddress(*a);
            view.publicAddressLog = net::host_info::LogText(*a);
        }
    }
    return view;
}

// ---- MemberReach (decision 51) ----

// The member side of the gathering RoomListing runs; the header says what it is for.

struct MemberReach::Impl {
    net::gather::Send send;
    std::shared_ptr<InfoSlot> info = std::make_shared<InfoSlot>();
    std::optional<net::gather::Gatherer> gatherer;
    bool done = false;
    bool reachable = false;
    bool released = false;
};

MemberReach::MemberReach(ReachOptions options, net::gather::Send send) : impl_(std::make_unique<Impl>()) {
    impl_->send = std::move(send);
    StartHostInfo(impl_->info, std::move(options.stunServers), options.localPort, options.dualStack,
                  options.advertise);
}

MemberReach::~MemberReach() = default;

void MemberReach::Update(uint32_t now) {
    Impl& m = *impl_;
    if (m.released) {
        return;
    }
    if (!m.gatherer) {
        std::lock_guard<std::mutex> guard(m.info->lock);
        if (!m.info->done) {
            return;
        }
        m.gatherer.emplace(std::move(m.info->inputs), m.send, RandomBytes);
        m.gatherer->Start(now);
    }
    m.gatherer->Update(now); // the mapping's renewal runs while the member stays
    if (!m.done && m.gatherer->Done(now)) {
        m.done = true;
        m.reachable = m.gatherer->Reachable();
    }
}

bool MemberReach::Receive(const net::NetAddress& from, const uint8_t* data, size_t len, uint32_t now) {
    return !impl_->released && impl_->gatherer && impl_->gatherer->Receive(from, data, len, now);
}

bool MemberReach::Done() const {
    return impl_->done;
}

bool MemberReach::Reachable() const {
    return impl_->reachable;
}

void MemberReach::Release(uint32_t now) {
    if (impl_->released) {
        return;
    }
    impl_->released = true;
    if (impl_->gatherer) {
        impl_->gatherer->Release(now);
    } else {
        AbandonInfo(*impl_->info);
    }
}

// ---- MemberRelay (decisions 69, 70, 72) ----

namespace {

// The relay credentials and the server they name, fetched on a helper thread.
struct GrantSlot {
    std::mutex lock;
    bool done = false;
    directory::Error error = directory::Error::None;
    uint32_t retryAfterSeconds = 0;
    net::turn::Credentials credentials;
    std::optional<net::NetAddress> server; // the first IPv4 address of the grant's servers, in its order
};

void FetchGrant(const std::shared_ptr<GrantSlot>& slot, std::vector<std::string> bases) {
    Spawn([slot, bases = std::move(bases)]() {
        const directory::Answer<directory::TurnGrant> answer = MakeClient(bases).TurnCredentials();
        std::optional<net::NetAddress> server;
        for (const std::string& text : answer.value.servers) {
            for (const net::NetAddress& a : net::host_info::Resolve(text, kStunPort)) {
                if (a.IsV4()) {
                    server = a;
                    break;
                }
            }
            if (server) {
                break;
            }
        }
        std::lock_guard<std::mutex> guard(slot->lock);
        slot->error = answer.error;
        slot->retryAfterSeconds = answer.retryAfterSeconds;
        slot->credentials.username = answer.value.username;
        slot->credentials.password = answer.value.password;
        slot->server = server;
        slot->done = true;
    });
}

} // namespace

struct MemberRelay::Impl {
    enum class Stage : uint8_t { Fetch, Allocate, Wait, Off };

    RelayOptions options;
    net::gather::Send send;
    Deliver deliver;
    Stage stage = Stage::Fetch;
    std::shared_ptr<GrantSlot> grant;
    std::optional<net::turn::Client> turn;
    net::NetAddress server;
    // The worker's clock is 32 bits of milliseconds; the TURN client's is 64.
    uint64_t clock = 0;
    uint32_t lastNow = 0;
    bool clockStarted = false;
    uint32_t deliverNow = 0; // the time of the datagram being received, for Deliver
    uint64_t retryAt = 0;
    uint32_t retryMs = kRelayRetryFirstMs;
    bool ready = false;
    bool released = false;

    uint64_t Tick(uint32_t now) {
        if (!clockStarted) {
            clockStarted = true;
            clock = now;
        } else {
            clock += static_cast<uint32_t>(now - lastNow);
        }
        lastNow = now;
        return clock;
    }

    void Fetch() {
        grant = std::make_shared<GrantSlot>();
        FetchGrant(grant, options.bases);
        stage = Stage::Fetch;
    }

    void Retry(uint64_t waitMs, const char* why) {
        gdx_port_logf("[online] relay: %s; trying again in %u s\n", why, static_cast<unsigned>(waitMs / 1000));
        stage = Stage::Wait;
        retryAt = clock + waitMs;
        retryMs = (std::min)(retryMs * 2, kRelayRetryMaxMs);
    }

    // The fetch finished: start the allocation, or wait, or give up for the session.
    void TakeGrant() {
        directory::Error error;
        uint32_t retryAfter;
        net::turn::Credentials credentials;
        std::optional<net::NetAddress> at;
        {
            std::lock_guard<std::mutex> guard(grant->lock);
            if (!grant->done) {
                return;
            }
            error = grant->error;
            retryAfter = grant->retryAfterSeconds;
            credentials = std::move(grant->credentials);
            at = grant->server;
        }
        grant.reset();
        if (error == directory::Error::TurnUnavailable || error == directory::Error::BadConfig) {
            gdx_port_logf("[online] relay: the room directory has none (%s); playing on without one\n",
                          directory::ErrorName(error));
            stage = Stage::Off;
            return;
        }
        if (error == directory::Error::RateLimited) {
            Retry((std::max)(static_cast<uint64_t>(retryAfter) * 1000, static_cast<uint64_t>(retryMs)),
                  "the room directory is busy");
            return;
        }
        if (error != directory::Error::None) {
            Retry(retryMs, directory::ErrorName(error));
            return;
        }
        if (!at) {
            Retry(retryMs, "no relay server resolved to an IPv4 address");
            return;
        }
        server = *at;
        net::turn::TransactionId seed{};
        RandomBytes(seed.data(), seed.size());
        turn->Start(credentials, seed, clock);
        stage = Stage::Allocate;
        gdx_port_logf("[online] relay: allocating at %s\n", net::host_info::LogText(server).c_str());
    }

    void Allocate() {
        turn->Update(clock);
        if (turn->state() == net::turn::Client::State::Ready) {
            if (!ready) {
                ready = true;
                retryMs = kRelayRetryFirstMs;
                gdx_port_logf("[online] relay: ready at %s, lifetime %u s\n",
                              net::host_info::LogText(turn->relayed()).c_str(),
                              static_cast<unsigned>(turn->lifetimeS()));
            }
            return;
        }
        if (turn->state() == net::turn::Client::State::Failed) {
            const char* why = net::turn::FailureName(turn->failure());
            ready = false;
            turn->Close();
            Retry(retryMs, why);
        }
    }
};

MemberRelay::MemberRelay(RelayOptions options, net::gather::Send send, Deliver deliver)
    : impl_(std::make_unique<Impl>()) {
    Impl* m = impl_.get();
    m->options = std::move(options);
    m->send = std::move(send);
    m->deliver = std::move(deliver);
    m->turn.emplace([m](const uint8_t* data, size_t len) { m->send(m->server, data, len); },
                    [m](const net::NetAddress& peer, const uint8_t* data, size_t len) {
                        if (m->deliver) {
                            m->deliver(peer, data, len, m->deliverNow);
                        }
                    });
    m->Fetch();
}

MemberRelay::~MemberRelay() = default;

void MemberRelay::Update(uint32_t now) {
    Impl& m = *impl_;
    if (m.released) {
        return;
    }
    m.Tick(now);
    switch (m.stage) {
        case Impl::Stage::Fetch:
            m.TakeGrant();
            break;
        case Impl::Stage::Allocate:
            m.Allocate();
            break;
        case Impl::Stage::Wait:
            if (m.clock >= m.retryAt) {
                m.Fetch();
            }
            break;
        case Impl::Stage::Off:
            break;
    }
}

bool MemberRelay::Receive(const net::NetAddress& from, const uint8_t* data, size_t len, uint32_t now) {
    Impl& m = *impl_;
    if (m.released || m.stage != Impl::Stage::Allocate || from != m.server) {
        return false;
    }
    m.deliverNow = now;
    return m.turn->Receive(data, len, m.Tick(now));
}

void MemberRelay::SetPeers(const std::vector<net::NetAddress>& peers, uint32_t now) {
    if (!impl_->released) {
        impl_->turn->SetPeers(peers, impl_->Tick(now));
    }
}

bool MemberRelay::SendTo(const net::NetAddress& peer, const uint8_t* data, size_t len) {
    return !impl_->released && impl_->stage == Impl::Stage::Allocate && impl_->turn->SendTo(peer, data, len);
}

std::optional<net::NetAddress> MemberRelay::Relayed() const {
    const Impl& m = *impl_;
    if (m.released || m.stage != Impl::Stage::Allocate || m.turn->state() != net::turn::Client::State::Ready) {
        return std::nullopt;
    }
    return m.turn->relayed();
}

void MemberRelay::Release() {
    Impl& m = *impl_;
    if (m.released) {
        return;
    }
    m.released = true;
    if (m.stage == Impl::Stage::Allocate) {
        m.turn->Close(); // a Refresh of lifetime 0, so the server frees the address now
    }
    m.stage = Impl::Stage::Off;
}

// ---- CodeLookup ----

struct CodeLookup::Slot {
    std::atomic<bool> done{ false };
    directory::Answer<std::optional<directory::Record>> answer;
};

CodeLookup::CodeLookup(std::vector<std::string> bases, uint32_t code) : slot_(std::make_shared<Slot>()) {
    std::shared_ptr<Slot> slot = slot_;
    const std::string text = net::listing::DirectoryCode(code);
    if (text.empty()) {
        slot->answer.error = directory::Error::Invalid;
        slot->done = true;
        return;
    }
    directory::Client client = MakeClient(bases);
    Spawn([slot, client, text]() {
        slot->answer = client.Lookup(text);
        slot->done.store(true, std::memory_order_release);
    });
}

bool CodeLookup::Done() const {
    return slot_->done.load(std::memory_order_acquire);
}

directory::Answer<std::optional<directory::Record>> CodeLookup::Result() const {
    return slot_->answer;
}

std::vector<net::NetAddress> JoinAddresses(const directory::Record& record, bool dualStack) {
    std::vector<net::NetAddress> out;
    std::string origin;
    const bool localTest = DevDirectory(origin); // a local test directory lists its rooms on loopback
    for (const directory::Endpoint& endpoint : record.draft.endpoints) {
        net::NetAddress a;
        if (!net::listing::AddressOf(endpoint, a) || !net::listing::Dialable(a, localTest)) {
            continue;
        }
        if (!a.IsV4() && !dualStack) {
            continue;
        }
        if (std::find(out.begin(), out.end(), a) == out.end()) {
            out.push_back(a);
        }
    }
    return out;
}

// ---- Lists ----

namespace {

struct ListsState {
    std::mutex lock;
    Lists lists;
    net::open_rooms::Model cooldown;
};

ListsState& State() {
    static ListsState state;
    return state;
}

} // namespace

bool AskDirectory(Ask ask, const std::string& friendKeyHex, uint32_t now, uint32_t& waitMs) {
    waitMs = 0;
    const std::vector<std::string> bases = DirectoryBases();
    ListsState& state = State();
    std::lock_guard<std::mutex> guard(state.lock);
    if (state.lists.status == Lists::Status::Asking) {
        return false; // one request at a time
    }
    if (bases.empty()) {
        state.lists.status = Lists::Status::Failed;
        state.lists.ask = ask;
        state.lists.text = FailureText(directory::Error::BadConfig);
        ++state.lists.serial;
        return false;
    }
    if (ask == Ask::Friend && !directory::IsHostKey(friendKeyHex)) {
        return false;
    }
    // One cooldown for every read a press sends (decision 38): a friend's rooms count as a refresh.
    const net::open_rooms::Action action = state.cooldown.Press(
        ask == Ask::Quick ? net::open_rooms::Intent::QuickRace : net::open_rooms::Intent::Refresh, now);
    if (!action.send) {
        waitMs = action.waitMs;
        return false;
    }
    state.lists.status = Lists::Status::Asking;
    state.lists.ask = ask;
    state.lists.text.clear();
    const directory::Client client = MakeClient(bases);
    const uint16_t protocol = net::kProtocolVersion;
    Spawn([client, ask, friendKeyHex, protocol]() {
        Lists result;
        result.ask = ask;
        directory::Error error = directory::Error::None;
        if (ask == Ask::Open) {
            directory::Answer<std::vector<directory::Record>> got = client.OpenRooms(protocol);
            error = got.error;
            result.rooms = std::move(got.value);
        } else if (ask == Ask::Quick) {
            directory::Answer<std::optional<directory::Record>> got = client.QuickRace(protocol);
            error = got.error;
            if (got.value) {
                result.quickFound = true;
                result.quick = std::move(*got.value);
            }
        } else {
            directory::Answer<std::vector<directory::Record>> got = client.HostRooms(friendKeyHex, protocol);
            error = got.error;
            result.rooms = std::move(got.value);
        }
        ListsState& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        if (error == directory::Error::None) {
            result.status = Lists::Status::Ready;
        } else {
            result.status = Lists::Status::Failed;
            result.text = FailureText(error);
        }
        result.serial = s.lists.serial + 1;
        s.lists = std::move(result);
    });
    return true;
}

Lists ListsSnapshot() {
    ListsState& state = State();
    std::lock_guard<std::mutex> guard(state.lock);
    return state.lists;
}

// ---- The friend list (O7.4) ----

namespace {

struct FriendBook {
    std::mutex lock;
    bool loaded = false;
    bool unreadable = false; // the file is there but could not be read: the list shown is empty, not the file's
    net::friends::FriendList list;
};

FriendBook& Book() {
    static FriendBook* book = new FriendBook(); // never destroyed: a page may call in during exit
    return *book;
}

std::filesystem::path FriendFile() {
    return net::friendstore::PathUnder(std::filesystem::path(Ship::Context::GetAppDirectoryPath()));
}

// Under the book's lock.
void LoadBook(FriendBook& book) {
    if (book.loaded) {
        return;
    }
    book.loaded = true;
    const std::filesystem::path file = FriendFile();
    net::friends::DecodeResult why = net::friends::DecodeResult::Ok;
    switch (net::friendstore::Read(file, book.list, why)) {
        case net::friendstore::ReadResult::Ok:
        case net::friendstore::ReadResult::Missing:
            break;
        case net::friendstore::ReadResult::Bad:
            gdx_port_logf("[online] the friend list failed its check (%s); kept as friends.gnfl.bad%s\n",
                          net::friends::DecodeResultName(why),
                          net::friendstore::SetAside(file) ? "" : ", which could not be moved");
            break;
        default:
            gdx_port_logf("[online] the friend list could not be read; showing none, and saving nothing over it\n");
            book.unreadable = true;
            break;
    }
}

// Under the book's lock. A write replaces the whole file, so it waits for a file that was read: an unreadable one
// (held by a backup or sync tool, say) is read again here, and while it still fails nothing is written over it.
bool BookWritable(FriendBook& book) {
    LoadBook(book);
    if (book.unreadable) {
        book.loaded = false;
        book.unreadable = false;
        book.list = net::friends::FriendList();
        LoadBook(book);
    }
    return !book.unreadable;
}

net::friends::Key FriendsKey(const lobby::Bytes32& key) {
    net::friends::Key out{};
    std::copy(key.begin(), key.end(), out.begin());
    return out;
}

lobby::Bytes32 LobbyKey(const net::friends::Key& key) {
    lobby::Bytes32 out{};
    std::copy(key.begin(), key.end(), out.begin());
    return out;
}

} // namespace

std::vector<FriendEntry> Friends() {
    FriendBook& book = Book();
    std::lock_guard<std::mutex> guard(book.lock);
    LoadBook(book);
    std::vector<FriendEntry> out;
    out.reserve(book.list.entries.size());
    for (const net::friends::Friend& entry : book.list.entries) {
        out.push_back(FriendEntry{ entry.name, LobbyKey(entry.key) });
    }
    return out;
}

std::vector<lobby::Bytes32> FriendKeys() {
    std::vector<lobby::Bytes32> keys;
    for (const FriendEntry& entry : Friends()) {
        keys.push_back(entry.key);
    }
    return keys;
}

bool IsFriend(const lobby::Bytes32& key) {
    FriendBook& book = Book();
    std::lock_guard<std::mutex> guard(book.lock);
    LoadBook(book);
    return net::friends::Find(book.list, FriendsKey(key)) >= 0;
}

FriendAdd AddFriend(const lobby::Bytes32& key, const std::string& name) {
    lobby::Bytes32 own{};
    if (HostKeyBytes(own) && own == key) {
        return FriendAdd::Self;
    }
    FriendBook& book = Book();
    std::lock_guard<std::mutex> guard(book.lock);
    if (!BookWritable(book)) {
        return FriendAdd::NotSaved;
    }
    const int at = net::friends::Find(book.list, FriendsKey(key));
    if (at >= 0 && book.list.entries[static_cast<size_t>(at)].name == name) {
        return FriendAdd::Same;
    }
    net::friends::FriendList next = book.list;
    switch (net::friends::Add(next, FriendsKey(key), name)) {
        case net::friends::AddResult::Full:
            return FriendAdd::Full;
        case net::friends::AddResult::BadName:
            return FriendAdd::BadName;
        default:
            break;
    }
    if (!net::friendstore::Write(FriendFile(), next)) {
        return FriendAdd::NotSaved;
    }
    book.list = std::move(next);
    return at >= 0 ? FriendAdd::Updated : FriendAdd::Added;
}

bool RemoveFriend(const lobby::Bytes32& key) {
    FriendBook& book = Book();
    std::lock_guard<std::mutex> guard(book.lock);
    if (!BookWritable(book)) {
        return false;
    }
    net::friends::FriendList next = book.list;
    if (!net::friends::Remove(next, FriendsKey(key)) || !net::friendstore::Write(FriendFile(), next)) {
        return false;
    }
    book.list = std::move(next);
    return true;
}

std::string NewFriendName() {
    const std::vector<FriendEntry> friends = Friends();
    for (size_t n = 1;; n++) {
        const std::string name = "Friend " + std::to_string(n);
        if (std::none_of(friends.begin(), friends.end(), [&](const FriendEntry& e) { return e.name == name; })) {
            return name;
        }
    }
}

std::string FriendCodeOf(const lobby::Bytes32& key) {
    return net::codes::FormatFriendCode(key.data());
}

} // namespace gdx::online
