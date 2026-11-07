// Copyright (c) Zorkats

#include "gdx_companion_dialog.h"

#include <cctype>
#include <filesystem>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "gdx_companion_paths.h"
#include "gdx_companion_process.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr char kSearchSeparator = ';';
#else
constexpr char kSearchSeparator = ':';
#endif

bool IsRunnableFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
#if defined(_WIN32)
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

bool ContainsKde(const std::string& desktop) {
    size_t begin = 0;
    while (begin <= desktop.size()) {
        size_t end = desktop.find(':', begin);
        if (end == std::string::npos) end = desktop.size();
        std::string entry = desktop.substr(begin, end - begin);
        for (char& c : entry) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (entry == "KDE") return true;
        begin = end + 1;
    }
    return false;
}

// The folder a dialog starts in and the file name it fills in, from the request's start.
void SplitStart(const BrowseRequest& request, const std::string& home, std::string& folder, std::string& name) {
    folder.clear();
    name.clear();
    const std::string& start = request.start.empty() ? home : request.start;
    if (start.empty()) return;
    const fs::path path = PathFromUtf8(start);
    std::error_code ec;
    if (request.kind == BrowseKind::Folder || fs::is_directory(path, ec)) {
        folder = start;
        return;
    }
    folder = PathToUtf8(path.parent_path());
    name = PathToUtf8(path.filename());
}

std::string WithTrailingSlash(const std::string& folder) {
    if (folder.empty() || folder.back() == '/') return folder;
    return folder + "/";
}

std::string FirstLine(const std::string& text) {
    const size_t end = text.find('\n');
    std::string line = text.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() > 200) line.resize(200);
    return line;
}

}  // namespace

const char* DialogToolName(DialogTool tool) {
    switch (tool) {
        case DialogTool::Zenity:
            return "zenity";
        case DialogTool::KDialog:
            return "kdialog";
        case DialogTool::None:
            break;
    }
    return "none";
}

std::string FindExecutable(const std::string& name, const std::string& search_path) {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return std::string();
    }
    const std::string list = search_path.empty() ? std::string("/usr/local/bin:/usr/bin:/bin") : search_path;
    const char separator = search_path.empty() ? ':' : kSearchSeparator;
    size_t begin = 0;
    while (begin <= list.size()) {
        size_t end = list.find(separator, begin);
        if (end == std::string::npos) end = list.size();
        const std::string entry = list.substr(begin, end - begin);
        begin = end + 1;
        if (entry.empty()) continue;
        const fs::path folder = PathFromUtf8(entry);
        if (!folder.is_absolute()) continue;
        const fs::path candidate = folder / PathFromUtf8(name);
        if (IsRunnableFile(candidate)) return PathToUtf8(candidate);
    }
    return std::string();
}

DialogToolChoice PickDialogTool(const std::string& preferred, const std::string& desktop,
                                const std::string& zenity_path, const std::string& kdialog_path) {
    if (preferred == "none") return {};
    if (preferred == "zenity" && !zenity_path.empty()) return {DialogTool::Zenity, zenity_path};
    if (preferred == "kdialog" && !kdialog_path.empty()) return {DialogTool::KDialog, kdialog_path};
    if (!kdialog_path.empty() && ContainsKde(desktop)) return {DialogTool::KDialog, kdialog_path};
    if (!zenity_path.empty()) return {DialogTool::Zenity, zenity_path};
    if (!kdialog_path.empty()) return {DialogTool::KDialog, kdialog_path};
    return {};
}

std::string FilterPatterns(const std::string& pattern) {
    std::string out;
    size_t begin = 0;
    while (begin <= pattern.size()) {
        size_t end = pattern.find(';', begin);
        if (end == std::string::npos) end = pattern.size();
        std::string entry = pattern.substr(begin, end - begin);
        begin = end + 1;
        while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
        while (!entry.empty() && entry.back() == ' ') entry.pop_back();
        if (entry.empty()) continue;
        if (!out.empty()) out += ' ';
        out += entry;
    }
    return out;
}

std::vector<std::string> BuildDialogArgv(const DialogToolChoice& choice, const BrowseRequest& request,
                                         const std::string& home) {
    std::vector<std::string> argv;
    if (choice.tool == DialogTool::None || choice.path.empty()) return argv;
    std::string folder;
    std::string name;
    SplitStart(request, home, folder, name);
    const std::string patterns = request.kind == BrowseKind::Folder ? std::string() : FilterPatterns(request.filter_pattern);
    const std::string filter_name = request.filter_name.empty() ? std::string("Files") : request.filter_name;

    argv.push_back(choice.path);
    if (choice.tool == DialogTool::Zenity) {
        argv.push_back("--file-selection");
        if (!request.title.empty()) argv.push_back("--title=" + request.title);
        if (request.kind == BrowseKind::Folder) argv.push_back("--directory");
        if (request.kind == BrowseKind::SaveFile) {
            argv.push_back("--save");
            argv.push_back("--confirm-overwrite");
        }
        if (!folder.empty()) {
            const bool fill_name = !name.empty() && request.kind == BrowseKind::SaveFile;
            argv.push_back("--filename=" + WithTrailingSlash(folder) + (fill_name ? name : std::string()));
        }
        if (!patterns.empty()) {
            argv.push_back("--file-filter=" + filter_name + " | " + patterns);
            argv.push_back("--file-filter=All files | *");
        }
        return argv;
    }

    // kdialog: the title option goes first, then the mode, its start path and its filter.
    if (!request.title.empty()) {
        argv.push_back("--title");
        argv.push_back(request.title);
    }
    std::string start = folder.empty() ? std::string(".") : folder;
    if (request.kind == BrowseKind::Folder) {
        argv.push_back("--getexistingdirectory");
        argv.push_back(start);
        return argv;
    }
    if (request.kind == BrowseKind::SaveFile && !name.empty()) start = WithTrailingSlash(start) + name;
    argv.push_back(request.kind == BrowseKind::SaveFile ? "--getsavefilename" : "--getopenfilename");
    argv.push_back(start);
    if (!patterns.empty()) argv.push_back(patterns + "|" + filter_name);
    return argv;
}

DialogOutcome ParseDialogResult(int exit_code, const std::string& output, std::string& picked,
                                std::string& error) {
    picked.clear();
    error.clear();
    if (exit_code == 1) return DialogOutcome::Cancelled;
    if (exit_code != 0) {
        error = "the file dialog exited with code " + std::to_string(exit_code);
        return DialogOutcome::Failed;
    }
    std::string path = output;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (path.empty()) return DialogOutcome::Cancelled;
    if (path.find('\n') != std::string::npos) {
        error = "the file dialog returned more than one path";
        return DialogOutcome::Failed;
    }
    picked = path;
    return DialogOutcome::Picked;
}

struct DialogRunner::Impl {
    std::unique_ptr<ChildProcess> process;
    std::string out;
    std::string err;
};

DialogRunner::DialogRunner() : impl_(std::make_unique<Impl>()) {}

DialogRunner::~DialogRunner() = default;  // ChildProcess ends a dialog that still runs

bool DialogRunner::Start(const std::vector<std::string>& argv, std::string& error) {
    if (running()) {
        error = "a file dialog is already open";
        return false;
    }
    if (argv.empty()) {
        error = "no file dialog program";
        return false;
    }
    auto process = std::make_unique<ChildProcess>();
    if (!process->Start(argv, error)) return false;
    process->CloseStdin();
    impl_->out.clear();
    impl_->err.clear();
    impl_->process = std::move(process);
    return true;
}

DialogOutcome DialogRunner::Poll(std::string& picked, std::string& error) {
    picked.clear();
    error.clear();
    if (!running()) return DialogOutcome::Running;
    ChildProcess& process = *impl_->process;
    process.ReadStdout(impl_->out);
    process.ReadStderr(impl_->err);
    if (!process.HasExited()) return DialogOutcome::Running;
    // The child has exited, so what is left in the pipes is all there is.
    while (process.ReadStdout(impl_->out) > 0) {
    }
    while (process.ReadStderr(impl_->err) > 0) {
    }
    const int code = process.exit_code();
    impl_->process.reset();
    const DialogOutcome outcome = ParseDialogResult(code, impl_->out, picked, error);
    if (outcome == DialogOutcome::Failed) {
        const std::string detail = FirstLine(impl_->err);
        if (!detail.empty()) error += ": " + detail;
    }
    return outcome;
}

bool DialogRunner::running() const { return impl_->process != nullptr; }

}  // namespace gdx_companion
