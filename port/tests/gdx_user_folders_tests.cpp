#include "gdx_user_folders.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("gdx-folders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto data = root / "data";
    std::string errors;
    assert(gdx::EnsureUserFolders(data, data / "exports", errors));
    assert(errors.empty());
    for (const auto* relative : {"mods/~romhacks", "music", "exports/machines"}) {
        assert(fs::is_directory(data / relative));
    }
    assert(!fs::exists(data / "music/custom"));
    assert(!fs::exists(data / "mods/music"));
    fs::create_directories(data / "music/custom");
    fs::create_directories(data / "mods/music/custom");
    {
        std::ofstream legacy(data / "mods/music/custom/legacy.ogg");
        legacy << "legacy bytes";
    }
    const auto existing = data / "music/keep.ogg";
    {
        std::ofstream file(existing, std::ios::binary);
        file << "keep original bytes";
    }
    const auto cwd = fs::current_path();
    fs::current_path(root);
    assert(gdx::EnsureUserFolders(data, data / "exports", errors));
    fs::current_path(cwd);
    {
        std::ifstream file(existing, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        assert(bytes == "keep original bytes");
    }
    assert(fs::is_regular_file(data / "mods/music/custom/legacy.ogg"));
    assert(fs::is_directory(data / "music/custom"));
    fs::remove(existing);
    fs::remove(data / "music/custom");
    fs::remove(data / "music");
    assert(gdx::EnsureUserFolders(data, data / "exports", errors));
    assert(fs::is_directory(data / "music"));
    assert(!fs::exists(data / "music/custom"));
    assert(!gdx::EnsureUserFolders({}, data / "exports", errors));
    assert(!gdx::EnsureUserFolders("relative", data / "exports", errors));
    const auto redirected = root / "redirected-data";
    const auto exports = root / "program/exports";
    assert(gdx::EnsureUserFolders(redirected, exports, errors));
    assert(fs::is_directory(redirected / "mods/~romhacks"));
    assert(fs::is_directory(exports.parent_path() / "music"));
    assert(!fs::exists(redirected / "music"));
    assert(!fs::exists(redirected / "music/custom"));
    assert(fs::is_directory(exports / "machines"));
    assert(!fs::exists(redirected / "exports"));
    assert(!gdx::EnsureUserFolders(data, {}, errors));
    assert(!gdx::EnsureUserFolders(data, "relative", errors));
    const auto blocked = root / "blocked";
    fs::create_directories(blocked);
    {
        std::ofstream file(blocked / "mods");
        file << "not a directory";
    }
    assert(!gdx::EnsureUserFolders(blocked, blocked / "exports", errors));
    assert(!errors.empty());
    assert(fs::is_regular_file(blocked / "mods"));
    assert(fs::is_directory(blocked / "exports/machines"));
    const auto blockedMusic = root / "blocked-music";
    fs::create_directories(blockedMusic);
    {
        std::ofstream file(blockedMusic / "music");
        file << "not a directory";
    }
    assert(!gdx::EnsureUserFolders(blockedMusic, blockedMusic / "exports", errors));
    assert(errors.find("music") != std::string::npos);
    assert(fs::is_regular_file(blockedMusic / "music"));
    assert(fs::is_directory(blockedMusic / "mods/~romhacks"));
    assert(fs::is_directory(blockedMusic / "exports/machines"));
    std::error_code ec;
    fs::remove_all(root, ec);
    assert(!ec);
    std::cout << "All user folder tests passed\n";
}
