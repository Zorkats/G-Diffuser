// port/gdx_export_archive.h -- "Export everything": walks the install and writes one zip of the groups the player
// picked. Which file belongs to which group, and what never leaves the install, is gdx_export_all.h.
//
// The zip keeps each file at its path relative to the install, so unzipping it over another install's folder puts
// every file back where the game looks for it. Packs, music and nested zips are stored, the rest deflated.
//
// The files are read when the zip is written, not when they are listed, so write while no track save is running.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gdx {

struct ExportFile {
    std::filesystem::path source; // where it is on disk
    std::string member;           // its name in the zip, '/' separated, UTF-8
    unsigned group;               // GDX_EXPORT_* bit
    uint64_t size;
};

struct ExportPlan {
    std::vector<ExportFile> files; // every exportable file, of every group, sorted by member name
    size_t count[4] = {};          // per group, in GDX_EXPORT_* bit order
    uint64_t bytes[4] = {};
    std::string error;             // a folder that could not be read, or empty
};

// Lists every exportable file. dataRoot holds the saves, ghosts, settings and mods; exeRoot is the executable's
// folder, where music/ and exports/ live. They are often the same folder. A file found under both is listed once,
// from dataRoot. Symbolic links are not followed.
ExportPlan PlanExport(const std::filesystem::path& dataRoot, const std::filesystem::path& exeRoot);

// Files of a plan in the picked groups.
size_t ExportCount(const ExportPlan& plan, unsigned groups);
uint64_t ExportBytes(const ExportPlan& plan, unsigned groups);

// Writes the files of the picked groups to zipPath (replaced when it exists). progress, when given, is called with
// 0..1 while the zip is written. Returns true on success; on failure no zip is left at zipPath and error says why.
bool WriteExport(const ExportPlan& plan, unsigned groups, const std::filesystem::path& zipPath,
                 void (*progress)(double done, void* ctx), void* ctx, std::string& error);

} // namespace gdx
