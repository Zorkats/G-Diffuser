// port/updater/gdx_update_main.cpp -- gdx-update, the process that replaces an installed build.
//
// The game copies this executable into <install>/.update/, starts the copy with the archive it just
// downloaded and verified, and quits through its own path so saves are flushed. From the moment
// this process starts, the install folder is his: wait for the game, unpack into staging, swap the
// shipped payload, and either relaunch the new build or put the old one back.
//
// The decisions all live in gdx_update_apply.cpp, which the tests exercise; what is left here is
// the process itself: arguments, the log, the wait and the relaunch.

#include "gdx_update_apply.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW, which windows.h does not pull in
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

// The game has 60 s to exit. A player who quits mid-race still has to write a ghost and a save, so
// the wait is generous; past it the update gives up rather than fight a running game for its files.
constexpr int kWaitTimeoutMs = 60000;

// Paths travel as UTF-8 text in the shared interface and become the host path type only here. The
// same three helpers exist in gdx_update_apply.cpp: the two files need them for different reasons
// (argument parsing and process creation here, archive members and the filesystem interface there)
// and the alternative was putting <filesystem> into the shared header for two call sites.
fs::path to_path(const std::string& utf8Text) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8Text.begin(), utf8Text.end()));
#else
    return fs::u8path(utf8Text);
#endif
}

std::string to_text(const fs::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}

std::string join_path(const std::string& folder, const std::string& name) {
    std::string base = folder;

    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    return base + "/" + name;
}

// A console-less executable has no stderr anyone reads, so every step goes to <install>/.update/
// update.log, one flushed line at a time: the process can be killed by the game closing or by the
// machine shutting down, and a lost buffer would hide the reason the update stopped.
class UpdateLog {
public:
    bool open(const fs::path& path) {
        mOut.open(path, std::ios::binary | std::ios::app);
        return mOut.is_open();
    }

    void line(const char* format, ...) {
        char text[1024];
        va_list args;

        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);

        std::time_t now = std::time(nullptr);
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        mOut << stamp << " " << text << "\n";
        mOut.flush();
    }

private:
    std::ofstream mOut;
};

struct Options {
    fs::path install;
    fs::path archive;
    fs::path relaunch;
    long pid = 0;
};

#ifdef _WIN32
std::vector<std::string> command_line_arguments(void) {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);

    if (wide == nullptr) {
        return args;
    }
    for (int i = 0; i < count; i++) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);

        if (length <= 1) {
            args.emplace_back();
            continue;
        }
        std::vector<char> buffer(static_cast<size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, buffer.data(), length, nullptr, nullptr);
        args.emplace_back(buffer.data());
    }
    LocalFree(wide);
    return args;
}
#else
std::vector<std::string> command_line_arguments(int argc, char** argv) {
    return std::vector<std::string>(argv, argv + argc);
}
#endif

bool parse_options(const std::vector<std::string>& args, Options* options, std::string* error) {
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& flag = args[i];

        if (flag != "--install" && flag != "--archive" && flag != "--relaunch" && flag != "--pid") {
            *error = "unknown argument " + flag;
            return false;
        }
        if (i + 1 >= args.size()) {
            *error = "missing value for " + flag;
            return false;
        }
        const std::string value = args[++i];
        if (flag == "--install") {
            options->install = to_path(value);
        } else if (flag == "--archive") {
            options->archive = to_path(value);
        } else if (flag == "--relaunch") {
            options->relaunch = to_path(value);
        } else {
            char* end = nullptr;
            const long pid = std::strtol(value.c_str(), &end, 10);

            if (end == value.c_str() || *end != '\0' || pid <= 0) {
                *error = "not a process id: " + value;
                return false;
            }
            options->pid = pid;
        }
    }
    if (options->install.empty() || options->archive.empty() || options->relaunch.empty() || options->pid <= 0) {
        *error = "usage: gdx-update --install <dir> --archive <file> --pid <pid> --relaunch <exe>";
        return false;
    }
    return true;
}

#ifdef _WIN32
bool wait_for_pid(long pid, UpdateLog* log) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));

    if (process == nullptr) {
        const DWORD error = GetLastError();

        // ERROR_INVALID_PARAMETER means the game is already gone, which is the common fast path: the
        // player quit and the helper started after the process object disappeared. Any other error (access
        // denied) may mean the game still runs, so the helper must not swap files under it.
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
            log->line("pid %ld is gone (error %lu)", pid, static_cast<unsigned long>(error));
            return true;
        }
        log->line("pid %ld cannot be opened (error %lu), not installing", pid, static_cast<unsigned long>(error));
        return false;
    }
    const DWORD waited = WaitForSingleObject(process, static_cast<DWORD>(kWaitTimeoutMs));
    CloseHandle(process);
    if (waited == WAIT_OBJECT_0) {
        return true;
    }
    log->line("pid %ld is still running after %d s", pid, kWaitTimeoutMs / 1000);
    return false;
}
#else
// Asked every 100 ms: cheap enough that a game that exits promptly is noticed promptly.
constexpr int kPollMs = 100;

bool wait_for_pid(long pid, UpdateLog* log) {
    int waited = 0;

    for (;;) {
        // Signal 0 asks the kernel whether the process exists without touching it. EPERM means it
        // exists and belongs to someone else, which cannot happen here but still counts as running.
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return true;
        }
        if (waited >= kWaitTimeoutMs) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        waited += kPollMs;
    }
    log->line("pid %ld is still running after %d s", pid, kWaitTimeoutMs / 1000);
    return false;
}
#endif

#ifdef _WIN32
bool relaunch(const fs::path& exe, const fs::path& installDir) {
    const std::wstring command = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());

    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup {};
    PROCESS_INFORMATION info {};
    startup.cb = sizeof(startup);
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, installDir.c_str(), &startup,
                       &info) == FALSE) {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}
#else
bool relaunch(const fs::path& exe, const fs::path& installDir) {
    const std::string exeText = exe.string();
    const std::string installText = installDir.string();

    // This process creates no threads, so the allocations below are safe after the fork.
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        setsid();
        if (chdir(installText.c_str()) != 0) {
            _exit(127);
        }
        char* const argv[] = { const_cast<char*>(exeText.c_str()), nullptr };
        execv(exeText.c_str(), argv);
        _exit(127);
    }
    return true;
}
#endif

std::vector<std::string> staged_items(const fs::path& stagingDir) {
    std::vector<std::string> items;
    std::error_code ec;
    fs::directory_iterator it(stagingDir, ec);

    if (ec) {
        return items;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        items.push_back(to_text(it->path().filename()));
    }
    return items;
}

void write_messages(UpdateLog* log, const std::vector<std::string>& messages) {
    for (const std::string& message : messages) {
        log->line("%s", message.c_str());
    }
}

int run(const Options& options, const fs::path& updateDir, UpdateLog* log) {
    const std::string installText = to_text(options.install);
    const std::string stagingText = join_path(installText, ".update/staging");
    const std::string backupText = join_path(installText, ".update/backup");
    const fs::path stagingDir = to_path(stagingText);

    log->line("gdx-update: install %s", installText.c_str());
    log->line("gdx-update: archive %s", to_text(options.archive).c_str());
    log->line("gdx-update: relaunch %s", to_text(options.relaunch).c_str());
    log->line("gdx-update: waiting for pid %ld", options.pid);
    if (!wait_for_pid(options.pid, log)) {
        log->line("gdx-update: giving up without touching the install folder");
        return GDX_UPD_ERR_WAIT;
    }
    log->line("gdx-update: the game has exited");

    std::error_code ec;
    fs::remove_all(stagingDir, ec);
    if (ec) {
        // Staging has to be empty: a leftover from an interrupted attempt would otherwise be swapped
        // in beside the files this run extracts.
        log->line("gdx-update: cannot clear %s: %s", stagingText.c_str(), ec.message().c_str());
        return GDX_UPD_ERR_IO;
    }
    fs::create_directories(stagingDir, ec);
    if (ec) {
        log->line("gdx-update: cannot create %s: %s", stagingText.c_str(), ec.message().c_str());
        return GDX_UPD_ERR_IO;
    }

    std::vector<std::string> messages;
    int rc = gdx_upd_extract_archive(to_text(options.archive), stagingText, &messages);
    write_messages(log, messages);
    if (rc != GDX_UPD_OK) {
        log->line("gdx-update: the archive was refused, the install folder is unchanged");
        return rc;
    }

    const fs::path stagedMain = stagingDir / to_path(gdx_upd_main_exe_name());
    if (!fs::is_regular_file(stagedMain, ec)) {
        log->line("gdx-update: the archive holds no %s, the install folder is unchanged", gdx_upd_main_exe_name());
        return GDX_UPD_ERR_ARCHIVE;
    }

    const std::vector<std::string> items = staged_items(stagingDir);
    log->line("gdx-update: swapping %zu staged item(s)", items.size());
    auto host = gdx_upd_make_host_fs();
    messages.clear();
    const GdxUpdSwapResult result = gdx_upd_swap_apply(host.get(), installText, stagingText, backupText, items, &messages);
    write_messages(log, messages);

    if (result.code != GDX_UPD_OK) {
        log->line("gdx-update: the swap failed at %s and the rollback is %s", result.failedItem.c_str(),
                  result.rollbackComplete ? "complete" : "INCOMPLETE");
        // Whatever the install folder now holds, the player gets a game rather than a logged-off
        // desktop: the old build is started, and the next update attempt clears staging again.
        if (!relaunch(options.relaunch, options.install)) {
            log->line("gdx-update: could not start %s after the rollback", to_text(options.relaunch).c_str());
            return GDX_UPD_ERR_IO;
        }
        return GDX_UPD_ERR_SWAP;
    }

    const fs::path appliedMarker = updateDir / "applied.ok";
    std::ofstream marker(appliedMarker, std::ios::binary | std::ios::trunc);
    if (marker) {
        marker << "applied " << to_text(options.archive.filename()) << "\n";
        marker.close();
    }
    if (!marker) {
        log->line("gdx-update: cannot write %s", to_text(appliedMarker).c_str());
        return GDX_UPD_ERR_IO;
    }

    // Staging and backup are this tool's own furniture. A locked leftover must not turn a finished
    // update into a failure, so the next boot cleans up whatever survives here.
    fs::remove_all(stagingDir, ec);
    if (ec) {
        log->line("gdx-update: could not remove %s: %s", stagingText.c_str(), ec.message().c_str());
    }
    fs::remove_all(to_path(backupText), ec);
    if (ec) {
        log->line("gdx-update: could not remove %s: %s", backupText.c_str(), ec.message().c_str());
    }

    log->line("gdx-update: update applied, starting %s", to_text(options.relaunch).c_str());
    if (!relaunch(options.relaunch, options.install)) {
        log->line("gdx-update: could not start %s", to_text(options.relaunch).c_str());
        return GDX_UPD_ERR_IO;
    }
    return GDX_UPD_OK;
}

int helper_main(const std::vector<std::string>& args) {
    Options options;
    std::string error;

    if (!parse_options(args, &options, &error)) {
        // The install folder is what could not be read, so there is no log to write this to.
        return GDX_UPD_ERR_ARGS;
    }
    const fs::path updateDir = options.install / ".update";
    std::error_code ec;
    fs::create_directories(updateDir, ec);

    UpdateLog log;
    if (!log.open(updateDir / "update.log")) {
        return GDX_UPD_ERR_IO;
    }
    return run(options, updateDir, &log);
}

} // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show) {
    (void)instance;
    (void)previous;
    (void)commandLine;
    (void)show;
    return helper_main(command_line_arguments());
}
#else
int main(int argc, char** argv) {
    return helper_main(command_line_arguments(argc, argv));
}
#endif
