// Stages an imported cartridge save beside the live save and applies it at the next launch.
//
// The running game writes its save one section at a time from the copy it holds in memory, so
// replacing the live file mid-session would mix the import with sections the game still holds.
// Instead the converted image waits in <save>.import, and gdx_sram_init applies it before anything
// reads the save: the live save is copied to a timestamped backup, then the staged file replaces it.
#ifndef GDX_SAVE_IMPORT_STAGE_H
#define GDX_SAVE_IMPORT_STAGE_H

#include "gdx_save_import.h"

#include <filesystem>
#include <string>

namespace gdx {

struct SaveImportStageResult {
    bool ok = false;
    GdxSaveImportFormat format = GDX_SAVE_IMPORT_FORMAT_NONE;
    std::string message;
};

enum class SaveImportApply { NothingStaged, Applied, Rejected, Failed };

struct SaveImportApplyResult {
    SaveImportApply outcome = SaveImportApply::NothingStaged;
    std::filesystem::path backup; // empty when there was no live save to back up
    std::string message;
};

// <save>.import next to the live save.
std::filesystem::path SaveImportStagedPath(const std::filesystem::path& livePath);

// <save>.import.rejected: where a staged file that fails its launch check is moved, so it is not
// retried on every launch.
std::filesystem::path SaveImportRejectedPath(const std::filesystem::path& livePath);

// Reads source, converts it, and writes the native image to the staged path through a temp file.
// Nothing is written when source is not a recognized F-Zero X save. A newer stage replaces an
// older one.
SaveImportStageResult StageSaveImport(const std::filesystem::path& source, const std::filesystem::path& livePath);

bool SaveImportPending(const std::filesystem::path& livePath);

// Deletes the staged file. Returns true when nothing is staged afterwards.
bool CancelSaveImport(const std::filesystem::path& livePath);

// Run once, before the live save is read. stamp becomes part of the backup name
// (<save>.before-import-<stamp>); a taken name gets a -2, -3 ... suffix. The live save is only
// replaced after its backup is written, and stays untouched on any failure.
SaveImportApplyResult ApplyStagedSaveImport(const std::filesystem::path& livePath, const std::string& stamp);

// A short name for a detected format, for status lines.
const char* SaveImportFormatName(GdxSaveImportFormat format);

} // namespace gdx

#endif
