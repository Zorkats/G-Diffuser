// port/gdx_content_pack.cpp -- .gdxp share packages. See gdx_content_pack.h.

#include "gdx_content_pack.h"

#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

enum class MemberKind { Container, Sidecar, Audio };

struct Member {
    std::string name;
    std::string stem;
    MemberKind kind;
    uint64_t size;
    zip_uint64_t index; // zip index on extraction; unused when writing
};

constexpr const char* kContainerExt = ".gdxc";
constexpr const char* kSidecarExts[] = { ".gdxe", ".gdxt", ".gdxs" };
constexpr const char* kAudioExts[] = { ".ogg", ".flac", ".wav" };
// Windows opens these as devices whatever extension follows, so a member may never be named after one.
constexpr const char* kReservedStems[] = { "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                           "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                           "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// Member names are plain ASCII, so they mean the same file on every system and code page.
bool unsafe_char(unsigned char c) {
    return c < 0x20 || c >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
           c == '<' || c == '>' || c == '|';
}

// Fills kind and stem for a plain file name with a package extension. Returns false for anything else:
// a path, a hidden or dot-only name, a Windows device name, or an unknown extension.
bool classify(const std::string& name, MemberKind* kind, std::string* stem) {
    if (name.empty() || name.size() > GDX_CONTENT_PACK_NAME_MAX || name.front() == '.' || name.back() == '.' ||
        name.back() == ' ') {
        return false;
    }
    for (char c : name) {
        if (unsafe_char(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return false;
    }
    const std::string device = upper(name.substr(0, name.find('.')));
    for (const char* reserved : kReservedStems) {
        if (device == reserved) {
            return false;
        }
    }

    const std::string ext = lower(name.substr(dot));
    *stem = name.substr(0, dot);
    if (ext == kContainerExt) {
        *kind = MemberKind::Container;
        return true;
    }
    for (const char* known : kSidecarExts) {
        if (ext == known) {
            *kind = MemberKind::Sidecar;
            return true;
        }
    }
    for (const char* known : kAudioExts) {
        if (ext == known) {
            *kind = MemberKind::Audio;
            return true;
        }
    }
    return false;
}

// Applies the package rules to a classified member list. container receives the .gdxc name.
int check_members(const std::vector<Member>& members, std::string* container) {
    std::vector<std::string> seen;
    const Member* found = nullptr;
    uint64_t total = 0;

    if (members.size() > GDX_CONTENT_PACK_MAX_MEMBERS) {
        return GDX_CONTENT_PACK_ERR_TOO_MANY;
    }
    for (const Member& member : members) {
        const std::string key = lower(member.name);
        const uint64_t cap =
            member.kind == MemberKind::Audio ? GDX_CONTENT_PACK_MAX_AUDIO_BYTES : GDX_CONTENT_PACK_MAX_DATA_BYTES;

        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            return GDX_CONTENT_PACK_ERR_DUPLICATE;
        }
        seen.push_back(key);
        if (member.size > cap) {
            return GDX_CONTENT_PACK_ERR_TOO_LARGE;
        }
        total += member.size;
        if (member.kind == MemberKind::Container) {
            if (found != nullptr) {
                return GDX_CONTENT_PACK_ERR_CONTAINER;
            }
            found = &member;
        }
    }
    if (found == nullptr) {
        return GDX_CONTENT_PACK_ERR_CONTAINER;
    }
    if (total > GDX_CONTENT_PACK_MAX_TOTAL_BYTES) {
        return GDX_CONTENT_PACK_ERR_TOO_LARGE;
    }
    // The importer finds a sidecar by swapping the container's extension, so the stems must match exactly.
    for (const Member& member : members) {
        if (member.kind == MemberKind::Sidecar && member.stem != found->stem) {
            return GDX_CONTENT_PACK_ERR_STEM;
        }
    }
    *container = found->name;
    return GDX_CONTENT_PACK_OK;
}

// The callers build host paths in the ANSI code page (fopen, GetModuleFileNameA), while libzip reads a
// plain char path as UTF-8 on Windows. The win32a sources keep both sides on the same encoding.
zip_t* open_archive(const char* path, int flags, int* zipError) {
#ifdef _WIN32
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_win32a_create(path, 0, ZIP_LENGTH_TO_END, &error);
    zip_t* archive = nullptr;

    if (source != nullptr) {
        archive = zip_open_from_source(source, flags, &error);
        if (archive == nullptr) {
            zip_source_free(source);
        }
    }
    *zipError = archive == nullptr ? zip_error_code_zip(&error) : ZIP_ER_OK;
    zip_error_fini(&error);
    return archive;
#else
    return zip_open(path, flags, zipError);
#endif
}

zip_source_t* file_source(zip_t* archive, const std::string& path) {
#ifdef _WIN32
    return zip_source_win32a(archive, path.c_str(), 0, ZIP_LENGTH_TO_END);
#else
    return zip_source_file(archive, path.c_str(), 0, ZIP_LENGTH_TO_END);
#endif
}

// Copies one opened member into target, reading no more than the size its directory entry declared.
int copy_member(zip_file_t* file, const std::filesystem::path& target, uint64_t declared) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(64 * 1024);
    uint64_t written = 0;

    if (!out) {
        return GDX_CONTENT_PACK_ERR_IO;
    }
    for (;;) {
        const zip_int64_t got = zip_fread(file, buffer.data(), buffer.size());

        if (got < 0) {
            return GDX_CONTENT_PACK_ERR_CORRUPT; // includes a zip CRC mismatch at the end of the member
        }
        if (got == 0) {
            break;
        }
        written += static_cast<uint64_t>(got);
        if (written > declared) {
            return GDX_CONTENT_PACK_ERR_CORRUPT;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(got));
        if (!out) {
            return GDX_CONTENT_PACK_ERR_IO;
        }
    }
    return written == declared ? GDX_CONTENT_PACK_OK : GDX_CONTENT_PACK_ERR_CORRUPT;
}

} // namespace

extern "C" int gdx_content_pack_clear_dir(const char* dir, int removeDir) {
    std::error_code ec;

    if (dir == nullptr || dir[0] == '\0') {
        return GDX_CONTENT_PACK_ERR_ARGS;
    }
    const std::filesystem::path folder(dir);
    std::filesystem::create_directories(folder, ec);
    if (ec || !std::filesystem::is_directory(folder, ec)) {
        return GDX_CONTENT_PACK_ERR_IO;
    }
    std::filesystem::directory_iterator it(folder, ec);
    if (ec) {
        return GDX_CONTENT_PACK_ERR_IO;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return GDX_CONTENT_PACK_ERR_IO;
        }
        std::error_code itemEc;
        if (it->is_regular_file(itemEc)) {
            std::filesystem::remove(it->path(), itemEc);
            if (itemEc) {
                return GDX_CONTENT_PACK_ERR_IO;
            }
        }
    }
    if (removeDir) {
        // Fails quietly when something other than a regular file is left inside, which is never deleted.
        std::filesystem::remove(folder, ec);
    }
    return GDX_CONTENT_PACK_OK;
}

extern "C" int gdx_content_pack_write_dir(const char* packPath, const char* dir) {
    std::vector<Member> members;
    std::string container;
    std::error_code ec;
    int zipError = 0;

    if (packPath == nullptr || dir == nullptr) {
        return GDX_CONTENT_PACK_ERR_ARGS;
    }
    const std::filesystem::path folder(dir);
    std::filesystem::directory_iterator it(folder, ec);
    if (ec) {
        return GDX_CONTENT_PACK_ERR_IO;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return GDX_CONTENT_PACK_ERR_IO;
        }
        std::error_code itemEc;
        if (!it->is_regular_file(itemEc)) {
            continue;
        }
        Member member;
        member.name = it->path().filename().string();
        if (!classify(member.name, &member.kind, &member.stem)) {
            return GDX_CONTENT_PACK_ERR_MEMBER;
        }
        member.size = static_cast<uint64_t>(it->file_size(itemEc));
        if (itemEc) {
            return GDX_CONTENT_PACK_ERR_IO;
        }
        member.index = 0;
        members.push_back(member);
    }
    const int rc = check_members(members, &container);
    if (rc != GDX_CONTENT_PACK_OK) {
        return rc;
    }
    // A stable member order keeps two exports of the same content byte-comparable.
    std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.name < b.name; });

    zip_t* archive = open_archive(packPath, ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    if (archive == nullptr) {
        return GDX_CONTENT_PACK_ERR_IO;
    }
    for (const Member& member : members) {
        const std::string source = (folder / member.name).string();
        zip_source_t* data = file_source(archive, source);

        if (data == nullptr) {
            zip_discard(archive);
            return GDX_CONTENT_PACK_ERR_IO;
        }
        const zip_int64_t index = zip_file_add(archive, member.name.c_str(), data, ZIP_FL_ENC_UTF_8);
        if (index < 0) {
            zip_source_free(data);
            zip_discard(archive);
            return GDX_CONTENT_PACK_ERR_IO;
        }
        // Ogg and FLAC are compressed already, and deflating a WAV saves little for the time it costs.
        if (member.kind == MemberKind::Audio &&
            zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0) {
            zip_discard(archive);
            return GDX_CONTENT_PACK_ERR_IO;
        }
    }
    // libzip writes a temporary file and renames it over packPath, so a failed close leaves the old package.
    if (zip_close(archive) != 0) {
        zip_discard(archive);
        return GDX_CONTENT_PACK_ERR_IO;
    }
    return GDX_CONTENT_PACK_OK;
}

extern "C" int gdx_content_pack_extract(const char* packPath, const char* destDir, int withAudio, char* outContainer,
                                        int outCap) {
    std::vector<Member> members;
    std::string container;
    int zipError = 0;

    if (packPath == nullptr || destDir == nullptr || outContainer == nullptr || outCap <= 0) {
        return GDX_CONTENT_PACK_ERR_ARGS;
    }
    outContainer[0] = '\0';
    zip_t* archive = open_archive(packPath, ZIP_RDONLY | ZIP_CHECKCONS, &zipError);
    if (archive == nullptr) {
        return zipError == ZIP_ER_NOENT || zipError == ZIP_ER_OPEN || zipError == ZIP_ER_READ
                   ? GDX_CONTENT_PACK_ERR_IO
                                                                    : GDX_CONTENT_PACK_ERR_NOT_ZIP;
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0 || count > GDX_CONTENT_PACK_MAX_MEMBERS) {
        zip_discard(archive);
        return count < 0 ? GDX_CONTENT_PACK_ERR_NOT_ZIP : GDX_CONTENT_PACK_ERR_TOO_MANY;
    }
    for (zip_int64_t i = 0; i < count; i++) {
        zip_stat_t st;
        Member member;

        zip_stat_init(&st);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &st) != 0 || (st.valid & ZIP_STAT_NAME) == 0 ||
            (st.valid & ZIP_STAT_SIZE) == 0) {
            zip_discard(archive);
            return GDX_CONTENT_PACK_ERR_NOT_ZIP;
        }
        member.name = st.name;
        if (!classify(member.name, &member.kind, &member.stem) ||
            ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && st.encryption_method != ZIP_EM_NONE)) {
            zip_discard(archive);
            return GDX_CONTENT_PACK_ERR_MEMBER;
        }
        member.size = st.size;
        member.index = static_cast<zip_uint64_t>(i);
        members.push_back(member);
    }
    int rc = check_members(members, &container);
    if (rc == GDX_CONTENT_PACK_OK && container.size() + 1 > static_cast<size_t>(outCap)) {
        rc = GDX_CONTENT_PACK_ERR_ARGS;
    }
    if (rc != GDX_CONTENT_PACK_OK) {
        zip_discard(archive); // a refused package leaves destDir as it was
        return rc;
    }
    rc = gdx_content_pack_clear_dir(destDir, 0);
    for (size_t i = 0; rc == GDX_CONTENT_PACK_OK && i < members.size(); i++) {
        const Member& member = members[i];

        if (member.kind == MemberKind::Audio && !withAudio) {
            continue;
        }
        zip_file_t* file = zip_fopen_index(archive, member.index, 0);
        if (file == nullptr) {
            rc = GDX_CONTENT_PACK_ERR_CORRUPT;
            break;
        }
        rc = copy_member(file, std::filesystem::path(destDir) / member.name, member.size);
        zip_fclose(file);
    }
    zip_discard(archive);
    if (rc != GDX_CONTENT_PACK_OK) {
        gdx_content_pack_clear_dir(destDir, 0);
        return rc;
    }
    std::snprintf(outContainer, static_cast<size_t>(outCap), "%s", container.c_str());
    return GDX_CONTENT_PACK_OK;
}

extern "C" int gdx_content_pack_is_package_name(const char* name) {
    if (name == nullptr) {
        return 0;
    }
    const std::string text(name);
    const size_t extLen = sizeof(GDX_CONTENT_PACK_EXT) - 1;
    return text.size() > extLen && lower(text.substr(text.size() - extLen)) == GDX_CONTENT_PACK_EXT ? 1 : 0;
}

extern "C" const char* gdx_content_pack_error_string(int rc) {
    switch (rc) {
        case GDX_CONTENT_PACK_OK:
            return "ok";
        case GDX_CONTENT_PACK_ERR_ARGS:
            return "bad arguments";
        case GDX_CONTENT_PACK_ERR_IO:
            return "file I/O error";
        case GDX_CONTENT_PACK_ERR_NOT_ZIP:
            return "not a readable .gdxp package";
        case GDX_CONTENT_PACK_ERR_MEMBER:
            return "the package holds a file it may not carry";
        case GDX_CONTENT_PACK_ERR_CONTAINER:
            return "the package must hold exactly one .gdxc";
        case GDX_CONTENT_PACK_ERR_STEM:
            return "a sidecar does not match the .gdxc name";
        case GDX_CONTENT_PACK_ERR_TOO_LARGE:
            return "a file in the package is too large";
        case GDX_CONTENT_PACK_ERR_TOO_MANY:
            return "the package holds too many files";
        case GDX_CONTENT_PACK_ERR_DUPLICATE:
            return "the package holds two files with the same name";
        case GDX_CONTENT_PACK_ERR_CORRUPT:
            return "a file in the package is damaged";
        default:
            return "unknown package error";
    }
}
