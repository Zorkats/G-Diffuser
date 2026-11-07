// Copyright (c) Zorkats

#include "gdx_companion_install.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>

#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"

namespace fs = std::filesystem;

namespace gdx_companion {

namespace {

constexpr const char* kSettingsName = "companion-settings.json";
constexpr const char* kInstallationKey = "installation";
constexpr std::uintmax_t kSettingsMaxBytes = 64 * 1024;

std::string GameName() { return PathToUtf8(GameExecutablePath(fs::path()).filename()); }

bool HasGame(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(GameExecutablePath(dir), ec);
}

// File names compare without case on Windows, as the file system does.
bool SameFileName(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
#else
    return a == b;
#endif
}

fs::path Absolute(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::absolute(path, ec);
    if (ec) return path.lexically_normal();
    return out.lexically_normal();
}

}  // namespace

fs::path InstallSettingsPath(const fs::path& exe_dir) { return exe_dir / kSettingsName; }

bool ReadSavedInstallation(const fs::path& settings, fs::path& dir, std::string& error) {
    dir.clear();
    error.clear();
    std::error_code ec;
    if (!fs::exists(settings, ec)) return true;
    const std::uintmax_t size = fs::file_size(settings, ec);
    if (ec) {
        error = "cannot read " + PathToUtf8(settings) + ": " + ec.message();
        return false;
    }
    if (size > kSettingsMaxBytes) {
        error = PathToUtf8(settings) + " is larger than 64 KiB";
        return false;
    }
    std::ifstream in(settings, std::ios::binary);
    if (!in) {
        error = "cannot open " + PathToUtf8(settings);
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JsonValue root;
    std::string parse_error;
    if (!JsonValue::Parse(text, root, parse_error)) {
        error = PathToUtf8(settings) + " is not valid JSON: " + parse_error;
        return false;
    }
    if (root.kind() != JsonValue::Kind::Object) {
        error = PathToUtf8(settings) + " does not hold a JSON object";
        return false;
    }
    const JsonValue* value = root.Find(kInstallationKey);
    if (value == nullptr) return true;
    if (value->kind() != JsonValue::Kind::String) {
        error = PathToUtf8(settings) + ": \"installation\" is not a string";
        return false;
    }
    dir = PathFromUtf8(value->AsString());
    return true;
}

bool WriteSavedInstallation(const fs::path& settings, const fs::path& dir, std::string& error) {
    error.clear();
    std::error_code ec;
    if (dir.empty()) {
        fs::remove(settings, ec);
        if (ec) {
            error = "cannot remove " + PathToUtf8(settings) + ": " + ec.message();
            return false;
        }
        return true;
    }
    fs::path temp = settings;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + PathToUtf8(temp);
            return false;
        }
        out << "{\n  \"" << kInstallationKey << "\": \"" << JsonEscape(PathToUtf8(dir)) << "\"\n}\n";
        out.flush();
        if (!out) {
            error = "cannot write " + PathToUtf8(temp);
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, settings, ec);
    if (ec) {
        error = "cannot replace " + PathToUtf8(settings) + ": " + ec.message();
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
    }
    return true;
}

bool InstallationFromChoice(const fs::path& picked, fs::path& dir, std::string& error) {
    dir.clear();
    error.clear();
    if (picked.empty()) {
        error = "no path was given";
        return false;
    }
    const fs::path path = Absolute(picked);
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        if (!SameFileName(PathToUtf8(path.filename()), GameName())) {
            error = PathToUtf8(path) + " is not " + GameName();
            return false;
        }
        dir = path.parent_path();
        return true;
    }
    if (fs::is_directory(path, ec)) {
        if (!HasGame(path)) {
            error = PathToUtf8(path) + " does not hold " + GameName();
            return false;
        }
        dir = path;
        return true;
    }
    error = PathToUtf8(path) + " does not exist";
    return false;
}

InstallCheck CheckInstallation(const fs::path& exe_dir, const fs::path& saved_dir) {
    InstallCheck check;
    std::string prefix;
    if (!saved_dir.empty()) {
        const fs::path chosen = Absolute(saved_dir);
        if (HasGame(chosen)) {
            check.state = InstallState::Validated;
            check.source = InstallSource::Chosen;
            check.game_dir = chosen;
            check.detail = "The folder chosen in Settings holds the game: " + PathToUtf8(GameExecutablePath(chosen));
            return check;
        }
        prefix = "The folder chosen in Settings no longer holds " + GameName() + ": " + PathToUtf8(chosen) + ". ";
    }

    const fs::path beside = Absolute(exe_dir);
    if (HasGame(beside)) {
        check.state = InstallState::Validated;
        check.source = InstallSource::Beside;
        check.game_dir = beside;
        check.detail = prefix + "The game was found beside the Companion: " + PathToUtf8(GameExecutablePath(beside));
        return check;
    }
    const fs::path parent = beside.parent_path();
    const bool has_parent = !parent.empty() && parent != beside;
    if (has_parent && HasGame(parent)) {
        check.state = InstallState::Validated;
        check.source = InstallSource::Parent;
        check.game_dir = parent;
        check.detail =
            prefix + "The game was found in the folder above the Companion: " + PathToUtf8(GameExecutablePath(parent));
        return check;
    }

    check.state = InstallState::Unavailable;
    check.detail = prefix + GameName() + " was not found beside the Companion (" + PathToUtf8(beside) + ")";
    if (has_parent) check.detail += " or in the folder above it (" + PathToUtf8(parent) + ")";
    check.detail += ". Choose the game's folder below.";
    return check;
}

}  // namespace gdx_companion
