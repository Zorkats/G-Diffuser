// port/tests/gdx_net_https_tests.cpp: standalone checks for the shared HTTPS helper against a local plain-socket server (G-NET O5.4).
// Copyright (c) Zorkats
//
// Usage: gdx_net_https_tests
//
// The server listens on 127.0.0.1 on a port the OS picks and answers one request at a time on its own thread. Every
// answer closes its connection. The checks cover the URL rule, the status, the body and its cap, the headers, POST,
// timeouts, refused connections, and the redirect rules that plain http can reach.
//
// Not covered here: a redirect followed to a live https server, the method rules for 303, 301 and 302 on POST, and
// the five-hop limit. Each needs an https server, and this program only hosts plain http. The https hop is still
// exercised: the helper attempts it and the closed port refuses it, which shows the redirect was followed.
//
// On WSL only the libcurl transport is built. The WinHTTP transport runs the same checks under MSVC on Windows.

#include "gdx_net_https.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

namespace https = gdx::net::https;
using https::Error;
using https::Request;
using https::Response;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

#define CHECK(expr) Check((expr), #expr)

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;

void CloseSocket(Socket s) {
    closesocket(s);
}

long SocketSend(Socket s, const char* data, size_t len) {
    return static_cast<long>(send(s, data, static_cast<int>(len), 0));
}

long SocketRecv(Socket s, char* buf, size_t len) {
    return static_cast<long>(recv(s, buf, static_cast<int>(len), 0));
}

void SetReceiveTimeout(Socket s, int ms) {
    DWORD value = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;

void CloseSocket(Socket s) {
    close(s);
}

long SocketSend(Socket s, const char* data, size_t len) {
    return static_cast<long>(send(s, data, len, 0));
}

long SocketRecv(Socket s, char* buf, size_t len) {
    return static_cast<long>(recv(s, buf, len, 0));
}

void SetReceiveTimeout(Socket s, int ms) {
    timeval value{};
    value.tv_sec = ms / 1000;
    value.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
}
#endif

// Winsock must start before the first socket call. A write to a peer that already closed raises SIGPIPE on POSIX, so
// it is ignored: the server then sees a failed send and carries on with the next request.
class SocketRuntime {
public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    return Lower(haystack).find(Lower(needle)) != std::string::npos;
}

const char* ErrorName(Error error) {
    switch (error) {
        case Error::None:
            return "None";
        case Error::Unsupported:
            return "Unsupported";
        case Error::RejectedUrl:
            return "RejectedUrl";
        case Error::Redirect:
            return "Redirect";
        case Error::TooLarge:
            return "TooLarge";
        case Error::Io:
            return "Io";
        case Error::Network:
            return "Network";
        case Error::Status:
            return "Status";
    }
    return "unknown";
}

// One line per response, so a failed check shows what the helper actually returned.
std::string Describe(const Response& r) {
    return std::string("error=") + ErrorName(r.error) + " status=" + std::to_string(r.status) +
           " ok=" + (r.ok ? "true" : "false") + " bodyBytes=" + std::to_string(r.body.size()) + " detail=\"" +
           r.detail + "\"";
}

Request MakeRequest(const std::string& url, uint64_t maxBodyBytes) {
    Request request;
    request.url = url;
    request.maxBodyBytes = maxBodyBytes;
    request.timeoutMs = 5000;
    return request;
}

long long ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

// A plain HTTP/1.1 server for one test run. It serves on its own thread, one connection at a time, and closes every
// connection after its answer, so no client socket outlives its request.
class LocalServer {
public:
    LocalServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kNoSocket) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        socklen_t length = sizeof(addr);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener_, 8) != 0 ||
            getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
            CloseSocket(listener_);
            listener_ = kNoSocket;
            return;
        }
        port_ = ntohs(addr.sin_port);
        closedPort_ = ReserveClosedPort();
        thread_ = std::thread([this] { Run(); });
    }

    ~LocalServer() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ != kNoSocket) {
            CloseSocket(listener_);
        }
    }

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    bool Ready() const {
        return thread_.joinable();
    }

    std::string Url(const char* path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    // A port nothing listens on. Connecting to it fails at once, which is what the refused-connection checks need.
    std::string ClosedHttpUrl(const char* path) const {
        return "http://127.0.0.1:" + std::to_string(closedPort_) + path;
    }

    std::string ClosedHttpsUrl(const char* path) const {
        return "https://127.0.0.1:" + std::to_string(closedPort_) + path;
    }

    int Hits() const {
        return hits_.load();
    }

private:
    static constexpr size_t kMaxHeadBytes = 65536;
    static constexpr size_t kMaxBodyBytes = 1 << 20;

    static uint16_t ReserveClosedPort() {
        Socket probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe == kNoSocket) {
            return 0;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        uint16_t port = 0;
        if (bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            socklen_t length = sizeof(addr);
            if (getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
                port = ntohs(addr.sin_port);
            }
        }
        CloseSocket(probe);
        return port;
    }

    void Run() {
        while (!stop_.load()) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            timeval wait{};
            wait.tv_usec = 50000;
            int ready = select(static_cast<int>(listener_) + 1, &readable, nullptr, nullptr, &wait);
            if (ready <= 0) {
                continue;
            }
            Socket client = accept(listener_, nullptr, nullptr);
            if (client == kNoSocket) {
                continue;
            }
            Serve(client);
            CloseSocket(client);
        }
    }

    static bool ReceiveSome(Socket s, std::string& buf) {
        char chunk[4096];
        long n = SocketRecv(s, chunk, sizeof(chunk));
        if (n <= 0) {
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        return true;
    }

    static void SendAll(Socket s, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            long n = SocketSend(s, data.data() + sent, data.size() - sent);
            if (n <= 0) {
                return;
            }
            sent += static_cast<size_t>(n);
        }
    }

    // The value of a header, or empty when it is absent. The name is given in lowercase.
    static std::string HeaderValue(const std::string& head, const char* name) {
        std::string key = std::string("\r\n") + name + ":";
        size_t at = Lower(head).find(key);
        if (at == std::string::npos) {
            return {};
        }
        size_t start = at + key.size();
        size_t end = head.find("\r\n", start);
        std::string value = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t first = value.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : value.substr(first);
    }

    static size_t ContentLength(const std::string& head) {
        std::string value = HeaderValue(head, "content-length");
        size_t n = 0;
        for (char c : value) {
            if (c < '0' || c > '9') {
                break;
            }
            n = n * 10 + static_cast<size_t>(c - '0');
            if (n > kMaxBodyBytes) {
                return kMaxBodyBytes;
            }
        }
        return n;
    }

    static void Reply(Socket client, int code, const char* reason, const std::string& extra, const std::string& body,
                      bool withLength) {
        std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
        if (withLength) {
            out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        out += "Connection: close\r\n" + extra + "\r\n";
        out += body;
        SendAll(client, out);
    }

    void Pause(int ms) {
        for (int waited = 0; waited < ms && !stop_.load(); waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void Serve(Socket client) {
        SetReceiveTimeout(client, 5000);
        std::string raw;
        size_t headEnd = std::string::npos;
        while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos) {
            if (raw.size() > kMaxHeadBytes || !ReceiveSome(client, raw)) {
                return;
            }
        }
        std::string head = raw.substr(0, headEnd);
        std::string body = raw.substr(headEnd + 4);
        size_t wanted = ContentLength(head);
        while (body.size() < wanted) {
            if (!ReceiveSome(client, body)) {
                return;
            }
        }
        body.resize(wanted);
        hits_.fetch_add(1);
        Route(client, head, body);
    }

    void Route(Socket client, const std::string& head, const std::string& body) {
        size_t sp1 = head.find(' ');
        if (sp1 == std::string::npos) {
            Reply(client, 400, "Bad Request", "", "", true);
            return;
        }
        size_t sp2 = head.find(' ', sp1 + 1);
        std::string method = head.substr(0, sp1);
        std::string path = head.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
        if (path == "/ok") {
            Reply(client, 200, "OK", "", "hello", true);
        } else if (path == "/big") {
            Reply(client, 200, "OK", "", std::string(4096, 'x'), true);
        } else if (path == "/nolength") {
            Reply(client, 200, "OK", "", std::string(4096, 'x'), false);
        } else if (path == "/status404") {
            Reply(client, 404, "Not Found", "", "nope", true);
        } else if (path == "/fail500") {
            Reply(client, 500, "Internal Server Error", "", "boom", true);
        } else if (path == "/limited") {
            Reply(client, 429, "Too Many Requests", "Retry-After:  7 \r\n", "slow down", true);
        } else if (path == "/limitednolength") {
            Reply(client, 429, "Too Many Requests", "", "slow down", false);
        } else if (path == "/created") {
            Reply(client, 201, "Created", "", "made", true);
        } else if (path == "/echo") {
            Reply(client, 200, "OK", "", method + " " + path + "|" + body, true);
        } else if (path == "/headers") {
            Reply(client, 200, "OK", "", head, true);
        } else if (path == "/slow") {
            Pause(3000); // WinHTTP fires short timeouts late, so the reply comes well after any of them
            Reply(client, 200, "OK", "", "late", true);
        } else if (path == "/redirhttp") {
            Reply(client, 302, "Found", "Location: " + Url("/ok") + "\r\n", "", true);
        } else if (path == "/redirhttps") {
            Reply(client, 302, "Found", "Location: " + ClosedHttpsUrl("/x") + "\r\n", "", true);
        } else if (path == "/redirhttps307") {
            Reply(client, 307, "Temporary Redirect", "Location: " + ClosedHttpsUrl("/x") + "\r\n", "", true);
        } else if (path == "/redirnoloc") {
            Reply(client, 302, "Found", "", "moved", true);
        } else {
            Reply(client, 404, "Not Found", "", "unknown", true);
        }
    }

    std::atomic<bool> stop_{false};
    std::atomic<int> hits_{0};
    Socket listener_ = kNoSocket;
    uint16_t port_ = 0;
    uint16_t closedPort_ = 0;
    std::thread thread_;
};

void UrlPolicyChecks() {
    struct UrlCase {
        const char* url;
        bool https;
        bool allowed;
    };
    const UrlCase cases[] = {
        {"https://api.github.com/repos/Zorkats/G-Diffuser/releases/latest", true, true},
        {"HTTPS://github.com/x", true, true},
        {"https://x.io", true, true},
        {"https://", false, false},
        {"https:///path", false, false},
        {"https:/x.io", false, false},
        {"http://api.github.com/x", false, false},
        {"http://127.0.0.1:8080/x", false, true},
        {"http://localhost/x", false, true},
        {"http://LOCALHOST:1/x", false, true},
        {"http://127.0.0.1.evil.com/x", false, false},
        {"http://localhost.evil.com/x", false, false},
        {"http://localhost@evil.com/x", false, false},
        {"http://127.0.0.2/x", false, false},
        {"ftp://example.com/x", false, false},
        {"https://user@host.example/x", false, false},
        {"https://host.example/a b", false, false},
        {"https://host.example\\x", false, false},
        {"https://host.example/\x7f", false, false},
        {"https://host.example/\n", false, false},
    };
    for (const UrlCase& c : cases) {
        Check(https::IsHttpsUrl(c.url) == c.https, std::string("IsHttpsUrl(") + c.url + ")");
        Check(https::IsAllowedRequestUrl(c.url) == c.allowed, std::string("IsAllowedRequestUrl(") + c.url + ")");
    }
    // The length cap is 8192 bytes: 8191 is the longest URL accepted, 8192 is refused.
    std::string longest = "https://" + std::string(8183, 'a');
    std::string tooLong = "https://" + std::string(8184, 'a');
    Check(longest.size() == 8191 && https::IsHttpsUrl(longest.c_str()), "a URL of 8191 bytes is accepted");
    Check(tooLong.size() == 8192 && !https::IsHttpsUrl(tooLong.c_str()), "a URL of 8192 bytes is refused");
    Check(!https::IsHttpsUrl(nullptr) && !https::IsAllowedRequestUrl(nullptr), "a null URL is refused");

    Response getRejected = https::Get(MakeRequest("http://example.com/", 1024));
    Check(getRejected.error == Error::RejectedUrl && !getRejected.ok && getRejected.status == 0,
          "Get refuses a plain http URL before any socket opens " + Describe(getRejected));
    Response postRejected = https::Post(MakeRequest("https://", 1024));
    Check(postRejected.error == Error::RejectedUrl && !postRejected.ok,
          "Post refuses a URL with no host " + Describe(postRejected));
}

void GetChecks(const LocalServer& server) {
    Response ok = https::Get(MakeRequest(server.Url("/ok"), 1024));
    Check(ok.ok && ok.status == 200 && ok.body == "hello" && ok.error == Error::None, "GET /ok " + Describe(ok));

    Response created = https::Get(MakeRequest(server.Url("/created"), 1024));
    Check(created.ok && created.status == 201 && created.body == "made",
          "a 2xx other than 200 is ok at the helper level " + Describe(created));

    Response big = https::Get(MakeRequest(server.Url("/big"), 4096));
    Check(big.ok && big.status == 200 && big.body == std::string(4096, 'x'), "GET /big at the cap " + Describe(big));

    Response over = https::Get(MakeRequest(server.Url("/big"), 4095));
    Check(over.error == Error::TooLarge && !over.ok && over.body.empty() && over.status == 200,
          "GET /big one byte over the cap " + Describe(over));

    Response zero = https::Get(MakeRequest(server.Url("/ok"), 0));
    Check(zero.error == Error::TooLarge && !zero.ok && zero.body.empty(),
          "maxBodyBytes 0 refuses any body " + Describe(zero));

    Response streamed = https::Get(MakeRequest(server.Url("/nolength"), 4096));
    Check(streamed.ok && streamed.body.size() == 4096, "GET /nolength within the cap " + Describe(streamed));

    Response streamOver = https::Get(MakeRequest(server.Url("/nolength"), 4095));
    Check(streamOver.error == Error::TooLarge && !streamOver.ok && streamOver.body.empty(),
          "GET /nolength refused as it arrives " + Describe(streamOver));

    size_t sinkBytes = 0;
    uint64_t lastDone = 0;
    Request sinkRequest = MakeRequest(server.Url("/big"), 4096);
    sinkRequest.onBody = [&sinkBytes](const void*, size_t len) {
        sinkBytes += len;
        return true;
    };
    sinkRequest.onProgress = [&lastDone](uint64_t done, uint64_t, bool) { lastDone = done; };
    Response sunk = https::Get(sinkRequest);
    Check(sunk.ok && sunk.body.empty() && sinkBytes == 4096 && lastDone == 4096,
          "onBody takes the body and progress ends at 4096 " + Describe(sunk));

    int calls = 0;
    Request refusing = MakeRequest(server.Url("/big"), 4096);
    refusing.onBody = [&calls](const void*, size_t) {
        calls++;
        return false;
    };
    Response refused = https::Get(refusing);
    Check(refused.error == Error::Io && !refused.ok && calls >= 1,
          "a sink that refuses a chunk stops the transfer with Io " + Describe(refused));

    Request headed = MakeRequest(server.Url("/headers"), 16384);
    headed.headers = {"X-Probe: yes", "Accept: application/vnd.github+json"};
    headed.userAgent = "GDX-Test-Agent/1.0";
    Response echoed = https::Get(headed);
    Check(echoed.ok && ContainsNoCase(echoed.body, "X-Probe: yes") &&
              ContainsNoCase(echoed.body, "Accept: application/vnd.github+json") &&
              ContainsNoCase(echoed.body, "User-Agent: GDX-Test-Agent/1.0"),
          "the headers and the user agent reach the server " + Describe(echoed));

    Response defaultAgent = https::Get(MakeRequest(server.Url("/headers"), 16384));
    Check(defaultAgent.ok && ContainsNoCase(defaultAgent.body, "User-Agent: gdx-net"),
          "an empty user agent is sent as gdx-net " + Describe(defaultAgent));

    Request noTimeout = MakeRequest(server.Url("/ok"), 1024);
    noTimeout.timeoutMs = 0;
    Response defaulted = https::Get(noTimeout);
    Check(defaulted.ok && defaulted.body == "hello", "timeoutMs 0 falls back to the default " + Describe(defaulted));
}

void PostChecks(const LocalServer& server) {
    const std::string binary("a\0b\xff", 4);
    Request echo = MakeRequest(server.Url("/echo"), 4096);
    echo.body = binary;
    Response echoed = https::Post(echo);
    Check(echoed.ok && echoed.status == 200 && echoed.body == "POST /echo|" + binary,
          "POST sends a binary body with its NUL intact " + Describe(echoed));

    Response empty = https::Post(MakeRequest(server.Url("/echo"), 4096));
    Check(empty.ok && empty.body == "POST /echo|", "POST with an empty body " + Describe(empty));

    Request large = MakeRequest(server.Url("/echo"), 1 << 20);
    large.body = std::string(3000, 'p');
    Response bigEcho = https::Post(large);
    Check(bigEcho.ok && bigEcho.body == "POST /echo|" + std::string(3000, 'p'),
          "POST sends a 3000 byte body in full " + Describe(bigEcho));

    Request tight = MakeRequest(server.Url("/echo"), 5);
    tight.body = "abc";
    Response overCap = https::Post(tight);
    Check(overCap.error == Error::TooLarge && !overCap.ok && overCap.body.empty(),
          "a POST answer over maxBodyBytes is refused " + Describe(overCap));

    Response posted = https::Post(MakeRequest(server.Url("/created"), 1024));
    Check(posted.ok && posted.status == 201 && posted.body == "made", "POST gets a 201 " + Describe(posted));
}

void FailureChecks(const LocalServer& server) {
    Response missing = https::Get(MakeRequest(server.Url("/status404"), 1024));
    Check(missing.error == Error::Status && missing.status == 404 && !missing.ok && missing.body.empty(),
          "a 404 is a Status error with no body " + Describe(missing));

    Response broken = https::Post(MakeRequest(server.Url("/fail500"), 1024));
    Check(broken.error == Error::Status && broken.status == 500 && !broken.ok && broken.body.empty(),
          "a 500 on POST is a Status error " + Describe(broken));

    Request keep = MakeRequest(server.Url("/status404"), 1024);
    keep.keepErrorBody = true;
    Response kept = https::Get(keep);
    Check(kept.error == Error::Status && kept.status == 404 && !kept.ok && kept.body == "nope",
          "keepErrorBody reads a 404's body and it is still a Status error " + Describe(kept));

    Request keepPost = MakeRequest(server.Url("/fail500"), 1024);
    keepPost.keepErrorBody = true;
    keepPost.body = "x";
    Response keptPost = https::Post(keepPost);
    Check(keptPost.error == Error::Status && keptPost.status == 500 && keptPost.body == "boom",
          "keepErrorBody reads a 500's body on POST " + Describe(keptPost));

    Request keepSmall = MakeRequest(server.Url("/status404"), 2);
    keepSmall.keepErrorBody = true;
    Response keptSmall = https::Get(keepSmall);
    Check(keptSmall.error == Error::TooLarge && keptSmall.body.empty(),
          "an error body over the cap is refused like any body " + Describe(keptSmall));

    Response limited = https::Get(MakeRequest(server.Url("/limited"), 1024));
    Check(limited.error == Error::Status && limited.status == 429 && limited.retryAfter == "7" && limited.body.empty(),
          "Retry-After is reported trimmed, and the body stays unread by default " + Describe(limited) +
              " retryAfter=" + limited.retryAfter);

    Request keepLimited = MakeRequest(server.Url("/limitednolength"), 1024);
    keepLimited.keepErrorBody = true;
    Response limitedBody = https::Get(keepLimited);
    Check(limitedBody.error == Error::Status && limitedBody.body == "slow down" && limitedBody.retryAfter.empty(),
          "an error body with no Content-Length is read to the close, and no Retry-After is empty " +
              Describe(limitedBody));

    Response okNoRetry = https::Get(MakeRequest(server.Url("/ok"), 1024));
    Check(okNoRetry.ok && okNoRetry.retryAfter.empty(), "a 200 with no Retry-After reports none");

    Response refusedGet = https::Get(MakeRequest(server.ClosedHttpUrl("/"), 1024));
    Check(refusedGet.error == Error::Network && refusedGet.status == 0 && !refusedGet.ok,
          "a refused connection is a Network error " + Describe(refusedGet));

    Response refusedPost = https::Post(MakeRequest(server.ClosedHttpUrl("/"), 1024));
    Check(refusedPost.error == Error::Network && !refusedPost.ok,
          "a refused POST is a Network error " + Describe(refusedPost));

    Response tls = https::Get(MakeRequest(server.ClosedHttpsUrl("/"), 1024));
    Check(tls.error == Error::Network && !tls.ok,
          "an https URL to a closed port fails at connect, after the URL rule " + Describe(tls));

    Request slow = MakeRequest(server.Url("/slow"), 1024);
    slow.timeoutMs = 300;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Response timedOut = https::Get(slow);
    long long elapsed = ElapsedMs(start);
    Check(timedOut.error == Error::Network && !timedOut.ok && timedOut.body.empty(),
          "a 300 ms timeout against a 3 s reply is a Network error " + Describe(timedOut));
    Check(elapsed >= 250 && elapsed < 2500,
          "the 300 ms timeout fires on time, took " + std::to_string(elapsed) + " ms");
}

void RedirectChecks(const LocalServer& server) {
    int before = server.Hits();
    Response toHttp = https::Get(MakeRequest(server.Url("/redirhttp"), 1024));
    Check(toHttp.error == Error::Redirect && !toHttp.ok && toHttp.status == 302,
          "a redirect to plain http is refused " + Describe(toHttp));
    Check(server.Hits() - before == 1, "the refused http target is never requested");

    before = server.Hits();
    Response toHttps = https::Get(MakeRequest(server.Url("/redirhttps"), 1024));
    Check(toHttps.error == Error::Network && !toHttps.ok,
          "a redirect to https is followed, and its closed port refuses it " + Describe(toHttps));
    Check(server.Hits() - before == 1, "the followed https hop never reaches this plain server");

    before = server.Hits();
    Request postRedirect = MakeRequest(server.Url("/redirhttps307"), 1024);
    postRedirect.body = "payload";
    Response postToHttps = https::Post(postRedirect);
    Check(postToHttps.error == Error::Network && !postToHttps.ok,
          "a 307 to https is followed on POST and refused at connect " + Describe(postToHttps));
    Check(server.Hits() - before == 1, "the 307 hop is attempted once, not repeated");

    Response noLocation = https::Get(MakeRequest(server.Url("/redirnoloc"), 1024));
    Check(noLocation.error == Error::Redirect && !noLocation.ok && noLocation.status == 302,
          "a redirect with no Location is a Redirect error " + Describe(noLocation));
}

}  // namespace

int main() {
    SocketRuntime runtime;
    UrlPolicyChecks();
    LocalServer server;
    if (!server.Ready()) {
        Check(false, "the local server started on 127.0.0.1");
    } else {
        GetChecks(server);
        PostChecks(server);
        FailureChecks(server);
        RedirectChecks(server);
    }
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
