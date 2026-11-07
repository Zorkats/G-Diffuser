// Copyright (c) Zorkats
//
// File dialogs a page asks for, and the external dialog programs that show them where the system has
// no dialog API the Companion links: zenity (GTK) or kdialog (KDE) on Linux and other Unix desktops.
// The program runs as a child process with no shell, and is polled once per frame so the window keeps
// drawing while the dialog is open. Windows uses its own system dialogs in gdx_companion_main.cpp.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace gdx_companion {

// A system file dialog a page asked for. The result lands in AppState::browse_results under target.
enum class BrowseKind { OpenFile, SaveFile, Folder };

struct BrowseRequest {
    std::string target;          // the page field that receives the pick
    BrowseKind kind = BrowseKind::OpenFile;
    std::string filter_name;     // for example "Texture caches"; empty for every file
    std::string filter_pattern;  // for example "*.htc;*.HTC"
    std::string start;           // UTF-8 folder or file to start in, or empty
    std::string title;
};

enum class DialogTool { None, Zenity, KDialog };

struct DialogToolChoice {
    DialogTool tool = DialogTool::None;
    std::string path;  // absolute path to the program; empty with DialogTool::None
};

const char* DialogToolName(DialogTool tool);

// The first file named name in the colon-separated search list that the user may run, as an absolute
// path, or empty. Relative entries are skipped, so the current folder is never searched. An empty list
// searches /usr/local/bin, /usr/bin and /bin.
std::string FindExecutable(const std::string& name, const std::string& search_path);

// The program for preferred ("zenity", "kdialog" or "none", from GDX_COMPANION_DIALOG) when it is
// installed. Otherwise kdialog on a KDE desktop (desktop is XDG_CURRENT_DESKTOP, a colon-separated
// list), then zenity, then kdialog. Either path may be empty when that program is missing.
DialogToolChoice PickDialogTool(const std::string& preferred, const std::string& desktop,
                                const std::string& zenity_path, const std::string& kdialog_path);

// The space-separated patterns of a filter such as "*.htc;*.HTC", or empty when pattern names none.
std::string FilterPatterns(const std::string& pattern);

// The argument vector that shows request with choice's program. A request with no start begins in home.
// Folders start inside the folder; files start beside the file, with its name filled in for a save.
std::vector<std::string> BuildDialogArgv(const DialogToolChoice& choice, const BrowseRequest& request,
                                         const std::string& home);

enum class DialogOutcome { Running, Picked, Cancelled, Failed };

// What a finished dialog program returned: exit code 0 with a path on standard output is a pick, exit
// code 1 is a cancel, anything else a failure with error set. Trailing line breaks are removed.
DialogOutcome ParseDialogResult(int exit_code, const std::string& output, std::string& picked,
                                std::string& error);

// One dialog program at a time. The destructor ends a dialog that is still open.
class DialogRunner {
public:
    DialogRunner();
    ~DialogRunner();
    DialogRunner(const DialogRunner&) = delete;
    DialogRunner& operator=(const DialogRunner&) = delete;

    bool Start(const std::vector<std::string>& argv, std::string& error);

    // Reads the program's output without blocking. Returns Running while it is open (and while the
    // runner is idle), then the outcome once; after that Start may be called again.
    DialogOutcome Poll(std::string& picked, std::string& error);

    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gdx_companion
