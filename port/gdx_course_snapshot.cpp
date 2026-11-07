#include "gdx_course_snapshot.h"
#include "gdx_sha256.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_recovery_file.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace gdx::snapshot {
namespace {
constexpr size_t HeaderBytes = 64;

uint32_t crc(const Bytes& bytes) {
    return gdx_course_registry_crc32(bytes.data(), bytes.size());
}

void put(Bytes& bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
    }
}

uint64_t get(const Bytes& bytes, size_t offset, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value |= uint64_t(bytes[offset + i]) << (i * 8);
    }
    return value;
}

bool validId(const std::string& id) {
    if (id.size() < 6 || id.size() > GDX_COURSE_FILE_ID_MAX || id[id.size() - 5] != '.') {
        return false;
    }
    for (const unsigned char c : id) {
        if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    const auto extension = id.substr(id.size() - 4);
    return extension == "CRSD" || extension == "CRSE" || extension == "GOST";
}

bool sealedValid(const Generation&);

bool valid(const Generation& generation) {
    if ((generation.version != 1 && generation.version != 2) ||
        !generation.disk || !validId(generation.courseId) || generation.base.size() != BaseBytes ||
        generation.base[1] > 64 || generation.pointCount > GDX_BRANCH_MAX_TOTAL_POINTS ||
        generation.base[1] != (std::min)(generation.pointCount, uint8_t(64))) {
        return false;
    }
    const uint32_t binding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(generation.courseId.data()), generation.courseId.size());
    const auto& extensions = generation.metadata[size_t(Metadata::Extensions)];
    const auto& title = generation.metadata[size_t(Metadata::Title)];
    const auto& forks = generation.metadata[size_t(Metadata::Forks)];
    const auto& settings = generation.metadata[size_t(Metadata::Settings)];
    GdxCourseExtBlock decodedExtensions{};
    char decodedTitle[GDX_COURSE_TITLE_MAX + 1]{};
    GdxBranchSet decodedForks{};
    GdxCourseSettings decodedSettings{};
    if ((!extensions.empty() && gdx_course_ext_decode(extensions.data(), extensions.size(), binding,
                                                      &decodedExtensions) != 0) ||
        (!title.empty() && gdx_course_title_decode(title.data(), title.size(), binding,
                                                 decodedTitle, sizeof(decodedTitle)) != 0) ||
        (!forks.empty() && gdx_branch_decode(forks.data(), forks.size(), binding, &decodedForks) != 0) ||
        (!settings.empty() && gdx_settings_decode(settings.data(), settings.size(), binding, &decodedSettings) != 0)) {
        return false;
    }
    if ((!forks.empty() && decodedForks.ringCount != generation.pointCount) ||
        (!settings.empty() && decodedSettings.ringCount != generation.pointCount)) {
        return false;
    }
    if ((generation.base[8] & 0x80) && !sealedValid(generation)) {
        return false;
    }
    if (generation.pointCount > 64) {
        return !forks.empty() && decodedForks.extendedCount == generation.pointCount - 64 &&
               decodedForks.baseCrc == gdx_course_registry_crc32(generation.base.data(), CourseBytes) &&
               decodedForks.courseChecksum == ((generation.base[8] & 0x80) ? generation.nativeChecksum :
                                               uint32_t(get(generation.base, 4, 4)));
    }
    return decodedForks.extendedCount == 0;
}

void appendHex(std::string& text, uint64_t value, size_t digits) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = digits; i > 0; --i) {
        text += hex[(value >> ((i - 1) * 4)) & 15];
    }
}

bool validKey(const std::string& text) {
    if (text.size() < 4 || text.size() > 100 || text.substr(text.size() - 4) != ".gcs") {
        return false;
    }
    for (size_t i = 0; i < text.size() - 4; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) {
            return false;
        }
    }
    return true;
}
}

namespace {
bool bindForks(Generation& g) {
    auto& bytes = g.metadata[size_t(Metadata::Forks)];
    if (bytes.empty()) {
        return g.pointCount <= 64;
    }
    const auto binding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
    GdxBranchSet forks{};
    if (gdx_branch_decode(bytes.data(), bytes.size(), binding, &forks) != 0) {
        return false;
    }
    forks.courseChecksum = g.nativeChecksum;
    forks.baseCrc = gdx_course_registry_crc32(g.base.data(), CourseBytes);
    Bytes encoded(GDX_BRANCH_BLOCK_MAX_BYTES);
    const int length = gdx_branch_encode(&forks, binding, encoded.data(), encoded.size());
    if (length <= 0) {
        return false;
    }
    encoded.resize(size_t(length));
    bytes = std::move(encoded);
    return true;
}

bool bindIdentity(Generation& g, uint64_t disk, uint16_t directory, const std::string& id) {
    if (!disk || !validId(id)) {
        return false;
    }
    const auto oldBinding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
    const auto newBinding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(id.data()), id.size());
    for (size_t i = 0; i < g.metadata.size(); ++i) {
        auto& bytes = g.metadata[i];
        if (bytes.empty()) {
            continue;
        }
        Bytes encoded(MaxBytes);
        int length = -1;
        switch (Metadata(i)) {
            case Metadata::Extensions: {
                GdxCourseExtBlock value{};
                if (gdx_course_ext_decode(bytes.data(), bytes.size(), oldBinding, &value) != 0) {
                    return false;
                }
                length = gdx_course_ext_encode(value.records, value.count, newBinding, encoded.data(), encoded.size());
                break;
            }
            case Metadata::Title: {
                char value[GDX_COURSE_TITLE_MAX + 1]{};
                if (gdx_course_title_decode(bytes.data(), bytes.size(), oldBinding, value, sizeof(value)) != 0) {
                    return false;
                }
                length = gdx_course_title_encode(value, newBinding, encoded.data(), encoded.size());
                break;
            }
            case Metadata::Forks: {
                GdxBranchSet value{};
                if (gdx_branch_decode(bytes.data(), bytes.size(), oldBinding, &value) != 0) {
                    return false;
                }
                length = gdx_branch_encode(&value, newBinding, encoded.data(), encoded.size());
                break;
            }
            case Metadata::Settings:
                // Byte for byte, so a seal over a block an older build wrote still matches.
                length = gdx_settings_rebind(bytes.data(), bytes.size(), oldBinding, newBinding, encoded.data(),
                                             encoded.size());
                break;
            default:
                return false;
        }
        if (length <= 0) {
            return false;
        }
        encoded.resize(size_t(length));
        bytes = std::move(encoded);
    }
    g.disk = disk;
    g.directory = directory;
    g.courseId = id;
    return true;
}

bool selector(const Generation& g, uint32_t& value) {
    Generation canonical = g;
    canonical.base[8] &= 0x7F;
    put(canonical.base, 4, canonical.nativeChecksum, 4);
    if (!bindIdentity(canonical, 1, 0, "CONTENT.CRSD") || !bindForks(canonical)) {
        return false;
    }
    Bytes bytes;
    if (!encode(canonical, bytes)) {
        return false;
    }
    if (canonical.version == 1) {
        value = gdx_course_registry_crc32(bytes.data(), bytes.size() - 4);
    } else {
        // CRC-protected sections cancel their own edits under another CRC.
        gdx::hash::Sha256 hash;
        unsigned char digest[32];
        gdx::hash::sha256_init(&hash);
        gdx::hash::sha256_update(&hash, bytes.data(), bytes.size() - 4);
        gdx::hash::sha256_final(&hash, digest);
        value = uint32_t(digest[0]) | uint32_t(digest[1]) << 8 |
                uint32_t(digest[2]) << 16 | uint32_t(digest[3]) << 24;
    }
    return true;
}

bool sealedValid(const Generation& g) {
    uint32_t value;
    return selector(g, value) && value == get(g.base, 4, 4);
}
}

bool rebind(Generation& generation, uint64_t disk, uint16_t directory, const std::string& courseId) {
    if (!valid(generation)) {
        return false;
    }
    Generation next = generation;
    if (!bindIdentity(next, disk, directory, courseId) || !valid(next)) {
        return false;
    }
    generation = std::move(next);
    return true;
}

bool legacyImage(Generation& generation) {
    if (generation.pointCount > 64 || !valid(generation)) {
        return false;
    }
    Generation next = generation;
    next.base[8] &= 0x7F;
    put(next.base, 4, next.nativeChecksum, 4);
    if (!bindForks(next) || !valid(next)) {
        return false;
    }
    generation = std::move(next);
    return true;
}

bool seal(Generation& generation) {
    if (generation.base.size() != BaseBytes || (generation.version != 1 && generation.version != 2)) {
        return false;
    }
    Generation next = generation;
    next.version = 2;
    if (!(next.base[8] & 0x80)) {
        next.nativeChecksum = uint32_t(get(next.base, 4, 4));
    }
    uint32_t value;
    if (!selector(next, value)) {
        return false;
    }
    next.base[8] |= 0x80;
    put(next.base, 4, value, 4);
    if (!bindForks(next) || !valid(next)) {
        return false;
    }
    generation = std::move(next);
    return true;
}

bool encode(const Generation& generation, Bytes& out) {
    if (!valid(generation)) {
        return false;
    }
    size_t length = HeaderBytes + generation.courseId.size() + generation.base.size() + 4;
    for (const auto& section : generation.metadata) {
        if (section.size() > MaxBytes || length > MaxBytes - section.size()) {
            return false;
        }
        length += section.size();
    }
    Bytes bytes(length, 0);
    std::memcpy(bytes.data(), "GCS1", 4);
    put(bytes, 4, generation.version, 2);
    put(bytes, 6, HeaderBytes, 2);
    put(bytes, 8, length, 4);
    put(bytes, 12, generation.disk, 8);
    put(bytes, 20, generation.directory, 2);
    bytes[22] = generation.pointCount;
    bytes[23] = static_cast<unsigned char>(generation.courseId.size());
    put(bytes, 24, generation.base.size(), 4);
    put(bytes, 44, crc(generation.base), 4);
    put(bytes, 48, generation.nativeChecksum, 4);
    size_t position = HeaderBytes;
    std::copy(generation.courseId.begin(), generation.courseId.end(), bytes.begin() + position);
    position += generation.courseId.size();
    std::copy(generation.base.begin(), generation.base.end(), bytes.begin() + position);
    position += generation.base.size();
    for (size_t i = 0; i < generation.metadata.size(); ++i) {
        const auto& section = generation.metadata[i];
        put(bytes, 28 + i * 4, section.size(), 4);
        std::copy(section.begin(), section.end(), bytes.begin() + position);
        position += section.size();
    }
    put(bytes, length - 4, gdx_course_registry_crc32(bytes.data(), length - 4), 4);
    out = std::move(bytes);
    return true;
}

bool decode(const Bytes& bytes, Generation& out) {
    if (bytes.size() < HeaderBytes + 4 || bytes.size() > MaxBytes ||
        std::memcmp(bytes.data(), "GCS1", 4) || (get(bytes, 4, 2) != 1 && get(bytes, 4, 2) != 2) ||
        get(bytes, 6, 2) != HeaderBytes || get(bytes, 8, 4) != bytes.size() ||
        get(bytes, 24, 4) != BaseBytes || bytes[23] > GDX_COURSE_FILE_ID_MAX ||
        get(bytes, bytes.size() - 4, 4) != gdx_course_registry_crc32(bytes.data(), bytes.size() - 4)) {
        return false;
    }
    for (size_t i = 52; i < HeaderBytes; ++i) {
        if (bytes[i]) {
            return false;
        }
    }
    size_t position = HeaderBytes;
    const size_t end = bytes.size() - 4;
    if (size_t(bytes[23]) + BaseBytes > end - position) {
        return false;
    }
    Generation generation;
    generation.version = uint16_t(get(bytes, 4, 2));
    generation.disk = get(bytes, 12, 8);
    generation.directory = uint16_t(get(bytes, 20, 2));
    generation.pointCount = bytes[22];
    generation.nativeChecksum = uint32_t(get(bytes, 48, 4));
    generation.courseId.assign(reinterpret_cast<const char*>(bytes.data() + position), bytes[23]);
    position += bytes[23];
    generation.base.assign(bytes.begin() + position, bytes.begin() + position + BaseBytes);
    position += BaseBytes;
    if (crc(generation.base) != get(bytes, 44, 4)) {
        return false;
    }
    for (size_t i = 0; i < generation.metadata.size(); ++i) {
        const auto length = size_t(get(bytes, 28 + i * 4, 4));
        if (length > end - position) {
            return false;
        }
        generation.metadata[i].assign(bytes.begin() + position, bytes.begin() + position + length);
        position += length;
    }
    if (position != end || !valid(generation)) {
        return false;
    }
    out = std::move(generation);
    return true;
}

bool portable(Generation& generation, bool stripGhosts) {
    Generation next = generation;
    if (!rebind(next, 1, 0, next.courseId)) {
        return false;
    }
    if (stripGhosts) {
        std::fill(next.base.begin() + CourseBytes, next.base.end(), 0);
    }
    if (!seal(next)) {
        return false;
    }
    generation = std::move(next);
    return true;
}

bool content(const Generation& generation, Bytes& out) {
    if (!valid(generation)) {
        return false;
    }
    Generation canonical = generation;
    canonical.version = 2;
    // An unsealed image still holds the native checksum where the seal goes (see seal()).
    if (!(canonical.base[8] & 0x80)) {
        canonical.nativeChecksum = uint32_t(get(canonical.base, 4, 4));
    }
    canonical.base[8] &= 0x7F;
    put(canonical.base, 4, canonical.nativeChecksum, 4);
    std::fill(canonical.base.begin() + CourseBytes, canonical.base.end(), 0);
    return bindIdentity(canonical, 1, 0, "CONTENT.CRSD") && bindForks(canonical) && encode(canonical, out);
}

bool packageEncode(const std::vector<Generation>& members, uint32_t containerCrc, Bytes& out) {
    if (members.empty() || members.size() > 6) {
        return false;
    }
    Bytes bytes(16, 0);
    std::memcpy(bytes.data(), "GCP1", 4);
    put(bytes, 4, 1, 2);
    put(bytes, 6, members.size(), 2);
    put(bytes, 8, containerCrc, 4);
    for (size_t i = 0; i < members.size(); ++i) {
        const auto& generation = members[i];
        if (generation.disk != 1 || generation.directory || !(generation.base.size() == BaseBytes &&
            (generation.base[8] & 0x80))) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (generation.courseId == members[j].courseId) {
                return false;
            }
        }
        Bytes encoded;
        if (!encode(generation, encoded)) {
            return false;
        }
        const auto position = bytes.size();
        bytes.resize(position + 4);
        put(bytes, position, encoded.size(), 4);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    const auto position = bytes.size();
    bytes.resize(position + 4);
    put(bytes, position, gdx_course_registry_crc32(bytes.data(), position), 4);
    out = std::move(bytes);
    return true;
}

bool packageDecode(const Bytes& bytes, uint32_t containerCrc, std::vector<Generation>& out) {
    if (bytes.size() < 20 || bytes.size() > 20 + 6 * (MaxBytes + 4) ||
        std::memcmp(bytes.data(), "GCP1", 4) || get(bytes, 4, 2) != 1 ||
        get(bytes, 6, 2) < 1 || get(bytes, 6, 2) > 6 || get(bytes, 8, 4) != containerCrc ||
        get(bytes, 12, 4) || get(bytes, bytes.size() - 4, 4) !=
        gdx_course_registry_crc32(bytes.data(), bytes.size() - 4)) {
        return false;
    }
    size_t position = 16;
    const size_t end = bytes.size() - 4;
    std::vector<Generation> members;
    for (size_t i = 0; i < get(bytes, 6, 2); ++i) {
        if (end - position < 4) {
            return false;
        }
        const auto size = size_t(get(bytes, position, 4));
        position += 4;
        if (size > MaxBytes || size > end - position) {
            return false;
        }
        Generation generation;
        const Bytes encoded(bytes.begin() + position, bytes.begin() + position + size);
        if (!decode(encoded, generation) || generation.disk != 1 || generation.directory ||
            !(generation.base[8] & 0x80)) {
            return false;
        }
        for (const auto& prior : members) {
            if (prior.courseId == generation.courseId) {
                return false;
            }
        }
        members.push_back(std::move(generation));
        position += size;
    }
    if (position != end) {
        return false;
    }
    out = std::move(members);
    return true;
}

std::string key(uint64_t disk, uint16_t directory, const std::string& courseId, const Bytes& base) {
    if (!disk || !validId(courseId) || base.size() != BaseBytes) {
        return {};
    }
    std::string text;
    text.reserve(86);
    appendHex(text, disk, 16);
    text += '-';
    appendHex(text, directory, 4);
    text += '-';
    for (const unsigned char c : courseId) {
        appendHex(text, c, 2);
    }
    text += '-';
    appendHex(text, crc(base), 8);
    text += ".gcs";
    return text;
}

Result publish(Backend& backend, const Generation& generation) {
    Bytes bytes;
    if (!encode(generation, bytes)) {
        return Result::Invalid;
    }
    const auto name = key(generation.disk, generation.directory, generation.courseId, generation.base);
    Bytes existing;
    const auto status = backend.read(name, existing);
    if (status == Result::Ok) {
        return existing == bytes ? Result::Ok : Result::Conflict;
    }
    if (status != Result::Missing) {
        return status;
    }
    const bool written = backend.publish(name, bytes);
    const auto verified = backend.read(name, existing);
    if (verified == Result::Ok && existing != bytes) {
        return Result::Conflict;
    }
    return written && verified == Result::Ok ? Result::Ok : Result::Io;
}

Result resolve(Backend& backend, uint64_t disk, uint16_t directory, const std::string& courseId,
               const Bytes& base, Generation& out) {
    const auto name = key(disk, directory, courseId, base);
    if (name.empty()) {
        return Result::Invalid;
    }
    Bytes bytes;
    const auto status = backend.read(name, bytes);
    if (status != Result::Ok) {
        return status;
    }
    Generation generation;
    if (!decode(bytes, generation)) {
        return Result::Invalid;
    }
    // A CRC locates a candidate; only exact identity and base bytes authorize it.
    if (generation.disk != disk || generation.directory != directory ||
        generation.courseId != courseId || generation.base != base) {
        return Result::Conflict;
    }
    out = std::move(generation);
    return Result::Ok;
}

Files::Files(std::filesystem::path path) : root(std::move(path)) {
    ready = root.is_absolute() && recovery::prepareDirectory(root);
}

Result Files::read(const std::string& name, Bytes& bytes) {
    if (!ready || !validKey(name)) {
        return Result::Io;
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(root / name, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return Result::Missing;
    }
    if (ec || !std::filesystem::is_regular_file(status)) {
        return Result::Io;
    }
    return recovery::readFile(root / name, bytes, MaxBytes) ? Result::Ok : Result::Io;
}

bool Files::publish(const std::string& name, const Bytes& bytes) {
    if (!ready || !validKey(name) || bytes.size() > MaxBytes) {
        return false;
    }
    return recovery::publishFile(root / name, bytes);
}
}
