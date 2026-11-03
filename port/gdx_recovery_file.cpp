#include "gdx_recovery_file.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace gdx::recovery {
namespace {
std::atomic<unsigned long> sequence{0};
}

bool readFile(const std::filesystem::path& path, Bytes& out, size_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    Bytes bytes(static_cast<size_t>(size));
    if (!file || (size && !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) ||
        file.peek() != std::char_traits<char>::eof() || file.bad()) {
        return false;
    }
    out = std::move(bytes);
    return true;
}

static bool writeFileImpl(const std::filesystem::path& path, const Bytes& bytes, bool replace) {
    auto temp = path;
    temp += ".writing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence);
    bool ok = true;
#ifdef _WIN32
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    size_t position = 0;
    while (position < bytes.size()) {
        DWORD wrote = 0;
        const DWORD count = DWORD((std::min)(bytes.size() - position, size_t(1024 * 1024)));
        if (!WriteFile(file, bytes.data() + position, count, &wrote, nullptr) || !wrote) {
            ok = false;
            break;
        }
        position += wrote;
    }
    if (!FlushFileBuffers(file)) {
        ok = false;
    }
    if (!CloseHandle(file)) {
        ok = false;
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (ok && !MoveFileExW(temp.c_str(), path.c_str(), flags)) {
        ok = false;
    }
#else
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return false;
    }
    size_t position = 0;
    while (position < bytes.size()) {
        const auto wrote = write(fd, bytes.data() + position, bytes.size() - position);
        if (wrote <= 0) {
            ok = false;
            break;
        }
        position += size_t(wrote);
    }
    if (fsync(fd)) {
        ok = false;
    }
    if (close(fd)) {
        ok = false;
    }
    if (ok) {
        if (replace) {
            if (rename(temp.c_str(), path.c_str())) {
                ok = false;
            }
        } else {
            // Linking publishes atomically without replacing a racing generation.
            if (link(temp.c_str(), path.c_str())) {
                ok = false;
            } else if (unlink(temp.c_str())) {
                ok = false;
            }
        }
    }
    if (ok) {
        const int parent = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (parent < 0) {
            ok = false;
        } else {
            if (fsync(parent)) {
                ok = false;
            }
            if (close(parent)) {
                ok = false;
            }
        }
    }
#endif
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    Bytes verified;
    return readFile(path, verified, bytes.size()) && verified == bytes;
}

bool prepareDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directory(path, ec);
    if (ec || !std::filesystem::is_directory(path, ec) || ec || std::filesystem::is_symlink(path, ec) || ec) {
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    const int parent = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (parent < 0) {
        return false;
    }
    const bool synced = fsync(parent) == 0;
    const bool closed = close(parent) == 0;
    return synced && closed;
#endif
}

FileLock::FileLock(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(file);
        } else {
            handle = reinterpret_cast<intptr_t>(file);
        }
    }
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            handle = fd;
        } else {
            close(fd);
        }
    }
#endif
}

FileLock::~FileLock() {
    if (handle != -1) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
        close(int(handle));
#endif
    }
}

bool FileLock::held() const {
    return handle != -1;
}
}


namespace gdx::recovery {
bool publishFile(const std::filesystem::path& path, const Bytes& bytes) {
    return writeFileImpl(path, bytes, false);
}
}


namespace gdx::recovery {
bool writeFile(const std::filesystem::path& path, const Bytes& bytes) {
    return writeFileImpl(path, bytes, true);
}
}
