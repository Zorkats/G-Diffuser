// port/updater/gdx_update_apply.cpp -- the testable half of gdx-update: member paths, the payload
// list, the ustar header, the archive extractor and the swap plan. The process itself, its log and
// the relaunch live in gdx_update_main.cpp.

#include "gdx_update_apply.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if GDX_UPDATE_HAVE_ARCHIVES
#include <zip.h>
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kTopFolder = "G-Diffuser/";
constexpr size_t kTopFolderLen = sizeof("G-Diffuser/") - 1;

// Names of the items an update may replace. The Windows build ships executables with .exe and the
// POSIX builds ship a lib/ folder beside them, so the list is per platform.
#ifdef _WIN32
const char* const kPayloadItems[] = { "G-Diffuser.exe",
                                      "gdx-extract.exe",
                                      "gdx-update.exe",
                                      "gdiffuser.o2r",
                                      "gamecontrollerdb.txt",
                                      "fonts",
                                      "decomp-recipes",
                                      "shaders",
                                      "LICENSE",
                                      "THIRD_PARTY_NOTICES.md",
                                      "LICENSES" };
#else
const char* const kPayloadItems[] = { "G-Diffuser",
                                      "gdx-extract",
                                      "gdx-update",
                                      "gdiffuser.o2r",
                                      "gamecontrollerdb.txt",
                                      "fonts",
                                      "decomp-recipes",
                                      "shaders",
                                      "LICENSE",
                                      "THIRD_PARTY_NOTICES.md",
                                      "LICENSES",
                                      "lib" };
#endif

void log_line(std::vector<std::string>* log, const std::string& text) {
    if (log != nullptr) {
        log->push_back(text);
    }
}

// Archive names are UTF-8 bytes; a host path is UTF-16 on Windows and native bytes elsewhere. This
// is the one place the two meet.
fs::path to_path(const std::string& utf8Text) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8Text.begin(), utf8Text.end()));
#else
    return fs::u8path(utf8Text);
#endif
}

// Builds a child path as text. '/' separates components on every host this runs on.
std::string join_path(const std::string& folder, const std::string& name) {
    std::string base = folder;

    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    return base + "/" + name;
}

std::string field_text(const unsigned char* block, size_t offset, size_t length) {
    std::string text;

    for (size_t i = 0; i < length; i++) {
        const unsigned char c = block[offset + i];
        if (c == 0) {
            break;
        }
        text.push_back(static_cast<char>(c));
    }
    return text;
}

// ustar numbers are zero padded octal digits with trailing spaces or NULs. A field of blank bytes
// counts as zero, which is how some writers store an empty size.
bool octal_field(const unsigned char* field, size_t length, uint64_t* out) {
    size_t i = 0;
    uint64_t value = 0;
    bool any = false;

    while (i < length && (field[i] == ' ' || field[i] == 0)) {
        i++;
    }
    for (; i < length && field[i] >= '0' && field[i] <= '7'; i++) {
        if (value > (UINT64_MAX >> 3)) {
            return false;
        }
        value = (value << 3) | static_cast<uint64_t>(field[i] - '0');
        any = true;
    }
    for (; i < length; i++) {
        if (field[i] != ' ' && field[i] != 0) {
            return false;
        }
    }
    *out = any ? value : 0;
    return true;
}

#ifndef _WIN32
fs::perms perms_from_mode(uint32_t mode) {
    fs::perms perms = fs::perms::none;

    if ((mode & 0400) != 0) {
        perms |= fs::perms::owner_read;
    }
    if ((mode & 0200) != 0) {
        perms |= fs::perms::owner_write;
    }
    if ((mode & 0100) != 0) {
        perms |= fs::perms::owner_exec;
    }
    if ((mode & 0040) != 0) {
        perms |= fs::perms::group_read;
    }
    if ((mode & 0020) != 0) {
        perms |= fs::perms::group_write;
    }
    if ((mode & 0010) != 0) {
        perms |= fs::perms::group_exec;
    }
    if ((mode & 0004) != 0) {
        perms |= fs::perms::others_read;
    }
    if ((mode & 0002) != 0) {
        perms |= fs::perms::others_write;
    }
    if ((mode & 0001) != 0) {
        perms |= fs::perms::others_exec;
    }
    return perms;
}
#endif

class HostFs final : public GdxUpdFs {
public:
    bool exists(const std::string& path) override {
        std::error_code ec;
        return fs::exists(to_path(path), ec);
    }

    bool move(const std::string& from, const std::string& to) override {
        std::error_code ec;

        // An antivirus scanner or the shell can hold a handle on a file this process just wrote,
        // which makes the first rename fail on Windows and succeed a moment later. It is the one
        // failure here that is usually temporary, so it is the one that is retried.
        for (int attempt = 0; attempt < 20; attempt++) {
            fs::rename(to_path(from), to_path(to), ec);
            if (!ec) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ec.clear();
        }
        return false;
    }

    bool remove_all(const std::string& path) override {
        std::error_code ec;
        fs::remove_all(to_path(path), ec);
        return !ec;
    }

    bool create_dirs(const std::string& path) override {
        std::error_code ec;
        fs::create_directories(to_path(path), ec);
        return fs::is_directory(to_path(path), ec);
    }
};

} // namespace

const char* gdx_upd_main_exe_name(void) {
#ifdef _WIN32
    return "G-Diffuser.exe";
#else
    return "G-Diffuser";
#endif
}

const char* gdx_upd_helper_exe_name(void) {
#ifdef _WIN32
    return "gdx-update.exe";
#else
    return "gdx-update";
#endif
}

bool gdx_upd_is_payload_item(const std::string& name) {
    const size_t count = sizeof(kPayloadItems) / sizeof(kPayloadItems[0]);

    for (size_t i = 0; i < count; i++) {
        if (name == kPayloadItems[i]) {
            return true;
        }
    }
    return false;
}

bool gdx_upd_member_rel_path(const std::string& memberPath, std::string* rel, std::string* reason) {
    auto refuse = [reason](const char* text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };

    if (rel != nullptr) {
        rel->clear();
    }
    if (memberPath.empty()) {
        return refuse("empty member path");
    }
    if (memberPath.front() == '/') {
        return refuse("member path is absolute");
    }
    for (size_t i = 0; i < memberPath.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(memberPath[i]);
        if (c == '\\') {
            return refuse("member path has a backslash");
        }
        if (c == ':') {
            return refuse("member path has a drive letter");
        }
        if (c < 0x20 || c == 0x7F) {
            return refuse("member path has a control character");
        }
    }
    if (memberPath.compare(0, kTopFolderLen, kTopFolder) != 0) {
        return refuse("member path is outside the G-Diffuser/ folder");
    }

    std::string rest = memberPath.substr(kTopFolderLen);
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    if (rest.empty()) {
        return true; // the folder entry itself
    }
    // Containment is decided from the path alone. A component that is empty, "." or ".." is the
    // only way a relative path can address something outside staging, so refusing it here means no
    // later step has to guess what the host filesystem would do with the name.
    size_t start = 0;
    for (;;) {
        const size_t slash = rest.find('/', start);
        const std::string part = rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start);

        if (part.empty() || part == "." || part == "..") {
            return refuse("member path has an empty, . or .. component");
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    if (rel != nullptr) {
        *rel = rest;
    }
    return true;
}

bool gdx_upd_tar_parse_header(const unsigned char* block, GdxUpdTarEntry* out, std::string* reason) {
    auto refuse = [reason](const char* text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };

    if (block == nullptr || out == nullptr) {
        return refuse("no tar header");
    }
    uint64_t storedSum = 0;
    if (!octal_field(block + 148, 8, &storedSum)) {
        return refuse("tar header checksum is not octal");
    }
    // The checksum covers the header with its own field read as spaces. Writers before POSIX also
    // summed signed chars, so both sums are accepted.
    uint64_t unsignedSum = 0;
    int64_t signedSum = 0;
    for (size_t i = 0; i < 512; i++) {
        const unsigned char byte = (i >= 148 && i < 156) ? static_cast<unsigned char>(' ') : block[i];
        unsignedSum += byte;
        signedSum += static_cast<int64_t>(static_cast<signed char>(byte));
    }
    if (storedSum != unsignedSum && static_cast<int64_t>(storedSum) != signedSum) {
        return refuse("tar header checksum mismatch");
    }
    if (std::memcmp(block + 257, "ustar", 5) != 0) {
        return refuse("tar header is not ustar");
    }

    const std::string name = field_text(block, 0, 100);
    const std::string prefix = field_text(block, 345, 155);
    if (name.empty()) {
        return refuse("tar header has no name");
    }
    uint64_t mode = 0;
    uint64_t size = 0;
    if (!octal_field(block + 100, 8, &mode)) {
        return refuse("tar header mode is not octal");
    }
    if (!octal_field(block + 124, 12, &size)) {
        return refuse("tar header size is not octal");
    }
    out->path = prefix.empty() ? name : prefix + "/" + name;
    out->mode = static_cast<uint32_t>(mode);
    out->size = size;
    out->type = block[156] == 0 ? '0' : static_cast<char>(block[156]);
    return true;
}

std::unique_ptr<GdxUpdFs> gdx_upd_make_host_fs(void) {
    return std::unique_ptr<GdxUpdFs>(new HostFs());
}

GdxUpdSwapResult gdx_upd_swap_apply(GdxUpdFs* host, const std::string& installDir, const std::string& stagingDir,
                                    const std::string& backupDir, const std::vector<std::string>& stagedItems,
                                    std::vector<std::string>* log) {
    GdxUpdSwapResult result;

    if (host == nullptr) {
        result.code = GDX_UPD_ERR_ARGS;
        return result;
    }

    struct Step {
        std::string item;
        bool hadOld;
    };
    std::vector<Step> steps;
    for (const std::string& item : stagedItems) {
        if (!gdx_upd_is_payload_item(item)) {
            log_line(log, "ignored " + item + ": not part of the shipped payload");
            continue;
        }
        steps.push_back(Step{ item, host->exists(join_path(installDir, item)) });
    }
    if (steps.empty()) {
        result.code = GDX_UPD_ERR_ARCHIVE;
        log_line(log, "the archive holds no payload item");
        return result;
    }

    // A backup left behind by an interrupted attempt would make the next move fail on an existing
    // destination, so the folder starts empty on every run.
    host->remove_all(backupDir);
    if (!host->create_dirs(backupDir)) {
        result.code = GDX_UPD_ERR_SWAP;
        log_line(log, "cannot create " + backupDir);
        return result;
    }

    struct Record {
        std::string item;
        bool backedUp;
    };
    std::vector<Record> records;
    bool failed = false;

    for (const Step& step : steps) {
        const std::string installed = join_path(installDir, step.item);
        const std::string backup = join_path(backupDir, step.item);
        const std::string staged = join_path(stagingDir, step.item);

        if (step.hadOld && !host->move(installed, backup)) {
            result.failedItem = step.item;
            failed = true;
            break;
        }
        // Recorded before the staged copy moves in, so a failure there still knows to restore the
        // backup this step just made.
        records.push_back(Record{ step.item, step.hadOld });
        if (!host->move(staged, installed)) {
            result.failedItem = step.item;
            failed = true;
            break;
        }
        log_line(log, "installed " + step.item);
    }

    if (!failed) {
        return result;
    }
    result.code = GDX_UPD_ERR_SWAP;
    log_line(log, "swap failed at " + result.failedItem + ", undoing the completed moves");

    for (size_t i = records.size(); i > 0; i--) {
        const Record& record = records[i - 1];
        const std::string installed = join_path(installDir, record.item);
        const std::string backup = join_path(backupDir, record.item);

        // Whatever sits at the install path now is either the new copy or nothing at all; both have
        // to go before the old one can be moved back over it.
        if (!host->remove_all(installed)) {
            result.rollbackComplete = false;
            log_line(log, "could not clear " + record.item + " before restoring it");
            continue;
        }
        if (!record.backedUp) {
            log_line(log, "removed " + record.item + ", which the archive had just added");
            continue;
        }
        if (!host->move(backup, installed)) {
            result.rollbackComplete = false;
            log_line(log, "could not restore " + record.item + " from the backup");
            continue;
        }
        log_line(log, "restored " + record.item);
    }
    return result;
}

#if GDX_UPDATE_HAVE_ARCHIVES

namespace {

// The extract cap the update contract promises, and a member cap that only exists so a pathological
// archive cannot keep the extractor spinning.
constexpr uint64_t kMaxExtractBytes = 2ull * 1024ull * 1024ull * 1024ull;
constexpr size_t kMaxMembers = 200000;
constexpr size_t kCopyChunk = 64 * 1024;

// The reverse of to_path, for the log lines the extractor writes.
std::string to_text(const fs::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}

bool block_is_zero(const unsigned char* block, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (block[i] != 0) {
            return false;
        }
    }
    return true;
}

bool has_suffix(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

zip_t* open_zip(const fs::path& archivePath) {
#ifdef _WIN32
    // The install folder may hold characters no ANSI code page can carry, so libzip is handed the
    // UTF-16 name directly instead of a narrow rendering of it.
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_win32w_create(archivePath.c_str(), 0, ZIP_LENGTH_TO_END, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return nullptr;
    }
    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error);
    if (archive == nullptr) {
        zip_source_free(source);
    }
    zip_error_fini(&error);
    return archive;
#else
    int error = 0;
    return zip_open(archivePath.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &error);
#endif
}

int extract_zip(const fs::path& archivePath, const fs::path& stagingDir, std::vector<std::string>* log) {
    struct Member {
        std::string rel;
        zip_uint64_t index;
        uint64_t size;
        uint32_t mode;
        bool isDir;
    };

    std::vector<Member> members;
    zip_t* archive = open_zip(archivePath);
    if (archive == nullptr) {
        log_line(log, "cannot open " + to_text(archivePath) + " as a zip");
        return GDX_UPD_ERR_ARCHIVE;
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0 || static_cast<uint64_t>(count) > kMaxMembers) {
        log_line(log, "the zip has no readable member table");
        zip_discard(archive);
        return GDX_UPD_ERR_ARCHIVE;
    }

    int rc = GDX_UPD_OK;
    uint64_t total = 0;
    // Pass one reads every name and size before a byte is written, so an archive with one bad
    // member is refused whole, the same way the tarball reader refuses one.
    for (zip_int64_t i = 0; i < count && rc == GDX_UPD_OK; i++) {
        zip_stat_t st;

        zip_stat_init(&st);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &st) != 0 || (st.valid & ZIP_STAT_NAME) == 0 ||
            (st.valid & ZIP_STAT_SIZE) == 0) {
            log_line(log, "the zip has a member with no name or size");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && st.encryption_method != ZIP_EM_NONE) {
            log_line(log, "the zip has an encrypted member");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        Member member;
        std::string reason;
        if (!gdx_upd_member_rel_path(st.name, &member.rel, &reason)) {
            log_line(log, std::string("refused ") + st.name + ": " + reason);
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        const size_t nameLen = std::strlen(st.name);
        member.isDir = nameLen > 0 && st.name[nameLen - 1] == '/';
        member.index = static_cast<zip_uint64_t>(i);
        member.size = st.size;
        member.mode = 0;
        zip_uint8_t opsys = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(archive, member.index, 0, &opsys, &attributes) == 0 &&
            opsys == ZIP_OPSYS_UNIX) {
            member.mode = attributes >> 16;
        }
        if (!member.isDir) {
            // Checked before the add, so a zip64 size near 2^64 cannot wrap the sum back under the cap.
            if (member.size > kMaxExtractBytes - total) {
                log_line(log, st.name + std::string(": the archive expands past the 2 GB cap"));
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            total += member.size;
        }
        members.push_back(member);
    }

    std::vector<char> buffer(kCopyChunk);
    for (size_t i = 0; i < members.size() && rc == GDX_UPD_OK; i++) {
        const Member& member = members[i];
        const fs::path target = stagingDir / to_path(member.rel);
        std::error_code ec;

        if (member.isDir) {
            if (!member.rel.empty()) {
                fs::create_directories(target, ec);
                if (ec) {
                    log_line(log, "cannot create " + member.rel);
                    rc = GDX_UPD_ERR_IO;
                }
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            log_line(log, "cannot create the folder for " + member.rel);
            rc = GDX_UPD_ERR_IO;
            break;
        }
        zip_file_t* file = zip_fopen_index(archive, member.index, 0);
        if (file == nullptr) {
            log_line(log, "cannot read " + member.rel + " from the zip");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        uint64_t written = 0;
        if (!out) {
            log_line(log, "cannot write " + to_text(target));
            zip_fclose(file);
            rc = GDX_UPD_ERR_IO;
            break;
        }
        for (;;) {
            const zip_int64_t got = zip_fread(file, buffer.data(), buffer.size());

            if (got < 0) {
                log_line(log, member.rel + ": the member is damaged");
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            if (got == 0) {
                break;
            }
            written += static_cast<uint64_t>(got);
            // The declared size is the only bound the writer cannot lie about cheaply, so it is
            // enforced while writing instead of trusted afterwards.
            if (written > member.size) {
                log_line(log, member.rel + ": the member is longer than its directory entry says");
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(got));
            if (!out) {
                log_line(log, "cannot write " + to_text(target));
                rc = GDX_UPD_ERR_IO;
                break;
            }
        }
        zip_fclose(file);
        out.close();
        if (rc != GDX_UPD_OK) {
            break;
        }
        if (written != member.size) {
            log_line(log, member.rel + ": the member is shorter than its directory entry says");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
#ifndef _WIN32
        if (member.mode != 0) {
            fs::permissions(target, perms_from_mode(member.mode), fs::perm_options::replace, ec);
        }
#endif
    }
    zip_discard(archive);
    return rc;
}

// Fills the buffer unless the stream ends first, then reports how much arrived. gzread may return
// short reads at any point, which is not an error in itself.
int read_exact(gzFile gz, unsigned char* out, size_t length) {
    size_t done = 0;

    while (done < length) {
        const int got = gzread(gz, out + done, static_cast<unsigned int>(length - done));
        if (got <= 0) {
            break;
        }
        done += static_cast<size_t>(got);
    }
    return static_cast<int>(done);
}

bool skip_bytes(gzFile gz, uint64_t length, std::vector<char>* buffer) {
    while (length > 0) {
        const size_t chunk = length < buffer->size() ? static_cast<size_t>(length) : buffer->size();
        const int got = read_exact(gz, reinterpret_cast<unsigned char*>(buffer->data()), chunk);

        if (got != static_cast<int>(chunk)) {
            return false;
        }
        length -= chunk;
    }
    return true;
}

int extract_tar_gz(const fs::path& archivePath, const fs::path& stagingDir, std::vector<std::string>* log) {
    const std::string native = to_text(archivePath);
    gzFile gz = gzopen(native.c_str(), "rb");
    if (gz == nullptr) {
        log_line(log, "cannot open " + native + " as gzip");
        return GDX_UPD_ERR_ARCHIVE;
    }

    int rc = GDX_UPD_OK;
    uint64_t total = 0;
    size_t memberCount = 0;
    bool sawEndMarker = false;
    std::vector<char> buffer(kCopyChunk);
    unsigned char header[512];

    for (;;) {
        const int got = read_exact(gz, header, sizeof(header));

        if (got < 0 || got < static_cast<int>(sizeof(header))) {
            if (got == 0) {
                break; // end of stream, checked below for the end-of-archive marker
            }
            log_line(log, "the tarball ends inside a header block");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        if (block_is_zero(header, sizeof(header))) {
            sawEndMarker = true;
            break;
        }
        GdxUpdTarEntry entry;
        std::string reason;
        if (!gdx_upd_tar_parse_header(header, &entry, &reason)) {
            log_line(log, "refused a member: " + reason);
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        if (++memberCount > kMaxMembers) {
            log_line(log, "the tarball has too many members");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        const uint64_t padded = ((entry.size + 511) / 512) * 512;
        if (entry.type != '0' && entry.type != '5') {
            // Symlinks, hardlinks, devices, pax and GNU long-name records: the release payload has
            // no use for any of them, and a link is the classic way out of an extraction folder.
            log_line(log, "skipped " + entry.path + " (unsupported tar type " + std::string(1, entry.type) + ")");
            if (!skip_bytes(gz, padded, &buffer)) {
                log_line(log, "the tarball ends inside " + entry.path);
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            continue;
        }
        std::string rel;
        if (!gdx_upd_member_rel_path(entry.path, &rel, &reason)) {
            log_line(log, "refused " + entry.path + ": " + reason);
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        if (entry.type == '5') {
            std::error_code ec;
            if (!rel.empty()) {
                fs::create_directories(stagingDir / to_path(rel), ec);
                if (ec) {
                    log_line(log, "cannot create " + rel);
                    rc = GDX_UPD_ERR_IO;
                    break;
                }
            }
            if (!skip_bytes(gz, padded, &buffer)) {
                log_line(log, "the tarball ends inside " + entry.path);
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            continue;
        }
        if (rel.empty()) {
            log_line(log, "refused " + entry.path + ": a regular file cannot be the top folder");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        total += entry.size;
        if (total > kMaxExtractBytes) {
            log_line(log, "the tarball expands past the 2 GB cap");
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
        const fs::path target = stagingDir / to_path(rel);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            log_line(log, "cannot create the folder for " + rel);
            rc = GDX_UPD_ERR_IO;
            break;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            log_line(log, "cannot write " + to_text(target));
            rc = GDX_UPD_ERR_IO;
            break;
        }
        uint64_t remaining = entry.size;
        while (remaining > 0) {
            const size_t chunk = remaining < buffer.size() ? static_cast<size_t>(remaining) : buffer.size();
            const int read = read_exact(gz, reinterpret_cast<unsigned char*>(buffer.data()), chunk);

            if (read != static_cast<int>(chunk)) {
                log_line(log, "the tarball ends inside the data of " + rel);
                rc = GDX_UPD_ERR_ARCHIVE;
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!out) {
                log_line(log, "cannot write " + to_text(target));
                rc = GDX_UPD_ERR_IO;
                break;
            }
            remaining -= chunk;
        }
        out.close();
        if (rc != GDX_UPD_OK) {
            break;
        }
        if (!skip_bytes(gz, padded - entry.size, &buffer)) {
            log_line(log, "the tarball ends inside the padding of " + rel);
            rc = GDX_UPD_ERR_ARCHIVE;
            break;
        }
#ifndef _WIN32
        if (entry.mode != 0) {
            // The archive carries the executable bits, and the relaunched game has to keep them.
            fs::permissions(target, perms_from_mode(entry.mode), fs::perm_options::replace, ec);
        }
#endif
    }
    gzclose(gz);
    if (rc == GDX_UPD_OK && !sawEndMarker) {
        // tar writes two zero blocks to end the archive, so a stream that stops without them was cut
        // short: some members would be missing from staging and the swap would install a partial tree.
        log_line(log, "the tarball ends without its end-of-archive marker, so it is truncated");
        rc = GDX_UPD_ERR_ARCHIVE;
    }
    return rc;
}

} // namespace

#endif // GDX_UPDATE_HAVE_ARCHIVES

int gdx_upd_extract_archive(const std::string& archivePath, const std::string& stagingDir,
                            std::vector<std::string>* log) {
#if GDX_UPDATE_HAVE_ARCHIVES
    const std::string name = lower(archivePath);

    if (has_suffix(name, ".zip")) {
        return extract_zip(to_path(archivePath), to_path(stagingDir), log);
    }
    if (has_suffix(name, ".tar.gz") || has_suffix(name, ".tgz")) {
        return extract_tar_gz(to_path(archivePath), to_path(stagingDir), log);
    }
    log_line(log, "unknown archive type: " + archivePath);
    return GDX_UPD_ERR_ARCHIVE;
#else
    (void)archivePath;
    (void)stagingDir;
    log_line(log, "this build has no archive support");
    return GDX_UPD_ERR_ARCHIVE;
#endif
}
