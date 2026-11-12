// port/net/gdx_net_https.h: blocking HTTPS GET and POST for the update check and the directory client (G-NET O5.4).
// Copyright (c) Zorkats
//
// The transport is WinHTTP on Windows and libcurl, loaded with dlopen, everywhere else. Both apply the same rules:
// certificates are verified against the system store, redirects are followed by hand (at most five, each one https),
// every body is capped, and one timeout covers the whole request. Nothing here throws; a failure is an Error value.
//
// Not headless: the transport opens sockets and blocks the calling thread. The caller runs it on its own worker
// thread, never on the game thread.
//
// Get and Post never retry and never cache. The only header they add is the User-Agent, so the caller owns Accept,
// the body format and what a status means.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gdx::net::https {

// Why a request failed. ok is false on every error, and detail carries the text for the log.
enum class Error : uint8_t {
    None,         // a response arrived and this module did not refuse it
    Unsupported,  // no transport here: libcurl is not installed (Linux and macOS only)
    RejectedUrl,  // the first URL is not allowed, see IsAllowedRequestUrl
    Redirect,     // a redirect had no usable Location, pointed off https, or there were too many
    TooLarge,     // the body is over maxBodyBytes, by its Content-Length or as it arrived
    Io,           // the body sink refused a chunk
    Network,      // connect, TLS, timeout, or a broken read
    Status,       // the server answered with a status outside 2xx
};

// Receives body bytes in arrival order, on the calling thread. Returning false stops the transfer with Error::Io.
using BodySink = std::function<bool(const void* data, size_t len)>;

// Reports progress on the calling thread. haveTotal is true when total is the advertised Content-Length.
using ProgressFn = std::function<void(uint64_t done, uint64_t total, bool haveTotal)>;

struct Request {
    std::string url;

    // Full header lines, "Name: value", sent as given.
    std::vector<std::string> headers;

    // Sent as the User-Agent. Empty means "gdx-net".
    std::string userAgent;

    // The POST payload. Get never sends it.
    std::string body;

    // The whole request, in milliseconds. Zero or less means 15000. WinHTTP applies it to every phase, with a coarse
    // timer: a 300 ms limit was measured firing after 1 s but before 2.5 s. libcurl applies it to the whole
    // transfer and limits connect to 15 seconds.
    int timeoutMs = 15000;

    // The largest body accepted. A body is refused as it arrives once it would pass this, and before it when the
    // Content-Length already says so. Zero refuses every non-empty body.
    uint64_t maxBodyBytes = 0;

    // When set, body bytes go here and Response.body stays empty. Use it for bodies too large to hold in memory.
    BodySink onBody;

    ProgressFn onProgress;

    // When true, the body of a final non-2xx answer is read too, under the same cap, into Response.body. The error
    // stays Status and ok stays false. The directory needs it: its 4xx and 5xx bodies carry the error token.
    bool keepErrorBody = false;
};

struct Response {
    // The final status, or 0 when no status line arrived.
    int status = 0;

    // The body of a 2xx answer, or of a non-2xx answer when keepErrorBody was set. Empty on every other error, and
    // whenever onBody was set.
    std::string body;

    // The final answer's Retry-After header as sent, trimmed, or empty when it had none.
    std::string retryAfter;

    // True when error is None and status is 2xx. The caller decides whether it wants a 200 or a 201.
    bool ok = false;

    Error error = Error::None;
    std::string detail;
};

// True for an https URL with a host. Control bytes, spaces, backslashes and '@' are refused, so the host cannot hide
// behind userinfo or a stray character.
bool IsHttpsUrl(const char* url);

// The rule for the first URL: IsHttpsUrl, or plain http to 127.0.0.1 or localhost, which is how the update test seam
// reaches a local server. A redirect target is never plain http, loopback or not.
bool IsAllowedRequestUrl(const char* url);

Response Get(const Request& request);
Response Post(const Request& request);

}  // namespace gdx::net::https
