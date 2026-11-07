// Copyright (c) Zorkats
//
// Companion project files: what a workspace's form held, saved only when the user presses Save, so the
// same build can be run again later. A project is one JSON document:
//
//   {"format": "gdx-companion-project", "version": 1, "workspace": "<id>",
//    "text": {...}, "paths": {...}, "flags": {...}}
//
// Paths are stored relative to the project file's folder with forward slashes, or absolute when they sit
// on another drive. Loading resolves them back and lists every path that no longer exists, so the page
// can ask the user to relink it. Nothing here writes anything except SaveProject.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gdx_companion {

inline constexpr char kProjectFormat[] = "gdx-companion-project";
inline constexpr int kProjectVersion = 1;
inline constexpr char kProjectExtension[] = ".gdxproj";

struct CompanionProject {
    std::string workspace;                        // textures, models, advanced, music
    std::map<std::string, std::string> text;      // field key to UTF-8 text
    std::map<std::string, std::filesystem::path> paths;  // field key to an absolute path; empty is unset
    std::map<std::string, bool> flags;            // field key to a tick box
};

// The stored form of path relative to base: "a/b.png", "../x", or the absolute path when they share no
// root. Empty for an empty path.
std::string RelativeProjectPath(const std::filesystem::path& base, const std::filesystem::path& path);

// The inverse of RelativeProjectPath. Empty text gives an empty path.
std::filesystem::path ResolveProjectPath(const std::filesystem::path& base, const std::string& stored);

// The JSON text of project as saved at file.
std::string EncodeProject(const CompanionProject& project, const std::filesystem::path& file);

// Writes project to file through a temporary file in the same folder. Returns false with error set.
bool SaveProject(const std::filesystem::path& file, const CompanionProject& project, std::string& error);

// Reads file. missing lists the path keys whose files or folders are gone. Returns false with error set
// when the file is unreadable, larger than 256 KiB, not a project, or a newer version.
bool LoadProject(const std::filesystem::path& file, CompanionProject& project, std::vector<std::string>& missing,
                 std::string& error);

}  // namespace gdx_companion
