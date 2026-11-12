// port/net/gdx_net_https.cpp: blocking HTTPS transport behind gdx_net_https.h, WinHTTP or libcurl (G-NET O5.4).
// Copyright (c) Zorkats

#include "gdx_net_https.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace gdx::net::https {
namespace {

constexpr int kDefaultTimeoutMs = 15000;
constexpr int kMaxRedirects = 5;
// Matches the cap the update check has always used. Longer URLs are refused, never cut.
constexpr size_t kMaxUrlBytes = 8192;
// The shortest https URL the updater has ever accepted. Checking the length first keeps the prefix read in bounds.
constexpr size_t kMinHttpsUrlBytes = 12;
// WinHTTP takes the body length as a 32-bit DWORD, so both transports refuse a larger body the same way.
constexpr size_t kMaxRequestBodyBytes = 0x7FFFFFFFu;

enum class Method : uint8_t { Get, Post };

// Everything one response hop produces. Only the final hop's body reaches the caller.
struct HopResult {
    int status = 0;
    bool haveLocation = false;
    std::string location;
    Error error = Error::None;
    std::string detail;
    std::string body;
    std::string retryAfter;
};

bool IsRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool IsSuccessStatus(int status) {
    return status >= 200 && status < 300;
}

std::string TrimSpaces(const std::string& text) {
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return {};
    }
    size_t end = text.find_last_not_of(" \t");
    return text.substr(start, end - start + 1);
}

int TimeoutMs(const Request& request) {
    return request.timeoutMs > 0 ? request.timeoutMs : kDefaultTimeoutMs;
}

// Compares text with a lowercase ASCII prefix, ignoring case. Stops at the end of text.
bool PrefixIgnoreCase(const char* text, const char* prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (text[i] == '\0' || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

// Bytes that could split a request line or hide the host. Checked on every byte of the URL.
bool UrlBytesSafe(const char* url, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c <= 0x20 || c == 0x7F || c == '\\' || c == '@') {
            return false;
        }
    }
    return true;
}

bool IsLoopbackHttpUrl(const char* url) {
    size_t n = std::strlen(url);
    if (n >= kMaxUrlBytes) {
        return false;
    }
    static const char* const kHosts[] = {"http://127.0.0.1", "http://localhost"};
    for (const char* prefix : kHosts) {
        if (PrefixIgnoreCase(url, prefix)) {
            // The host must end here, so "127.0.0.1.example" and "localhost@example" stay refused.
            char next = url[std::strlen(prefix)];
            return (next == '\0' || next == ':' || next == '/') && UrlBytesSafe(url, n);
        }
    }
    return false;
}

// Log text for a refused body chunk, so the detail says which limit or sink refused it.
const char* BodyErrorText(Error error) {
    return error == Error::TooLarge ? "body over maxBodyBytes" : "body sink refused the chunk";
}

// Takes the body of one 2xx response, into memory or into the caller's sink. It enforces maxBodyBytes and reports
// progress. It is the only place a body byte is counted.
class BodyTarget {
public:
    // memory receives the body when request.onBody is not set. Pass null when it is set.
    BodyTarget(const Request& request, std::string* memory) : request_(request), memory_(memory) {}

    // False when the chunk is refused. error() then says why. A zero-length chunk is always accepted.
    bool Accept(const void* data, size_t len) {
        if (len == 0) {
            return true;
        }
        if (len > request_.maxBodyBytes - written_) {
            error_ = Error::TooLarge;
            return false;
        }
        if (request_.onBody) {
            if (!request_.onBody(data, len)) {
                error_ = Error::Io;
                return false;
            }
        } else {
            memory_->append(static_cast<const char*>(data), len);
        }
        written_ += len;
        if (request_.onProgress) {
            request_.onProgress(written_, 0, false);
        }
        return true;
    }

    // Called once when a Content-Length is known, before any body byte.
    void ReportTotal(uint64_t total) const {
        if (request_.onProgress) {
            request_.onProgress(0, total, true);
        }
    }

    Error error() const {
        return error_;
    }

    uint64_t written() const {
        return written_;
    }

private:
    const Request& request_;
    std::string* memory_;
    uint64_t written_ = 0;
    Error error_ = Error::None;
};

#ifdef _WIN32

// Read in 64 KB pieces, as the updater always did, so a large download never sits whole in the stack frame.
constexpr size_t kChunkBytes = 65536;

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out((size_t)n, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), n) != n) {
        return {};
    }
    return out;
}

struct InternetHandle {
    HINTERNET handle = nullptr;
    InternetHandle() = default;
    ~InternetHandle() {
        reset();
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    void reset(HINTERNET next = nullptr) {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
        handle = next;
    }
};

// One WinHTTP session per call. Its user agent and TLS protocols are fixed when it opens.
struct Transport {
    InternetHandle session;
};

bool OpenTransport(const Request& request, Transport& transport, Response& response) {
    std::wstring agent = Utf8ToWide(request.userAgent.empty() ? std::string("gdx-net") : request.userAgent);
    if (agent.empty()) {
        response.error = Error::Network;
        response.detail = "user agent";
        return false;
    }
    transport.session.reset(
        WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (transport.session.handle == nullptr) {
        response.error = Error::Network;
        response.detail = "winhttp open failed";
        return false;
    }
    int ms = TimeoutMs(request);
    WinHttpSetTimeouts(transport.session.handle, ms, ms, ms, ms);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(transport.session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    return true;
}

// Splits an http or https URL. secure reports which one it was.
bool CrackUrl(const std::wstring& url, std::wstring* host, INTERNET_PORT* port, std::wstring* object, bool* secure) {
    URL_COMPONENTSW parts{};
    wchar_t hostBuf[256];
    wchar_t pathBuf[8192];
    wchar_t extraBuf[8192];
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = hostBuf;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = pathBuf;
    parts.dwUrlPathLength = 8191;
    parts.lpszExtraInfo = extraBuf;
    parts.dwExtraInfoLength = 8191;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        return false;
    }
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) {
        *secure = true;
    } else if (parts.nScheme == INTERNET_SCHEME_HTTP) {
        *secure = false;
    } else {
        return false;
    }
    if (parts.dwHostNameLength == 0 || parts.dwHostNameLength >= 255) {
        return false;
    }
    hostBuf[parts.dwHostNameLength] = L'\0';
    *host = hostBuf;
    *port = parts.nPort;
    object->assign(pathBuf, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        object->append(extraBuf, parts.dwExtraInfoLength);
    }
    if (object->empty()) {
        *object = L"/";
    }
    return true;
}

// True when the request's final URL is still https. Used only for https requests, as a second check on the policy.
bool RequestUrlIsHttps(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &bytes);
    if (bytes < sizeof(wchar_t) || bytes > 16384) {
        return false;
    }
    std::wstring url(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &bytes)) {
        return false;
    }
    return url.rfind(L"https://", 0) == 0 || url.rfind(L"HTTPS://", 0) == 0;
}

// False when the header is present but is not a plain decimal. Absent means present is false, and still true.
bool ParseContentLength(HINTERNET request, uint64_t* out, bool* present) {
    *present = false;
    *out = 0;
    wchar_t buf[32];
    DWORD bytes = sizeof(buf);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return true;
    }
    uint64_t value = 0;
    for (wchar_t* p = buf; *p != L'\0'; p++) {
        if (*p < L'0' || *p > L'9') {
            return false;
        }
        uint64_t digit = (uint64_t)(*p - L'0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *present = true;
    *out = value;
    return true;
}

// The Retry-After header, narrowed and trimmed, or empty when it is absent or not plain ASCII.
std::string QueryRetryAfter(HINTERNET request) {
    wchar_t buf[64];
    DWORD bytes = sizeof(buf);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, buf, &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    std::string out;
    for (DWORD i = 0; i < bytes / sizeof(wchar_t) && buf[i] != L'\0'; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7e) {
            return {};
        }
        out.push_back(static_cast<char>(buf[i]));
    }
    return TrimSpaces(out);
}

HopResult RunHop(Transport& transport, Method method, const std::string& url, const Request& request) {
    HopResult hop;
    std::wstring wideUrl = Utf8ToWide(url);
    std::wstring host;
    std::wstring object;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (wideUrl.empty() || !CrackUrl(wideUrl, &host, &port, &object, &secure)) {
        hop.error = Error::RejectedUrl;
        hop.detail = "url rejected";
        return hop;
    }
    InternetHandle connection;
    connection.reset(WinHttpConnect(transport.session.handle, host.c_str(), port, 0));
    if (connection.handle == nullptr) {
        hop.error = Error::Network;
        hop.detail = "connect failed";
        return hop;
    }
    InternetHandle handle;
    handle.reset(WinHttpOpenRequest(connection.handle, method == Method::Post ? L"POST" : L"GET", object.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (handle.handle == nullptr) {
        hop.error = Error::Network;
        hop.detail = "open request failed";
        return hop;
    }
    // Redirects are followed by hand, so each hop's URL passes the https rule before it is requested. Set no
    // redirect policy here: the policy and this flag share one switch, and a policy set later turns redirects back on.
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(handle.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    for (const std::string& line : request.headers) {
        std::wstring wideLine = Utf8ToWide(line);
        if (wideLine.empty() || !WinHttpAddRequestHeaders(handle.handle, wideLine.c_str(), (DWORD)-1,
                                                          WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            hop.error = Error::Network;
            hop.detail = "header failed";
            return hop;
        }
    }
    bool post = method == Method::Post;
    const void* payload = post ? static_cast<const void*>(request.body.data()) : nullptr;
    DWORD length = post ? static_cast<DWORD>(request.body.size()) : 0;
    if (!WinHttpSendRequest(handle.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<void*>(payload), length, length,
                            0) ||
        !WinHttpReceiveResponse(handle.handle, nullptr)) {
        hop.error = Error::Network;
        hop.detail = "request failed";
        return hop;
    }
    if (secure && !RequestUrlIsHttps(handle.handle)) {
        hop.error = Error::RejectedUrl;
        hop.detail = "final url was not https";
        return hop;
    }
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) {
        hop.error = Error::Network;
        hop.detail = "status missing";
        return hop;
    }
    hop.status = (int)status;
    if (IsRedirectStatus(hop.status)) {
        DWORD locBytes = 0;
        WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                            &locBytes, WINHTTP_NO_HEADER_INDEX);
        if (locBytes == 0 || locBytes > 16384) {
            hop.error = Error::Redirect;
            hop.detail = "redirect missing location";
            return hop;
        }
        std::wstring location(locBytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                                 &locBytes, WINHTTP_NO_HEADER_INDEX)) {
            hop.error = Error::Redirect;
            hop.detail = "redirect location failed";
            return hop;
        }
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, location.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) {
            hop.error = Error::Redirect;
            hop.detail = "redirect location rejected";
            return hop;
        }
        std::vector<char> narrow((size_t)n);
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, location.c_str(), -1, narrow.data(), n, nullptr, nullptr);
        hop.location.assign(narrow.data(), (size_t)n - 1);
        hop.haveLocation = true;
        return hop;
    }
    hop.retryAfter = QueryRetryAfter(handle.handle);
    bool success = IsSuccessStatus(hop.status);
    if (!success && !request.keepErrorBody) {
        hop.error = Error::Status;
        hop.detail = "http status";
        return hop;
    }
    uint64_t contentLength = 0;
    bool haveLength = false;
    if (!ParseContentLength(handle.handle, &contentLength, &haveLength)) {
        hop.error = Error::Network;
        hop.detail = "bad content length";
        return hop;
    }
    BodyTarget target(request, request.onBody ? nullptr : &hop.body);
    if (haveLength) {
        if (contentLength > request.maxBodyBytes) {
            hop.error = Error::TooLarge;
            hop.detail = "content length over cap";
            return hop;
        }
        target.ReportTotal(contentLength);
    }
    uint8_t buf[kChunkBytes];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(handle.handle, buf, sizeof(buf), &read)) {
            hop.error = Error::Network;
            hop.detail = "read failed";
            return hop;
        }
        if (read == 0) {
            break;
        }
        if (!target.Accept(buf, read)) {
            hop.error = target.error();
            hop.detail = BodyErrorText(hop.error);
            return hop;
        }
    }
    if (haveLength && target.written() != contentLength) {
        hop.error = Error::Network;
        hop.detail = "short body";
    } else if (!success) {
        hop.error = Error::Status;
        hop.detail = "http status";
    }
    return hop;
}

#else  // libcurl, loaded with dlopen so the binary does not link it.

// Option and info numbers from curl/curl.h. There is no curl.h on the build machine, so the WSL test is what proves
// them: a wrong number changes the timeout, the body or the status it sees.
constexpr int kCurlWriteData = 10001;
constexpr int kCurlUrl = 10002;
constexpr int kCurlErrorBuffer = 10010;
constexpr int kCurlPostFields = 10015;
constexpr int kCurlUserAgent = 10018;
constexpr int kCurlHttpHeader = 10023;
constexpr int kCurlHeaderData = 10029;
constexpr int kCurlPost = 47;
constexpr int kCurlFollowLocation = 52;
constexpr int kCurlPostFieldSize = 60;
constexpr int kCurlSslVerifyPeer = 64;
constexpr int kCurlConnectTimeout = 78;
constexpr int kCurlSslVerifyHost = 81;
constexpr int kCurlNoSignal = 99;
constexpr int kCurlTimeoutMs = 155;
constexpr int kCurlWriteFunction = 20011;
constexpr int kCurlHeaderFunction = 20079;
constexpr int kCurlInfoResponseCode = 0x200002;
// libcurl caps the connect phase at 15 s for every request, as the updater always has. WinHTTP applies the request
// timeout to connect as well, so the two differ only for a connect that hangs longer than 15 s.
constexpr long kConnectTimeoutSec = 15;

// curl_slist is two pointers. curl_slist_append is outside the symbol list the release is checked against, so the
// header list is built by hand.
struct CurlSlist {
    char* data;
    CurlSlist* next;
};

struct CurlApi {
    void* lib = nullptr;
    void* (*easy_init)(void) = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    int (*global_init)(long) = nullptr;
    bool ready = false;
    bool failed = false;
};

template <typename T>
bool LoadSymbol(void* lib, const char* name, T* out) {
    *out = reinterpret_cast<T>(dlsym(lib, name));
    return *out != nullptr;
}

CurlApi& CurlApiInstance() {
    static CurlApi api;
    if (api.ready || api.failed) {
        return api;
    }
    const char* paths[] = {
#if defined(__APPLE__)
        "/usr/lib/libcurl.4.dylib",
#else
        "libcurl.so.4",
        "libcurl.so",
#endif
        nullptr};
    for (int i = 0; paths[i] != nullptr && api.lib == nullptr; i++) {
        api.lib = dlopen(paths[i], RTLD_NOW);
    }
    if (api.lib == nullptr) {
        api.failed = true;
        return api;
    }
    if (!LoadSymbol(api.lib, "curl_easy_init", &api.easy_init) ||
        !LoadSymbol(api.lib, "curl_easy_setopt", &api.easy_setopt) ||
        !LoadSymbol(api.lib, "curl_easy_perform", &api.easy_perform) ||
        !LoadSymbol(api.lib, "curl_easy_getinfo", &api.easy_getinfo) ||
        !LoadSymbol(api.lib, "curl_easy_cleanup", &api.easy_cleanup) ||
        !LoadSymbol(api.lib, "curl_easy_strerror", &api.easy_strerror) ||
        !LoadSymbol(api.lib, "curl_global_init", &api.global_init) || api.global_init(3) != 0) {
        api.failed = true;
        return api;
    }
    api.ready = true;
    return api;
}

struct Transport {
    CurlApi* api = nullptr;
};

bool OpenTransport(const Request&, Transport& transport, Response& response) {
    CurlApi& api = CurlApiInstance();
    if (!api.ready) {
        response.error = Error::Unsupported;
        response.detail = "libcurl unavailable";
        return false;
    }
    transport.api = &api;
    return true;
}

// Frees an easy handle on every path out of a hop.
class Easy {
public:
    explicit Easy(CurlApi& api) : api_(api), handle_(api.easy_init()) {}
    ~Easy() {
        if (handle_ != nullptr) {
            api_.easy_cleanup(handle_);
        }
    }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
    void* get() const {
        return handle_;
    }

private:
    CurlApi& api_;
    void* handle_;
};

// What the libcurl callbacks write into, for one hop.
struct CurlHopState {
    BodyTarget* body = nullptr;
    bool acceptBody = false;
    bool keepErrorBody = false;
    bool haveLocation = false;
    bool badLocation = false;
    std::string location;
    std::string retryAfter;
};

bool HeaderNameIs(const char* line, size_t len, const char* name) {
    size_t n = std::strlen(name);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) {
            return false;
        }
    }
    return true;
}

void NoteLocation(CurlHopState* state, const char* value, size_t len) {
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
        len--;
    }
    size_t start = 0;
    while (start < len && (value[start] == ' ' || value[start] == '\t')) {
        start++;
    }
    if (start >= len || len - start >= kMaxUrlBytes) {
        state->badLocation = true;
        state->haveLocation = false;
        state->location.clear();
        return;
    }
    state->location.assign(value + start, len - start);
    state->haveLocation = true;
    state->badLocation = false;
}

size_t CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t nbytes = size * nmemb;
    CurlHopState* state = static_cast<CurlHopState*>(userdata);
    if (!state->acceptBody) {
        return nbytes;
    }
    if (!state->body->Accept(ptr, nbytes)) {
        return 0;
    }
    return nbytes;
}

size_t CurlHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t nbytes = size * nmemb;
    CurlHopState* state = static_cast<CurlHopState*>(userdata);
    size_t len = nbytes;
    while (len > 0 && (ptr[len - 1] == '\n' || ptr[len - 1] == '\r')) {
        len--;
    }
    if (len >= 5 && HeaderNameIs(ptr, len, "http/")) {
        // Every read is bounded by len first: a status line with no line ending has no byte past len to look at.
        size_t i = 0;
        while (i < len && ptr[i] != ' ') {
            i++;
        }
        while (i < len && ptr[i] == ' ') {
            i++;
        }
        int code = 0;
        int digits = 0;
        while (i < len && digits < 3 && std::isdigit(static_cast<unsigned char>(ptr[i]))) {
            code = code * 10 + (ptr[i] - '0');
            i++;
            digits++;
        }
        // Informational lines (1xx) come before the final status and must not open the body.
        if (digits == 3 && code >= 200) {
            // A redirect's body is never read. Any other final answer opens it when it is 2xx, or when asked.
            state->acceptBody = IsSuccessStatus(code) || (state->keepErrorBody && !IsRedirectStatus(code));
            state->retryAfter.clear();
        }
    } else if (HeaderNameIs(ptr, len, "location:")) {
        NoteLocation(state, ptr + 9, len - 9);
    } else if (HeaderNameIs(ptr, len, "retry-after:")) {
        std::string value(ptr + 12, len - 12);
        bool printable = true;
        for (char c : value) {
            printable = printable && c >= 0x20 && c <= 0x7e;
        }
        state->retryAfter = printable && value.size() < 64 ? TrimSpaces(value) : std::string();
    }
    return nbytes;
}

HopResult RunHop(Transport& transport, Method method, const std::string& url, const Request& request) {
    CurlApi& api = *transport.api;
    HopResult hop;
    Easy easy(api);
    if (easy.get() == nullptr) {
        hop.error = Error::Network;
        hop.detail = "curl init failed";
        return hop;
    }
    // Owned by this frame, and alive until perform returns.
    std::vector<CurlSlist> lines(request.headers.size());
    CurlSlist* list = nullptr;
    for (size_t i = lines.size(); i-- > 0;) {
        lines[i].data = const_cast<char*>(request.headers[i].c_str());
        lines[i].next = list;
        list = &lines[i];
    }
    BodyTarget target(request, request.onBody ? nullptr : &hop.body);
    CurlHopState state;
    state.body = &target;
    state.keepErrorBody = request.keepErrorBody;
    char errbuf[256];
    errbuf[0] = '\0';
    const char* agent = request.userAgent.empty() ? "gdx-net" : request.userAgent.c_str();
    bool post = method == Method::Post;
    long timeoutMs = (long)TimeoutMs(request);
    bool optionsOk = api.easy_setopt(easy.get(), kCurlUrl, url.c_str()) == 0 &&
                     api.easy_setopt(easy.get(), kCurlUserAgent, agent) == 0 &&
                     api.easy_setopt(easy.get(), kCurlHttpHeader, list) == 0 &&
                     api.easy_setopt(easy.get(), kCurlWriteFunction, CurlWrite) == 0 &&
                     api.easy_setopt(easy.get(), kCurlWriteData, &state) == 0 &&
                     api.easy_setopt(easy.get(), kCurlHeaderFunction, CurlHeader) == 0 &&
                     api.easy_setopt(easy.get(), kCurlHeaderData, &state) == 0 &&
                     api.easy_setopt(easy.get(), kCurlFollowLocation, (long)0) == 0 &&
                     api.easy_setopt(easy.get(), kCurlSslVerifyPeer, (long)1) == 0 &&
                     api.easy_setopt(easy.get(), kCurlSslVerifyHost, (long)2) == 0 &&
                     api.easy_setopt(easy.get(), kCurlNoSignal, (long)1) == 0 &&
                     api.easy_setopt(easy.get(), kCurlConnectTimeout, kConnectTimeoutSec) == 0 &&
                     api.easy_setopt(easy.get(), kCurlTimeoutMs, timeoutMs) == 0 &&
                     api.easy_setopt(easy.get(), kCurlErrorBuffer, errbuf) == 0;
    if (optionsOk && post) {
        optionsOk = api.easy_setopt(easy.get(), kCurlPost, (long)1) == 0 &&
                    api.easy_setopt(easy.get(), kCurlPostFieldSize, (long)request.body.size()) == 0 &&
                    api.easy_setopt(easy.get(), kCurlPostFields, const_cast<char*>(request.body.data())) == 0;
    }
    if (!optionsOk) {
        hop.error = Error::Network;
        hop.detail = "curl option failed";
        return hop;
    }
    int rc = api.easy_perform(easy.get());
    // Read even when the transfer stopped early, so a refused body still reports the status the server sent.
    long status = 0;
    api.easy_getinfo(easy.get(), kCurlInfoResponseCode, &status);
    hop.status = (int)status;
    hop.retryAfter = state.retryAfter;
    if (rc != 0) {
        // A refused chunk aborts the transfer, so the sink's reason is the one to report.
        if (target.error() != Error::None) {
            hop.error = target.error();
            hop.detail = BodyErrorText(hop.error);
        } else {
            hop.error = Error::Network;
            hop.detail = errbuf[0] != '\0' ? errbuf : "curl perform failed";
        }
        return hop;
    }
    if (IsRedirectStatus(hop.status)) {
        if (!state.haveLocation || state.badLocation) {
            hop.error = Error::Redirect;
            hop.detail = "redirect location rejected";
            return hop;
        }
        hop.haveLocation = true;
        hop.location = state.location;
        return hop;
    }
    if (!IsSuccessStatus(hop.status)) {
        hop.error = Error::Status;
        hop.detail = "http status";
    }
    return hop;
}

#endif

// Follows redirects by hand. A POST becomes a GET on 303, and on 301 and 302, as browsers do. 307 and 308 keep the
// method and the body. Every hop after the first must be https, loopback included.
Response Send(Method method, const Request& request) {
    Response response;
    if (!IsAllowedRequestUrl(request.url.c_str())) {
        response.error = Error::RejectedUrl;
        response.detail = "url rejected";
        return response;
    }
    if (method == Method::Post && request.body.size() > kMaxRequestBodyBytes) {
        response.error = Error::TooLarge;
        response.detail = "request body too large";
        return response;
    }
    Transport transport;
    if (!OpenTransport(request, transport, response)) {
        return response;
    }
    std::string url = request.url;
    Method current = method;
    for (int hop = 0; hop < kMaxRedirects; hop++) {
        HopResult result = RunHop(transport, current, url, request);
        response.status = result.status;
        if (result.haveLocation) {
            if (!IsHttpsUrl(result.location.c_str())) {
                response.error = Error::Redirect;
                response.detail = "redirect was not https";
                return response;
            }
            if (result.status == 303 || ((result.status == 301 || result.status == 302) && current == Method::Post)) {
                current = Method::Get;
            }
            url = result.location;
            continue;
        }
        response.error = result.error;
        response.detail = result.detail;
        response.retryAfter = std::move(result.retryAfter);
        if (result.error == Error::None && IsSuccessStatus(result.status)) {
            response.ok = true;
            response.body = std::move(result.body);
        } else if (result.error == Error::Status && request.keepErrorBody) {
            response.body = std::move(result.body);
        }
        return response;
    }
    response.error = Error::Redirect;
    response.detail = "too many redirects";
    return response;
}

}  // namespace

bool IsHttpsUrl(const char* url) {
    if (url == nullptr) {
        return false;
    }
    size_t n = std::strlen(url);
    if (n < kMinHttpsUrlBytes || n >= kMaxUrlBytes) {
        return false;
    }
    if (!PrefixIgnoreCase(url, "https://")) {
        return false;
    }
    if (url[8] == '/' || url[8] == '\\') {
        return false;
    }
    return UrlBytesSafe(url, n);
}

bool IsAllowedRequestUrl(const char* url) {
    if (url == nullptr) {
        return false;
    }
    return IsHttpsUrl(url) || IsLoopbackHttpUrl(url);
}

Response Get(const Request& request) {
    return Send(Method::Get, request);
}

Response Post(const Request& request) {
    return Send(Method::Post, request);
}

}  // namespace gdx::net::https
