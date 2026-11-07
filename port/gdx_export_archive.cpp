// port/gdx_export_archive.cpp -- "Export everything" zip writer. See gdx_export_archive.h.

#include "gdx_export_archive.h"

#include "gdx_export_all.h"

#include <zip.h>

#include <algorithm>
#include <set>
#include <system_error>

namespace gdx {
namespace {

int group_index(unsigned group) {
    switch (group) {
        case GDX_EXPORT_TRACKS:
            return 0;
        case GDX_EXPORT_PLAYER:
            return 1;
        case GDX_EXPORT_SETTINGS:
            return 2;
        case GDX_EXPORT_KEYS:
            return 3;
        default:
            return -1;
    }
}

std::string utf8_name(const std::filesystem::path& path) {
    const auto name = path.filename().u8string();
    return std::string(name.begin(), name.end());
}

// Archives, packs and media are compressed already: deflating them again costs time for nothing.
bool stored(const std::string& member) {
    static const char* const kStored[] = { ".o2r", ".otr", ".zip", ".gdxp", ".ogg", ".flac", ".mp3", ".wav", ".png" };
    std::string lower = member;

    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
    for (const char* ext : kStored) {
        const size_t n = std::char_traits<char>::length(ext);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0) {
            return true;
        }
    }
    return false;
}

void walk(const std::filesystem::path& root, const std::string& rel, ExportPlan& plan, std::set<std::string>& seen,
          int depth) {
    std::error_code ec;
    const std::filesystem::path folder = rel.empty() ? root : root / std::filesystem::u8path(rel);
    std::filesystem::directory_iterator it(folder, ec);

    // Nothing the export takes sits deeper than a few folders; the cap stops a junction loop that slipped past.
    if (depth > 16) {
        return;
    }
    if (ec) {
        if (rel.empty() || ec != std::errc::no_such_file_or_directory) {
            if (plan.error.empty()) {
                plan.error = folder.string() + ": " + ec.message();
            }
        }
        return;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            if (plan.error.empty()) {
                plan.error = folder.string() + ": " + ec.message();
            }
            return;
        }
        std::error_code itemEc;
        const std::filesystem::file_status status = it->symlink_status(itemEc);
        if (itemEc || std::filesystem::is_symlink(status)) {
            continue;
        }
        const std::string name = utf8_name(it->path());
        const std::string member = rel.empty() ? name : rel + "/" + name;

        if (std::filesystem::is_directory(status)) {
            if (gdx_export_folder_wanted(member.c_str())) {
                walk(root, member, plan, seen, depth + 1);
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const unsigned group = gdx_export_classify(member.c_str());
        const int index = group_index(group);
        if (index < 0) {
            continue;
        }
        // Windows folds case, so two spellings of one name are one file.
        std::string key = member;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
        if (!seen.insert(key).second) {
            continue;
        }
        const uint64_t size = static_cast<uint64_t>(it->file_size(itemEc));
        if (itemEc) {
            continue;
        }
        plan.files.push_back(ExportFile{ it->path(), member, group, size });
        plan.count[index]++;
        plan.bytes[index] += size;
    }
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
        return true;
    }
    return a.lexically_normal() == b.lexically_normal();
}

zip_t* create_archive(const std::filesystem::path& path, std::string& error) {
    zip_error_t zerr;
    zip_error_init(&zerr);
#ifdef _WIN32
    zip_source_t* source = zip_source_win32w_create(path.wstring().c_str(), 0, ZIP_LENGTH_TO_END, &zerr);
#else
    zip_source_t* source = zip_source_file_create(path.string().c_str(), 0, ZIP_LENGTH_TO_END, &zerr);
#endif
    zip_t* archive = nullptr;

    if (source != nullptr) {
        archive = zip_open_from_source(source, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
        if (archive == nullptr) {
            zip_source_free(source);
        }
    }
    if (archive == nullptr) {
        error = std::string("Could not create the zip: ") + zip_error_strerror(&zerr);
    }
    zip_error_fini(&zerr);
    return archive;
}

zip_source_t* file_source(zip_t* archive, const std::filesystem::path& path) {
#ifdef _WIN32
    return zip_source_win32w(archive, path.wstring().c_str(), 0, ZIP_LENGTH_TO_END);
#else
    return zip_source_file(archive, path.string().c_str(), 0, ZIP_LENGTH_TO_END);
#endif
}

struct ProgressRelay {
    void (*progress)(double, void*);
    void* ctx;
};

void relay_progress(zip_t*, double done, void* ud) {
    const ProgressRelay* relay = static_cast<const ProgressRelay*>(ud);
    relay->progress(done, relay->ctx);
}

} // namespace

ExportPlan PlanExport(const std::filesystem::path& dataRoot, const std::filesystem::path& exeRoot) {
    ExportPlan plan;
    std::set<std::string> seen;

    walk(dataRoot, "", plan, seen, 0);
    if (!exeRoot.empty() && !same_folder(dataRoot, exeRoot)) {
        // Only music/ and exports/ live beside the executable when the data folder is elsewhere.
        for (const char* folder : { "music", "exports" }) {
            walk(exeRoot, folder, plan, seen, 1);
        }
    }
    std::sort(plan.files.begin(), plan.files.end(),
              [](const ExportFile& a, const ExportFile& b) { return a.member < b.member; });
    return plan;
}

size_t ExportCount(const ExportPlan& plan, unsigned groups) {
    size_t total = 0;
    for (const ExportFile& file : plan.files) {
        if ((file.group & groups) != 0) {
            total++;
        }
    }
    return total;
}

uint64_t ExportBytes(const ExportPlan& plan, unsigned groups) {
    uint64_t total = 0;
    for (const ExportFile& file : plan.files) {
        if ((file.group & groups) != 0) {
            total += file.size;
        }
    }
    return total;
}

bool WriteExport(const ExportPlan& plan, unsigned groups, const std::filesystem::path& zipPath,
                 void (*progress)(double done, void* ctx), void* ctx, std::string& error) {
    error.clear();
    if (ExportCount(plan, groups) == 0) {
        error = "Nothing to export: the picked groups have no files.";
        return false;
    }
    std::error_code ec;
    if (zipPath.has_parent_path()) {
        std::filesystem::create_directories(zipPath.parent_path(), ec);
    }
    zip_t* archive = create_archive(zipPath, error);
    if (archive == nullptr) {
        return false;
    }
    for (const ExportFile& file : plan.files) {
        if ((file.group & groups) == 0) {
            continue;
        }
        zip_source_t* data = file_source(archive, file.source);
        if (data == nullptr) {
            error = "Could not read " + file.member + ": " + zip_strerror(archive);
            zip_discard(archive);
            return false;
        }
        const zip_int64_t index = zip_file_add(archive, file.member.c_str(), data, ZIP_FL_ENC_UTF_8);
        if (index < 0) {
            error = "Could not add " + file.member + ": " + zip_strerror(archive);
            zip_source_free(data);
            zip_discard(archive);
            return false;
        }
        if (stored(file.member) &&
            zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0) {
            error = "Could not add " + file.member + ": " + zip_strerror(archive);
            zip_discard(archive);
            return false;
        }
    }
    ProgressRelay relay{ progress, ctx };
    if (progress != nullptr) {
        zip_register_progress_callback_with_state(archive, 0.01, relay_progress, nullptr, &relay);
    }
    // libzip reads every file now and writes a temporary file it renames over zipPath, so a failed close leaves
    // no half-written zip behind.
    if (zip_close(archive) != 0) {
        error = std::string("Could not write the zip: ") + zip_strerror(archive);
        zip_discard(archive);
        return false;
    }
    return true;
}

} // namespace gdx
