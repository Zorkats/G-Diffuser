// port/gdx_course_music.cpp -- custom soundtracks for Course Edit tracks (plan section 7A).
// Registry, store and stream glue for gdx_course_music.h. The stream and the file copies run on the
// game thread; see the header.

#include "gdx_course_music.h"

#include "gdx_course_registry.h"
#include "gdx_course_state.h"
#include "gdx_music_stream.h"
#include "gdx_music_files.h"
#include "port_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr int kStockBgmCount = 14;                       // stock BGM menu entries (raw IDs 0..13)
constexpr uintmax_t kMaxCopyBytes = 32u * 1024u * 1024u; // larger music files are not copied
constexpr const char* kKeyPrefix = GDX_COURSE_MUSIC_KEY_PREFIX;
constexpr size_t kKeyPrefixLen = sizeof(GDX_COURSE_MUSIC_KEY_PREFIX) - 1;

bool sScanned = false;

std::filesystem::path exe_relative(const char* relative) {
    char buf[1024];
    const char* full = gdx_exe_relative_path(buf, sizeof buf, relative);
    return std::filesystem::path(full != nullptr ? full : relative);
}

bool is_music_file(const std::filesystem::path& p) {
    return gdx::music::ExtensionRank(p) >= 0;
}

int scan_dir(const std::filesystem::path& dir, bool canonical) {
    std::error_code ec;
    int added = 0;
    std::vector<std::filesystem::path> files;

    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return 0;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& p = it->path();
        if (is_music_file(p) && it->is_regular_file(ec) &&
            !(canonical && gdx::music::IsReplacementStem(p.stem().string()))) {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end(), gdx::music::Preferred);
    for (const auto& p : files) {
        if (!std::filesystem::is_regular_file(p, ec)) {
            continue;
        }
        const std::string stem = p.stem().string();
        const std::string full = p.string();
        char key[GDX_COURSE_KEY_MAX + 1];
        char label[GDX_COURSE_DISPLAY_MAX + 1];

        if (gdx_course_registry_music_key(stem.c_str(), key, sizeof key) == 0 ||
            gdx_course_registry_music_display(stem.c_str(), label, sizeof label) == 0) {
            gdx_port_logf("[music] custom track %s: no usable key, skipped\n", full.c_str());
            continue;
        }
        const int rc = full.size() < GDX_COURSE_PATH_MAX
                           ? gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, key, label, 0, full.c_str())
                           : GDX_COURSE_ERR_PATH;
        if (rc == GDX_COURSE_OK) {
            added++;
            gdx_port_logf("[music] custom track %s: key %s\n", full.c_str(), key);
        } else if (rc != GDX_COURSE_ERR_DUPLICATE) {
            gdx_port_logf("[music] custom track %s: not registered (%d)\n", full.c_str(), rc);
        }
    }
    return added;
}

// Scans the mods folder first, matching the lookup order of gdx_music_stream_find_file.
int scan_all(void) {
    int added = 0;
    for (const char* folder : gdx::music::kCourseFolders) {
        added += scan_dir(exe_relative(folder), std::strcmp(folder, "music") == 0);
    }
    return added;
}

// Boot runs the file scan first (gdx_course_music_scan_files), then packs and the state file. Calling
// boot here means every entry point sees the same registry, race included.
void ensure_scanned(void) {
    if (!sScanned) {
        gdx_course_boot();
    }
}

int music_count(void) {
    ensure_scanned();
    return gdx_course_registry_count(GDX_COURSE_KIND_BGM) - gdx_course_registry_stock_count(GDX_COURSE_KIND_BGM);
}

const GdxCourseElement* music_entry(int extIndex) {
    if (extIndex < 0 || extIndex >= music_count()) {
        return nullptr;
    }
    return gdx_course_registry_at(GDX_COURSE_KIND_BGM, kStockBgmCount + extIndex);
}

// The course-wide BGM record of a course, or NULL.
const GdxCourseExtRecord* course_bgm_record(const char* courseId) {
    int count = 0;
    const GdxCourseExtRecord* records;

    if (courseId == nullptr) {
        return nullptr;
    }
    records = gdx_course_ext_records(courseId, &count);
    for (int i = 0; records != nullptr && i < count; i++) {
        if (records[i].kind == GDX_COURSE_KIND_BGM && records[i].scope == GDX_COURSE_SCOPE_COURSE) {
            return &records[i];
        }
    }
    return nullptr;
}

const GdxCourseElement* course_music_entry(const char* courseId) {
    const GdxCourseExtRecord* rec;

    ensure_scanned(); // race start can be the first call of the session
    rec = course_bgm_record(courseId);
    return rec == nullptr ? nullptr : gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, rec->key);
}

// File name of a key without the prefix: the key "bgm.custom.ocean_theme" gives "ocean_theme".
std::string key_suffix(const char* key) {
    return std::string(key + kKeyPrefixLen);
}

// The file for the suffix in either custom folder, with any supported extension, or an empty path.
std::filesystem::path custom_file_for(const std::string& suffix) {
    for (const char* folder : gdx::music::kCourseFolders) {
        if (std::strcmp(folder, "music") == 0 && gdx::music::IsReplacementStem(suffix)) {
            continue;
        }
        const auto candidate = gdx::music::FindFile(exe_relative(folder), suffix);
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return {};
}

// Reads a whole music file. False when it is unreadable or over the copy limit.
bool read_music_file(const std::filesystem::path& path, std::vector<unsigned char>* out) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);

    if (ec || size > kMaxCopyBytes) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    out->resize(static_cast<size_t>(size));
    if (!in || (size > 0 && !in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size)))) {
        return false;
    }
    return true;
}

// True when the file at path holds exactly bytes.
bool same_content(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    std::vector<unsigned char> other;

    if (std::filesystem::file_size(path, ec) != bytes.size() || ec) {
        return false;
    }
    return read_music_file(path, &other) && other == bytes;
}

// The file already registered or stored under key, or an empty path.
std::filesystem::path occupant_for(const char* key) {
    const GdxCourseElement* entry = gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, key);

    if (entry != nullptr && entry->sourcePath != nullptr) {
        return std::filesystem::path(entry->sourcePath);
    }
    return custom_file_for(key_suffix(key));
}

void copy_key(const char* key, char* outKey, int outCap) {
    if (outKey != nullptr && outCap > 0 && outKey != key) {
        std::snprintf(outKey, static_cast<size_t>(outCap), "%s", key);
    }
}

} // namespace

extern "C" void gdx_course_music_scan_files(void) {
    if (!sScanned) {
        sScanned = true;
        scan_all();
    }
}

extern "C" int gdx_course_music_scan(void) {
    gdx_course_boot();
    return music_count();
}

extern "C" int gdx_course_music_count(void) {
    return music_count();
}

extern "C" const char* gdx_course_music_display_at(int extIndex) {
    const GdxCourseElement* entry = music_entry(extIndex);
    return entry != nullptr ? entry->displayName : nullptr;
}

extern "C" int gdx_course_music_course_id(const char* name, const char* extension, char* out, int outCap) {
    int nameLen = 0;
    int extLen = 0;

    if (name == nullptr || extension == nullptr || out == nullptr || outCap <= 0) {
        return 0;
    }
    while (nameLen < GDX_COURSE_TITLE_MAX && name[nameLen] != '\0') {
        nameLen++;
    }
    while (extLen < 5 && extension[extLen] != '\0') {
        extLen++;
    }
    if (nameLen == 0 || extLen == 0 || nameLen + 1 + extLen + 1 > outCap) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(outCap), "%.*s.%.*s", nameLen, name, extLen, extension);
    return 1;
}

extern "C" int gdx_course_music_choice(const char* courseId) {
    const GdxCourseElement* entry = course_music_entry(courseId);

    if (entry == nullptr) {
        return -1;
    }
    for (int i = 0; i < music_count(); i++) {
        if (music_entry(i) == entry) {
            return i;
        }
    }
    return -1;
}

extern "C" int gdx_course_music_set_choice(const char* courseId, int extIndex) {
    const GdxCourseElement* entry;
    int rc;

    ensure_scanned();
    if (courseId == nullptr) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (extIndex < 0) {
        rc = gdx_course_ext_clear(courseId, GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE);
        gdx_course_state_flush();
        return rc == GDX_COURSE_ERR_UNKNOWN ? GDX_COURSE_OK : rc;
    }
    entry = music_entry(extIndex);
    if (entry == nullptr) {
        return GDX_COURSE_ERR_UNKNOWN;
    }
    rc = gdx_course_ext_set(courseId, GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, entry->key);
    if (rc < 0) {
        return rc;
    }
    gdx_course_state_flush();
    return entry->stockId;
}

extern "C" int gdx_course_music_preview(int extIndex) {
    const GdxCourseElement* entry = music_entry(extIndex);

    if (entry == nullptr || entry->sourcePath == nullptr) {
        gdx_port_logf("[music] preview of custom row %d: no file\n", extIndex);
        return 0;
    }
    const int queued = gdx_music_stream_play_file(entry->sourcePath, entry->displayName);
    gdx_port_logf("[music] preview %s: %s\n", entry->sourcePath, queued != 0 ? "queued" : "not queued");
    return queued;
}

extern "C" void gdx_course_music_arm_for_course(const char* courseId, int bgmId) {
    const GdxCourseElement* entry = course_music_entry(courseId);

    if (entry == nullptr || entry->sourcePath == nullptr) {
        // A song chosen on another install, or a file removed since, leaves a key with no file behind it.
        const GdxCourseExtRecord* rec = course_bgm_record(courseId);
        if (rec != nullptr) {
            gdx_port_logf("[music] %s: song %s is not in music/, the stock track plays\n", courseId, rec->key);
        }
        gdx_music_stream_set_course_override(bgmId, nullptr, nullptr);
        return;
    }
    gdx_port_logf("[music] %s: course song %s for BGM id %d\n", courseId, entry->sourcePath, bgmId);
    gdx_music_stream_set_course_override(bgmId, entry->sourcePath, entry->displayName);
}

extern "C" int gdx_course_music_export_key(const char* key, const char* directory) {
    if (!key || !directory) {
        return 0;
    }
    const auto* entry = gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, key);
    if (!entry || !entry->sourcePath) {
        gdx_port_logf("[music] export: no source file for key %s\n", key);
        return 0;
    }
    std::error_code ec;
    const std::filesystem::path source(entry->sourcePath);
    const auto destination = std::filesystem::path(directory) / (key_suffix(key) + source.extension().string());
    const auto size = std::filesystem::file_size(source, ec);
    if (ec || size > kMaxCopyBytes) {
        gdx_port_logf("[music] export: %s not copied (%s)\n", source.string().c_str(),
                      ec ? "unreadable" : "too large");
        return 0;
    }
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        gdx_port_logf("[music] export: copy to %s failed\n", destination.string().c_str());
        return 0;
    }
    return 1;
}

extern "C" int gdx_course_music_export_course(const char* courseId, const char* directory) {
    int count = 0;
    int copied = 0;
    const auto* records = gdx_course_ext_records(courseId, &count);
    for (int i = 0; records && i < count; ++i) {
        if (records[i].kind == GDX_COURSE_KIND_BGM) {
            copied += gdx_course_music_export_key(records[i].key, directory);
        }
    }
    return copied;
}

extern "C" int gdx_course_music_adopt(const char* gdxcPath, const char* key, char* outKey, int outCap) {
    std::filesystem::path source;
    std::filesystem::path occupant;
    std::vector<unsigned char> bytes;
    std::error_code ec;
    char fingerprintKey[GDX_COURSE_KEY_MAX + 1];
    const char* targetKey = key;

    if (gdxcPath == nullptr || key == nullptr || std::strncmp(key, kKeyPrefix, kKeyPrefixLen) != 0 ||
        !gdx_course_key_is_valid(key) || key[kKeyPrefixLen] == '\0') {
        return 0;
    }
    copy_key(key, outKey, outCap);
    ensure_scanned();
    const bool registered = gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, key) != nullptr;
    const std::filesystem::path dir = std::filesystem::path(gdxcPath).parent_path();
    source = gdx::music::FindFile(dir, key_suffix(key));
    if (source.empty()) {
        if (!registered) {
            gdx_port_logf("[music] import %s: no audio file beside the .gdxc\n", key);
        }
        return registered ? 1 : 0;
    }
    if (!read_music_file(source, &bytes)) {
        gdx_port_logf("[music] import %s: %s not copied (unreadable or over 32 MB)\n", key, source.string().c_str());
        return registered ? 1 : -1;
    }

    occupant = occupant_for(key);
    // A legacy course named like a global slot must never install a menu replacement.
    const bool reserved = gdx::music::IsReplacementStem(key_suffix(key));
    if (reserved && registered && !occupant.empty() && same_content(occupant, bytes)) {
        return 1;
    }
    if (reserved || (!occupant.empty() && !same_content(occupant, bytes))) {
        const uint32_t crc = gdx_course_registry_crc32(bytes.data(), bytes.size());
        if (gdx_course_registry_music_fingerprint_key(key, crc, fingerprintKey, sizeof fingerprintKey) == 0) {
            return -1;
        }
        targetKey = fingerprintKey;
        occupant = occupant_for(targetKey);
        if (!occupant.empty() && !same_content(occupant, bytes)) {
            gdx_port_logf("[music] import %s: %s holds a different song, not copied\n", key, targetKey);
            return -1;
        }
        gdx_port_logf("[music] import %s: %s, stored as %s\n", key,
                      reserved ? "name reserved for a global replacement" : "a different song uses this name",
                      targetKey);
    }
    if (occupant.empty()) {
        const std::filesystem::path folder = exe_relative("music");
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            gdx_port_logf("[music] import %s: cannot create music directory\n", targetKey);
            return -1;
        }
        const auto destination = folder / (key_suffix(targetKey) + gdx::music::Fold(source.extension().string()));
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::skip_existing, ec);
        if (ec || !same_content(destination, bytes)) {
            gdx_port_logf("[music] import %s: copy into music failed or destination changed\n", targetKey);
            return -1;
        }
        gdx_port_logf("[music] import %s: available in music\n", targetKey);
    }
    scan_all();
    if (gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, targetKey) == nullptr) {
        return -1;
    }
    copy_key(targetKey, outKey, outCap);
    return 1;
}
