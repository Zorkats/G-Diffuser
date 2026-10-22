// port/gdx_update.cpp -- in-game update check, verified download, and the gdx-update handoff.

#include "gdx_update.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "rapidjson/document.h"

#ifndef GDX_VERSION_STRING
#define GDX_VERSION_STRING "0.0.0"
#endif
#ifndef GDX_BUILD_CHANNEL
#define GDX_BUILD_CHANNEL "dev"
#endif
#ifndef GDX_BUILD_COMMIT
#define GDX_BUILD_COMMIT ""
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#ifndef GDX_UPDATE_TESTS_STANDALONE
#include "port_log.h"
#include "ship/Context.h"
extern "C" void gdx_request_quit(void);
#else
static void gdx_port_logf(const char* fmt, ...) {
    (void)fmt;
}
#endif

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

extern "C" int gdx_update_nightly_offer(const char* remoteDate, const char* remoteHash, const char* localDate,
                                        const char* localHash);

namespace {

constexpr int kCheckTimeoutSec = 15;
// A download may be up to 512 MB, so it cannot share the 15s check timeout.
// The cap still ends a stuck socket instead of pinning the worker.
constexpr int kDownloadTimeoutSec = 30 * 60;
constexpr int kMaxRedirects = 5;
constexpr uint64_t kMaxDownloadBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kMaxCheckBytes = 2ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSumsBytes = 1ull * 1024ull * 1024ull;
constexpr size_t kMaxAssetName = 200;
constexpr size_t kMaxUrl = 2048;
constexpr size_t kMaxRedirectUrl = 8192;

constexpr const char* kErrLibcurl = "Automatic updates need libcurl; open the release page instead";
constexpr const char* kErrVerify = "Download failed verification";
constexpr const char* kErrHelper = "gdx-update is missing; reinstall from the release page";
constexpr const char* kErrCheck = "Update check failed";
constexpr const char* kErrDownload = "Download failed";
constexpr const char* kErrPath = "Update path is unavailable";
constexpr const char* kErrLaunch = "Could not start gdx-update";

struct Sha256 {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t blockLen;
    bool overflow;
};

uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

uint32_t load_be(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

void store_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void sha256_block(Sha256* s, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be(block + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0];
    uint32_t b = s->h[1];
    uint32_t c = s->h[2];
    uint32_t d = s->h[3];
    uint32_t e = s->h[4];
    uint32_t f = s->h[5];
    uint32_t g = s->h[6];
    uint32_t hh = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += hh;
}

void sha256_init(Sha256* s) {
    s->h[0] = 0x6a09e667u;
    s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu;
    s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu;
    s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->blockLen = 0;
    s->overflow = false;
}

void sha256_update(Sha256* s, const void* data, size_t len) {
    if (s->overflow || data == nullptr || len == 0) {
        if (data == nullptr && len > 0) {
            s->overflow = true;
        }
        return;
    }
    if (len > (UINT64_MAX / 8) || s->bits > UINT64_MAX - (uint64_t)len * 8) {
        s->overflow = true;
        return;
    }
    s->bits += (uint64_t)len * 8;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        size_t space = 64 - s->blockLen;
        size_t take = len < space ? len : space;
        std::memcpy(s->block + s->blockLen, p, take);
        s->blockLen += take;
        p += take;
        len -= take;
        if (s->blockLen == 64) {
            sha256_block(s, s->block);
            s->blockLen = 0;
        }
    }
}

void sha256_final(Sha256* s, unsigned char out[32]) {
    uint8_t block[64];
    std::memcpy(block, s->block, s->blockLen);
    block[s->blockLen] = 0x80;
    if (s->blockLen >= 56) {
        std::memset(block + s->blockLen + 1, 0, 64 - (s->blockLen + 1));
        sha256_block(s, block);
        std::memset(block, 0, 64);
    } else {
        std::memset(block + s->blockLen + 1, 0, 56 - (s->blockLen + 1));
    }
    for (int i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)(s->bits >> (i * 8));
    }
    sha256_block(s, block);
    for (int i = 0; i < 8; i++) {
        store_be(out + i * 4, s->h[i]);
    }
}

void to_hex(const unsigned char in[32], char out[65]) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0F];
    }
    out[64] = '\0';
}

bool is_hex(unsigned char c) {
    return std::isxdigit(c) != 0;
}

bool eight_digits(const char* s) {
    if (s == nullptr) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return s[8] == '\0';
}

bool channel_is(const char* name) {
    return std::strcmp(GDX_BUILD_CHANNEL, name) == 0;
}

bool channel_offers() {
    return channel_is("stable") || channel_is("nightly");
}

const char* update_platform() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__)
    return "macos-arm64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__)
    return "linux-x64";
#else
    return "";
#endif
}

const char* helper_file_name() {
#ifdef _WIN32
    return "gdx-update.exe";
#else
    return "gdx-update";
#endif
}

const char* helper_run_name() {
#ifdef _WIN32
    return "gdx-update-run.exe";
#else
    return "gdx-update-run";
#endif
}

const char* game_file_name() {
#ifdef _WIN32
    return "G-Diffuser.exe";
#else
    return "G-Diffuser";
#endif
}

void copy_text(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

void copy_notes(char* dst, size_t cap, const char* src, size_t srcLen) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    if (srcLen >= cap) {
        srcLen = cap - 1;
    }
    while (srcLen > 0 && (static_cast<unsigned char>(src[srcLen]) & 0xC0) == 0x80) {
        srcLen--;
    }
    std::memcpy(dst, src, srcLen);
    dst[srcLen] = '\0';
}

bool copy_bounded(char* dst, size_t cap, const char* src, size_t srcLen) {
    if (dst == nullptr || cap == 0) {
        return false;
    }
    if (src == nullptr || srcLen >= cap) {
        dst[0] = '\0';
        return false;
    }
    std::memcpy(dst, src, srcLen);
    dst[srcLen] = '\0';
    return true;
}

bool starts_with(const char* text, const char* prefix) {
    size_t n = std::strlen(prefix);
    return std::strncmp(text, prefix, n) == 0;
}

bool ends_with(const char* text, const char* suffix) {
    size_t n = std::strlen(text);
    size_t s = std::strlen(suffix);
    if (n < s) {
        return false;
    }
    return std::memcmp(text + n - s, suffix, s) == 0;
}

bool is_https_url(const char* url) {
    if (url == nullptr) {
        return false;
    }
    size_t n = std::strlen(url);
    if (n < 12 || n >= kMaxRedirectUrl) {
        return false;
    }
    const char* prefix = "https://";
    for (int i = 0; prefix[i] != '\0'; i++) {
        if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) {
            return false;
        }
    }
    if (url[8] == '\0' || url[8] == '/' || url[8] == '\\') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c <= 0x20 || c == 0x7F || c == '\\' || c == '@') {
            return false;
        }
    }
    return true;
}

bool safe_asset_name(const char* name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t n = std::strlen(name);
    if (n > kMaxAssetName) {
        return false;
    }
    if (std::strstr(name, "..") != nullptr) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c >= 0x7F) {
            return false;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            return false;
        }
    }
    return true;
}

bool path_text_ok(const std::string& path) {
    if (path.empty() || path == "." || path.size() > 32000) {
        return false;
    }
    for (unsigned char c : path) {
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

std::string join_path(const std::string& dir, const char* leaf) {
    if (!path_text_ok(dir) || leaf == nullptr || leaf[0] == '\0') {
        return {};
    }
    for (const char* p = leaf; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            return {};
        }
    }
    char sep = '/';
#ifdef _WIN32
    sep = '\\';
#endif
    if (dir.back() == '/' || dir.back() == '\\') {
        return dir + leaf;
    }
    std::string out = dir;
    out.push_back(sep);
    out += leaf;
    return out;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& text) {
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
#endif

struct FileWriter {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool open(const std::string& path) {
        close();
        std::wstring wide = utf8_to_wide(path);
        if (wide.empty()) {
            return false;
        }
        handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle != INVALID_HANDLE_VALUE;
    }
    bool write(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (len > 0) {
            DWORD chunk = len > (1u << 20) ? (1u << 20) : (DWORD)len;
            DWORD wrote = 0;
            if (!WriteFile(handle, p, chunk, &wrote, nullptr) || wrote == 0) {
                return false;
            }
            p += wrote;
            len -= wrote;
        }
        return true;
    }
    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
#else
    int fd = -1;
    bool open(const std::string& path) {
        close();
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        return fd >= 0;
    }
    bool write(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (len > 0) {
            ssize_t wrote = ::write(fd, p, len);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (wrote == 0) {
                return false;
            }
            p += wrote;
            len -= (size_t)wrote;
        }
        return true;
    }
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
#endif
    ~FileWriter() {
        close();
    }
    FileWriter() = default;
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
};

bool remove_path(const std::string& path) {
    if (!path_text_ok(path)) {
        return false;
    }
#ifdef _WIN32
    std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wide.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    return DeleteFileW(wide.c_str()) != 0;
#else
    if (unlink(path.c_str()) == 0 || errno == ENOENT) {
        return true;
    }
    return false;
#endif
}

bool file_exists(const std::string& path) {
    if (!path_text_ok(path)) {
        return false;
    }
#ifdef _WIN32
    std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool ensure_dir(const std::string& path) {
    if (!path_text_ok(path)) {
        return false;
    }
#ifdef _WIN32
    std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wide.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return CreateDirectoryW(wide.c_str(), nullptr) != 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
#endif
}

bool sha256_file(const std::string& path, unsigned char out[32]) {
    Sha256 ctx;
    sha256_init(&ctx);
    uint8_t buf[65536];
    uint64_t total = 0;
#ifdef _WIN32
    std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return false;
    }
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buf, sizeof(buf), &read, nullptr)) {
            CloseHandle(handle);
            return false;
        }
        if (read == 0) {
            break;
        }
        if (total > kMaxDownloadBytes - read) {
            CloseHandle(handle);
            return false;
        }
        total += read;
        sha256_update(&ctx, buf, read);
    }
    CloseHandle(handle);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    for (;;) {
        ssize_t read = ::read(fd, buf, sizeof(buf));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        if (read == 0) {
            break;
        }
        if (total > kMaxDownloadBytes - (uint64_t)read) {
            ::close(fd);
            return false;
        }
        total += (uint64_t)read;
        sha256_update(&ctx, buf, (size_t)read);
    }
    ::close(fd);
#endif
    if (ctx.overflow) {
        return false;
    }
    sha256_final(&ctx, out);
    return true;
}

bool copy_file(const std::string& src, const std::string& dst) {
#ifdef _WIN32
    std::wstring wideSrc = utf8_to_wide(src);
    std::wstring wideDst = utf8_to_wide(dst);
    if (wideSrc.empty() || wideDst.empty()) {
        return false;
    }
    return CopyFileW(wideSrc.c_str(), wideDst.c_str(), FALSE) != 0;
#else
    int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) {
        return false;
    }
    int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        ::close(in);
        return false;
    }
    uint8_t buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t read = ::read(in, buf, sizeof(buf));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        size_t left = (size_t)read;
        uint8_t* p = buf;
        while (left > 0) {
            ssize_t wrote = ::write(out, p, left);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }
            p += wrote;
            left -= (size_t)wrote;
        }
        if (!ok) {
            break;
        }
    }
    if (::close(out) != 0) {
        ok = false;
    }
    ::close(in);
    if (ok && chmod(dst.c_str(), 0755) != 0) {
        ok = false;
    }
    return ok;
#endif
}

struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    bool started = false;
    bool stop = false;
    bool cmdCleanup = false;
    bool cmdCheck = false;
    int checkManual = 0;
    bool cmdDownload = false;
    bool cmdApply = false;
    bool launchedHelper = false;
    GdxUpdateStatus status{};
    char assetName[kMaxAssetName + 1]{};
    char assetUrl[kMaxUrl]{};
    char sumsUrl[kMaxUrl]{};

    Worker() = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stop = true;
        }
        cv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

Worker& worker() {
    static Worker instance;
    return instance;
}

void note_progress(uint64_t done, uint64_t total, bool haveTotal) {
    Worker& w = worker();
    std::lock_guard<std::mutex> lock(w.mu);
    w.status.bytesDone = done;
    if (haveTotal) {
        w.status.bytesTotal = total;
    }
}

void set_error(const char* message) {
    Worker& w = worker();
    std::lock_guard<std::mutex> lock(w.mu);
    w.status.state = GDX_UPDATE_ERROR;
    copy_text(w.status.errorMessage, sizeof(w.status.errorMessage), message);
}

void publish(GdxUpdateState state, const char* version, const char* notes, size_t notesLen, const char* error,
             const char* assetName, const char* assetUrl, const char* sumsUrl) {
    Worker& w = worker();
    std::lock_guard<std::mutex> lock(w.mu);
    w.status.state = state;
    copy_text(w.status.latestVersion, sizeof(w.status.latestVersion), version);
    copy_notes(w.status.releaseNotes, sizeof(w.status.releaseNotes), notes, notesLen);
    if (error != nullptr && error[0] != '\0') {
        copy_text(w.status.errorMessage, sizeof(w.status.errorMessage), error);
    } else {
        w.status.errorMessage[0] = '\0';
    }
    if (state == GDX_UPDATE_CHECKING || state == GDX_UPDATE_AVAILABLE || state == GDX_UPDATE_UP_TO_DATE ||
        state == GDX_UPDATE_DISABLED) {
        w.status.bytesDone = 0;
        w.status.bytesTotal = 0;
    }
    if (state == GDX_UPDATE_AVAILABLE && assetName != nullptr && assetUrl != nullptr && sumsUrl != nullptr) {
        copy_text(w.assetName, sizeof(w.assetName), assetName);
        copy_text(w.assetUrl, sizeof(w.assetUrl), assetUrl);
        copy_text(w.sumsUrl, sizeof(w.sumsUrl), sumsUrl);
    } else if (state != GDX_UPDATE_DOWNLOADING && state != GDX_UPDATE_VERIFYING && state != GDX_UPDATE_READY) {
        w.assetName[0] = '\0';
        w.assetUrl[0] = '\0';
        w.sumsUrl[0] = '\0';
    }
}

struct Transfer {
    FileWriter* writer = nullptr;
    const std::string* filePath = nullptr;
    std::string* memory = nullptr;
    uint64_t maxBytes = 0;
    uint64_t written = 0;
    bool tooLarge = false;
    bool ioError = false;
    bool acceptBody = false;
    bool opened = false;
    bool trackProgress = false;
    char location[kMaxRedirectUrl]{};
    bool haveLocation = false;
    bool badLocation = false;
};

bool accept_bytes(Transfer* t, const void* data, size_t nbytes) {
    if (nbytes == 0) {
        return true;
    }
    if (t->written > t->maxBytes || nbytes > t->maxBytes - t->written) {
        t->tooLarge = true;
        return false;
    }
    if (t->filePath != nullptr) {
        if (!t->opened) {
            t->opened = t->writer->open(*t->filePath);
            if (!t->opened) {
                t->ioError = true;
                return false;
            }
        }
        if (!t->writer->write(data, nbytes)) {
            t->ioError = true;
            return false;
        }
    } else if (t->memory != nullptr) {
        t->memory->append(static_cast<const char*>(data), nbytes);
    } else {
        return false;
    }
    t->written += nbytes;
    if (t->trackProgress) {
        note_progress(t->written, 0, false);
    }
    return true;
}

#ifndef _WIN32
void note_location(Transfer* t, const char* value, size_t len) {
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
        len--;
    }
    size_t start = 0;
    while (start < len && (value[start] == ' ' || value[start] == '\t')) {
        start++;
    }
    if (start >= len || len - start >= sizeof(t->location)) {
        t->badLocation = true;
        t->location[0] = '\0';
        t->haveLocation = false;
        return;
    }
    std::memcpy(t->location, value + start, len - start);
    t->location[len - start] = '\0';
    t->haveLocation = true;
    t->badLocation = false;
}

bool header_name_is(const char* line, size_t len, const char* name) {
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
#endif

#ifdef _WIN32
struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    void reset(HINTERNET next = nullptr) {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
        handle = next;
    }
};

bool crack_https(const std::wstring& url, std::wstring* host, INTERNET_PORT* port, std::wstring* object) {
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
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.dwHostNameLength == 0 || parts.dwHostNameLength >= 255) {
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

bool request_is_https(HINTERNET request) {
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

bool parse_content_length(HINTERNET request, uint64_t* out, bool* present) {
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

struct HopResult {
    int status = 0;
    bool transportOk = false;
    bool tooLarge = false;
    bool ioError = false;
    bool rejectedUrl = false;
    std::string detail;
    char location[kMaxRedirectUrl]{};
    bool haveLocation = false;
};

HopResult winhttp_hop(HINTERNET session, const char* url, uint64_t maxBytes, Transfer* transfer) {
    HopResult hop;
    if (!is_https_url(url)) {
        hop.rejectedUrl = true;
        hop.detail = "non-https url";
        return hop;
    }
    std::wstring wideUrl = utf8_to_wide(url);
    std::wstring host;
    std::wstring object;
    INTERNET_PORT port = 0;
    if (wideUrl.empty() || !crack_https(wideUrl, &host, &port, &object)) {
        hop.rejectedUrl = true;
        hop.detail = "url rejected";
        return hop;
    }
    InternetHandle connection;
    connection.reset(WinHttpConnect(session, host.c_str(), port, 0));
    if (connection.handle == nullptr) {
        hop.detail = "connect failed";
        return hop;
    }
    InternetHandle request;
    request.reset(WinHttpOpenRequest(connection.handle, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.handle == nullptr) {
        hop.detail = "open request failed";
        return hop;
    }
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpAddRequestHeaders(request.handle, L"Accept: application/vnd.github+json", (DWORD)-1,
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        hop.detail = "header failed";
        return hop;
    }
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        hop.detail = "request failed";
        return hop;
    }
    if (!request_is_https(request.handle)) {
        hop.rejectedUrl = true;
        hop.detail = "final url was not https";
        return hop;
    }
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) {
        hop.detail = "status missing";
        return hop;
    }
    hop.status = (int)status;
    hop.transportOk = true;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        DWORD locBytes = 0;
        WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                            &locBytes, WINHTTP_NO_HEADER_INDEX);
        if (locBytes == 0 || locBytes > 16384) {
            hop.detail = "redirect missing location";
            return hop;
        }
        std::wstring location(locBytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                                 &locBytes, WINHTTP_NO_HEADER_INDEX)) {
            hop.detail = "redirect location failed";
            return hop;
        }
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, location.c_str(), -1, hop.location,
                                    (int)sizeof(hop.location), nullptr, nullptr);
        if (n <= 1) {
            hop.detail = "redirect location rejected";
            return hop;
        }
        hop.haveLocation = true;
        return hop;
    }
    if (status != 200) {
        hop.detail = "http status";
        return hop;
    }
    uint64_t contentLength = 0;
    bool haveLength = false;
    if (!parse_content_length(request.handle, &contentLength, &haveLength)) {
        hop.detail = "bad content length";
        return hop;
    }
    if (haveLength && contentLength > maxBytes) {
        hop.tooLarge = true;
        hop.detail = "content length over cap";
        return hop;
    }
    if (haveLength && transfer->trackProgress) {
        note_progress(0, contentLength, true);
    }
    if (transfer->filePath != nullptr && !transfer->opened) {
        transfer->opened = transfer->writer->open(*transfer->filePath);
        if (!transfer->opened) {
            hop.ioError = true;
            hop.detail = "could not create file";
            return hop;
        }
    }
    uint8_t buf[65536];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buf, sizeof(buf), &read)) {
            hop.transportOk = false;
            hop.detail = "read failed";
            return hop;
        }
        if (read == 0) {
            break;
        }
        if (!accept_bytes(transfer, buf, read)) {
            hop.tooLarge = transfer->tooLarge;
            hop.ioError = transfer->ioError;
            hop.detail = transfer->tooLarge ? "download cap" : "write failed";
            return hop;
        }
    }
    if (haveLength && transfer->written != contentLength) {
        hop.detail = "short body";
        hop.transportOk = false;
    }
    return hop;
}

struct HttpResult {
    int status = 0;
    bool ok = false;
    bool tooLarge = false;
    bool rejectedUrl = false;
    bool libcurlMissing = false;
    std::string body;
    std::string detail;
};

HttpResult http_get(const char* url, int timeoutSec, uint64_t maxBytes, const std::string* filePath, bool progress) {
    HttpResult result;
    char userAgentUtf8[160];
    std::snprintf(userAgentUtf8, sizeof(userAgentUtf8), "G-Diffuser/%s", GDX_VERSION_STRING);
    std::wstring userAgent = utf8_to_wide(userAgentUtf8);
    if (userAgent.empty()) {
        result.detail = "user agent";
        return result;
    }
    InternetHandle session;
    session.reset(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.handle == nullptr) {
        result.detail = "winhttp open failed";
        return result;
    }
    int ms = timeoutSec > 0 ? timeoutSec * 1000 : 15000;
    WinHttpSetTimeouts(session.handle, ms, ms, ms, ms);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    std::string current = url;
    FileWriter writer;
    for (int hop = 0; hop < kMaxRedirects; hop++) {
        Transfer transfer;
        transfer.writer = &writer;
        transfer.filePath = filePath;
        transfer.memory = filePath == nullptr ? &result.body : nullptr;
        transfer.maxBytes = maxBytes;
        transfer.trackProgress = progress;
        HopResult once = winhttp_hop(session.handle, current.c_str(), maxBytes, &transfer);
        result.status = once.status;
        result.tooLarge = once.tooLarge;
        result.rejectedUrl = once.rejectedUrl;
        result.detail = once.detail;
        if (once.haveLocation) {
            if (!is_https_url(once.location)) {
                result.rejectedUrl = true;
                result.detail = "redirect was not https";
                result.ok = false;
                return result;
            }
            current = once.location;
            continue;
        }
        result.ok = once.transportOk && once.status == 200 && !once.tooLarge && !once.ioError;
        return result;
    }
    result.detail = "too many redirects";
    return result;
}
#else
constexpr int kCurlWriteData = 10001;
constexpr int kCurlUrl = 10002;
constexpr int kCurlErrorBuffer = 10010;
constexpr int kCurlWriteFunction = 20011;
constexpr int kCurlTimeout = 13;
constexpr int kCurlUserAgent = 10018;
constexpr int kCurlHttpHeader = 10023;
constexpr int kCurlFollowLocation = 52;
constexpr int kCurlSslVerifyPeer = 64;
constexpr int kCurlConnectTimeout = 78;
constexpr int kCurlHeaderFunction = 20079;
constexpr int kCurlSslVerifyHost = 81;
constexpr int kCurlNoSignal = 99;
constexpr int kCurlHeaderData = 10029;
constexpr int kCurlInfoResponseCode = 0x200002;

// curl_slist is two pointers. curl_slist_append is outside the allowed symbol list.
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
bool load_symbol(void* lib, const char* name, T* out) {
    *out = reinterpret_cast<T>(dlsym(lib, name));
    return *out != nullptr;
}

CurlApi& curl_api() {
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
    if (!load_symbol(api.lib, "curl_easy_init", &api.easy_init) ||
        !load_symbol(api.lib, "curl_easy_setopt", &api.easy_setopt) ||
        !load_symbol(api.lib, "curl_easy_perform", &api.easy_perform) ||
        !load_symbol(api.lib, "curl_easy_getinfo", &api.easy_getinfo) ||
        !load_symbol(api.lib, "curl_easy_cleanup", &api.easy_cleanup) ||
        !load_symbol(api.lib, "curl_easy_strerror", &api.easy_strerror) ||
        !load_symbol(api.lib, "curl_global_init", &api.global_init) || api.global_init(3) != 0) {
        api.failed = true;
        return api;
    }
    api.ready = true;
    return api;
}

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t nbytes = size * nmemb;
    Transfer* transfer = static_cast<Transfer*>(userdata);
    if (!transfer->acceptBody) {
        return nbytes;
    }
    if (!accept_bytes(transfer, ptr, nbytes)) {
        return 0;
    }
    return nbytes;
}

size_t curl_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t nbytes = size * nmemb;
    Transfer* transfer = static_cast<Transfer*>(userdata);
    size_t len = nbytes;
    while (len > 0 && (ptr[len - 1] == '\n' || ptr[len - 1] == '\r')) {
        len--;
    }
    if (len >= 5 && header_name_is(ptr, len, "http/")) {
        const char* p = ptr;
        while (*p != '\0' && *p != ' ' && (size_t)(p - ptr) < len) {
            p++;
        }
        while ((size_t)(p - ptr) < len && *p == ' ') {
            p++;
        }
        int code = 0;
        int digits = 0;
        while ((size_t)(p - ptr) < len && std::isdigit(static_cast<unsigned char>(*p)) && digits < 3) {
            code = code * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 3 && code >= 200) {
            transfer->acceptBody = code == 200;
            if (code == 200 && transfer->filePath != nullptr && !transfer->opened) {
                transfer->opened = transfer->writer->open(*transfer->filePath);
                if (!transfer->opened) {
                    transfer->ioError = true;
                }
            }
        }
    } else if (header_name_is(ptr, len, "location:")) {
        note_location(transfer, ptr + 9, len - 9);
    }
    return nbytes;
}

struct HttpResult {
    int status = 0;
    bool ok = false;
    bool tooLarge = false;
    bool rejectedUrl = false;
    bool libcurlMissing = false;
    std::string body;
    std::string detail;
};

HttpResult http_get(const char* url, int timeoutSec, uint64_t maxBytes, const std::string* filePath, bool progress) {
    HttpResult result;
    CurlApi& api = curl_api();
    if (!api.ready) {
        result.libcurlMissing = true;
        result.detail = "libcurl unavailable";
        return result;
    }
    std::string current = url;
    char userAgent[160];
    std::snprintf(userAgent, sizeof(userAgent), "G-Diffuser/%s", GDX_VERSION_STRING);
    char acceptHeader[] = "Accept: application/vnd.github+json";
    FileWriter writer;
    for (int hop = 0; hop < kMaxRedirects; hop++) {
        if (!is_https_url(current.c_str())) {
            result.rejectedUrl = true;
            result.detail = "non-https url";
            return result;
        }
        void* easy = api.easy_init();
        if (easy == nullptr) {
            result.detail = "curl init failed";
            return result;
        }
        Transfer transfer;
        transfer.writer = &writer;
        transfer.filePath = filePath;
        transfer.memory = filePath == nullptr ? &result.body : nullptr;
        transfer.maxBytes = maxBytes;
        transfer.trackProgress = progress;
        char errbuf[256];
        errbuf[0] = '\0';
        CurlSlist headers;
        headers.data = acceptHeader;
        headers.next = nullptr;
        bool optionsOk = api.easy_setopt(easy, kCurlUrl, current.c_str()) == 0 &&
                         api.easy_setopt(easy, kCurlUserAgent, userAgent) == 0 &&
                         api.easy_setopt(easy, kCurlHttpHeader, &headers) == 0 &&
                         api.easy_setopt(easy, kCurlWriteFunction, curl_write) == 0 &&
                         api.easy_setopt(easy, kCurlWriteData, &transfer) == 0 &&
                         api.easy_setopt(easy, kCurlHeaderFunction, curl_header) == 0 &&
                         api.easy_setopt(easy, kCurlHeaderData, &transfer) == 0 &&
                         api.easy_setopt(easy, kCurlFollowLocation, (long)0) == 0 &&
                         api.easy_setopt(easy, kCurlSslVerifyPeer, (long)1) == 0 &&
                         api.easy_setopt(easy, kCurlSslVerifyHost, (long)2) == 0 &&
                         api.easy_setopt(easy, kCurlNoSignal, (long)1) == 0 &&
                         api.easy_setopt(easy, kCurlConnectTimeout, (long)kCheckTimeoutSec) == 0 &&
                         api.easy_setopt(easy, kCurlTimeout, (long)timeoutSec) == 0 &&
                         api.easy_setopt(easy, kCurlErrorBuffer, errbuf) == 0;
        int rc = optionsOk ? api.easy_perform(easy) : 1;
        long status = 0;
        if (rc == 0) {
            api.easy_getinfo(easy, kCurlInfoResponseCode, &status);
        }
        result.status = (int)status;
        result.tooLarge = transfer.tooLarge;
        result.detail = errbuf[0] != '\0' ? errbuf : (rc == 0 ? "" : "curl perform failed");
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (rc == 0 && redirect && transfer.haveLocation && !transfer.badLocation) {
            api.easy_cleanup(easy);
            if (!is_https_url(transfer.location)) {
                result.rejectedUrl = true;
                result.detail = "redirect was not https";
                return result;
            }
            current = transfer.location;
            result.body.clear();
            continue;
        }
        api.easy_cleanup(easy);
        result.ok = optionsOk && rc == 0 && status == 200 && !transfer.tooLarge && !transfer.ioError && !transfer.badLocation;
        return result;
    }
    result.detail = "too many redirects";
    return result;
}
#endif

std::string install_dir() {
#ifndef GDX_UPDATE_TESTS_STANDALONE
    std::string path = Ship::Context::GetAppBundlePath();
    // "." is the failure return, which is the working directory. Never update that.
    if (!path_text_ok(path) || path == ".") {
        return {};
    }
    return path;
#else
    return {};
#endif
}

bool version_from_asset(const char* name, const char* platform, char* out, size_t cap) {
    if (!gdx_update_asset_matches(name, platform) || out == nullptr || cap == 0) {
        return false;
    }
    const char* prefix = "G-Diffuser-v";
    size_t prefixLen = std::strlen(prefix);
    const char* ext = std::strcmp(platform, "windows-x64") == 0 ? ".zip" : ".tar.gz";
    char suffix[64];
    if (std::snprintf(suffix, sizeof(suffix), "-%s%s", platform, ext) <= 0) {
        return false;
    }
    size_t nameLen = std::strlen(name);
    size_t suffixLen = std::strlen(suffix);
    if (nameLen < prefixLen + suffixLen + 1) {
        return false;
    }
    size_t verLen = nameLen - prefixLen - suffixLen;
    if (verLen >= cap) {
        return false;
    }
    std::memcpy(out, name + prefixLen, verLen);
    out[verLen] = '\0';
    return true;
}

struct ReleaseInfo {
    char tagVersion[128]{};
    char body[8192]{};
    size_t bodyLen = 0;
    char assetName[kMaxAssetName + 1]{};
    char assetUrl[kMaxUrl]{};
    char sumsUrl[kMaxUrl]{};
    bool assetOk = false;
    bool sumsOk = false;
};

bool parse_release(const std::string& json, ReleaseInfo* info) {
    rapidjson::Document doc;
    // Iterative: a deeply nested body is a parse error, not a stack overflow on the game thread.
    doc.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        return false;
    }
    auto tag = doc.FindMember("tag_name");
    if (tag == doc.MemberEnd() || !tag->value.IsString()) {
        return false;
    }
    const char* tagText = tag->value.GetString();
    size_t tagLen = tag->value.GetStringLength();
    if (tagLen > 0 && (tagText[0] == 'v' || tagText[0] == 'V')) {
        tagText++;
        tagLen--;
    }
    if (!copy_bounded(info->tagVersion, sizeof(info->tagVersion), tagText, tagLen)) {
        return false;
    }
    auto body = doc.FindMember("body");
    if (body != doc.MemberEnd() && body->value.IsString()) {
        copy_notes(info->body, sizeof(info->body), body->value.GetString(), body->value.GetStringLength());
        info->bodyLen = std::strlen(info->body);
    }
    auto assets = doc.FindMember("assets");
    if (assets == doc.MemberEnd() || !assets->value.IsArray()) {
        return false;
    }
    const char* platform = update_platform();
    rapidjson::SizeType count = assets->value.Size();
    if (count > 64) {
        count = 64;
    }
    for (rapidjson::SizeType i = 0; i < count; i++) {
        const rapidjson::Value& asset = assets->value[i];
        if (!asset.IsObject()) {
            continue;
        }
        auto name = asset.FindMember("name");
        auto url = asset.FindMember("browser_download_url");
        if (name == asset.MemberEnd() || url == asset.MemberEnd() || !name->value.IsString() || !url->value.IsString()) {
            continue;
        }
        const char* nameText = name->value.GetString();
        size_t nameLen = name->value.GetStringLength();
        const char* urlText = url->value.GetString();
        size_t urlLen = url->value.GetStringLength();
        if (std::strlen(nameText) != nameLen || std::strlen(urlText) != urlLen) {
            continue;
        }
        if (nameLen == 10 && std::memcmp(nameText, "SHA256SUMS", 10) == 0) {
            if (!info->sumsOk && urlLen < sizeof(info->sumsUrl) && is_https_url(urlText)) {
                std::memcpy(info->sumsUrl, urlText, urlLen);
                info->sumsUrl[urlLen] = '\0';
                info->sumsOk = true;
            }
            continue;
        }
        if (info->assetOk || platform[0] == '\0' || !gdx_update_asset_matches(nameText, platform) ||
            !safe_asset_name(nameText) || !is_https_url(urlText) || urlLen >= sizeof(info->assetUrl) ||
            nameLen >= sizeof(info->assetName)) {
            continue;
        }
        std::memcpy(info->assetName, nameText, nameLen);
        info->assetName[nameLen] = '\0';
        std::memcpy(info->assetUrl, urlText, urlLen);
        info->assetUrl[urlLen] = '\0';
        info->assetOk = true;
    }
    return true;
}

void run_check(int manual) {
    if (!channel_offers()) {
        publish(GDX_UPDATE_DISABLED, "", "", 0, nullptr, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] check disabled for channel %s\n", GDX_BUILD_CHANNEL);
        return;
    }
    gdx_port_logf("[update] check channel %s version %s commit %s (%s)\n", GDX_BUILD_CHANNEL, GDX_VERSION_STRING,
                  GDX_BUILD_COMMIT, manual ? "manual" : "automatic");
#ifdef GDX_UPDATE_TEST_API_URL
    // Test builds only (port/CMakeLists.txt): every https and checksum rule below still applies.
    const char* url = GDX_UPDATE_TEST_API_URL;
#else
    const char* url = channel_is("nightly") ? "https://api.github.com/repos/Zorkats/G-Diffuser/releases/tags/nightly"
                                            : "https://api.github.com/repos/Zorkats/G-Diffuser/releases/latest";
#endif
    HttpResult response = http_get(url, kCheckTimeoutSec, kMaxCheckBytes, nullptr, false);
    if (response.libcurlMissing) {
        publish(GDX_UPDATE_ERROR, "", "", 0, kErrLibcurl, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] %s\n", response.detail.c_str());
        return;
    }
    if (!response.ok) {
        publish(GDX_UPDATE_ERROR, "", "", 0, kErrCheck, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] check failed (%s, status %d)\n", response.detail.c_str(), response.status);
        return;
    }
    ReleaseInfo info;
    if (!parse_release(response.body, &info)) {
        publish(GDX_UPDATE_ERROR, "", "", 0, kErrCheck, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] release JSON rejected\n");
        return;
    }
    if (!info.assetOk || !info.sumsOk) {
        publish(GDX_UPDATE_UP_TO_DATE, info.tagVersion, info.body, info.bodyLen, nullptr, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] no verifiable %s asset\n", update_platform());
        return;
    }
    if (channel_is("nightly")) {
        char remoteDate[9];
        char remoteHash[41];
        char localDate[9];
        char localHash[41];
        char display[128];
        if (gdx_update_parse_nightly(info.assetName, remoteDate, remoteHash) != 0 ||
            gdx_update_parse_nightly(GDX_VERSION_STRING, localDate, localHash) != 0 ||
            !version_from_asset(info.assetName, update_platform(), display, sizeof(display))) {
            publish(GDX_UPDATE_UP_TO_DATE, info.tagVersion, info.body, info.bodyLen, nullptr, nullptr, nullptr, nullptr);
            gdx_port_logf("[update] nightly version did not parse\n");
            return;
        }
        if (gdx_update_nightly_offer(remoteDate, remoteHash, localDate, localHash) == 1) {
            publish(GDX_UPDATE_AVAILABLE, display, info.body, info.bodyLen, nullptr, info.assetName, info.assetUrl,
                    info.sumsUrl);
            gdx_port_logf("[update] nightly %s available\n", display);
            return;
        }
        publish(GDX_UPDATE_UP_TO_DATE, display, info.body, info.bodyLen, nullptr, nullptr, nullptr, nullptr);
        gdx_port_logf("[update] nightly %s is not newer\n", display);
        return;
    }
    {
        // The asset must carry the tag's version, so a stale asset on the release cannot install an older build.
        char assetVersion[128];
        if (!version_from_asset(info.assetName, update_platform(), assetVersion, sizeof(assetVersion)) ||
            std::strcmp(assetVersion, info.tagVersion) != 0) {
            publish(GDX_UPDATE_UP_TO_DATE, info.tagVersion, info.body, info.bodyLen, nullptr, nullptr, nullptr, nullptr);
            gdx_port_logf("[update] stable asset %s does not match tag %s\n", info.assetName, info.tagVersion);
            return;
        }
    }
    if (gdx_update_compare_stable(info.tagVersion, GDX_VERSION_STRING) > 0) {
        publish(GDX_UPDATE_AVAILABLE, info.tagVersion, info.body, info.bodyLen, nullptr, info.assetName, info.assetUrl,
                info.sumsUrl);
        gdx_port_logf("[update] stable %s available\n", info.tagVersion);
        return;
    }
    publish(GDX_UPDATE_UP_TO_DATE, info.tagVersion, info.body, info.bodyLen, nullptr, nullptr, nullptr, nullptr);
    gdx_port_logf("[update] stable %s is not newer\n", info.tagVersion);
}

void run_download() {
    char assetName[kMaxAssetName + 1];
    char assetUrl[kMaxUrl];
    char sumsUrl[kMaxUrl];
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        if (w.status.state != GDX_UPDATE_DOWNLOADING) {
            return;
        }
        copy_text(assetName, sizeof(assetName), w.assetName);
        copy_text(assetUrl, sizeof(assetUrl), w.assetUrl);
        copy_text(sumsUrl, sizeof(sumsUrl), w.sumsUrl);
        w.status.bytesDone = 0;
        w.status.bytesTotal = 0;
    }
    std::string install = install_dir();
    std::string updateDir = join_path(install, ".update");
    std::string partPath = join_path(updateDir, (std::string(assetName) + ".part").c_str());
    std::string finalPath = join_path(updateDir, assetName);
    auto fail = [&](const char* message) {
        if (!partPath.empty()) {
            remove_path(partPath);
        }
        set_error(message);
    };
    if (install.empty() || updateDir.empty() || partPath.empty() || finalPath.empty() || !safe_asset_name(assetName) ||
        !is_https_url(assetUrl) || !is_https_url(sumsUrl) || !ensure_dir(updateDir)) {
        gdx_port_logf("[update] download path rejected\n");
        fail(install.empty() ? kErrPath : kErrDownload);
        return;
    }
    HttpResult asset = http_get(assetUrl, kDownloadTimeoutSec, kMaxDownloadBytes, &partPath, true);
    if (asset.libcurlMissing) {
        fail(kErrLibcurl);
        return;
    }
    if (!asset.ok || asset.rejectedUrl || asset.tooLarge) {
        gdx_port_logf("[update] asset download failed (%s)\n", asset.detail.c_str());
        fail(kErrDownload);
        return;
    }
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        w.status.state = GDX_UPDATE_VERIFYING;
    }
    HttpResult sums = http_get(sumsUrl, kCheckTimeoutSec, kMaxSumsBytes, nullptr, false);
    if (!sums.ok || sums.rejectedUrl || sums.tooLarge) {
        gdx_port_logf("[update] SHA256SUMS download failed (%s)\n", sums.detail.c_str());
        fail(kErrDownload);
        return;
    }
    char expected[65];
    unsigned char digest[32];
    char got[65];
    if (gdx_update_find_sha256(sums.body.c_str(), assetName, expected) != 0 || !sha256_file(partPath, digest)) {
        gdx_port_logf("[update] verification data missing\n");
        fail(kErrVerify);
        return;
    }
    to_hex(digest, got);
    if (std::strcmp(got, expected) != 0) {
        gdx_port_logf("[update] SHA-256 mismatch\n");
        fail(kErrVerify);
        return;
    }
#ifdef _WIN32
    std::wstring widePart = utf8_to_wide(partPath);
    std::wstring wideFinal = utf8_to_wide(finalPath);
    if (widePart.empty() || wideFinal.empty() ||
        !MoveFileExW(widePart.c_str(), wideFinal.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fail(kErrDownload);
        return;
    }
#else
    if (rename(partPath.c_str(), finalPath.c_str()) != 0) {
        fail(kErrDownload);
        return;
    }
#endif
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        w.status.state = GDX_UPDATE_READY;
        w.status.errorMessage[0] = '\0';
    }
    gdx_port_logf("[update] %s verified\n", assetName);
}

#ifdef _WIN32
void append_quoted(std::wstring* cmd, const std::wstring& arg) {
    cmd->push_back(L'"');
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        if (c == L'"') {
            cmd->append(slashes * 2 + 1, L'\\');
            cmd->push_back(L'"');
            slashes = 0;
            continue;
        }
        if (slashes > 0) {
            cmd->append(slashes, L'\\');
            slashes = 0;
        }
        cmd->push_back(c);
    }
    if (slashes > 0) {
        cmd->append(slashes * 2, L'\\');
    }
    cmd->push_back(L'"');
}
#endif

void run_apply() {
    char assetName[kMaxAssetName + 1];
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        if (w.status.state != GDX_UPDATE_READY) {
            return;
        }
        copy_text(assetName, sizeof(assetName), w.assetName);
    }
    std::string install = install_dir();
    std::string updateDir = join_path(install, ".update");
    std::string helperSrc = join_path(install, helper_file_name());
    std::string helperDst = join_path(updateDir, helper_run_name());
    std::string archive = join_path(updateDir, assetName);
    std::string relaunch = join_path(install, game_file_name());
    if (install.empty() || helperSrc.empty() || helperDst.empty() || archive.empty() || relaunch.empty()) {
        set_error(kErrPath);
        return;
    }
    if (!file_exists(helperSrc)) {
        set_error(kErrHelper);
        gdx_port_logf("[update] helper missing\n");
        return;
    }
    if (!file_exists(archive) || !ensure_dir(updateDir) || !copy_file(helperSrc, helperDst)) {
        set_error(kErrLaunch);
        gdx_port_logf("[update] could not stage helper\n");
        return;
    }
    char pidText[32];
#ifdef _WIN32
    std::snprintf(pidText, sizeof(pidText), "%lu", (unsigned long)GetCurrentProcessId());
    std::wstring wideHelper = utf8_to_wide(helperDst);
    std::wstring wideInstall = utf8_to_wide(install);
    std::wstring wideArchive = utf8_to_wide(archive);
    std::wstring wideRelaunch = utf8_to_wide(relaunch);
    std::wstring widePid = utf8_to_wide(pidText);
    if (wideHelper.empty() || wideInstall.empty() || wideArchive.empty() || wideRelaunch.empty() || widePid.empty()) {
        set_error(kErrLaunch);
        return;
    }
    std::wstring cmd;
    append_quoted(&cmd, wideHelper);
    cmd += L" --install ";
    append_quoted(&cmd, wideInstall);
    cmd += L" --archive ";
    append_quoted(&cmd, wideArchive);
    cmd += L" --pid ";
    cmd += widePid;
    cmd += L" --relaunch ";
    append_quoted(&cmd, wideRelaunch);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(wideHelper.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, wideInstall.c_str(), &startup, &process);
    if (!ok) {
        gdx_port_logf("[update] CreateProcessW failed (%lu)\n", (unsigned long)GetLastError());
        set_error(kErrLaunch);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    std::snprintf(pidText, sizeof(pidText), "%ld", (long)getpid());
    if (chmod(helperDst.c_str(), 0755) != 0) {
        set_error(kErrLaunch);
        return;
    }
    char helperBuf[4096];
    char installBuf[4096];
    char archiveBuf[4096];
    char relaunchBuf[4096];
    char pidBuf[32];
    if (std::snprintf(helperBuf, sizeof(helperBuf), "%s", helperDst.c_str()) >= (int)sizeof(helperBuf) ||
        std::snprintf(installBuf, sizeof(installBuf), "%s", install.c_str()) >= (int)sizeof(installBuf) ||
        std::snprintf(archiveBuf, sizeof(archiveBuf), "%s", archive.c_str()) >= (int)sizeof(archiveBuf) ||
        std::snprintf(relaunchBuf, sizeof(relaunchBuf), "%s", relaunch.c_str()) >= (int)sizeof(relaunchBuf) ||
        std::snprintf(pidBuf, sizeof(pidBuf), "%s", pidText) >= (int)sizeof(pidBuf)) {
        set_error(kErrPath);
        return;
    }
    char* argv[] = {helperBuf,   "--install", installBuf, "--archive", archiveBuf, "--pid",
                    pidBuf,      "--relaunch", relaunchBuf, nullptr};
    pid_t child = fork();
    if (child < 0) {
        set_error(kErrLaunch);
        return;
    }
    if (child == 0) {
        // Other threads may hold locks. Only async-signal-safe calls until exec.
        setsid();
        chdir(installBuf);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) {
                close(devnull);
            }
        }
        for (int fd = 3; fd < 1024; fd++) {
            close(fd);
        }
        execv(helperBuf, argv);
        _exit(127);
    }
#endif
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        w.launchedHelper = true;
    }
    gdx_port_logf("[update] helper launched, requesting quit\n");
#ifndef GDX_UPDATE_TESTS_STANDALONE
    gdx_request_quit();
#endif
}

bool name_ends(const char* name, const char* suffix) {
    return ends_with(name, suffix);
}

void cleanup_dir(const std::string& updateDir, bool keepRunHelper) {
#ifdef _WIN32
    std::wstring wideDir = utf8_to_wide(updateDir);
    if (wideDir.empty()) {
        return;
    }
    std::wstring pattern = wideDir + L"\\*";
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        char name[512];
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, name, (int)sizeof(name), nullptr,
                                    nullptr);
        if (n <= 1) {
            continue;
        }
        bool part = name_ends(name, ".part");
        bool runHelper = std::strcmp(name, helper_run_name()) == 0;
        bool archive = starts_with(name, "G-Diffuser-v") && (name_ends(name, ".zip") || name_ends(name, ".tar.gz"));
        if (part || (runHelper && !keepRunHelper) || archive) {
            std::string full = join_path(updateDir, name);
            if (!full.empty()) {
                remove_path(full);
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    DIR* dir = opendir(updateDir.c_str());
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string full = join_path(updateDir, ent->d_name);
        if (full.empty()) {
            continue;
        }
        struct stat st;
        if (lstat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        bool part = name_ends(ent->d_name, ".part");
        bool runHelper = std::strcmp(ent->d_name, helper_run_name()) == 0;
        bool archive = starts_with(ent->d_name, "G-Diffuser-v") &&
                       (name_ends(ent->d_name, ".zip") || name_ends(ent->d_name, ".tar.gz"));
        if (part || (runHelper && !keepRunHelper) || archive) {
            unlink(full.c_str());
        }
    }
    closedir(dir);
#endif
}

void run_cleanup() {
    bool keepRunHelper = false;
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        keepRunHelper = w.launchedHelper;
    }
    std::string install = install_dir();
    std::string updateDir = join_path(install, ".update");
    if (updateDir.empty()) {
        return;
    }
    std::string runHelper = join_path(updateDir, helper_run_name());
    if (!keepRunHelper && !runHelper.empty()) {
        remove_path(runHelper);
    }
    std::string applied = join_path(updateDir, "applied.ok");
    bool appliedOk = !applied.empty() && file_exists(applied);
    if (appliedOk) {
        cleanup_dir(updateDir, keepRunHelper);
        remove_path(applied);
    } else {
#ifdef _WIN32
        std::wstring wideDir = utf8_to_wide(updateDir);
        if (!wideDir.empty()) {
            std::wstring pattern = wideDir + L"\\*.part";
            WIN32_FIND_DATAW data;
            HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        continue;
                    }
                    char name[512];
                    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, name, (int)sizeof(name),
                                            nullptr, nullptr) > 1) {
                        std::string full = join_path(updateDir, name);
                        if (!full.empty()) {
                            remove_path(full);
                        }
                    }
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
        }
#else
        DIR* dir = opendir(updateDir.c_str());
        if (dir != nullptr) {
            while (dirent* ent = readdir(dir)) {
                if (!name_ends(ent->d_name, ".part")) {
                    continue;
                }
                std::string full = join_path(updateDir, ent->d_name);
                if (!full.empty()) {
                    unlink(full.c_str());
                }
            }
            closedir(dir);
        }
#endif
    }
    gdx_port_logf("[update] cleanup finished\n");
}

void ensure_thread(Worker& w) {
    if (w.started) {
        return;
    }
    w.thread = std::thread([] {
        Worker& workerRef = worker();
        std::unique_lock<std::mutex> lock(workerRef.mu);
        while (!workerRef.stop) {
            workerRef.cv.wait(lock, [&workerRef] {
                return workerRef.stop || workerRef.cmdCleanup || workerRef.cmdCheck || workerRef.cmdDownload ||
                       workerRef.cmdApply;
            });
            if (workerRef.stop && !workerRef.cmdCleanup && !workerRef.cmdCheck && !workerRef.cmdDownload &&
                !workerRef.cmdApply) {
                break;
            }
            bool doCleanup = workerRef.cmdCleanup;
            bool doCheck = workerRef.cmdCheck;
            int manual = workerRef.checkManual;
            bool doDownload = workerRef.cmdDownload;
            bool doApply = workerRef.cmdApply;
            workerRef.cmdCleanup = false;
            workerRef.cmdCheck = false;
            workerRef.cmdDownload = false;
            workerRef.cmdApply = false;
            lock.unlock();
            try {
                if (doCleanup) {
                    run_cleanup();
                }
                if (doCheck) {
                    run_check(manual);
                }
                if (doDownload) {
                    run_download();
                }
                if (doApply) {
                    run_apply();
                }
            } catch (...) {
                gdx_port_logf("[update] worker failed\n");
                set_error(kErrCheck);
            }
            lock.lock();
        }
    });
    w.started = true;
}

}  // namespace

extern "C" int gdx_update_nightly_offer(const char* remoteDate, const char* remoteHash, const char* localDate,
                                        const char* localHash) {
    if (!eight_digits(remoteDate) || !eight_digits(localDate) || remoteHash == nullptr || localHash == nullptr) {
        return 0;
    }
    size_t remoteLen = std::strlen(remoteHash);
    size_t localLen = std::strlen(localHash);
    if (remoteLen == 0 || remoteLen != localLen || remoteLen > 40) {
        return 0;
    }
    bool same = true;
    for (size_t i = 0; i < remoteLen; i++) {
        unsigned char a = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(remoteHash[i])));
        unsigned char b = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(localHash[i])));
        if (!std::isxdigit(a) || !std::isxdigit(b)) {
            return 0;
        }
        if (a != b) {
            same = false;
        }
    }
    if (same || std::strcmp(remoteDate, localDate) < 0) {
        return 0;
    }
    return 1;
}

int gdx_update_compare_stable(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return 0;
    }
    if (a[0] == 'v' || a[0] == 'V') {
        a++;
    }
    if (b[0] == 'v' || b[0] == 'V') {
        b++;
    }
    if (a[0] == '\0' || b[0] == '\0') {
        return 0;
    }
    for (;;) {
        if (a[0] == '\0' && b[0] == '\0') {
            return 0;
        }
        const char* aStart = "0";
        const char* bStart = "0";
        size_t aLen = 1;
        size_t bLen = 1;
        if (a[0] != '\0') {
            if (!std::isdigit(static_cast<unsigned char>(a[0]))) {
                return 0;
            }
            aStart = a;
            while (std::isdigit(static_cast<unsigned char>(*a))) {
                a++;
            }
            aLen = (size_t)(a - aStart);
            if (a[0] == '.') {
                a++;
                if (a[0] == '\0') {
                    return 0;
                }
            } else if (a[0] != '\0') {
                return 0;
            }
        }
        if (b[0] != '\0') {
            if (!std::isdigit(static_cast<unsigned char>(b[0]))) {
                return 0;
            }
            bStart = b;
            while (std::isdigit(static_cast<unsigned char>(*b))) {
                b++;
            }
            bLen = (size_t)(b - bStart);
            if (b[0] == '.') {
                b++;
                if (b[0] == '\0') {
                    return 0;
                }
            } else if (b[0] != '\0') {
                return 0;
            }
        }
        while (aLen > 0 && aStart[0] == '0') {
            aStart++;
            aLen--;
        }
        while (bLen > 0 && bStart[0] == '0') {
            bStart++;
            bLen--;
        }
        if (aLen != bLen) {
            return aLen > bLen ? 1 : -1;
        }
        if (aLen > 0) {
            int cmp = std::memcmp(aStart, bStart, aLen);
            if (cmp < 0) {
                return -1;
            }
            if (cmp > 0) {
                return 1;
            }
        }
    }
}

int gdx_update_parse_nightly(const char* version, char* dateOut, char* hashOut) {
    if (dateOut != nullptr) {
        dateOut[0] = '\0';
    }
    if (hashOut != nullptr) {
        hashOut[0] = '\0';
    }
    if (version == nullptr || dateOut == nullptr || hashOut == nullptr) {
        return -1;
    }
    const char* mark = std::strstr(version, "-nightly.");
    if (mark == nullptr) {
        return -1;
    }
    const char* p = mark + 9;
    for (int i = 0; i < 8; i++) {
        if (!std::isdigit(static_cast<unsigned char>(p[i]))) {
            return -1;
        }
        dateOut[i] = p[i];
    }
    dateOut[8] = '\0';
    if (p[8] != '.') {
        return -1;
    }
    p += 9;
    size_t n = 0;
    while (is_hex(static_cast<unsigned char>(p[n]))) {
        n++;
    }
    if (n < 4 || n > 40) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        hashOut[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(p[i])));
    }
    hashOut[n] = '\0';
    return 0;
}

int gdx_update_asset_matches(const char* assetName, const char* platform) {
    if (assetName == nullptr || platform == nullptr || platform[0] == '\0') {
        return 0;
    }
    const char* ext = nullptr;
    if (std::strcmp(platform, "windows-x64") == 0) {
        ext = ".zip";
    } else if (std::strcmp(platform, "linux-x64") == 0 || std::strcmp(platform, "linux-arm64") == 0 ||
               std::strcmp(platform, "macos-arm64") == 0) {
        ext = ".tar.gz";
    } else {
        return 0;
    }
    const char* prefix = "G-Diffuser-v";
    size_t prefixLen = std::strlen(prefix);
    size_t nameLen = std::strlen(assetName);
    if (nameLen < prefixLen || std::strncmp(assetName, prefix, prefixLen) != 0) {
        return 0;
    }
    for (size_t i = 0; i < nameLen; i++) {
        unsigned char c = static_cast<unsigned char>(assetName[i]);
        if (c < 0x20 || c >= 0x7F || c == '/' || c == '\\' || c == ':') {
            return 0;
        }
    }
    char suffix[64];
    int wrote = std::snprintf(suffix, sizeof(suffix), "-%s%s", platform, ext);
    if (wrote <= 0 || (size_t)wrote >= sizeof(suffix)) {
        return 0;
    }
    if (nameLen < prefixLen + 1 + (size_t)wrote) {
        return 0;
    }
    return std::memcmp(assetName + nameLen - (size_t)wrote, suffix, (size_t)wrote) == 0 ? 1 : 0;
}

int gdx_update_find_sha256(const char* sumsText, const char* assetName, char out[65]) {
    if (out != nullptr) {
        out[0] = '\0';
    }
    if (sumsText == nullptr || assetName == nullptr || assetName[0] == '\0' || out == nullptr) {
        return -1;
    }
    size_t assetLen = std::strlen(assetName);
    const char* p = sumsText;
    while (*p != '\0') {
        const char* line = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        const char* lineEnd = p;
        if (*p == '\n') {
            p++;
        }
        if (lineEnd > line && lineEnd[-1] == '\r') {
            lineEnd--;
        }
        while (lineEnd > line && (lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) {
            lineEnd--;
        }
        while (line < lineEnd && (*line == ' ' || *line == '\t')) {
            line++;
        }
        if ((size_t)(lineEnd - line) < 65) {
            continue;
        }
        bool hex = true;
        for (int i = 0; i < 64; i++) {
            if (!is_hex(static_cast<unsigned char>(line[i]))) {
                hex = false;
                break;
            }
        }
        if (!hex || (line[64] != ' ' && line[64] != '\t')) {
            continue;
        }
        const char* name = line + 65;
        while (name < lineEnd && (*name == ' ' || *name == '\t')) {
            name++;
        }
        size_t nameLen = (size_t)(lineEnd - name);
        if (nameLen == assetLen && std::memcmp(name, assetName, assetLen) == 0) {
            for (int i = 0; i < 64; i++) {
                out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
            }
            out[64] = '\0';
            return 0;
        }
    }
    return -1;
}

void gdx_update_sha256(const void* data, size_t len, unsigned char out[32]) {
    if (out == nullptr) {
        return;
    }
    Sha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

const char* gdx_update_release_page_url(void) {
    if (channel_is("nightly")) {
        return "https://github.com/Zorkats/G-Diffuser/releases/tag/nightly";
    }
    return "https://github.com/Zorkats/G-Diffuser/releases/latest";
}

void gdx_update_start_check(int manual) {
    try {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        if (!channel_offers()) {
            w.status.state = GDX_UPDATE_DISABLED;
            w.status.errorMessage[0] = '\0';
            w.cmdCheck = false;
            gdx_port_logf("[update] check disabled for channel %s\n", GDX_BUILD_CHANNEL);
            return;
        }
        bool busy = w.status.state == GDX_UPDATE_CHECKING || w.status.state == GDX_UPDATE_DOWNLOADING ||
                    w.status.state == GDX_UPDATE_VERIFYING;
        if (busy || (w.status.state == GDX_UPDATE_READY && !manual)) {
            return;
        }
        w.cmdCheck = true;
        w.checkManual = manual;
        w.status.state = GDX_UPDATE_CHECKING;
        w.status.errorMessage[0] = '\0';
        ensure_thread(w);
        w.cv.notify_one();
    } catch (...) {
        gdx_port_logf("[update] could not start check\n");
    }
}

void gdx_update_poll(GdxUpdateStatus* out) {
    if (out == nullptr) {
        return;
    }
    Worker& w = worker();
    std::lock_guard<std::mutex> lock(w.mu);
    if (!channel_offers() && w.status.state == GDX_UPDATE_IDLE) {
        w.status.state = GDX_UPDATE_DISABLED;
    }
    *out = w.status;
}

void gdx_update_start_download(void) {
    try {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        if (w.status.state != GDX_UPDATE_AVAILABLE || w.assetUrl[0] == '\0' || w.sumsUrl[0] == '\0' ||
            w.assetName[0] == '\0') {
            return;
        }
        w.status.state = GDX_UPDATE_DOWNLOADING;
        w.status.bytesDone = 0;
        w.status.bytesTotal = 0;
        w.status.errorMessage[0] = '\0';
        w.cmdDownload = true;
        ensure_thread(w);
        w.cv.notify_one();
    } catch (...) {
        gdx_port_logf("[update] could not start download\n");
    }
}

void gdx_update_apply_and_restart(void) {
    try {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        if (w.status.state != GDX_UPDATE_READY || w.assetName[0] == '\0') {
            return;
        }
        w.cmdApply = true;
        ensure_thread(w);
        w.cv.notify_one();
    } catch (...) {
        gdx_port_logf("[update] could not start apply\n");
    }
}

void gdx_update_cleanup_on_boot(void) {
    try {
        Worker& w = worker();
        std::lock_guard<std::mutex> lock(w.mu);
        w.cmdCleanup = true;
        ensure_thread(w);
        w.cv.notify_one();
    } catch (...) {
        gdx_port_logf("[update] could not start cleanup\n");
    }
}
