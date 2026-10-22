// port/updater/gdx_update_apply.h -- the testable half of gdx-update. See gdx_update_apply.cpp.
//
// gdx-update replaces an installed build. Everything it decides before it touches a file lives
// here: which archive member paths are safe, which top-level items belong to the shipped payload,
// how a ustar header reads, and how the swap is undone when a move fails. gdx_update_main.cpp
// keeps only the process concerns: arguments, the log, waiting for the game and relaunching it.
//
// Paths cross this interface as UTF-8 text, the encoding an archive member name uses. Turning that
// into the host path type (UTF-16 on Windows) is the business of the two translation units, so the
// shared declaration stays free of <filesystem>.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Process exit codes. 2, 3 and 4 are the ones the update contract promises: the game did not exit
// in time, the archive was refused, and the swap failed and was rolled back.
enum {
    GDX_UPD_OK = 0,
    GDX_UPD_ERR_ARGS = 1,
    GDX_UPD_ERR_WAIT = 2,
    GDX_UPD_ERR_ARCHIVE = 3,
    GDX_UPD_ERR_SWAP = 4,
    GDX_UPD_ERR_IO = 5,
};

// The shipped payload: exactly these top-level items belong to the build. Anything else in an
// install folder is user data (saves, exports, ghosts, mods, the extracted generic.o2r, configs and
// logs) and an update neither reads nor writes it.
const char* gdx_upd_main_exe_name(void);
const char* gdx_upd_helper_exe_name(void);
bool gdx_upd_is_payload_item(const std::string& name);

// A member path arrives from an archive, so every part of it is untrusted. Accepts only
// "G-Diffuser/<relative path>", writes the part after the folder into rel (empty for the folder
// entry itself) and returns false with a short reason for anything else.
bool gdx_upd_member_rel_path(const std::string& memberPath, std::string* rel, std::string* reason);

// One ustar header, as far as the extractor needs it. type is the raw typeflag, with the NUL that
// old writers use for a regular file normalized to '0'.
struct GdxUpdTarEntry {
    std::string path;
    uint64_t size = 0;
    uint32_t mode = 0;
    char type = '0';
};

bool gdx_upd_tar_parse_header(const unsigned char* block, GdxUpdTarEntry* out, std::string* reason);

// The filesystem the swap plan runs on. main.cpp uses the host implementation; the tests wrap it to
// fail on demand, which is how a rollback gets exercised without a real install folder. Paths are
// UTF-8 text.
class GdxUpdFs {
public:
    virtual ~GdxUpdFs() = default;
    virtual bool exists(const std::string& path) = 0;
    // Renames, replacing an existing file. Callers remove a directory first, because moving onto an
    // existing directory is refused by every host.
    virtual bool move(const std::string& from, const std::string& to) = 0;
    virtual bool remove_all(const std::string& path) = 0;
    virtual bool create_dirs(const std::string& path) = 0;
};

std::unique_ptr<GdxUpdFs> gdx_upd_make_host_fs(void);

// The verdict of a swap. The per-item record is the log the caller collects, which is also what
// the update.log gets, so nothing is reported twice.
struct GdxUpdSwapResult {
    int code = GDX_UPD_OK;
    std::string failedItem;
    bool rollbackComplete = true;
};

// Pays out the staged tree into the install: each payload item that exists is moved to the backup
// folder first, then the staged copy takes its place. Any failed move undoes every completed one
// before returning. Staged items outside the payload list are logged and left alone.
GdxUpdSwapResult gdx_upd_swap_apply(GdxUpdFs* host, const std::string& installDir, const std::string& stagingDir,
                                    const std::string& backupDir, const std::vector<std::string>& stagedItems,
                                    std::vector<std::string>* log);

// Extracts a .zip or .tar.gz into stagingDir, which the caller has already emptied. A build without
// GDX_UPDATE_HAVE_ARCHIVES (the standalone test target) returns GDX_UPD_ERR_ARCHIVE.
int gdx_upd_extract_archive(const std::string& archivePath, const std::string& stagingDir,
                            std::vector<std::string>* log);
