// Copyright (c) Zorkats

#include "gdx_companion_process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace gdx_companion {

namespace {

constexpr size_t kMaxReadPerPoll = 1024 * 1024;

bool IsAbsolutePath(const std::string& path) {
    return std::filesystem::path(path).is_absolute();
}

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// Quotes one argument with the MSVC runtime rules, so argv survives CreateProcessW's single
// command-line string unchanged.
std::string QuoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// parent_writes: the parent keeps the write end (stdin). Otherwise it keeps the read end.
bool MakePipe(bool parent_writes, HANDLE* parent_end, HANDLE* child_end) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    HANDLE keep = parent_writes ? write_end : read_end;
    HANDLE pass = parent_writes ? read_end : write_end;
    SetHandleInformation(keep, HANDLE_FLAG_INHERIT, 0);
    *parent_end = keep;
    *child_end = pass;
    return true;
}

void CloseHandleIfOpen(HANDLE& handle) {
    if (handle != nullptr) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

size_t DrainPipe(HANDLE pipe, bool& eof, std::string& out) {
    if (pipe == nullptr || eof) return 0;
    size_t total = 0;
    char buffer[4096];
    while (total < kMaxReadPerPoll) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            eof = true;
            break;
        }
        if (avail == 0) break;
        const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buffer)));
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, want, &got, nullptr) || got == 0) {
            eof = true;
            break;
        }
        out.append(buffer, got);
        total += got;
    }
    return total;
}

#else

void SetCloseOnExec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

size_t DrainPipe(int fd, bool& eof, std::string& out) {
    if (fd < 0 || eof) return 0;
    size_t total = 0;
    char buffer[4096];
    while (total < kMaxReadPerPoll) {
        const ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            eof = true;
            break;
        }
        if (got == 0) {
            eof = true;
            break;
        }
        out.append(buffer, static_cast<size_t>(got));
        total += static_cast<size_t>(got);
    }
    return total;
}

int DecodeStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

#endif

}  // namespace

struct ChildProcess::Impl {
    bool started = false;
    bool exited = false;
    int exit_code = -1;
    bool stdout_eof = false;
    bool stderr_eof = false;
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE err_read = nullptr;
#else
    pid_t pid = -1;
    int in_write = -1;
    int out_read = -1;
    int err_read = -1;
#endif
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    Terminate();
#ifdef _WIN32
    CloseHandleIfOpen(impl_->process);
#endif
}

bool ChildProcess::started() const {
    return impl_->started;
}

int ChildProcess::exit_code() const {
    return impl_->exit_code;
}

#ifdef _WIN32

bool ChildProcess::Start(const std::vector<std::string>& argv, std::string& error) {
    if (impl_->started) {
        error = "child process already started";
        return false;
    }
    if (argv.empty() || !IsAbsolutePath(argv[0])) {
        error = "executable path must be absolute";
        return false;
    }

    HANDLE in_parent = nullptr, in_child = nullptr;
    HANDLE out_parent = nullptr, out_child = nullptr;
    HANDLE err_parent = nullptr, err_child = nullptr;
    if (!MakePipe(true, &in_parent, &in_child) || !MakePipe(false, &out_parent, &out_child) ||
        !MakePipe(false, &err_parent, &err_child)) {
        error = "CreatePipe failed with error " + std::to_string(GetLastError());
        for (HANDLE* h : {&in_parent, &in_child, &out_parent, &out_child, &err_parent, &err_child}) {
            CloseHandleIfOpen(*h);
        }
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_child;
    si.hStdOutput = out_child;
    si.hStdError = err_child;
    PROCESS_INFORMATION pi{};

    std::wstring command;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) command += L' ';
        command += Utf8ToWide(QuoteArg(argv[i]));
    }
    const std::wstring application = Utf8ToWide(argv[0]);

    const BOOL created = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD create_error = created ? 0 : GetLastError();

    // The child owns its ends now. The parent keeps only its own.
    CloseHandleIfOpen(in_child);
    CloseHandleIfOpen(out_child);
    CloseHandleIfOpen(err_child);

    if (!created) {
        error = "CreateProcessW failed with error " + std::to_string(create_error);
        CloseHandleIfOpen(in_parent);
        CloseHandleIfOpen(out_parent);
        CloseHandleIfOpen(err_parent);
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->in_write = in_parent;
    impl_->out_read = out_parent;
    impl_->err_read = err_parent;
    impl_->started = true;
    return true;
}

bool ChildProcess::WriteStdin(const std::string& bytes, std::string& error) {
    if (impl_->in_write == nullptr) {
        error = "stdin is closed";
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 65536));
        DWORD wrote = 0;
        if (!WriteFile(impl_->in_write, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            error = "WriteFile failed with error " + std::to_string(GetLastError());
            return false;
        }
        done += wrote;
    }
    return true;
}

size_t ChildProcess::ReadStdout(std::string& out) {
    return DrainPipe(impl_->out_read, impl_->stdout_eof, out);
}

size_t ChildProcess::ReadStderr(std::string& out) {
    return DrainPipe(impl_->err_read, impl_->stderr_eof, out);
}

void ChildProcess::CloseStdin() {
    CloseHandleIfOpen(impl_->in_write);
}

bool ChildProcess::HasExited() {
    if (impl_->exited) return true;
    if (!impl_->started) return true;
    if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    impl_->exit_code = GetExitCodeProcess(impl_->process, &code) ? static_cast<int>(code) : -1;
    impl_->exited = true;
    return true;
}

bool ChildProcess::WaitForExit(int timeout_ms) {
    if (HasExited()) return true;
    WaitForSingleObject(impl_->process, static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms));
    return HasExited();
}

void ChildProcess::Terminate() {
    if (impl_->started && !HasExited()) {
        TerminateProcess(impl_->process, 1);
        WaitForExit(3000);
    }
    CloseHandleIfOpen(impl_->in_write);
    CloseHandleIfOpen(impl_->out_read);
    CloseHandleIfOpen(impl_->err_read);
}

#else

bool ChildProcess::Start(const std::vector<std::string>& argv, std::string& error) {
    if (impl_->started) {
        error = "child process already started";
        return false;
    }
    if (argv.empty() || !IsAbsolutePath(argv[0])) {
        error = "executable path must be absolute";
        return false;
    }

    // fds: [0] child stdin read, [1] parent stdin write, [2] parent stdout read,
    //      [3] child stdout write, [4] parent stderr read, [5] child stderr write.
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    auto fail = [&fds, &error](const std::string& message) {
        for (int fd : fds) {
            if (fd >= 0) close(fd);
        }
        error = message;
        return false;
    };
    if (pipe(fds) != 0 || pipe(fds + 2) != 0 || pipe(fds + 4) != 0) {
        return fail(std::strerror(errno));
    }
    for (int fd : fds) SetCloseOnExec(fd);
    SetNonBlocking(fds[2]);
    SetNonBlocking(fds[4]);

    // A child end that sits on 0..2 would be clobbered by the dup2 calls below, and dup2 onto
    // itself leaves it close-on-exec. Move such ends above 2 first.
    const int child_slots[3] = {0, 3, 5};
    for (int slot : child_slots) {
        if (fds[slot] <= 2) {
            const int moved = fcntl(fds[slot], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) return fail(std::strerror(errno));
            close(fds[slot]);
            fds[slot] = moved;
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
    posix_spawn_file_actions_adddup2(&actions, fds[3], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[5], 2);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return fail(std::strerror(rc));

    close(fds[0]);
    close(fds[3]);
    close(fds[5]);
    impl_->pid = pid;
    impl_->in_write = fds[1];
    impl_->out_read = fds[2];
    impl_->err_read = fds[4];
    impl_->started = true;
    return true;
}

bool ChildProcess::WriteStdin(const std::string& bytes, std::string& error) {
    if (impl_->in_write < 0) {
        error = "stdin is closed";
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote = write(impl_->in_write, bytes.data() + done, bytes.size() - done);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        done += static_cast<size_t>(wrote);
    }
    return true;
}

size_t ChildProcess::ReadStdout(std::string& out) {
    return DrainPipe(impl_->out_read, impl_->stdout_eof, out);
}

size_t ChildProcess::ReadStderr(std::string& out) {
    return DrainPipe(impl_->err_read, impl_->stderr_eof, out);
}

void ChildProcess::CloseStdin() {
    if (impl_->in_write >= 0) {
        close(impl_->in_write);
        impl_->in_write = -1;
    }
}

bool ChildProcess::HasExited() {
    if (impl_->exited) return true;
    if (!impl_->started) return true;
    int status = 0;
    const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
    if (result == 0) return false;
    impl_->exited = true;
    impl_->exit_code = result < 0 ? -1 : DecodeStatus(status);
    return true;
}

bool ChildProcess::WaitForExit(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!HasExited()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void ChildProcess::Terminate() {
    if (impl_->started && !HasExited()) {
        kill(impl_->pid, SIGKILL);
        int status = 0;
        waitpid(impl_->pid, &status, 0);
        impl_->exited = true;
        impl_->exit_code = DecodeStatus(status);
    }
    if (impl_->in_write >= 0) close(impl_->in_write);
    if (impl_->out_read >= 0) close(impl_->out_read);
    if (impl_->err_read >= 0) close(impl_->err_read);
    impl_->in_write = -1;
    impl_->out_read = -1;
    impl_->err_read = -1;
}

#endif

}  // namespace gdx_companion
