#include "gdx_recovery_metadata.h"
#include "gdx_course_registry.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_settings.h"

namespace gdx::recovery {
namespace {
std::string identity(const Record& r) {
    return r.name + "." + r.extension;
}
}

bool readMetadata(const Record& r, Kind kind, Bytes& out) {
    const auto id = identity(r);
    const auto bind = gdx_course_registry_crc32(reinterpret_cast<const uint8_t*>(id.data()), id.size());
    Bytes bytes(65536);
    int size = 0;
    if (kind == Kind::Extensions) {
        int count = 0;
        const auto* records = gdx_course_ext_records(id.c_str(), &count);
        if (count) {
            size = gdx_course_ext_encode(records, count, bind, bytes.data(), bytes.size());
        }
    } else if (kind == Kind::Title) {
        char title[GDX_COURSE_TITLE_MAX + 1]{};
        if (gdx_course_title_get(id.c_str(), title, sizeof(title))) {
            size = gdx_course_title_encode(title, bind, bytes.data(), bytes.size());
        }
    } else if (kind == Kind::Forks) {
        GdxBranchSet forks{};
        if (gdx_branch_store_get(id.c_str(), &forks)) {
            size = gdx_branch_encode(&forks, bind, bytes.data(), bytes.size());
        }
    } else if (kind == Kind::Settings) {
        GdxCourseSettings settings{};
        if (gdx_settings_store_get(id.c_str(), &settings)) {
            size = gdx_settings_encode(&settings, bind, bytes.data(), bytes.size());
        }
    } else {
        return false;
    }
    if (size < 0) {
        return false;
    }
    bytes.resize(size_t(size));
    out = std::move(bytes);
    return true;
}

bool writeMetadata(const Record& r, Kind kind, const Bytes& bytes) {
    const auto id = identity(r);
    const auto bind = gdx_course_registry_crc32(reinterpret_cast<const uint8_t*>(id.data()), id.size());
    if (kind == Kind::Extensions) {
        GdxCourseExtBlock ext{};
        if (!bytes.empty() && gdx_course_ext_decode(bytes.data(), bytes.size(), bind, &ext) != 0) {
            return false;
        }
        return gdx_course_ext_restore(id.c_str(), ext.records, ext.count) == 0;
    }
    if (kind == Kind::Title) {
        if (bytes.empty()) {
            gdx_course_title_clear(id.c_str());
            return true;
        }
        char title[GDX_COURSE_TITLE_MAX + 1]{};
        return gdx_course_title_decode(bytes.data(), bytes.size(), bind, title, sizeof(title)) == 0 &&
               gdx_course_title_set(id.c_str(), title) == 0;
    }
    if (kind == Kind::Forks) {
        GdxBranchSet forks{};
        return (bytes.empty() || gdx_branch_decode(bytes.data(), bytes.size(), bind, &forks) == 0) &&
               gdx_branch_store_set(id.c_str(), &forks) == 0;
    }
    if (kind == Kind::Settings) {
        GdxCourseSettings settings{};
        return (bytes.empty() || gdx_settings_decode(bytes.data(), bytes.size(), bind, &settings) == 0) &&
               gdx_settings_store_set(id.c_str(), &settings) == 0;
    }
    return false;
}

bool canRestoreMetadata(const Record& r) {
    const auto id = identity(r);
    const auto bind = gdx_course_registry_crc32(reinterpret_cast<const uint8_t*>(id.data()), id.size());
    struct Counts { int extensions = 0; int titles = 0; } counts;
    Bytes store(GDX_COURSE_STATE_MAX_BYTES);
    const int length = gdx_course_store_encode_file(store.data(), store.size());
    const auto count = [](void* context, const GdxCourseStateEntry* entry) -> int {
        auto& result = *static_cast<Counts*>(context);
        result.extensions += entry->recordCount != 0;
        result.titles += entry->hasTitle != 0;
        return 0;
    };
    if (length < 0 || gdx_course_state_decode(store.data(), size_t(length), count, &counts) != 0) {
        return false;
    }
    for (const auto& c : r.changes) {
        if (int(c.kind) < 2 || int(c.kind) > 5 || c.before.empty()) {
            continue;
        }
        Bytes current;
        if (!readMetadata(r, c.kind, current)) {
            return false;
        }
        const bool occupied = !current.empty();
        if (c.kind == Kind::Extensions) {
            GdxCourseExtBlock block{};
            if (gdx_course_ext_decode(c.before.data(), c.before.size(), bind, &block) != 0 ||
                (!occupied && counts.extensions >= GDX_COURSE_STORE_SLOTS)) {
                return false;
            }
        } else if (c.kind == Kind::Title) {
            char title[GDX_COURSE_TITLE_MAX + 1]{};
            if (gdx_course_title_decode(c.before.data(), c.before.size(), bind, title, sizeof(title)) != 0 ||
                (!occupied && counts.titles >= GDX_COURSE_TITLE_SLOTS)) {
                return false;
            }
        } else {
            int size;
            if (c.kind == Kind::Forks) {
                GdxBranchSet forks{};
                if (gdx_branch_decode(c.before.data(), c.before.size(), bind, &forks) != 0) {
                    return false;
                }
                store.resize(GDX_BRANCH_FILE_MAX_BYTES);
                size = gdx_branch_store_encode_file(store.data(), store.size());
            } else {
                GdxCourseSettings settings{};
                if (gdx_settings_decode(c.before.data(), c.before.size(), bind, &settings) != 0) {
                    return false;
                }
                store.resize(GDX_SETTINGS_FILE_MAX_BYTES);
                size = gdx_settings_store_encode_file(store.data(), store.size());
            }
            if (size < 8 || (!occupied && unsigned(store[6] | (unsigned(store[7]) << 8)) >= GDX_BRANCH_STORE_SLOTS)) {
                return false;
            }
        }
    }
    return true;
}
}
