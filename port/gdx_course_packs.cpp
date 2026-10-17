// port/gdx_course_packs.cpp -- pack manifests (mods/<pack>/elements.txt) and skybox menu glue
// (plan slice 9, scopes B and C). Parsing is in gdx_course_registry.c; this file owns the files.

#include "gdx_course_packs.h"

#include "gdx_course_registry.h"
#include "gdx_course_state.h"
#include "port_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr const char* kManifestName = "elements.txt";

bool sPacksLoaded = false;

std::filesystem::path exe_relative(const char* relative) {
    char buf[1024];
    const char* full = gdx_exe_relative_path(buf, sizeof buf, relative);
    return std::filesystem::path(full != nullptr ? full : relative);
}

// Reads one line into buf (capacity cap, which must exceed GDX_COURSE_MANIFEST_LINE_MAX so an overlong
// line is detected). Returns -1 at end of file. Otherwise returns the full byte count of the line without
// its newline; bytes past cap are counted but not stored, so the parser rejects them by length.
long read_line(std::FILE* f, char* buf, size_t cap) {
    long n = 0;
    bool any = false;
    int c;

    while ((c = std::fgetc(f)) != EOF) {
        any = true;
        if (c == '\n') {
            break;
        }
        if (static_cast<size_t>(n) < cap) {
            buf[n] = static_cast<char>(c);
        }
        n++;
    }
    if (!any) {
        return -1;
    }
    if (static_cast<size_t>(n) < cap) {
        buf[n] = '\0';
    }
    return n;
}

// Registers one accepted manifest entry. Returns 1 when it was added.
int register_entry(const std::filesystem::path& packDir, const std::string& manifest, int lineNo,
                   const GdxCourseManifestEntry& e) {
    int rc;

    if (e.path[0] != '\0') {
        const std::filesystem::path full = packDir / e.path;
        const std::string fullText = full.string();
        std::error_code ec;

        if (fullText.size() >= GDX_COURSE_PATH_MAX) {
            gdx_port_logf("[elements] %s: line %d: music path is too long, skipped\n", manifest.c_str(), lineNo);
            return 0;
        }
        if (!std::filesystem::is_regular_file(full, ec)) {
            gdx_port_logf("[elements] %s: line %d: music file %s not found, skipped\n", manifest.c_str(), lineNo,
                          fullText.c_str());
            return 0;
        }
        rc = gdx_course_registry_register_file(e.kind, e.key, e.displayName, e.fallbackId, fullText.c_str());
    } else {
        rc = gdx_course_registry_register(e.kind, e.key, e.displayName, e.fallbackId);
    }

    if (rc == GDX_COURSE_OK) {
        if (e.kind == GDX_COURSE_KIND_BGM || e.kind == GDX_COURSE_KIND_SKYBOX) {
            gdx_port_logf("[elements] %s: line %d: registered %s\n", manifest.c_str(), lineNo, e.key);
        } else {
            gdx_port_logf("[elements] %s: line %d: registered %s (no menu in this build)\n", manifest.c_str(), lineNo,
                          e.key);
        }
        return 1;
    }
    if (rc == GDX_COURSE_ERR_DUPLICATE) {
        gdx_port_logf("[elements] %s: line %d: key %s is already registered, skipped\n", manifest.c_str(), lineNo,
                      e.key);
    } else {
        gdx_port_logf("[elements] %s: line %d: key %s not registered (%d)\n", manifest.c_str(), lineNo, e.key, rc);
    }
    return 0;
}

int load_manifest(const std::filesystem::path& packDir) {
    const std::filesystem::path manifest = packDir / kManifestName;
    const std::string manifestText = manifest.string();
    char buf[GDX_COURSE_MANIFEST_LINE_MAX + 1];
    std::FILE* f = std::fopen(manifestText.c_str(), "rb");
    int lineNo = 0;
    int added = 0;
    long n;

    if (f == nullptr) {
        gdx_port_logf("[elements] %s: cannot open\n", manifestText.c_str());
        return 0;
    }
    while ((n = read_line(f, buf, sizeof buf)) >= 0) {
        GdxCourseManifestEntry entry;
        char reason[GDX_COURSE_MANIFEST_REASON_MAX];
        int rc;

        lineNo++;
        rc = gdx_course_manifest_parse_line(buf, static_cast<size_t>(n), lineNo, &entry, reason, sizeof reason);
        if (rc == GDX_COURSE_MANIFEST_SKIP) {
            continue;
        }
        if (rc < 0) {
            gdx_port_logf("[elements] %s: %s, skipped\n", manifestText.c_str(), reason);
            continue;
        }
        added += register_entry(packDir, manifestText, lineNo, entry);
    }
    std::fclose(f);
    return added;
}

// Skybox extension entry at extIndex (0-based), or nullptr.
const GdxCourseElement* skybox_entry(int extIndex) {
    const int stock = gdx_course_registry_stock_count(GDX_COURSE_KIND_SKYBOX);

    if (extIndex < 0 || extIndex >= gdx_course_registry_count(GDX_COURSE_KIND_SKYBOX) - stock) {
        return nullptr;
    }
    return gdx_course_registry_at(GDX_COURSE_KIND_SKYBOX, stock + extIndex);
}

// The course-wide skybox record of a course, or nullptr.
const GdxCourseExtRecord* course_skybox_record(const char* courseId) {
    int count = 0;
    const GdxCourseExtRecord* records;

    if (courseId == nullptr) {
        return nullptr;
    }
    records = gdx_course_ext_records(courseId, &count);
    for (int i = 0; records != nullptr && i < count; i++) {
        if (records[i].kind == GDX_COURSE_KIND_SKYBOX && records[i].scope == GDX_COURSE_SCOPE_COURSE) {
            return &records[i];
        }
    }
    return nullptr;
}

} // namespace

extern "C" int gdx_course_packs_load(void) {
    std::error_code ec;
    std::vector<std::filesystem::path> packs;
    const std::filesystem::path root = exe_relative("mods");
    int total = 0;

    if (sPacksLoaded) {
        return 0;
    }
    sPacksLoaded = true;
    if (!std::filesystem::is_directory(root, ec)) {
        return 0;
    }
    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
        gdx_port_logf("[elements] %s: cannot list packs\n", root.string().c_str());
        return 0;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        const std::filesystem::path p = it->path();
        if (std::filesystem::is_directory(p, entryEc) &&
            std::filesystem::is_regular_file(p / kManifestName, entryEc)) {
            packs.push_back(p);
        }
    }
    std::sort(packs.begin(), packs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.filename().string() < b.filename().string();
    });
    for (const std::filesystem::path& pack : packs) {
        total += load_manifest(pack);
    }
    if (total > 0) {
        gdx_port_logf("[elements] %d element(s) registered from %d pack(s)\n", total, static_cast<int>(packs.size()));
    }
    return total;
}

extern "C" int gdx_course_skybox_count(void) {
    gdx_course_boot();
    return gdx_course_registry_count(GDX_COURSE_KIND_SKYBOX) - gdx_course_registry_stock_count(GDX_COURSE_KIND_SKYBOX);
}

extern "C" const char* gdx_course_skybox_display_at(int extIndex) {
    const GdxCourseElement* entry;

    gdx_course_boot();
    entry = skybox_entry(extIndex);
    return entry != nullptr ? entry->displayName : nullptr;
}

extern "C" int gdx_course_skybox_choice(const char* courseId) {
    const GdxCourseExtRecord* record;

    gdx_course_boot();
    record = course_skybox_record(courseId);
    if (record == nullptr) {
        return -1;
    }
    for (int i = 0; i < gdx_course_skybox_count(); i++) {
        const GdxCourseElement* entry = skybox_entry(i);
        if (entry != nullptr && std::strcmp(entry->key, record->key) == 0) {
            return i;
        }
    }
    return -1;
}

extern "C" int gdx_course_skybox_set_choice(const char* courseId, int extIndex) {
    const GdxCourseElement* entry;
    int rc;

    gdx_course_boot();
    if (courseId == nullptr) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (extIndex < 0) {
        rc = gdx_course_ext_clear(courseId, GDX_COURSE_KIND_SKYBOX, GDX_COURSE_SCOPE_COURSE);
        gdx_course_state_flush();
        return rc == GDX_COURSE_ERR_UNKNOWN ? GDX_COURSE_OK : rc;
    }
    entry = skybox_entry(extIndex);
    if (entry == nullptr) {
        return GDX_COURSE_ERR_UNKNOWN;
    }
    rc = gdx_course_ext_set(courseId, GDX_COURSE_KIND_SKYBOX, GDX_COURSE_SCOPE_COURSE, entry->key);
    if (rc < 0) {
        return rc;
    }
    gdx_course_state_flush();
    return entry->stockId; // the fallback: a stock skybox 0..7
}

extern "C" int gdx_course_skybox_resolve(const char* courseId) {
    const GdxCourseExtRecord* record;
    int missing = 0;
    int stockId;

    gdx_course_boot();
    record = course_skybox_record(courseId);
    if (record == nullptr) {
        return -1;
    }
    stockId = gdx_course_registry_resolve(GDX_COURSE_KIND_SKYBOX, record->key, &missing);
    return missing != 0 ? -1 : stockId;
}
