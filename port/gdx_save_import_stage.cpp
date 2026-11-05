#include "gdx_save_import_stage.h"

#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace gdx {
namespace {

// The largest input the converter accepts. Anything bigger is refused before it is read, so
// picking a ROM or a video by mistake costs nothing.
constexpr std::uintmax_t kMaxInputSize = GDX_SAVE_IMPORT_SRM_SIZE;

fs::path WithSuffix(const fs::path& path, const char* suffix) {
    fs::path out = path;
    out += suffix;
    return out;
}

bool ReadSmallFile(const fs::path& path, std::vector<unsigned char>& out, std::string& why) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        why = "the file could not be opened";
        return false;
    }
    if (size == 0 || size > kMaxInputSize) {
        why = "the file is not the size of an F-Zero X save";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "the file could not be opened";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        why = "the file could not be read to the end";
        return false;
    }
    return true;
}

// Writes path.tmp, then renames it over path, so a crash never leaves a half-written file under
// the real name.
bool WriteFileAtomically(const fs::path& path, const std::vector<unsigned char>& data) {
    const fs::path temp = WithSuffix(path, ".tmp");
    std::error_code ec;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (out.fail()) {
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

fs::path FreeBackupPath(const fs::path& livePath, const std::string& stamp) {
    std::error_code ec;
    for (int n = 1; n < 100; n++) {
        fs::path name = livePath.filename();
        name += ".before-import-";
        name += stamp;
        if (n > 1) {
            name += "-" + std::to_string(n);
        }
        const fs::path candidate = livePath.parent_path() / name;
        if (!fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
}

} // namespace

fs::path SaveImportStagedPath(const fs::path& livePath) {
    return WithSuffix(livePath, ".import");
}

fs::path SaveImportRejectedPath(const fs::path& livePath) {
    return WithSuffix(livePath, ".import.rejected");
}

const char* SaveImportFormatName(GdxSaveImportFormat format) {
    switch (format) {
        case GDX_SAVE_IMPORT_FORMAT_NATIVE:
            return "G-Diffuser save";
        case GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL:
            return "RetroArch parallel-n64 save (.srm)";
        case GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN:
            return "cartridge save (.sra)";
        case GDX_SAVE_IMPORT_FORMAT_SRA_WORD_SWAPPED:
            return "cartridge save (.sra, word-swapped)";
        case GDX_SAVE_IMPORT_FORMAT_NONE:
            break;
    }
    return "unknown format";
}

SaveImportStageResult StageSaveImport(const fs::path& source, const fs::path& livePath) {
    SaveImportStageResult r;
    std::vector<unsigned char> input;
    std::string why;
    if (!ReadSmallFile(source, input, why)) {
        r.message = "Could not import: " + why + ".";
        return r;
    }

    std::vector<unsigned char> native(GDX_SAVE_IMPORT_SRAM_SIZE);
    GdxSaveImportInfo info{};
    if (!gdx_save_import_convert(input.data(), static_cast<unsigned long>(input.size()), native.data(), &info)) {
        r.message = "This is not an F-Zero X cartridge save, or it is damaged. Supported: .sra (32 KB) and "
                    "RetroArch parallel-n64 .srm.";
        return r;
    }
    if (!WriteFileAtomically(SaveImportStagedPath(livePath), native)) {
        r.message = "Could not write the import next to your save.";
        return r;
    }
    r.ok = true;
    r.format = info.format;
    r.message = std::string("Ready: ") + SaveImportFormatName(info.format) +
                ". It replaces your save the next time G-Diffuser starts.";
    return r;
}

bool SaveImportPending(const fs::path& livePath) {
    std::error_code ec;
    return fs::is_regular_file(SaveImportStagedPath(livePath), ec);
}

bool CancelSaveImport(const fs::path& livePath) {
    std::error_code ec;
    fs::remove(SaveImportStagedPath(livePath), ec);
    return !SaveImportPending(livePath);
}

SaveImportApplyResult ApplyStagedSaveImport(const fs::path& livePath, const std::string& stamp) {
    SaveImportApplyResult r;
    const fs::path staged = SaveImportStagedPath(livePath);
    std::error_code ec;
    if (!fs::is_regular_file(staged, ec)) {
        return r;
    }

    // Checked again at launch: the staged file was written by StageSaveImport as a native image,
    // so anything else means it was damaged or replaced in the meantime.
    std::vector<unsigned char> image;
    std::vector<unsigned char> native(GDX_SAVE_IMPORT_SRAM_SIZE);
    std::string why;
    GdxSaveImportInfo info{};
    bool valid = ReadSmallFile(staged, image, why);
    if (valid && (image.size() != GDX_SAVE_IMPORT_SRAM_SIZE ||
                  !gdx_save_import_convert(image.data(), static_cast<unsigned long>(image.size()), native.data(),
                                           &info) ||
                  info.format != GDX_SAVE_IMPORT_FORMAT_NATIVE)) {
        valid = false;
        why = "the staged file is not a complete save";
    }
    if (!valid) {
        const fs::path rejected = SaveImportRejectedPath(livePath);
        fs::remove(rejected, ec);
        fs::rename(staged, rejected, ec);
        if (ec) {
            fs::remove(staged, ec);
        }
        r.outcome = SaveImportApply::Rejected;
        r.message = "Save import not applied: " + why + ". Your save was not changed.";
        return r;
    }

    if (fs::exists(livePath, ec)) {
        const fs::path backup = FreeBackupPath(livePath, stamp);
        bool copied = false;
        if (!backup.empty()) {
            copied = fs::copy_file(livePath, backup, fs::copy_options::none, ec) && !ec;
        }
        if (!copied) {
            r.outcome = SaveImportApply::Failed;
            r.message = "Save import not applied: the current save could not be backed up. It will be tried "
                        "again next launch.";
            return r;
        }
        r.backup = backup;
    }

    fs::rename(staged, livePath, ec);
    if (ec) {
        r.outcome = SaveImportApply::Failed;
        r.message = "Save import not applied: the save could not be replaced. It will be tried again next launch.";
        return r;
    }
    r.outcome = SaveImportApply::Applied;
    r.message = r.backup.empty() ? "Save import applied." : "Save import applied. The previous save was backed up.";
    return r;
}

} // namespace gdx
