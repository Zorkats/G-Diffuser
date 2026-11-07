// Copyright (c) Zorkats
//
// Headless tests for the external file dialogs: finding zenity and kdialog on a search list, picking one
// for the desktop, the argument vectors for each kind of request, reading what the program returned, and
// (outside Windows) running a stand-in program through the runner. No dialog is shown: the runner tests
// use /bin/sh in place of the dialog program. Links gdx_companion_host only.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "gdx_companion_dialog.h"
#include "gdx_companion_paths.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

#if defined(_WIN32)
constexpr const char* kSeparator = ";";
#else
constexpr const char* kSeparator = ":";
#endif

void WriteFile(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "#!/bin/sh\n";
}

void TestNamesAndFilters() {
    Check(std::string(DialogToolName(DialogTool::Zenity)) == "zenity", "tool name zenity");
    Check(std::string(DialogToolName(DialogTool::KDialog)) == "kdialog", "tool name kdialog");
    Check(std::string(DialogToolName(DialogTool::None)) == "none", "tool name none");

    Check(FilterPatterns("*.htc;*.HTC") == "*.htc *.HTC", "filter splits on semicolons");
    Check(FilterPatterns("") == "", "empty filter has no patterns");
    Check(FilterPatterns(" *.o2r ; ;*.gdxp") == "*.o2r *.gdxp", "filter drops blanks and empty entries");
}

void TestPick() {
    const std::string z = "/usr/bin/zenity";
    const std::string k = "/usr/bin/kdialog";
    DialogToolChoice c = PickDialogTool("", "KDE", z, k);
    Check(c.tool == DialogTool::KDialog && c.path == k, "KDE desktop picks kdialog");
    c = PickDialogTool("", "kde", z, k);
    Check(c.tool == DialogTool::KDialog, "desktop name is case-insensitive");
    c = PickDialogTool("", "ubuntu:GNOME", z, k);
    Check(c.tool == DialogTool::Zenity && c.path == z, "GNOME desktop picks zenity");
    c = PickDialogTool("", "XKDE", z, k);
    Check(c.tool == DialogTool::Zenity, "a desktop name containing KDE is not KDE");
    c = PickDialogTool("", "GNOME:KDE", z, k);
    Check(c.tool == DialogTool::KDialog, "KDE anywhere in the desktop list picks kdialog");
    c = PickDialogTool("", "KDE", z, "");
    Check(c.tool == DialogTool::Zenity, "KDE without kdialog falls back to zenity");
    c = PickDialogTool("", "GNOME", "", k);
    Check(c.tool == DialogTool::KDialog && c.path == k, "no zenity falls back to kdialog");
    c = PickDialogTool("", "", "", "");
    Check(c.tool == DialogTool::None && c.path.empty(), "neither program means no dialogs");
    c = PickDialogTool("zenity", "KDE", z, k);
    Check(c.tool == DialogTool::Zenity, "preferred zenity wins on KDE");
    c = PickDialogTool("kdialog", "GNOME", z, k);
    Check(c.tool == DialogTool::KDialog, "preferred kdialog wins on GNOME");
    c = PickDialogTool("kdialog", "GNOME", z, "");
    Check(c.tool == DialogTool::Zenity, "a preferred program that is missing falls back");
    c = PickDialogTool("none", "KDE", z, k);
    Check(c.tool == DialogTool::None, "preferred none turns dialogs off");
}

void TestFind(const fs::path& root) {
    const fs::path first = root / "first";
    const fs::path second = root / "second";
    fs::create_directories(first);
    fs::create_directories(second);
    WriteFile(second / "zenity");
#if !defined(_WIN32)
    fs::permissions(second / "zenity", fs::perms::owner_all);
#endif
    const std::string list = PathToUtf8(first) + kSeparator + PathToUtf8(second);
    Check(FindExecutable("zenity", list) == PathToUtf8(second / "zenity"), "finds the program in a later entry");
    Check(FindExecutable("kdialog", list).empty(), "a missing program is empty");
    Check(FindExecutable("../second/zenity", list).empty(), "a name with a slash is refused");
    Check(FindExecutable("", list).empty(), "an empty name is refused");
    Check(FindExecutable("zenity", std::string("relative") + kSeparator + "also-relative").empty(),
          "relative entries are skipped");
    fs::create_directories(first / "kdialog");
    Check(FindExecutable("kdialog", list).empty(), "a folder with the program's name is not the program");
#if !defined(_WIN32)
    WriteFile(first / "zenity");
    fs::permissions(first / "zenity", fs::perms::owner_read | fs::perms::owner_write);
    Check(FindExecutable("zenity", list) == PathToUtf8(second / "zenity"), "a file the user may not run is skipped");
#endif
}

void TestZenityArgv(const fs::path& root) {
    const DialogToolChoice zenity{DialogTool::Zenity, "/usr/bin/zenity"};
    const std::string dir = PathToUtf8(root);
    const std::string file = PathToUtf8(root / "pack.o2r");

    BrowseRequest open;
    open.kind = BrowseKind::OpenFile;
    open.title = "Pack (.o2r or .gdxp)";
    open.filter_name = "Packs";
    open.filter_pattern = "*.o2r;*.gdxp";
    open.start = dir;
    std::vector<std::string> expected = {"/usr/bin/zenity", "--file-selection", "--title=Pack (.o2r or .gdxp)",
                                         "--filename=" + dir + "/", "--file-filter=Packs | *.o2r *.gdxp",
                                         "--file-filter=All files | *"};
    Check(BuildDialogArgv(zenity, open, "/home/user") == expected, "zenity open starts inside a folder");

    BrowseRequest save = open;
    save.kind = BrowseKind::SaveFile;
    save.title.clear();
    save.filter_name.clear();
    save.filter_pattern = "*.o2r";
    save.start = file;
    expected = {"/usr/bin/zenity", "--file-selection", "--save", "--confirm-overwrite",
                "--filename=" + dir + "/pack.o2r", "--file-filter=Files | *.o2r", "--file-filter=All files | *"};
    Check(BuildDialogArgv(zenity, save, "/home/user") == expected, "zenity save fills in the file name");

    BrowseRequest open_file = open;
    open_file.start = file;
    open_file.title.clear();
    open_file.filter_pattern.clear();
    expected = {"/usr/bin/zenity", "--file-selection", "--filename=" + dir + "/"};
    Check(BuildDialogArgv(zenity, open_file, "/home/user") == expected,
          "zenity open starts beside a file, with no filter when none is named");

    BrowseRequest folder;
    folder.kind = BrowseKind::Folder;
    folder.filter_pattern = "*.o2r";
    expected = {"/usr/bin/zenity", "--file-selection", "--directory", "--filename=/home/user/"};
    Check(BuildDialogArgv(zenity, folder, "/home/user") == expected,
          "zenity folder starts at home and ignores the filter");
    expected = {"/usr/bin/zenity", "--file-selection", "--directory"};
    Check(BuildDialogArgv(zenity, folder, "") == expected, "zenity with no start and no home has no file name");

    Check(BuildDialogArgv(DialogToolChoice{}, open, "/home/user").empty(), "no program gives no argument vector");
}

void TestKDialogArgv(const fs::path& root) {
    const DialogToolChoice kdialog{DialogTool::KDialog, "/usr/bin/kdialog"};
    const std::string dir = PathToUtf8(root);
    const std::string file = PathToUtf8(root / "pack.o2r");

    BrowseRequest open;
    open.kind = BrowseKind::OpenFile;
    open.title = "Recording";
    open.filter_name = "Recordings";
    open.filter_pattern = "*.ogg;*.flac;*.wav";
    open.start = dir;
    std::vector<std::string> expected = {"/usr/bin/kdialog", "--title", "Recording", "--getopenfilename", dir,
                                         "*.ogg *.flac *.wav|Recordings"};
    Check(BuildDialogArgv(kdialog, open, "/home/user") == expected, "kdialog open with a filter");

    BrowseRequest save;
    save.kind = BrowseKind::SaveFile;
    save.filter_pattern = "*.o2r";
    save.start = file;
    expected = {"/usr/bin/kdialog", "--getsavefilename", dir + "/pack.o2r", "*.o2r|Files"};
    Check(BuildDialogArgv(kdialog, save, "/home/user") == expected, "kdialog save keeps the file name");

    BrowseRequest folder;
    folder.kind = BrowseKind::Folder;
    folder.start = dir;
    expected = {"/usr/bin/kdialog", "--getexistingdirectory", dir};
    Check(BuildDialogArgv(kdialog, folder, "/home/user") == expected, "kdialog folder");

    folder.start.clear();
    expected = {"/usr/bin/kdialog", "--getexistingdirectory", "."};
    Check(BuildDialogArgv(kdialog, folder, "") == expected, "kdialog with no start and no home starts at .");
}

void TestParse() {
    std::string picked;
    std::string error;
    Check(ParseDialogResult(0, "/home/user/My Pack.o2r\n", picked, error) == DialogOutcome::Picked &&
              picked == "/home/user/My Pack.o2r" && error.empty(),
          "exit 0 is a pick without the line break");
    Check(ParseDialogResult(0, "/a/b\r\n", picked, error) == DialogOutcome::Picked && picked == "/a/b",
          "a carriage return is removed too");
    Check(ParseDialogResult(1, "", picked, error) == DialogOutcome::Cancelled && picked.empty() && error.empty(),
          "exit 1 is a cancel");
    Check(ParseDialogResult(0, "\n", picked, error) == DialogOutcome::Cancelled, "exit 0 with no path is a cancel");
    Check(ParseDialogResult(5, "", picked, error) == DialogOutcome::Failed &&
              error.find("code 5") != std::string::npos,
          "another exit code is a failure that names it");
    Check(ParseDialogResult(0, "/a\n/b\n", picked, error) == DialogOutcome::Failed && picked.empty(),
          "two paths are a failure");
}

#if !defined(_WIN32)
DialogOutcome RunToEnd(DialogRunner& runner, std::string& picked, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const DialogOutcome outcome = runner.Poll(picked, error);
        if (outcome != DialogOutcome::Running) return outcome;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return DialogOutcome::Running;
}

void TestRunner() {
    DialogRunner runner;
    std::string picked;
    std::string error;
    Check(!runner.running(), "a new runner is idle");
    Check(runner.Poll(picked, error) == DialogOutcome::Running, "an idle runner polls as running");

    Check(runner.Start({"/bin/sh", "-c", "printf '/tmp/my pack.o2r\\n'"}, error), "runner starts a program");
    Check(RunToEnd(runner, picked, error) == DialogOutcome::Picked && picked == "/tmp/my pack.o2r",
          "runner returns the printed path");
    Check(!runner.running(), "runner is idle after the outcome");

    Check(runner.Start({"/bin/sh", "-c", "exit 1"}, error), "runner starts again");
    Check(RunToEnd(runner, picked, error) == DialogOutcome::Cancelled, "exit 1 through the runner is a cancel");

    Check(runner.Start({"/bin/sh", "-c", "echo 'Gtk-WARNING: no display' >&2; exit 3"}, error), "runner starts a failing program");
    Check(RunToEnd(runner, picked, error) == DialogOutcome::Failed && error.find("code 3") != std::string::npos &&
              error.find("no display") != std::string::npos,
          "a failure carries the first line of standard error");

    Check(runner.Start({"/bin/sh", "-c", "sleep 30"}, error), "runner starts a long program");
    Check(!runner.Start({"/bin/sh", "-c", "exit 0"}, error) && !error.empty(), "a second dialog is refused while one is open");
    Check(runner.Poll(picked, error) == DialogOutcome::Running, "an open dialog polls as running");

    DialogRunner empty;
    Check(!empty.Start({}, error) && !empty.running(), "an empty argument vector is refused");

    const auto before = std::chrono::steady_clock::now();
    {
        DialogRunner closing;
        closing.Start({"/bin/sh", "-c", "sleep 30"}, error);
    }
    Check(std::chrono::steady_clock::now() - before < std::chrono::seconds(5), "the destructor ends an open dialog");

    DialogRunner missing;
    Check(!missing.Start({"/nonexistent/zenity"}, error) || RunToEnd(missing, picked, error) == DialogOutcome::Failed,
          "a missing program fails to start or fails");
}
#endif

}  // namespace

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "gdx_companion_dialog_tests";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    TestNamesAndFilters();
    TestPick();
    TestFind(root);
    TestZenityArgv(root);
    TestKDialogArgv(root);
    TestParse();
#if !defined(_WIN32)
    TestRunner();
#endif

    fs::remove_all(root, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
