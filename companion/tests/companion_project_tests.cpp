// Copyright (c) Zorkats
//
// Headless tests for Companion project files: relative paths survive moving the project folder, missing
// files are listed for relinking, and files from another format or a newer version are refused. Every
// file is made in a fresh temporary folder. Links gdx_companion_host only.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gdx_companion_paths.h"
#include "gdx_companion_project.h"

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

void Relative() {
#if defined(_WIN32)
    const fs::path base = "C:/work/project";
    Check(RelativeProjectPath(base, "C:/work/project/images/a.png") == "images/a.png", "a path inside is relative");
    Check(RelativeProjectPath(base, "C:/work/dump") == "../dump", "a sibling path is relative");
    Check(RelativeProjectPath(base, "Z:/packs/x.o2r") == "Z:/packs/x.o2r", "another drive stays absolute");
    Check(ResolveProjectPath(base, "../dump") == fs::path("C:/work/dump").lexically_normal(), "relative resolves");
    Check(ResolveProjectPath(base, "Z:/packs/x.o2r") == fs::path("Z:/packs/x.o2r").lexically_normal(),
          "absolute resolves to itself");
#else
    const fs::path base = "/work/project";
    Check(RelativeProjectPath(base, "/work/project/images/a.png") == "images/a.png", "a path inside is relative");
    Check(RelativeProjectPath(base, "/work/dump") == "../dump", "a sibling path is relative");
    Check(ResolveProjectPath(base, "../dump") == fs::path("/work/dump"), "relative resolves");
#endif
    Check(RelativeProjectPath(base, fs::path()).empty() && ResolveProjectPath(base, "").empty(),
          "an empty path stays empty");
}

void RoundTrip(const fs::path& root) {
    const fs::path first = root / "first";
    fs::create_directories(first / "images");
    WriteFile(first / "images" / "a.png", "png");
    CompanionProject project;
    project.workspace = "textures";
    project.text["packer"] = "texture";
    project.text["name"] = "Pack \"one\"";
    project.paths["input"] = first / "images";
    project.paths["manifest"] = first / "images" / "a.png";
    project.paths["unset"] = fs::path();
    project.flags["direct_keys"] = true;
    project.flags["overwrite"] = false;
    const fs::path file = first / ("pack" + std::string(kProjectExtension));
    std::string error;
    Check(SaveProject(file, project, error), "a project saves");
    const std::string text = ReadFile(file);
    Check(Contains(text, "\"format\": \"gdx-companion-project\"") && Contains(text, "\"version\": 1") &&
              Contains(text, "\"images/a.png\"") && text.find('\r') == std::string::npos &&
              !fs::exists(fs::path(file) += ".tmp"),
          "the file is LF JSON with relative paths and no temporary left");

    // Moving the whole folder keeps every path.
    const fs::path moved = root / "moved";
    fs::rename(first, moved);
    CompanionProject loaded;
    std::vector<std::string> missing;
    Check(LoadProject(moved / file.filename(), loaded, missing, error), "the moved project loads");
    Check(loaded.workspace == "textures" && loaded.text["name"] == "Pack \"one\"" && loaded.flags["direct_keys"] &&
              !loaded.flags["overwrite"],
          "text and flags come back");
    Check(loaded.paths["input"] == (moved / "images").lexically_normal() && loaded.paths["unset"].empty(),
          "paths follow the moved folder");
    Check(missing.empty(), "nothing is missing after the move");

    fs::remove(moved / "images" / "a.png");
    Check(LoadProject(moved / file.filename(), loaded, missing, error) && missing.size() == 1 &&
              missing[0] == "manifest",
          "a removed file is listed for relinking");
}

void Refusals(const fs::path& root) {
    const fs::path dir = root / "refusals";
    fs::create_directories(dir);
    const fs::path file = dir / "p.gdxproj";
    CompanionProject project;
    std::vector<std::string> missing;
    std::string error;

    WriteFile(file, "{\"format\": \"other\", \"version\": 1, \"workspace\": \"x\"}");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "not a Companion project"),
          "another format is refused");
    WriteFile(file, "{\"format\": \"gdx-companion-project\", \"version\": 2, \"workspace\": \"x\"}");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "newer"), "a newer version is refused");
    WriteFile(file, "{\"format\": \"gdx-companion-project\", \"version\": 1}");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "workspace"), "a missing workspace is refused");
    WriteFile(file, "{\"format\": \"gdx-companion-project\", \"version\": 1, \"workspace\": \"x\", \"flags\": {\"a\": 1}}");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "true or false"), "a non-bool flag is refused");
    WriteFile(file, "{\"format\": \"gdx-companion-project\", \"version\": 1, \"workspace\": \"x\", \"paths\": [1]}");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "object"), "a paths list is refused");
    WriteFile(file, "not json");
    Check(!LoadProject(file, project, missing, error) && Contains(error, "JSON"), "broken JSON is refused");
    WriteFile(file, std::string(300 * 1024, ' '));
    Check(!LoadProject(file, project, missing, error) && Contains(error, "256 KiB"), "an oversized file is refused");
    Check(!LoadProject(dir / "absent.gdxproj", project, missing, error), "a missing file is refused");

    Check(!SaveProject(dir, project, error), "saving onto a folder is refused");
    Check(!SaveProject(dir / "nowhere" / "p.gdxproj", project, error), "saving into a missing folder is refused");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_project_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    Relative();
    RoundTrip(base);
    Refusals(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
