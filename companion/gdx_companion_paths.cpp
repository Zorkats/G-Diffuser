// Copyright (c) Zorkats

#include "gdx_companion_paths.h"

#include <cstdint>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#include <string>
#else
#include <unistd.h>
#include <cstddef>
#endif

namespace fs = std::filesystem;

namespace gdx_companion {

namespace {

#if defined(_WIN32)
constexpr const char* kWorkerName = "gdx_companion_worker.exe";
constexpr const char* kGameName = "G-Diffuser.exe";
#else
constexpr const char* kWorkerName = "gdx_companion_worker";
constexpr const char* kGameName = "G-Diffuser";
#endif

}  // namespace

fs::path ExecutableDirectory(std::string& error) {
    fs::path exe;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            error = "GetModuleFileNameW failed";
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    exe = fs::path(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string raw(size, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0) {
        error = "_NSGetExecutablePath failed";
        return {};
    }
    char resolved[PATH_MAX];
    if (realpath(raw.c_str(), resolved) == nullptr) {
        error = "realpath failed for the executable";
        return {};
    }
    exe = resolved;
#else
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        error = "readlink /proc/self/exe failed";
        return {};
    }
    buffer[length] = '\0';
    exe = buffer;
#endif
    return exe.parent_path();
}

std::string PathToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path PathFromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

fs::path GameExecutablePath(const fs::path& exe_dir) {
    return exe_dir / kGameName;
}

bool ResolveWorkerLaunch(const WorkerLaunchOptions& options, const fs::path& exe_dir,
                         std::vector<std::string>& argv, std::string& description, std::string& error) {
    argv.clear();
    description.clear();
    const bool have_python = !options.python.empty();
    const bool have_script = !options.worker_script.empty();
    if (have_python != have_script) {
        error = "--python and --worker must be given together";
        return false;
    }

    std::error_code ec;
    if (have_python) {
        const fs::path python = fs::absolute(options.python, ec);
        if (ec) {
            error = "cannot resolve --python: " + ec.message();
            return false;
        }
        const fs::path script = fs::absolute(options.worker_script, ec);
        if (ec) {
            error = "cannot resolve --worker: " + ec.message();
            return false;
        }
        if (!fs::is_regular_file(python, ec)) {
            error = "--python is not a file: " + PathToUtf8(python);
            return false;
        }
        if (!fs::is_regular_file(script, ec)) {
            error = "--worker is not a file: " + PathToUtf8(script);
            return false;
        }
        argv = {PathToUtf8(python), "-I", PathToUtf8(script)};
        description = "dev worker: " + PathToUtf8(python) + " -I " + PathToUtf8(script);
        return true;
    }

    const fs::path frozen = exe_dir / "worker" / kWorkerName;
    if (!fs::is_regular_file(frozen, ec)) {
        error = "frozen worker not found at " + PathToUtf8(frozen);
        return false;
    }
    argv = {PathToUtf8(frozen)};
    description = "frozen worker: " + PathToUtf8(frozen);
    return true;
}

bool NarrowPathForC(const fs::path& path, std::string& out) {
    const std::string utf8 = PathToUtf8(path);
    bool ascii = true;
    for (const unsigned char c : utf8) ascii = ascii && c < 128;
    if (ascii) {
        out = utf8;
        return true;
    }
#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    const DWORD needed = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (needed == 0) return false;
    std::wstring shortened(needed, L'\0');
    const DWORD written = GetShortPathNameW(wide.c_str(), shortened.data(), needed);
    if (written == 0 || written >= needed) return false;
    shortened.resize(written);
    out.clear();
    for (const wchar_t c : shortened) {
        if (c >= 128) return false;
        out.push_back(static_cast<char>(c));
    }
    return true;
#else
    out = utf8;
    return true;
#endif
}

}  // namespace gdx_companion
