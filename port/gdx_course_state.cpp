// port/gdx_course_state.cpp -- boot order and the GXS1 state file of the Course Edit element store
// (plan slice 9, scope A). Codec and store live in gdx_course_registry.c; this file owns the file system.

#include "gdx_course_state.h"

#include "gdx_course_music.h"
#include "gdx_course_packs.h"
#include "gdx_course_registry.h"
#include "port_log.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr const char* kStateFile = "course-elements.gdxs";

// Course id of the file the Course Edit course came from, empty when it has none.
char sEditFileId[GDX_COURSE_FILE_ID_MAX + 1] = "";

bool sBooted = false;
// True once the store reflects the state file (or the file is known to be absent). Saving is refused
// until then, so a half-read store can never overwrite the file.
bool sLoaded = false;
unsigned long sSavedRevision = 0;

std::filesystem::path exe_relative(const char* relative) {
    char buf[1024];
    const char* full = gdx_exe_relative_path(buf, sizeof buf, relative);
    return std::filesystem::path(full != nullptr ? full : relative);
}

// Moves a rejected file aside so the next save does not reuse it. Failures are logged, not fatal.
void set_aside(const std::filesystem::path& path) {
    const std::filesystem::path bad = std::filesystem::path(path.string() + ".bad");
    std::error_code ec;
    std::filesystem::rename(path, bad, ec);
    if (ec) {
        gdx_port_logf("[state] %s: could not move the rejected file aside (%s)\n", path.string().c_str(),
                      ec.message().c_str());
    } else {
        gdx_port_logf("[state] %s: moved to %s\n", path.string().c_str(), bad.string().c_str());
    }
}

} // namespace

extern "C" int gdx_course_boot(void) {
    if (sBooted) {
        return 0;
    }
    sBooted = true;
    gdx_course_music_scan_files();
    gdx_course_packs_load();
    gdx_course_state_load();
    return 0;
}

extern "C" int gdx_course_state_load(void) {
    const std::filesystem::path path = exe_relative(kStateFile);
    std::error_code ec;
    std::vector<unsigned char> bytes;
    unsigned char chunk[4096];
    size_t got;
    int rc;

    if (!std::filesystem::exists(path, ec)) {
        sLoaded = true;
        sSavedRevision = gdx_course_store_revision();
        return 0;
    }
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        gdx_port_logf("[state] %s: cannot open, saving is off until restart\n", path.string().c_str());
        return -1;
    }
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
        if (bytes.size() > GDX_COURSE_STATE_MAX_BYTES) {
            break;
        }
    }
    const bool ioError = std::ferror(f) != 0;
    std::fclose(f);
    if (ioError) {
        gdx_port_logf("[state] %s: read error, saving is off until restart\n", path.string().c_str());
        return -1;
    }

    if (bytes.empty()) {
        rc = GDX_COURSE_STATE_ERR_TRUNCATED;
    } else if (bytes.size() > GDX_COURSE_STATE_MAX_BYTES) {
        rc = GDX_COURSE_STATE_ERR_LENGTH;
    } else {
        rc = gdx_course_store_restore_file(bytes.data(), bytes.size());
    }
    sLoaded = true;
    sSavedRevision = gdx_course_store_revision();
    if (rc != GDX_COURSE_OK) {
        gdx_port_logf("[state] %s rejected (%d), store starts empty\n", path.string().c_str(), rc);
        set_aside(path);
        return rc;
    }
    gdx_port_logf("[state] course element store restored from %s (%zu bytes)\n", path.string().c_str(),
                  bytes.size());
    return 1;
}

extern "C" int gdx_course_state_flush(void) {
    const std::filesystem::path path = exe_relative(kStateFile);
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");
    const unsigned long revision = gdx_course_store_revision();
    std::vector<unsigned char> buf;
    std::FILE* f;
    std::size_t wrote;
    int n;
    int closeRc;
    std::error_code ec;

    if (!sLoaded || revision == sSavedRevision) {
        return 0;
    }
    buf.resize(GDX_COURSE_STATE_MAX_BYTES);
    n = gdx_course_store_encode_file(buf.data(), buf.size());
    if (n < 0) {
        gdx_port_logf("[state] encode failed (%d), file not written\n", n);
        return n;
    }
    f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        gdx_port_logf("[state] %s: cannot open for writing\n", tmp.string().c_str());
        return -1;
    }
    wrote = std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), f);
    closeRc = std::fclose(f);
    if (wrote != static_cast<std::size_t>(n) || closeRc != 0) {
        std::remove(tmp.string().c_str());
        gdx_port_logf("[state] %s: write failed\n", tmp.string().c_str());
        return -1;
    }
    // Same-volume rename that replaces the old file, so a crash leaves either the old or the new file.
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.string().c_str());
        gdx_port_logf("[state] %s: replace failed (%s)\n", path.string().c_str(), ec.message().c_str());
        return -1;
    }
    sSavedRevision = revision;
    return 1;
}

extern "C" void gdx_course_state_on_file_renamed(const char* oldName, const char* oldExtension,
                                                 const char* newName, const char* newExtension) {
    char oldId[GDX_COURSE_FILE_ID_MAX + 1];
    char newId[GDX_COURSE_FILE_ID_MAX + 1];
    int rc;

    if (!gdx_course_music_course_id(oldName, oldExtension, oldId, sizeof oldId) ||
        !gdx_course_music_course_id(newName, newExtension, newId, sizeof newId)) {
        return;
    }
    if (std::strcmp(sEditFileId, oldId) == 0) {
        std::snprintf(sEditFileId, sizeof sEditFileId, "%s", newId);
    }
    gdx_course_boot();
    rc = gdx_course_store_rename(oldId, newId);
    if (rc != GDX_COURSE_OK) {
        gdx_port_logf("[state] rename %s to %s: keys not moved (%d)\n", oldId, newId, rc);
        return;
    }
    gdx_course_state_flush();
}

extern "C" void gdx_course_state_on_file_deleted(const char* name, const char* extension) {
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];

    if (!gdx_course_music_course_id(name, extension, courseId, sizeof courseId)) {
        return;
    }
    if (std::strcmp(sEditFileId, courseId) == 0) {
        sEditFileId[0] = '\0';
    }
    gdx_course_boot();
    gdx_course_ext_clear_course(courseId);
    gdx_course_title_clear(courseId);
    gdx_course_state_flush();
}

extern "C" void gdx_course_state_set_edit_file(const char* name, const char* extension) {
    if (!gdx_course_music_course_id(name, extension, sEditFileId, sizeof sEditFileId)) {
        sEditFileId[0] = '\0';
    }
}

extern "C" void gdx_course_state_clear_edit_file(void) {
    sEditFileId[0] = '\0';
}

extern "C" int gdx_course_state_edit_file_id(char* out, int outCap) {
    if (out == nullptr || outCap <= 0 || sEditFileId[0] == '\0' ||
        std::strlen(sEditFileId) >= static_cast<std::size_t>(outCap)) {
        return 0;
    }
    std::snprintf(out, static_cast<std::size_t>(outCap), "%s", sEditFileId);
    return 1;
}
