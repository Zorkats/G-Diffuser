#pragma once

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace gdx::music {

inline constexpr const char* kBgmNames[] = {
    "bgm_mute_city", "bgm_silence", "bgm_sand_ocean", "bgm_port_town", "bgm_big_blue",
    "bgm_devils_forest", "bgm_red_canyon", "bgm_sector", "bgm_white_land", "bgm_rainbow_road",
    "bgm_new_03", "bgm_new_02", "bgm_new_01", "bgm_new_04", "bgm_title",
    "bgm_select", "bgm_option", "bgm_deathrace", "bgm_course_editor", "bgm_machine_editor",
    "bgm_ead_demo", "bgm_21", "bgm_22", "bgm_23", "bgm_24",
    "bgm_gameover", "bgm_end_cs", "bgm_credits", "bgm_demo", "bgm_deathrace2"
};
inline constexpr int kBgmNamedCount = sizeof(kBgmNames) / sizeof(kBgmNames[0]);
inline constexpr int kBgmNone = 99;
inline constexpr const char* kExtensions[] = {".ogg", ".flac", ".wav"};
inline constexpr const char* kCourseFolders[] = {"music", "mods/music/custom", "music/custom"};
inline constexpr const char* kReplacementFolders[] = {"music", "mods/music"};

inline std::string Fold(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

inline int ExtensionRank(const std::filesystem::path& path) {
    const std::string extension = Fold(path.extension().string());
    for (int i = 0; i < 3; ++i) {
        if (extension == kExtensions[i]) {
            return i;
        }
    }
    return -1;
}

inline bool IsReplacementStem(const std::string& stem) {
    const std::string folded = Fold(stem);
    for (const char* name : kBgmNames) {
        if (folded == name) {
            return true;
        }
    }
    if (folded.compare(0, 4, "bgm_") != 0) {
        return false;
    }
    int id = 0;
    const auto parsed = std::from_chars(folded.data() + 4, folded.data() + folded.size(), id, 16);
    if (parsed.ec != std::errc() || parsed.ptr != folded.data() + folded.size() ||
        id < kBgmNamedCount || id == kBgmNone) {
        return false;
    }
    char key[32];
    std::snprintf(key, sizeof key, "bgm_%02x", id);
    return folded == key;
}

inline bool Preferred(const std::filesystem::path& a, const std::filesystem::path& b) {
    const int rankA = ExtensionRank(a);
    const int rankB = ExtensionRank(b);
    if (rankA != rankB) {
        return rankA < rankB;
    }
    return a.filename().string() < b.filename().string();
}

inline std::filesystem::path FindFile(const std::filesystem::path& directory, const std::string& stem) {
    const std::string folded = Fold(stem);
    std::filesystem::path best;
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, ec);
    if (ec) {
        return {};
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& candidate = it->path();
        if (ExtensionRank(candidate) >= 0 && Fold(candidate.stem().string()) == folded &&
            it->is_regular_file(ec) && (best.empty() || Preferred(candidate, best))) {
            best = candidate;
        }
    }
    return best;
}

}
