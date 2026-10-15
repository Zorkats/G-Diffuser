// port/gdx_dump_launch.cpp — per-class offline "Dump All" launcher. See the header for the contract.
// The UI thread only ever READS GdxDumpSnapshot(); all child-process work happens on a detached
// worker thread. Per-class failure isolation is the load-bearing property: one child PER CLASS, so a
// class that exits non-zero is recorded and the worker moves on.
//
// The native (`gdx-extract dump --classes <name> -d <dir> ...`) and Python
// (tools/gen_dump_all.py) backends share the same child-process plumbing (runChild) and differ only
// in the argv they build and the stdout count-parse they use.

#include "gdx_dump_launch.h"
#include "port_log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <ctime>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#include <limits.h>        // PATH_MAX
#endif
#endif

namespace fs = std::filesystem;

namespace gdx {
namespace {

// ── Constants ────────────────────────────────────────────────────────────────────────────────────
constexpr int kMaxParentWalk = 6;         // how far up from the exe to search for tools/
constexpr size_t kLastLineCap = 240;      // truncate captured diagnostic lines to this many chars
constexpr unsigned kChildDeadlineSecs = 900; // hang guard: kill a child that runs longer than this

// Sentinel class name the self-test uses to prove per-class failure isolation. It is not in any
// registry, so the child exits non-zero and the batch must still continue past it.
constexpr const char* kBogusClass = "__gdx_bogus_class__";

// ── Native backend file/dir names (mirror port/gdx_extract_launch.cpp & gdx_firstboot.cpp) ──────────
#ifdef _WIN32
constexpr const char* kExtractBinaryName = "gdx-extract.exe";
#else
constexpr const char* kExtractBinaryName = "gdx-extract";
#endif
constexpr const char* kRecipesDirName = "decomp-recipes";                 // --recipes
constexpr const char* kEkSliceManifestName = "ek_slice_manifest.txt";     // --manifest (under recipes)
constexpr const char* kCartArchiveName = "fzerox.o2r";                    // real cart archive (data dir)
constexpr const char* kCartArchiveDevName = "generic.o2r";               // dev tree assets/extracted/
constexpr const char* kDiskArchiveName = "fzerox-disk.o2r";              // --disk-archive
constexpr const char* kIplArchiveName = "n64ddipl.o2r";                  // --ipl-archive
constexpr const char* kRomName = "baserom.us.rev0.z64";                  // --rom (optional)

// ── exe dir ───────────────────────────────────────────────────────────────────────────────────────
fs::path exeDir() {
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#else
    char buf[4096];
#ifdef __APPLE__
    {
        uint32_t bufSize = sizeof(buf);
        if (_NSGetExecutablePath(buf, &bufSize) == 0) {
            char resolved[PATH_MAX];
            if (realpath(buf, resolved) != nullptr) {
                return fs::path(resolved).parent_path();
            }
            return fs::path(buf).parent_path();
        }
    }
#else
    ssize_t rl = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (rl > 0) {
        buf[rl] = '\0';
        return fs::path(buf).parent_path();
    }
#endif
#endif
    return fs::current_path(ec);
}

std::string truncateLine(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    if (s.size() > kLastLineCap) {
        s = s.substr(0, kLastLineCap - 3) + "...";
    }
    return s;
}

// Best-effort "<n> dumped" parse (matches gen_dump_all.py's "textures: N dumped, ..." summary and the
// analogous per-class lines). Returns the last such N seen, or -1.
int parseDumpedCount(const std::string& line, int previous) {
    size_t pos = line.find(" dumped");
    if (pos == std::string::npos) {
        return previous;
    }
    size_t end = pos;
    while (end > 0 && (line[end - 1] == ' ')) {
        --end;
    }
    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1]))) {
        --start;
    }
    if (start == end) {
        return previous;
    }
    return std::atoi(line.substr(start, end - start).c_str());
}

// Native `gdx-extract dump` count parse. One class can emit MORE THAN ONE counting line (textures
// prints "textures: 1520 dumped" AND "ek textures: 310 dumped" -> 1830), so unlike the Python
// "last N wins" parse above this ACCUMULATES the integer before every "<n> dumped" / "<n> WAV
// decoded" / "<n> MIDI written". Coverage/census lines ("N archive-covered", "archiveReads=N") word
// their counts differently and are ignored on purpose. `items` starts at -1 (unknown) and flips to a
// running sum on the first match.
void accumulateNativeCount(const std::string& line, int& items) {
    static const char* const kVerbs[] = {" dumped", " WAV decoded", " MIDI written"};
    for (const char* verb : kVerbs) {
        const size_t vlen = std::strlen(verb);
        size_t pos = 0;
        while ((pos = line.find(verb, pos)) != std::string::npos) {
            size_t end = pos;
            while (end > 0 && line[end - 1] == ' ') {
                --end;
            }
            size_t start = end;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1]))) {
                --start;
            }
            if (start < end) {
                if (items < 0) {
                    items = 0;
                }
                items += std::atoi(line.substr(start, end - start).c_str());
            }
            pos += vlen;
        }
    }
}

// ── PATH interpreter search (no subprocess — safe on the render thread) ─────────────────────────────
#ifdef _WIN32
constexpr char kPathSep = ';';
const char* const kPythonCandidates[] = {"python.exe", "py.exe"};
#else
constexpr char kPathSep = ':';
const char* const kPythonCandidates[] = {"python3", "python"};
#endif

// Resolved ABSOLUTE path of the first Python interpreter on PATH, or "" if none. No process is
// spawned. Returning the exact path validated with fs::is_regular_file, rather than the bare
// "python"/"py" name, keeps CreateProcess from re-resolving against PATH a second time and landing on
// a different match (PATH changed since the check, or the child's search order disagrees with ours).
std::string findPythonOnPath() {
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) {
        return {};
    }
    std::string path(pathEnv);
    std::vector<std::string> dirs;
    size_t start = 0;
    while (start <= path.size()) {
        size_t sep = path.find(kPathSep, start);
        std::string dir = path.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!dir.empty()) {
            dirs.push_back(dir);
        }
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 1;
    }
    std::error_code ec;
    for (size_t i = 0; i < (sizeof(kPythonCandidates) / sizeof(kPythonCandidates[0])); ++i) {
        for (const auto& dir : dirs) {
            fs::path cand = fs::path(dir) / kPythonCandidates[i];
            if (fs::is_regular_file(cand, ec)) {
                // A relative PATH entry (e.g. ".") would otherwise break the ABSOLUTE-path contract
                // above. Fall back to the unnormalized candidate on failure.
                std::error_code absEc;
                fs::path absCand = fs::absolute(cand, absEc);
                return absEc ? cand.string() : absCand.string();
            }
        }
    }
    return {};
}

// ── tool discovery ──────────────────────────────────────────────────────────────────────────────────
// tools/gen_dump_all.py next to the exe, then walking up to kMaxParentWalk parents (covers the dev
// tree where the exe is build/x64/port/Release/ and tools/ sits at the checkout root).
std::string findToolPath() {
    std::error_code ec;
    fs::path dir = exeDir();
    for (int i = 0; i <= kMaxParentWalk; ++i) {
        fs::path cand = dir / "tools" / "gen_dump_all.py";
        if (fs::is_regular_file(cand, ec)) {
            return cand.string();
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            break; // reached filesystem root
        }
        dir = parent;
    }
    return {};
}

// ── Native backend discovery (gdx-extract.exe + explicit source paths) ──────────────────────────────
bool isRegularFile(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}
bool isDir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

// gdx-extract.exe next to the game exe — same convention as port/gdx_extract_launch.cpp (exeDir /
// kExtractBinaryName). Returns "" when absent (the Python fallback then takes over).
std::string findExtractBinary() {
    fs::path cand = exeDir() / kExtractBinaryName;
    return isRegularFile(cand) ? cand.string() : std::string();
}

// Resolve the REAL cart archive, never the gdiffuser.o2r port-assets placeholder:
//   1. <exeDir>/fzerox.o2r          (deployed layout: dataDir == exeDir)
//   2. assets/extracted/generic.o2r (dev tree, walking up from the exe to kMaxParentWalk parents)
// Returns "" when neither is found; the launcher then omits --archive rather than passing a dead path
// (and never lets the tool's own auto-discovery fall back to gdiffuser.o2r).
std::string resolveCartArchive() {
    fs::path deployed = exeDir() / kCartArchiveName;
    if (isRegularFile(deployed)) {
        return deployed.string();
    }
    fs::path dir = exeDir();
    for (int i = 0; i <= kMaxParentWalk; ++i) {
        fs::path cand = dir / "assets" / "extracted" / kCartArchiveDevName;
        if (isRegularFile(cand)) {
            return cand.string();
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

// Optional sources live next to the exe in the deployed/dev layout (dataDir == exeDir). Each returns
// "" when absent so the caller omits the corresponding flag.
std::string resolveNextToExe(const char* name) {
    fs::path cand = exeDir() / name;
    return isRegularFile(cand) ? cand.string() : std::string();
}

// ── Child process runner ──────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quote a single argument for the Windows command line (handles spaces and embedded quotes/backslashes
// per the CommandLineToArgvW rules).
std::wstring quoteArg(const std::string& arg) {
    std::wstring w = widen(arg);
    bool needQuote = w.empty() || w.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needQuote) {
        return w;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// Runs the child (the discovered backend executable) with a hidden window, capturing stdout+stderr.
// Fills exitCode, lastLine and itemsDumped. `nativeParse` selects the count parser: the accumulating
// native parser vs. the Python "last N dumped" parser. Returns true iff the process spawned and exited 0.
bool runChild(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir,
              bool nativeParse, int& exitCode, std::string& lastLine, int& itemsDumped) {
    exitCode = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        gdx_port_logf("[dump] ERROR: CreatePipe failed (%lu)\n",
                      static_cast<unsigned long>(GetLastError()));
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // lpApplicationName stays NULL so a bare name (e.g. "python") resolves against PATH; the native
    // backend passes an absolute exe, so the spawn is exact either way.
    std::wstring cmd = quoteArg(exe);
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd.append(quoteArg(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    // GUI-subsystem processes often have no inherited console handle (STD_INPUT_HANDLE resolves to
    // INVALID_HANDLE_VALUE, sometimes nullptr); with STARTF_USESTDHANDLES set, handing that through
    // as-is would give the child a bogus stdin handle instead of leaving stdin unset.
    {
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == nullptr || h == INVALID_HANDLE_VALUE) {
            h = nullptr;
        }
        si.hStdInput = h;
    }

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd; // CreateProcessW may modify the buffer
    std::wstring wWorkDir = widen(workDir);
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, wWorkDir.empty() ? nullptr : wWorkDir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        gdx_port_logf("[dump] ERROR: could not launch python (Windows error %lu)\n",
                      static_cast<unsigned long>(GetLastError()));
        CloseHandle(readPipe);
        return false;
    }

    std::string acc;
    char buf[4096];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) {
            break;
        }
        acc.append(buf, n);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            std::string clean = truncateLine(line);
            if (!clean.empty()) {
                gdx_port_logf("[dump]   %s\n", clean.c_str());
                lastLine = clean;
                if (nativeParse) {
                    accumulateNativeCount(line, itemsDumped);
                } else {
                    itemsDumped = parseDumpedCount(line, itemsDumped);
                }
            }
        }
    }
    if (!acc.empty()) {
        std::string clean = truncateLine(acc);
        if (!clean.empty()) {
            gdx_port_logf("[dump]   %s\n", clean.c_str());
            lastLine = clean;
            if (nativeParse) {
                accumulateNativeCount(acc, itemsDumped);
            } else {
                itemsDumped = parseDumpedCount(acc, itemsDumped);
            }
        }
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, kChildDeadlineSecs * 1000u);
    bool timedOut = false;
    if (wait == WAIT_TIMEOUT) {
        gdx_port_logf("[dump] ERROR: class exceeded the %us deadline; terminating child\n",
                      kChildDeadlineSecs);
        TerminateProcess(pi.hProcess, 124u);
        WaitForSingleObject(pi.hProcess, 5000);
        timedOut = true;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = timedOut ? 124 : static_cast<int>(code);

    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

#else // POSIX

bool runChild(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir,
              bool nativeParse, int& exitCode, std::string& lastLine, int& itemsDumped) {
    exitCode = 1;
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        gdx_port_logf("[dump] ERROR: pipe() failed: %s\n", std::strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        gdx_port_logf("[dump] ERROR: fork() failed: %s\n", std::strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!workDir.empty()) {
            if (chdir(workDir.c_str()) != 0) {
                _exit(127);
            }
        }
        std::vector<char*> argv;
        std::string py = exe;
        argv.push_back(const_cast<char*>(py.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(py.c_str(), argv.data()); // PATH-resolved
        _exit(127);
    }
    close(pipefd[1]);
    std::string acc;
    char buf[4096];
    const time_t deadline = time(nullptr) + static_cast<time_t>(kChildDeadlineSecs);
    bool timedOut = false;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = {1, 0};
        int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0 && errno == EINTR) {
            continue;
        }
        if (sel > 0) {
            ssize_t rd = read(pipefd[0], buf, sizeof(buf));
            if (rd <= 0) {
                break;
            }
            acc.append(buf, static_cast<size_t>(rd));
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                std::string clean = truncateLine(line);
                if (!clean.empty()) {
                    gdx_port_logf("[dump]   %s\n", clean.c_str());
                    lastLine = clean;
                    itemsDumped = parseDumpedCount(line, itemsDumped);
                }
            }
        }
        if (time(nullptr) >= deadline) {
            gdx_port_logf("[dump] ERROR: class exceeded the %us deadline; killing child\n",
                          kChildDeadlineSecs);
            kill(pid, SIGKILL);
            timedOut = true;
            break;
        }
    }
    if (!acc.empty()) {
        std::string clean = truncateLine(acc);
        if (!clean.empty()) {
            gdx_port_logf("[dump]   %s\n", clean.c_str());
            lastLine = clean;
            if (nativeParse) {
                accumulateNativeCount(acc, itemsDumped);
            } else {
                itemsDumped = parseDumpedCount(acc, itemsDumped);
            }
        }
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut) {
        exitCode = 124;
    } else if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = 1;
    }
    return exitCode == 0;
}

#endif

// ── Shared batch state (UI thread reads; worker writes) ─────────────────────────────────────────────
struct BatchState {
    std::mutex mtx;
    std::atomic<bool> running{false};
    std::atomic<bool> cancelRequested{false};
    std::vector<DumpClassProgress> classes; // guarded by mtx
    std::string summary;                     // guarded by mtx
};

BatchState& batch() {
    // Leaked on purpose: the detached worker threads started below can still be writing here at
    // process exit. A function-local `static BatchState` would be destructed during CRT static
    // teardown while a detached thread races it (closing the game mid-dump).
    static BatchState* s = new BatchState;
    return *s;
}

// ── Class list (fallback, upgradeable by the --list-classes probe) ──────────────────────────────────
struct ClassListState {
    std::mutex mtx;
    std::vector<std::string> classes;
    bool initialized = false;
    std::atomic<bool> probeStarted{false};
};

ClassListState& classList() {
    // Leaked on purpose, same reason as batch() above: the detached class-list probe thread can still
    // be writing here at process exit.
    static ClassListState* s = new ClassListState;
    return *s;
}

const char* const kFallbackClasses[] = {
    "textures", "coursedata", "dlists", "vertexdata", "tables",
    "ghosts",   "fonts",      "audio",  "midi",       "models",
};

// Keeps the checkbox order stable across the fallback -> probe swap: curated kFallbackClasses names
// keep their curated rank, and anything new the probe returned is appended alphabetically. Without
// this, `sorted(...)` on the Python side (tools/gen_dump_all.py --list-classes) reshuffles the
// panel's checkboxes the moment the probe result lands, moments after the panel opened.
std::vector<std::string> ReorderProbedClasses(const std::vector<std::string>& probed) {
    std::vector<std::string> ordered;
    ordered.reserve(probed.size());
    std::vector<std::string> extras;
    for (const char* curated : kFallbackClasses) {
        for (const std::string& name : probed) {
            if (name == curated) {
                ordered.push_back(name);
                break;
            }
        }
    }
    for (const std::string& name : probed) {
        bool isCurated = false;
        for (const char* curated : kFallbackClasses) {
            if (name == curated) {
                isCurated = true;
                break;
            }
        }
        if (!isCurated) {
            // A duplicate non-curated entry in `probed` would otherwise produce a duplicate
            // checkbox; curated names are already deduped by the outer loop above.
            bool alreadyExtra = false;
            for (const std::string& existing : extras) {
                if (existing == name) {
                    alreadyExtra = true;
                    break;
                }
            }
            if (!alreadyExtra) {
                extras.push_back(name);
            }
        }
    }
    std::sort(extras.begin(), extras.end());
    ordered.insert(ordered.end(), extras.begin(), extras.end());
    return ordered;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────────────────────────────

std::vector<std::string> GdxDumpFallbackClasses() {
    std::vector<std::string> v;
    for (const char* c : kFallbackClasses) {
        v.emplace_back(c);
    }
    return v;
}

std::string GdxDumpPrettyName(const std::string& rawClass) {
    if (rawClass == "textures") return "Textures";
    if (rawClass == "coursedata") return "Course Data";
    if (rawClass == "dlists") return "Display Lists";
    if (rawClass == "vertexdata") return "Vertex Data";
    if (rawClass == "tables") return "Tables";
    if (rawClass == "ghosts") return "Ghosts";
    if (rawClass == "fonts") return "Fonts";
    if (rawClass == "audio") return "Audio Samples (WAV)";
    if (rawClass == "midi") return "Music (MIDI)";
    if (rawClass == "models") return "Models (OBJ)";
    // Unknown/extra class from --list-classes: title-case the raw token so it still reads cleanly.
    std::string s = rawClass;
    if (!s.empty()) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
    return s;
}

DumpEnvironment GdxDumpDiscover() {
    DumpEnvironment env;

    // ── Prefer the native backend: gdx-extract.exe next to the game exe. ──
    env.extractBin = findExtractBinary();
    if (!env.extractBin.empty()) {
        env.backend = DumpBackend::Native;
        env.available = true;
        // Resolve explicit source paths once (empty == omit the flag; the tool degrades gracefully).
        env.archivePath = resolveCartArchive();     // fzerox.o2r / generic.o2r, never gdiffuser.o2r
        env.diskArchivePath = resolveNextToExe(kDiskArchiveName);
        env.iplArchivePath = resolveNextToExe(kIplArchiveName);
        env.romPath = resolveNextToExe(kRomName);    // optional: the tool is archive-only capable
        fs::path recipes = exeDir() / kRecipesDirName;
        if (isDir(recipes)) {
            env.recipesDir = recipes.string();
            fs::path manifest = recipes / kEkSliceManifestName;
            if (isRegularFile(manifest)) {
                env.manifestPath = manifest.string();
            }
        }
        return env;
    }

    // ── Dev fallback: a Python interpreter (PATH) + tools/gen_dump_all.py. ──
    env.pythonExe = findPythonOnPath();
    env.toolPath = findToolPath();
    if (!env.pythonExe.empty() && !env.toolPath.empty()) {
        env.backend = DumpBackend::Python;
        env.available = true;
        return env;
    }

    env.backend = DumpBackend::None;
    env.available = false;
    env.reason = "Dump tool (gdx-extract) not found next to the game.";
    return env;
}

void GdxDumpBeginClassListProbe(const DumpEnvironment& env) {
    ClassListState& cl = classList();
    {
        std::lock_guard<std::mutex> lk(cl.mtx);
        if (!cl.initialized) {
            cl.classes = GdxDumpFallbackClasses();
            cl.initialized = true;
        }
    }
    if (!env.available) {
        return;
    }
    bool expected = false;
    if (!cl.probeStarted.compare_exchange_strong(expected, true)) {
        return; // probe already kicked once this process
    }
    DumpEnvironment envCopy = env;
    std::thread([envCopy]() {
        // Try `python <tool> --list-classes`; one class name per line, exit 0. If the flag is unknown
        // (an older tool build without the flag) the child errors and we keep the fallback list.
        int exitCode = 1, items = -1;
        std::string lastLine;
        // runChild keeps only the last output line, so this probe does its own full-stdout capture.
        std::vector<std::string> names;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rd = nullptr, wr = nullptr;
        if (CreatePipe(&rd, &wr, &sa, 0)) {
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            std::wstring cmd;
            if (envCopy.backend == DumpBackend::Native) {
                cmd = quoteArg(envCopy.extractBin) + L" " + quoteArg("dump") + L" " +
                      quoteArg("--list-classes");
            } else {
                cmd = quoteArg(envCopy.pythonExe) + L" " + quoteArg(envCopy.toolPath) + L" " +
                      quoteArg("--list-classes");
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = wr;
            si.hStdError = wr;
            // Same STARTF_USESTDHANDLES stdin-handle guard as runChild() above.
            {
                HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
                if (stdinHandle == nullptr || stdinHandle == INVALID_HANDLE_VALUE) {
                    stdinHandle = nullptr;
                }
                si.hStdInput = stdinHandle;
            }
            PROCESS_INFORMATION pi{};
            std::wstring mutableCmd = cmd;
            if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi)) {
                CloseHandle(wr);
                std::string acc;
                char buf[2048];
                DWORD n = 0;
                while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
                    acc.append(buf, n);
                }
                WaitForSingleObject(pi.hProcess, 60000);
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                exitCode = static_cast<int>(code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                size_t start = 0;
                while (start <= acc.size()) {
                    size_t nl = acc.find('\n', start);
                    std::string line = acc.substr(start, nl == std::string::npos ? std::string::npos
                                                                                 : nl - start);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                        line.pop_back();
                    }
                    if (!line.empty() && line.find(' ') == std::string::npos) {
                        names.push_back(line);
                    }
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            } else {
                CloseHandle(wr);
            }
            CloseHandle(rd);
        }
#else
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                std::vector<const char*> argv;
                if (envCopy.backend == DumpBackend::Native) {
                    argv = {envCopy.extractBin.c_str(), "dump", "--list-classes", nullptr};
                } else {
                    argv = {envCopy.pythonExe.c_str(), envCopy.toolPath.c_str(), "--list-classes",
                            nullptr};
                }
                execvp(argv[0], const_cast<char* const*>(argv.data()));
                _exit(127);
            } else if (pid > 0) {
                close(pipefd[1]);
                std::string acc;
                char buf[2048];
                ssize_t r;
                while ((r = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    acc.append(buf, static_cast<size_t>(r));
                }
                close(pipefd[0]);
                int status = 0;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                size_t start = 0;
                while (start <= acc.size()) {
                    size_t nl = acc.find('\n', start);
                    std::string line = acc.substr(start, nl == std::string::npos ? std::string::npos
                                                                                 : nl - start);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                        line.pop_back();
                    }
                    if (!line.empty() && line.find(' ') == std::string::npos) {
                        names.push_back(line);
                    }
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            } else {
                close(pipefd[0]);
                close(pipefd[1]);
            }
        }
#endif
        (void)items;
        (void)lastLine;
        if (exitCode == 0 && !names.empty()) {
            ClassListState& cl = classList();
            std::lock_guard<std::mutex> lk(cl.mtx);
            cl.classes = ReorderProbedClasses(names);
            gdx_port_logf("[dump] --list-classes upgraded the class list (%zu classes)\n", names.size());
        }
    }).detach();
}

std::vector<std::string> GdxDumpCurrentClasses() {
    ClassListState& cl = classList();
    std::lock_guard<std::mutex> lk(cl.mtx);
    if (!cl.initialized) {
        cl.classes = GdxDumpFallbackClasses();
        cl.initialized = true;
    }
    return cl.classes;
}

bool GdxDumpRunOneClass(const DumpEnvironment& env, const std::string& className,
                        const std::string& dumpDir, DumpClassProgress& out) {
    out.name = className;
    out.phase = DumpPhase::Running;
    out.exitCode = 0;
    out.itemsDumped = -1;
    out.lastLine.clear();

    const bool native = env.backend == DumpBackend::Native;
    std::string exe;
    std::vector<std::string> args;
    if (native) {
        // gdx-extract dump --classes <name> -d <dumpDir> [--archive ...] [--disk-archive ...]
        //   [--ipl-archive ...] [--recipes ...] [--manifest ...] [--rom ...]
        // An optional flag is passed only when its source resolved, so the tool never gets a dead
        // path and falls back to its graceful per-class behavior instead. --archive, when present, is
        // always the REAL cart archive, never gdiffuser.o2r.
        exe = env.extractBin;
        args = {"dump", "--classes", className, "-d", dumpDir};
        if (!env.archivePath.empty()) {
            args.push_back("--archive");
            args.push_back(env.archivePath);
        }
        if (!env.diskArchivePath.empty()) {
            args.push_back("--disk-archive");
            args.push_back(env.diskArchivePath);
        }
        if (!env.iplArchivePath.empty()) {
            args.push_back("--ipl-archive");
            args.push_back(env.iplArchivePath);
        }
        if (!env.recipesDir.empty()) {
            args.push_back("--recipes");
            args.push_back(env.recipesDir);
        }
        if (!env.manifestPath.empty()) {
            args.push_back("--manifest");
            args.push_back(env.manifestPath);
        }
        if (!env.romPath.empty()) {
            args.push_back("--rom");
            args.push_back(env.romPath);
        }
    } else {
        // Dev fallback: python <tool> --classes <name> --dump-dir <dumpDir>.
        exe = env.pythonExe;
        args = {env.toolPath, "--classes", className, "--dump-dir", dumpDir};
    }

    // The exact command line is the only record of which backend ran and what --archive resolved to.
    {
        std::string cmdline = exe;
        for (const auto& a : args) {
            cmdline += ' ';
            cmdline += a;
        }
        gdx_port_logf("[dump] %s cmd: %s\n", native ? "native" : "python", cmdline.c_str());
    }

    auto t0 = std::chrono::steady_clock::now();
    int exitCode = 1, items = -1;
    std::string lastLine;
    bool ok = runChild(exe, args, /*workDir=*/std::string(), native, exitCode, lastLine, items);
    auto t1 = std::chrono::steady_clock::now();

    out.elapsedSeconds = std::chrono::duration<double>(t1 - t0).count();
    out.exitCode = exitCode;
    out.itemsDumped = items;
    out.lastLine = lastLine;
    out.phase = ok ? DumpPhase::Done : DumpPhase::Failed;
    return ok;
}

bool GdxDumpBatchRunning() {
    return batch().running.load();
}

void GdxDumpRequestCancel() {
    batch().cancelRequested.store(true);
}

DumpBatchSnapshot GdxDumpSnapshot() {
    BatchState& b = batch();
    DumpBatchSnapshot snap;
    snap.running = b.running.load();
    snap.cancelRequested = b.cancelRequested.load();
    std::lock_guard<std::mutex> lk(b.mtx);
    snap.classes = b.classes;
    snap.summary = b.summary;
    return snap;
}

void GdxDumpStartBatch(const DumpEnvironment& env, const std::vector<std::string>& classes,
                       const std::string& dumpDir) {
    BatchState& b = batch();
    if (b.running.load() || classes.empty() || !env.available) {
        return;
    }
    bool expected = false;
    if (!b.running.compare_exchange_strong(expected, true)) {
        return; // another thread just started one
    }
    b.cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lk(b.mtx);
        b.summary.clear();
        b.classes.clear();
        b.classes.reserve(classes.size());
        for (const auto& c : classes) {
            DumpClassProgress p;
            p.name = c;
            p.phase = DumpPhase::Queued;
            b.classes.push_back(p);
        }
    }

    DumpEnvironment envCopy = env;
    std::vector<std::string> classesCopy = classes;
    std::string dumpDirCopy = dumpDir;
    std::thread([envCopy, classesCopy, dumpDirCopy]() {
        BatchState& bs = batch();
        gdx_port_logf("[dump] batch start: %zu class(es) -> %s\n", classesCopy.size(),
                      dumpDirCopy.c_str());
        int ranOk = 0, ranFail = 0, skipped = 0;
        for (size_t i = 0; i < classesCopy.size(); ++i) {
            if (bs.cancelRequested.load()) {
                // Remaining classes stay Queued; they are only counted as skipped.
                std::lock_guard<std::mutex> lk(bs.mtx);
                for (size_t j = i; j < bs.classes.size(); ++j) {
                    ++skipped;
                }
                break;
            }
            {
                std::lock_guard<std::mutex> lk(bs.mtx);
                bs.classes[i].phase = DumpPhase::Running;
            }
            DumpClassProgress result;
            // Per-class failure isolation: a throwing/failed class must never abort the batch.
            bool ok = false;
            try {
                ok = GdxDumpRunOneClass(envCopy, classesCopy[i], dumpDirCopy, result);
            } catch (...) {
                result.name = classesCopy[i];
                result.phase = DumpPhase::Failed;
                result.exitCode = -1;
                result.lastLine = "internal error running class";
            }
            gdx_port_logf("[dump] class '%s': %s (exit %d, items %d, %.1fs)\n", classesCopy[i].c_str(),
                          ok ? "done" : "FAILED", result.exitCode, result.itemsDumped,
                          result.elapsedSeconds);
            {
                std::lock_guard<std::mutex> lk(bs.mtx);
                bs.classes[i] = result;
            }
            if (ok) {
                ++ranOk;
            } else {
                ++ranFail;
            }
        }
        {
            std::lock_guard<std::mutex> lk(bs.mtx);
            char line[256];
            std::snprintf(line, sizeof(line),
                          "Batch complete: %d succeeded, %d failed%s.", ranOk, ranFail,
                          skipped > 0 ? " (canceled — remaining classes skipped)" : "");
            bs.summary = line;
        }
        gdx_port_logf("[dump] batch complete: %d ok, %d failed, %d skipped\n", ranOk, ranFail, skipped);
        bs.running.store(false);
    }).detach();
}

// ── Env-gated headless self-test ────────────────────────────────────────────────────────────────────
// GDX_DUMP_SELFTEST=<class> (or =1 for textures) runs a two-class batch at process start on a detached
// thread, covering backend discovery, child spawn + exit-code capture, and per-class failure isolation
// (the bogus class FAILs first and the real class still runs). GDX_DUMP_SELFTEST=all drives all ten
// classes through GdxDumpStartBatch instead. Never blocks the game thread; writes only into <exe>/dump.
namespace {

const char* backendName(DumpBackend b) {
    switch (b) {
    case DumpBackend::Native: return "native (gdx-extract)";
    case DumpBackend::Python: return "python fallback";
    default: return "none";
    }
}

void logEnv(const DumpEnvironment& env) {
    gdx_port_logf("[dump][selftest] backend=%s\n", backendName(env.backend));
    if (env.backend == DumpBackend::Native) {
        gdx_port_logf("[dump][selftest] extractBin='%s'\n", env.extractBin.c_str());
        gdx_port_logf("[dump][selftest] archive='%s' disk='%s' ipl='%s'\n", env.archivePath.c_str(),
                      env.diskArchivePath.c_str(), env.iplArchivePath.c_str());
        gdx_port_logf("[dump][selftest] recipes='%s' manifest='%s' rom='%s'\n", env.recipesDir.c_str(),
                      env.manifestPath.c_str(), env.romPath.c_str());
    } else {
        gdx_port_logf("[dump][selftest] python='%s' tool='%s'\n", env.pythonExe.c_str(),
                      env.toolPath.c_str());
    }
}

// Blocks this self-test thread (never the game thread) until the batch finishes. The bogus class is
// prepended so failure isolation is exercised INSIDE the batch, and so every real class's count starts
// from nothing pre-dumped.
void runFullBatchSelfTest(const DumpEnvironment& env, const std::string& dumpDir) {
    std::vector<std::string> classes;
    classes.push_back(kBogusClass);
    for (const auto& c : GdxDumpFallbackClasses()) {
        classes.push_back(c);
    }
    gdx_port_logf("[dump][selftest] starting full batch of %zu classes (bogus + 10 real) through "
                  "GdxDumpStartBatch\n", classes.size());
    GdxDumpStartBatch(env, classes, dumpDir);
    while (GdxDumpBatchRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    DumpBatchSnapshot snap = GdxDumpSnapshot();
    for (const auto& p : snap.classes) {
        gdx_port_logf("[dump][selftest] batch class '%s' -> %s (exit=%d, items=%d, %.1fs)\n",
                      p.name.c_str(),
                      p.phase == DumpPhase::Done ? "DONE"
                                                 : (p.phase == DumpPhase::Failed ? "FAILED" : "?"),
                      p.exitCode, p.itemsDumped, p.elapsedSeconds);
    }
    gdx_port_logf("[dump][selftest] batch summary: %s\n", snap.summary.c_str());
}

void runSelfTest(std::string requested) {
    if (requested.empty() || requested == "1" || requested == "true") {
        requested = "textures";
    }
    gdx_port_logf("[dump][selftest] GDX_DUMP_SELFTEST=%s — starting headless launcher self-test\n",
                  requested.c_str());
    DumpEnvironment env = GdxDumpDiscover();
    if (!env.available) {
        gdx_port_logf("[dump][selftest] environment unavailable: %s\n", env.reason.c_str());
        logEnv(env);
        return;
    }
    logEnv(env);

    std::string dumpDir = (exeDir() / "dump").string();
    std::error_code ec;
    fs::create_directories(dumpDir, ec);

    // GDX_DUMP_SELFTEST=all: run the whole registry (bogus + 10 real) through the real batch path,
    // which proves per-class isolation inside the batch and yields clean per-class counts.
    if (requested == "all") {
        runFullBatchSelfTest(env, dumpDir);
        return;
    }

    // Single-class mode: prove per-class failure isolation directly. The BOGUS class must FAIL, and the
    // requested class must STILL run afterward (one class failing does not abort the batch).
    std::string classes[2] = {kBogusClass, requested};
    int okCount = 0, failCount = 0;
    for (int i = 0; i < 2; ++i) {
        DumpClassProgress p;
        bool ok = GdxDumpRunOneClass(env, classes[i], dumpDir, p);
        gdx_port_logf("[dump][selftest] class '%s' -> %s (exit=%d, items=%d, %.1fs) last='%s'\n",
                      classes[i].c_str(), ok ? "DONE" : "FAILED", p.exitCode, p.itemsDumped,
                      p.elapsedSeconds, p.lastLine.c_str());
        if (ok) {
            ++okCount;
        } else {
            ++failCount;
        }
    }
    gdx_port_logf("[dump][selftest] isolation proof: %d ok, %d failed (expected the bogus class to fail "
                  "and the real class to still run)\n", okCount, failCount);
}

struct SelfTestTrigger {
    SelfTestTrigger() {
        const char* env = std::getenv("GDX_DUMP_SELFTEST");
        if (env == nullptr || env[0] == '\0' || env[0] == '0') {
            return;
        }
        std::string requested(env);
        std::thread([requested]() { runSelfTest(requested); }).detach();
    }
};

SelfTestTrigger gSelfTestTrigger;

} // namespace

} // namespace gdx
