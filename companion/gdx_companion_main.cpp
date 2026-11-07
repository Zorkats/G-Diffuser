// Copyright (c) Zorkats
//
// G-Diffuser Companion entry point.
//
//   G-Diffuser-Companion [--python <interpreter> --worker <worker.py>] [--selftest] [--theme <id>]
//                        [--section <music|textures|models|tracks|inspect|advanced|settings>]
//
// --selftest runs the worker handshake and shutdown without a window, prints the result, and exits
// 0 on success or 1 on failure. Without it, a window opens and the worker starts in the background.
// Without --python and --worker, the frozen worker beside the executable is used. --theme starts with
// that theme id (for example user.big_blue) when it exists; the theme is not saved, so this is how a
// shortcut keeps one. --section opens on that workspace instead of Settings. The only setting the
// Companion saves is the game folder chosen in Settings, in companion-settings.json beside the executable.
//
// Outside Windows, Browse runs zenity or kdialog (see gdx_companion_dialog.h). GDX_COMPANION_DIALOG set to
// zenity, kdialog or none picks one; with neither installed, paths are typed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <shobjidl.h>
#include <cwchar>
#endif

#include <SDL.h>
#include <SDL_opengl.h>
#if defined(_WIN32)
#include <SDL_syswm.h>
#endif

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include "gdx_companion_backdrop.h"
#include "gdx_companion_dialog.h"
#include "gdx_companion_install.h"
#include "gdx_companion_paths.h"
#include "gdx_companion_preview.h"
#include "gdx_companion_protocol.h"
#include "gdx_companion_state.h"
#include "gdx_companion_theme.h"
#include "gdx_companion_themes.h"
#include "gdx_companion_ui.h"
#include "gdx_companion_worker.h"

namespace fs = std::filesystem;
using namespace gdx_companion;

namespace {

constexpr const char* kUsage =
    "usage: G-Diffuser-Companion [--selftest] [--python <interpreter> --worker <worker.py>] [--theme <id>]\n"
    "                            [--section <music|textures|models|tracks|inspect|advanced|settings>]\n";

// The shortest frame, about 125 frames a second; vsync normally paces the loop at the display rate.
constexpr Uint64 kMinFrameMs = 8;

struct Options {
    bool selftest = false;
    std::string theme;
    Section section = Section::Settings;
    WorkerLaunchOptions worker;
};

#if defined(_WIN32)
// Pairs of name and pattern, ending with an empty pair (the literal's own terminator).
constexpr const wchar_t kGameFilter[] = L"G-Diffuser (G-Diffuser.exe)\0G-Diffuser.exe\0Programs (*.exe)\0*.exe\0";

std::string WideToUtf8(const wchar_t* text) {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<size_t>(needed - 1));
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// The GUI subsystem has no standard output unless the launcher redirected it. Attach to the parent
// console for the headless paths only, and only when no output handle exists.
void AttachConsoleForOutput() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}
#endif

// Arguments as UTF-8. On Windows the wide command line is used, so non-ASCII paths survive.
std::vector<std::string> CommandLineArguments(int argc, char** argv) {
    std::vector<std::string> out;
#if defined(_WIN32)
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int i = 0; i < count; ++i) out.push_back(WideToUtf8(wide[i]));
        LocalFree(wide);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

bool ParseArguments(const std::vector<std::string>& args, Options& out, std::string& error) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--selftest") {
            out.selftest = true;
        } else if (arg == "--theme") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                error = "--theme needs a theme id";
                return false;
            }
            out.theme = args[++i];
        } else if (arg == "--section") {
            const std::string name = i + 1 < args.size() ? args[++i] : std::string();
            static const std::pair<const char*, Section> kNames[] = {
                {"music", Section::Music},     {"textures", Section::Textures}, {"models", Section::Models},
                {"tracks", Section::Tracks},   {"inspect", Section::Inspect},   {"advanced", Section::Advanced},
                {"settings", Section::Settings},
            };
            bool found = false;
            for (const auto& entry : kNames) {
                if (name == entry.first) {
                    out.section = entry.second;
                    found = true;
                }
            }
            if (!found) {
                error = "--section needs one of music, textures, models, tracks, inspect, advanced, settings";
                return false;
            }
        } else if (arg == "--python" || arg == "--worker") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                error = arg + " needs a path";
                return false;
            }
            std::string& target = arg == "--python" ? out.worker.python : out.worker.worker_script;
            target = args[++i];
        } else {
            error = "unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

std::string JoinOps(const std::vector<std::string>& ops) {
    std::string out;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) out += ",";
        out += ops[i];
    }
    return out;
}

const char* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

// Rebuilds the theme list from the themes folder. The active theme keeps its place when its id is
// still there; otherwise Circuit becomes active.
void ReloadThemes(AppState& app, const fs::path& dir) {
    const std::string keep = app.themes.empty() ? std::string() : app.themes[app.theme_index].id;
    app.themes.clear();
    app.themes.push_back(CircuitTheme());
    ThemeScanResult scan = ScanUserThemes(dir);
    for (const std::string& note : scan.notes) app.log.Append(LogLevel::Warning, note);
    for (ThemeTokens& theme : scan.themes) app.themes.push_back(std::move(theme));
    app.log.Append(LogLevel::Info, std::to_string(app.themes.size() - 1) + " user theme(s) in " + app.themes_dir);
    app.theme_index = 0;
    for (size_t i = 0; i < app.themes.size(); ++i) {
        if (app.themes[i].id == keep) app.theme_index = i;
    }
}

// Loads the theme's backdrop, settles its accent, and recolours the UI. A theme that fails its
// contrast check is refused and the current theme stays.
bool ActivateTheme(AppState& app, size_t index, Backdrop& backdrop, CompanionUi& ui) {
    if (index >= app.themes.size()) return false;
    ThemeTokens theme = app.themes[index];
    Backdrop next;
    if (!theme.backdrop_path.empty()) {
        Rgb8 dominant{};
        std::string error;
        if (next.Load(theme.backdrop_path, theme.pixel_art, theme.accent, dominant, error)) {
            if (theme.accent_from_art) theme.accent = dominant;
        } else {
            app.log.Append(LogLevel::Warning, "theme " + theme.id + ": backdrop not shown: " + error);
        }
    }
    std::string note;
    std::string error;
    if (!FinishTheme(theme, note, error)) {
        app.log.Append(LogLevel::Error, "theme " + theme.id + " not applied: " + error);
        return false;
    }
    if (!note.empty()) app.log.Append(LogLevel::Info, "theme " + theme.id + ": " + note);
    theme.accent_from_art = false;  // settled; switching back reuses it
    app.themes[index] = theme;
    app.theme_index = index;
    backdrop.Swap(next);  // next now holds the old image and frees it on return
    ui.SetTheme(theme);
    app.log.Append(LogLevel::Info, "theme: " + theme.display_name + " (" + theme.id + ")");
    return true;
}

// Shows dir in the system file manager. what names it in errors. create makes the folder first.
void OpenFolder(AppState& app, const fs::path& dir, const std::string& what, bool create) {
    std::error_code ec;
    if (create) fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        app.log.Append(LogLevel::Error, "cannot open " + what + (ec ? ": " + ec.message() : ": it does not exist"));
        return;
    }
    // file:///C:/... on Windows, file:///home/... elsewhere.
    const std::u8string generic = dir.generic_u8string();
    const std::string path(reinterpret_cast<const char*>(generic.data()), generic.size());
    const std::string url = path.rfind('/', 0) == 0 ? "file://" + path : "file:///" + path;
    if (SDL_OpenURL(url.c_str()) != 0) {
        app.log.Append(LogLevel::Error, "cannot open " + what + ": " + SDL_GetError());
    }
}

void ApplyInstallCheck(AppState& app, const InstallCheck& check) {
    app.installation = check.state;
    app.installation_detail = check.detail;
    app.installation_dir = check.game_dir.empty() ? std::string() : PathToUtf8(check.game_dir);
    app.log.Append(check.state == InstallState::Validated ? LogLevel::Info : LogLevel::Warning, check.detail);
}

void LoadInstallation(AppState& app, const fs::path& exe_dir) {
    fs::path saved;
    std::string error;
    if (!ReadSavedInstallation(InstallSettingsPath(exe_dir), saved, error)) {
        app.log.Append(LogLevel::Warning, "the saved game folder was ignored: " + error);
    }
    app.installation_chosen = !saved.empty();
    ApplyInstallCheck(app, CheckInstallation(exe_dir, saved));
}

// Uses the picked executable or folder, saves it, and checks it again.
void ChooseInstallation(AppState& app, const fs::path& exe_dir, const fs::path& picked) {
    fs::path dir;
    std::string error;
    if (!InstallationFromChoice(picked, dir, error)) {
        app.installation_error = "Not used: " + error + ".";
        app.log.Append(LogLevel::Warning, "game folder not used: " + error);
        return;
    }
    app.installation_error.clear();
    if (WriteSavedInstallation(InstallSettingsPath(exe_dir), dir, error)) {
        app.log.Append(LogLevel::Info, "game folder saved: " + PathToUtf8(dir));
    } else {
        app.log.Append(LogLevel::Warning, "game folder used until the Companion closes, not saved: " + error);
    }
    app.installation_chosen = true;
    ApplyInstallCheck(app, CheckInstallation(exe_dir, dir));
}

void ResetInstallation(AppState& app, const fs::path& exe_dir) {
    std::string error;
    if (!WriteSavedInstallation(InstallSettingsPath(exe_dir), fs::path(), error)) {
        app.log.Append(LogLevel::Warning, "the saved game folder was not removed: " + error);
    }
    app.installation_chosen = false;
    app.installation_error.clear();
    ApplyInstallCheck(app, CheckInstallation(exe_dir, fs::path()));
}

#if defined(_WIN32)
// The system file picker, owned by the Companion window. Returns false when the user cancels.
bool BrowseForGame(AppState& app, SDL_Window* window, const fs::path& start, fs::path& picked) {
    HWND owner = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) == SDL_TRUE && info.subsystem == SDL_SYSWM_WINDOWS) {
        owner = info.info.win.window;
    }
    std::wstring buffer(32768, L' ');
    buffer[0] = L'\0';
    const std::wstring initial = start.wstring();
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = kGameFilter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.lpstrTitle = L"Choose G-Diffuser.exe";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD code = CommDlgExtendedError();
        if (code != 0) {
            app.log.Append(LogLevel::Error, "the file picker failed, code " + std::to_string(code));
        }
        return false;
    }
    picked = fs::path(std::wstring(buffer.c_str()));
    return true;
}
#endif

#if defined(_WIN32)
HWND OwnerWindow(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) == SDL_TRUE && info.subsystem == SDL_SYSWM_WINDOWS) {
        return info.info.win.window;
    }
    return nullptr;
}

// The extension of the first "*.ext" in pattern, for the save dialog's default, or empty.
std::wstring DefaultExtension(const std::string& pattern) {
    if (pattern.rfind("*.", 0) != 0) return std::wstring();
    const size_t end = pattern.find(';');
    const std::string ext = pattern.substr(2, end == std::string::npos ? std::string::npos : end - 2);
    if (ext.empty() || ext.find('*') != std::string::npos) return std::wstring();
    return Utf8ToWide(ext);
}

// A page's file or folder dialog (IFileOpenDialog or IFileSaveDialog), owned by the Companion window.
// Returns false when the user cancels or the dialog cannot open.
bool BrowseWithDialog(AppState& app, SDL_Window* window, const BrowseRequest& request, fs::path& picked) {
    const bool save = request.kind == BrowseKind::SaveFile;
    IFileDialog* dialog = nullptr;
    HRESULT hr = E_FAIL;
    if (save) {
        IFileSaveDialog* save_dialog = nullptr;
        hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&save_dialog));
        dialog = save_dialog;
    } else {
        IFileOpenDialog* open_dialog = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&open_dialog));
        dialog = open_dialog;
    }
    if (FAILED(hr) || dialog == nullptr) {
        app.log.Append(LogLevel::Error, "the file dialog did not open");
        return false;
    }
    FILEOPENDIALOGOPTIONS flags = 0;
    dialog->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    if (request.kind == BrowseKind::Folder) flags |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
    if (request.kind == BrowseKind::OpenFile) flags |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (save) flags |= FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
    dialog->SetOptions(flags);

    const std::wstring filter_name = Utf8ToWide(request.filter_name.empty() ? "Files" : request.filter_name);
    const std::wstring filter_pattern = Utf8ToWide(request.filter_pattern);
    if (request.kind != BrowseKind::Folder && !filter_pattern.empty()) {
        const COMDLG_FILTERSPEC specs[] = {{filter_name.c_str(), filter_pattern.c_str()}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, specs);
        dialog->SetFileTypeIndex(1);
        if (save) {
            const std::wstring ext = DefaultExtension(request.filter_pattern);
            if (!ext.empty()) dialog->SetDefaultExtension(ext.c_str());
        }
    }
    if (!request.title.empty()) dialog->SetTitle(Utf8ToWide(request.title).c_str());

    if (!request.start.empty()) {
        const fs::path start = PathFromUtf8(request.start);
        std::error_code ec;
        fs::path folder = start;
        if (!fs::is_directory(start, ec)) {
            folder = start.parent_path();
            if (save && !start.filename().empty()) dialog->SetFileName(start.filename().wstring().c_str());
        }
        if (!folder.empty() && fs::is_directory(folder, ec)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dialog->SetFolder(item);
                item->Release();
            }
        }
    }

    bool ok = false;
    hr = dialog->Show(OwnerWindow(window));
    if (SUCCEEDED(hr)) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result))) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide != nullptr) {
                picked = fs::path(std::wstring(wide));
                CoTaskMemFree(wide);
                ok = true;
            }
            result->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        app.log.Append(LogLevel::Error, "the file dialog failed");
    }
    dialog->Release();
    return ok;
}
#endif

#if !defined(_WIN32)
// Browse outside Windows: one zenity or kdialog process at a time, polled each frame.
constexpr const char* kInstallTarget = "settings.installation";

struct ExternalDialogs {
    DialogToolChoice choice;
    std::string home;
    DialogRunner runner;
    std::string target;  // the request the open dialog answers
};

std::string EnvironmentText(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

void FindExternalDialog(AppState& app, ExternalDialogs& dialogs) {
    const std::string search = EnvironmentText("PATH");
    dialogs.choice = PickDialogTool(EnvironmentText("GDX_COMPANION_DIALOG"), EnvironmentText("XDG_CURRENT_DESKTOP"),
                                    FindExecutable("zenity", search), FindExecutable("kdialog", search));
    dialogs.home = EnvironmentText("HOME");
    app.browse_available = dialogs.choice.tool != DialogTool::None;
    if (app.browse_available) {
        app.log.Append(LogLevel::Info, std::string("file dialogs use ") + dialogs.choice.path);
    } else {
        app.log.Append(LogLevel::Warning, "file dialogs are off: install zenity or kdialog, or type the paths");
    }
}

// Opens the dialog a page or Settings asked for, and hands over what the open one returned.
void PumpExternalDialog(AppState& app, ExternalDialogs& dialogs, const fs::path& exe_dir) {
    std::optional<BrowseRequest> request;
    if (app.install_browse) {
        app.install_browse = false;
        BrowseRequest install;
        install.target = kInstallTarget;
        install.kind = BrowseKind::Folder;
        install.start = app.installation_dir.empty() ? PathToUtf8(exe_dir.parent_path()) : app.installation_dir;
        install.title = "Choose the G-Diffuser folder";
        request = install;
    } else if (app.browse.has_value()) {
        request = *app.browse;
        app.browse.reset();
    }
    if (request.has_value()) {
        if (dialogs.runner.running()) {
            app.log.Append(LogLevel::Info, "a file dialog is already open");
        } else if (app.browse_available) {
            std::string error;
            if (dialogs.runner.Start(BuildDialogArgv(dialogs.choice, *request, dialogs.home), error)) {
                dialogs.target = request->target;
            } else {
                app.log.Append(LogLevel::Error, "the file dialog did not open: " + error);
            }
        }
    }
    if (!dialogs.runner.running()) return;
    std::string picked;
    std::string error;
    switch (dialogs.runner.Poll(picked, error)) {
        case DialogOutcome::Running:
        case DialogOutcome::Cancelled:
            break;
        case DialogOutcome::Picked:
            if (dialogs.target == kInstallTarget) {
                ChooseInstallation(app, exe_dir, PathFromUtf8(picked));
            } else {
                app.browse_results[dialogs.target] = picked;
            }
            break;
        case DialogOutcome::Failed:
            app.log.Append(LogLevel::Error, error);
            break;
    }
}
#endif

// Starts the worker; on failure the worker status says why.
void StartWorker(AppState& app, WorkerSession& session, const Options& options, const fs::path& exe_dir) {
    std::vector<std::string> argv;
    std::string description;
    std::string error;
    if (ResolveWorkerLaunch(options.worker, exe_dir, argv, description, error)) {
        session.Start(argv, description);
    } else {
        app.worker.state = WorkerState::Failed;
        app.worker.detail = error;
        app.log.Append(LogLevel::Error, "worker not started: " + error);
    }
}

// Carries out what the pages asked for this frame: dialogs, jobs, cancel and restart.
void HandlePageRequests(AppState& app, WorkerSession& session, const Options& options, const fs::path& exe_dir,
                        SDL_Window* window) {
#if defined(_WIN32)
    if (app.browse.has_value()) {
        const BrowseRequest request = *app.browse;
        app.browse.reset();
        fs::path picked;
        if (BrowseWithDialog(app, window, request, picked)) app.browse_results[request.target] = PathToUtf8(picked);
    }
#else
    // PumpExternalDialog has already taken any request.
    (void)window;
#endif
    if (app.job_cancel) {
        app.job_cancel = false;
        session.CancelJob();
    }
    if (app.worker_restart) {
        app.worker_restart = false;
        if (app.worker.state == WorkerState::Stopped || app.worker.state == WorkerState::Failed) {
            StartWorker(app, session, options, exe_dir);
        }
    }
    if (app.job_request.has_value()) {
        PageJobRequest request = std::move(*app.job_request);
        app.job_request.reset();
        std::string error;
        if (session.StartJob(request.request, error)) {
            app.job_owner = request.owner;
            app.job_request_error.clear();
            app.job_request_error_owner.clear();
        } else {
            app.job_request_error = error;
            app.job_request_error_owner = request.owner;
            app.log.Append(LogLevel::Warning, request.owner + ": job not started: " + error);
        }
    }
}

int RunSelfTest(const Options& options, const fs::path& exe_dir) {
    AppState app;
    std::vector<std::string> argv;
    std::string description;
    std::string error;
    if (!ResolveWorkerLaunch(options.worker, exe_dir, argv, description, error)) {
        std::printf("selftest: FAIL %s\n", error.c_str());
        return 1;
    }
    std::printf("selftest: launching %s\n", description.c_str());

    WorkerSession session(app);
    session.Start(argv, description);
    while (app.worker.state == WorkerState::Starting) {
        session.Poll();
        if (app.worker.state == WorkerState::Starting) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool ok = app.worker.state == WorkerState::Ready;
    if (ok) {
        std::printf("selftest: hello ok worker_version=%s protocol=%lld ops=%s\n",
                    app.worker.version.c_str(), static_cast<long long>(kProtocolVersion),
                    JoinOps(app.worker.ops).c_str());
    } else {
        std::printf("selftest: hello FAILED: %s\n", app.worker.detail.c_str());
    }

    session.Shutdown();
    if (ok) {
        ok = app.worker.state == WorkerState::Stopped;
        if (ok) {
            std::printf("selftest: shutdown ok\n");
        } else {
            std::printf("selftest: shutdown FAILED: %s\n", app.worker.detail.c_str());
        }
    }

    for (const LogEntry& entry : app.log.entries()) {
        std::printf("log %s: %s\n", LevelName(entry.level), entry.text.c_str());
    }
    std::printf(ok ? "selftest: PASS\n" : "selftest: FAIL\n");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

int RunWindow(const Options& options, const fs::path& exe_dir) {
    AppState app;
    app.log.Append(LogLevel::Info, "companion started");
    app.exe_dir = PathToUtf8(exe_dir);
    app.section = options.section;
    LoadInstallation(app, exe_dir);
#if defined(_WIN32)
    // The file dialogs are COM objects that need a single-threaded apartment on this thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    app.browse_available = SUCCEEDED(com);
    if (!app.browse_available) app.log.Append(LogLevel::Warning, "file dialogs are off: COM did not start");
#else
    ExternalDialogs dialogs;
    FindExternalDialog(app, dialogs);
#endif

    WorkerSession session(app);
    StartWorker(app, session, options, exe_dir);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        app.log.Append(LogLevel::Error, std::string("SDL_Init failed: ") + SDL_GetError());
        session.Shutdown();
        std::fprintf(stderr, "G-Diffuser Companion: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // Audio is only for the music preview, so the Companion runs without it.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        app.preview.available = true;
    } else {
        app.log.Append(LogLevel::Warning, std::string("music preview is off: ") + SDL_GetError());
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window* window =
        SDL_CreateWindow("G-Diffuser Companion", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 720,
                         window_flags);
    SDL_GLContext gl = window != nullptr ? SDL_GL_CreateContext(window) : nullptr;
    if (window == nullptr || gl == nullptr) {
        const std::string message = std::string("window or OpenGL context failed: ") + SDL_GetError();
        app.log.Append(LogLevel::Error, message);
        session.Shutdown();
        std::fprintf(stderr, "G-Diffuser Companion: %s\n", message.c_str());
        if (gl != nullptr) SDL_GL_DeleteContext(gl);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowMinimumSize(window, 720, 480);

    const fs::path themes_dir = UserThemesDirectory(exe_dir);
    app.themes_dir = PathToUtf8(themes_dir);
    ReloadThemes(app, themes_dir);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // ImGui writes no layout file
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplSDL2_InitForOpenGL(window, gl);
        ImGui_ImplOpenGL3_Init("#version 150");

        CompanionUi ui(app.themes[0]);
        Backdrop backdrop;
        AudioPreview preview;
        if (!options.theme.empty()) {
            bool found = false;
            for (size_t i = 0; i < app.themes.size() && !found; ++i) {
                if (app.themes[i].id == options.theme) {
                    found = true;
                    ActivateTheme(app, i, backdrop, ui);
                }
            }
            if (!found) app.log.Append(LogLevel::Warning, "--theme " + options.theme + ": no theme has that id");
        }
        bool running = true;
        const Uint64 ticks_per_second = SDL_GetPerformanceFrequency();
        while (running) {
            const Uint64 frame_start = SDL_GetPerformanceCounter();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(window)) {
                    running = false;
                }
            }

            session.Poll();

            if (app.theme_rescan) {
                app.theme_rescan = false;
                const std::string active = app.themes[app.theme_index].id;
                ReloadThemes(app, themes_dir);
                if (app.themes[app.theme_index].id != active || !ActivateTheme(app, app.theme_index, backdrop, ui)) {
                    ActivateTheme(app, 0, backdrop, ui);
                }
            }
            if (app.theme_request >= 0) {
                const size_t request = static_cast<size_t>(app.theme_request);
                app.theme_request = -1;
                ActivateTheme(app, request, backdrop, ui);
            }
            if (app.theme_open_folder) {
                app.theme_open_folder = false;
                OpenFolder(app, themes_dir, "the themes folder", true);
            }
            if (!app.open_folder.empty()) {
                const std::string folder = app.open_folder;
                app.open_folder.clear();
                OpenFolder(app, PathFromUtf8(folder), folder, false);
            }
#if defined(_WIN32)
            if (app.install_browse) {
                app.install_browse = false;
                const fs::path start =
                    app.installation_dir.empty() ? exe_dir.parent_path() : PathFromUtf8(app.installation_dir);
                fs::path picked;
                if (BrowseForGame(app, window, start, picked)) ChooseInstallation(app, exe_dir, picked);
            }
#else
            PumpExternalDialog(app, dialogs, exe_dir);
#endif
            if (!app.install_choice.empty()) {
                const std::string choice = app.install_choice;
                app.install_choice.clear();
                ChooseInstallation(app, exe_dir, PathFromUtf8(choice));
            }
            if (app.install_reset) {
                app.install_reset = false;
                ResetInstallation(app, exe_dir);
            }
            HandlePageRequests(app, session, options, exe_dir, window);
            if (app.preview_request.has_value()) {
                const PreviewRequest request = *app.preview_request;
                app.preview_request.reset();
                preview.Handle(request, app.preview);
            } else {
                preview.Pump(app.preview);
            }
            const ThemeTokens& theme = app.themes[app.theme_index];

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            {
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                backdrop.Draw(ImGui::GetBackgroundDrawList(), viewport->Pos,
                              ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
            }
            ui.Draw(app);
            ImGui::Render();

            int width = 0;
            int height = 0;
            SDL_GL_GetDrawableSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(static_cast<float>(theme.backdrop.r) / 255.0f,
                         static_cast<float>(theme.backdrop.g) / 255.0f,
                         static_cast<float>(theme.backdrop.b) / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(window);
            // Vsync does not always block (XWayland, a hidden or minimized window), and then the loop
            // would spin a whole core. Each frame takes at least kMinFrameMs.
            const Uint64 elapsed_ms = (SDL_GetPerformanceCounter() - frame_start) * 1000 / ticks_per_second;
            if (elapsed_ms < kMinFrameMs) SDL_Delay(static_cast<Uint32>(kMinFrameMs - elapsed_ms));
        }

        preview.Stop();
        session.Shutdown();
        backdrop.Clear();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
    }
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
#if defined(_WIN32)
    if (SUCCEEDED(com)) CoUninitialize();
#endif
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = CommandLineArguments(argc, argv);
    Options options;
    std::string error;
    if (!ParseArguments(args, options, error)) {
#if defined(_WIN32)
        AttachConsoleForOutput();
#endif
        std::fprintf(stderr, "G-Diffuser Companion: %s\n%s", error.c_str(), kUsage);
        return 2;
    }

    std::string dir_error;
    const fs::path exe_dir = ExecutableDirectory(dir_error);
    if (exe_dir.empty()) {
        std::fprintf(stderr, "G-Diffuser Companion: %s\n", dir_error.c_str());
        return 1;
    }

    if (options.selftest) {
#if defined(_WIN32)
        AttachConsoleForOutput();
#endif
        return RunSelfTest(options, exe_dir);
    }
    return RunWindow(options, exe_dir);
}
