// Copyright (c) Zorkats
//
// Headless tests for the installation check: the search order (chosen folder, beside the Companion,
// the folder above it), what a pick may be, and the companion-settings.json round trip. Every file is
// made in a fresh temporary folder. Links gdx_companion_host only: no SDL, OpenGL or ImGui.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gdx_companion_install.h"
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

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

void WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void PlaceGame(const fs::path& dir) {
    fs::create_directories(dir);
    WriteFile(GameExecutablePath(dir), "not a real game");
}

// root/companion is the Companion's folder in every case; root holds or lacks the game.
struct Layout {
    fs::path root;
    fs::path companion;
};

Layout MakeLayout(const fs::path& base, const char* name) {
    Layout layout;
    layout.root = base / name;
    layout.companion = layout.root / "companion";
    fs::create_directories(layout.companion);
    return layout;
}

void SearchOrder(const fs::path& base) {
    {
        const Layout layout = MakeLayout(base, "beside");
        PlaceGame(layout.companion);
        const InstallCheck check = CheckInstallation(layout.companion, fs::path());
        Check(check.state == InstallState::Validated && check.source == InstallSource::Beside &&
                  check.game_dir == fs::absolute(layout.companion).lexically_normal(),
              "the game beside the Companion is found");
        Check(Contains(check.detail, "beside the Companion"), "the beside detail says where");
    }
    {
        const Layout layout = MakeLayout(base, "parent");
        PlaceGame(layout.root);
        const InstallCheck check = CheckInstallation(layout.companion, fs::path());
        Check(check.state == InstallState::Validated && check.source == InstallSource::Parent &&
                  check.game_dir == fs::absolute(layout.root).lexically_normal(),
              "the game in the folder above the Companion is found (companion subfolder layout)");
        Check(Contains(check.detail, "folder above the Companion"), "the parent detail says where");
    }
    {
        const Layout layout = MakeLayout(base, "both");
        PlaceGame(layout.root);
        PlaceGame(layout.companion);
        const InstallCheck check = CheckInstallation(layout.companion, fs::path());
        Check(check.source == InstallSource::Beside, "beside wins over the folder above");
    }
    {
        const Layout layout = MakeLayout(base, "none");
        const InstallCheck check = CheckInstallation(layout.companion, fs::path());
        Check(check.state == InstallState::Unavailable && check.source == InstallSource::None &&
                  check.game_dir.empty(),
              "no game anywhere is Unavailable");
        Check(Contains(check.detail, PathToUtf8(fs::absolute(layout.companion).lexically_normal())) &&
                  Contains(check.detail, PathToUtf8(fs::absolute(layout.root).lexically_normal())) &&
                  Contains(check.detail, "Choose the game's folder"),
              "the Unavailable detail names both folders and the way out");
    }
    {
        const Layout layout = MakeLayout(base, "chosen");
        PlaceGame(layout.companion);
        const fs::path elsewhere = base / "chosen-elsewhere";
        PlaceGame(elsewhere);
        const InstallCheck check = CheckInstallation(layout.companion, elsewhere);
        Check(check.state == InstallState::Validated && check.source == InstallSource::Chosen &&
                  check.game_dir == fs::absolute(elsewhere).lexically_normal(),
              "the chosen folder wins over beside");
    }
    {
        const Layout layout = MakeLayout(base, "stale");
        PlaceGame(layout.root);
        const fs::path gone = base / "stale-gone";
        const InstallCheck check = CheckInstallation(layout.companion, gone);
        Check(check.state == InstallState::Validated && check.source == InstallSource::Parent,
              "a chosen folder without the game falls back to the search");
        Check(Contains(check.detail, "no longer holds") && Contains(check.detail, "stale-gone"),
              "the fallback detail names the stale choice");
    }
}

void Choices(const fs::path& base) {
    const fs::path game_dir = base / "pick";
    PlaceGame(game_dir);
    const fs::path absolute = fs::absolute(game_dir).lexically_normal();
    fs::path dir;
    std::string error;

    Check(InstallationFromChoice(GameExecutablePath(game_dir), dir, error) && dir == absolute && error.empty(),
          "picking the game executable gives its folder");
    Check(InstallationFromChoice(game_dir, dir, error) && dir == absolute, "picking the folder gives the folder");
#if defined(_WIN32)
    const fs::path upper = game_dir / "G-DIFFUSER.EXE";
    Check(InstallationFromChoice(upper, dir, error) && dir == absolute,
          "the executable name matches without case on Windows");
#endif

    const fs::path other = game_dir / "notes.txt";
    WriteFile(other, "x");
    Check(!InstallationFromChoice(other, dir, error) && dir.empty() && Contains(error, "is not"),
          "another file is rejected");
    const fs::path empty_dir = base / "pick-empty";
    fs::create_directories(empty_dir);
    Check(!InstallationFromChoice(empty_dir, dir, error) && Contains(error, "does not hold"),
          "a folder without the game is rejected");
    Check(!InstallationFromChoice(base / "pick-missing", dir, error) && Contains(error, "does not exist"),
          "a missing path is rejected");
    Check(!InstallationFromChoice(fs::path(), dir, error) && !error.empty(), "an empty path is rejected");
}

void Settings(const fs::path& base) {
    const fs::path folder = base / "settings";
    fs::create_directories(folder);
    const fs::path settings = InstallSettingsPath(folder);
    Check(settings == folder / "companion-settings.json", "the settings file sits beside the Companion");

    fs::path dir;
    std::string error;
    Check(ReadSavedInstallation(settings, dir, error) && dir.empty() && error.empty(),
          "a missing settings file reads as no choice");

    // Non-ASCII, a quote and backslashes survive the round trip.
    const fs::path chosen = PathFromUtf8(PathToUtf8(base) + "/Juego \"F\" \xC3\xB1" + "/x\\y");
    Check(WriteSavedInstallation(settings, chosen, error) && error.empty(), "the choice is written");
    Check(!fs::exists(fs::path(settings).concat(".tmp")), "no temporary file is left behind");
    Check(ReadSavedInstallation(settings, dir, error) && dir == chosen, "the choice reads back unchanged");

    const fs::path second = base / "second";
    Check(WriteSavedInstallation(settings, second, error) && ReadSavedInstallation(settings, dir, error) &&
              dir == second,
          "a new choice replaces the old one");

    Check(WriteSavedInstallation(settings, fs::path(), error) && !fs::exists(settings),
          "an empty choice removes the file");
    Check(WriteSavedInstallation(settings, fs::path(), error), "removing a missing file is fine");

    WriteFile(settings, "{\"theme\": \"circuit\"}");
    Check(ReadSavedInstallation(settings, dir, error) && dir.empty(), "a file without the key reads as no choice");
    WriteFile(settings, "{not json");
    Check(!ReadSavedInstallation(settings, dir, error) && dir.empty() && Contains(error, "not valid JSON"),
          "a broken file is reported");
    WriteFile(settings, "{\"installation\": 5}");
    Check(!ReadSavedInstallation(settings, dir, error) && Contains(error, "not a string"),
          "a non-string choice is reported");
    WriteFile(settings, "[\"installation\"]");
    Check(!ReadSavedInstallation(settings, dir, error) && Contains(error, "object"), "a non-object file is reported");
    WriteFile(settings, std::string(70 * 1024, ' '));
    Check(!ReadSavedInstallation(settings, dir, error) && Contains(error, "64 KiB"), "an oversized file is reported");

    const std::string written = (WriteSavedInstallation(settings, second, error), ReadFile(settings));
    Check(written.find('\r') == std::string::npos && Contains(written, "\"installation\""),
          "the file is LF JSON with the installation key");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_install_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    SearchOrder(base);
    Choices(base);
    Settings(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
