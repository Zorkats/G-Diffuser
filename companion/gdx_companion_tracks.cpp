// Copyright (c) Zorkats

#include "gdx_companion_tracks.h"

#include <cstring>
#include <fstream>
#include <system_error>

#include "gdx_companion_paths.h"
#include "gdx_mfs_image.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

constexpr size_t kHeaderSize = 0x41;
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kTypeTrack = 1;
constexpr size_t kNameField = 21;

void PutU32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    out[at] = static_cast<uint8_t>(value);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
    out[at + 2] = static_cast<uint8_t>(value >> 16);
    out[at + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t FourCc(const std::string& text) {
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24);
}

std::string Printable(const char* text) {
    std::string out;
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    return out;
}

bool IsTrackExtension(const std::string& extension) { return extension == "CRSD" || extension == "CRSE"; }

}  // namespace

TrackInspection InspectTrackPayload(const uint8_t* raw, size_t size, bool big_endian) {
    TrackInspection out;
    const int encoding = big_endian ? GDX_FOREIGN_BIG_ENDIAN : GDX_FOREIGN_HOST_ORDER;
    if (raw == nullptr || size != GDX_FOREIGN_TRACK_PAYLOAD_SIZE) {
        out.stock_errors = out.extended_errors = GDX_FOREIGN_TRACK_ERR_BAD_SIZE;
        return out;
    }
    std::vector<uint8_t> host(GDX_FOREIGN_TRACK_PAYLOAD_SIZE);
    GdxForeignCourseDiagnostic stock{};
    out.stock_errors = gdx_foreign_track_normalize(raw, static_cast<int32_t>(size), encoding, host.data(),
                                                   GDX_FOREIGN_TRACK_STOCK_MAX_Y, &stock, nullptr, nullptr);
    out.diagnostic = stock;
    if (out.stock_errors == 0) {
        out.fit = TrackFit::Stock;
        out.extended_errors = 0;
    } else {
        GdxForeignCourseDiagnostic extended{};
        out.extended_errors = gdx_foreign_track_normalize(raw, static_cast<int32_t>(size), encoding, host.data(),
                                                          GDX_FOREIGN_TRACK_EXTENDED_MAX_Y, &extended, nullptr,
                                                          nullptr);
        if (out.extended_errors == 0) {
            out.fit = TrackFit::NeedsExtended;
            out.diagnostic = extended;
        }
    }
    GdxForeignCourseData course;
    std::memcpy(&course, host.data(), sizeof(course));
    out.venue = course.venue;
    out.bgm = course.bgm;
    out.control_points = course.controlPointCount;
    if (out.fit != TrackFit::Refused) out.payload = std::move(host);
    return out;
}

std::vector<std::string> DescribeTrackErrors(uint32_t errors) {
    std::vector<std::string> out;
    if (errors & GDX_FOREIGN_TRACK_ERR_BAD_SIZE) out.push_back("The track file is not the size of a Course Edit track.");
    if (errors & GDX_FOREIGN_TRACK_ERR_ENCODING) out.push_back("The track's byte order is not supported.");
    if (errors & GDX_FOREIGN_TRACK_ERR_CHECKSUM) {
        out.push_back("The track's checksum does not match its contents, or the track is marked unsaved.");
    }
    if (errors & GDX_FOREIGN_TRACK_ERR_CREATOR) out.push_back("The track was not made with Course Edit.");
    if (errors & GDX_FOREIGN_TRACK_ERR_BGM) out.push_back("The track names a music track the game does not have.");
    if (errors & GDX_FOREIGN_TRACK_ERR_GEOMETRY) {
        out.push_back("A control point is outside the area the game accepts, or the point count is not 0 to 64.");
    }
    const uint32_t known = GDX_FOREIGN_TRACK_ERR_BAD_SIZE | GDX_FOREIGN_TRACK_ERR_ENCODING |
                           GDX_FOREIGN_TRACK_ERR_CHECKSUM | GDX_FOREIGN_TRACK_ERR_CREATOR |
                           GDX_FOREIGN_TRACK_ERR_BGM | GDX_FOREIGN_TRACK_ERR_GEOMETRY;
    if (errors & ~known) out.push_back("The track failed a check this Companion does not know.");
    return out;
}

bool ReadDiskImage(const fs::path& image, ImageListing& out, std::string& error) {
    out = ImageListing{};
    out.path = PathToUtf8(image);
    std::string narrow;
    if (!NarrowPathForC(image, narrow)) {
        error = "the image's path has characters the disk reader cannot open; move it to a folder with a plain "
                "name";
        return false;
    }
    GdxMfsImage* handle = nullptr;
    const int opened = gdx_mfsimg_open(narrow.c_str(), &handle);
    if (opened != GDX_MFSIMG_OK || handle == nullptr) {
        error = std::string("not a disk image the game can read: ") + gdx_mfsimg_strerror(opened);
        return false;
    }
    const int count = gdx_mfsimg_list(handle, nullptr, 0);
    if (count < 0) {
        error = std::string("the disk's file list cannot be read: ") + gdx_mfsimg_strerror(count);
        gdx_mfsimg_close(handle);
        return false;
    }
    std::vector<GdxMfsImageEntry> entries(static_cast<size_t>(count));
    const int listed = count == 0 ? 0 : gdx_mfsimg_list(handle, entries.data(), count);
    if (listed < 0) {
        error = std::string("the disk's file list cannot be read: ") + gdx_mfsimg_strerror(listed);
        gdx_mfsimg_close(handle);
        return false;
    }
    entries.resize(static_cast<size_t>(listed));
    for (const GdxMfsImageEntry& entry : entries) {
        if (entry.contentType == GDX_MFSIMG_TYPE_MACHINE) {
            out.machines.push_back({Printable(entry.name), Printable(entry.extension)});
            continue;
        }
        if (entry.contentType != GDX_MFSIMG_TYPE_TRACK) continue;
        ImageTrack track;
        track.entry_id = entry.entryId;
        track.name = entry.name;
        track.display_name = Printable(entry.name);
        track.extension = entry.extension;
        track.size = entry.fileSize;
        track.big_endian = entry.sourceBigEndian != 0;
        if (!IsTrackExtension(track.extension)) {
            track.read_error = "The file's type, " + Printable(entry.extension) + ", is not a saved track.";
            track.inspection.stock_errors = track.inspection.extended_errors = GDX_FOREIGN_TRACK_ERR_BAD_SIZE;
        } else if (entry.encoded) {
            track.read_error = "The track is stored encoded, which the game cannot import.";
        } else if (entry.fileSize != GDX_FOREIGN_TRACK_PAYLOAD_SIZE) {
            track.read_error = "The track file is " + std::to_string(entry.fileSize) + " bytes, not " +
                               std::to_string(GDX_FOREIGN_TRACK_PAYLOAD_SIZE) + ".";
            track.inspection.stock_errors = track.inspection.extended_errors = GDX_FOREIGN_TRACK_ERR_BAD_SIZE;
        } else {
            std::vector<uint8_t> raw(static_cast<size_t>(entry.fileSize));
            const int read = gdx_mfsimg_read_file_current(handle, entry.entryId, raw.data(), entry.fileSize);
            if (read < 0) {
                track.read_error = std::string("The track cannot be read: ") + gdx_mfsimg_strerror(read);
            } else {
                track.inspection = InspectTrackPayload(raw.data(), raw.size(), track.big_endian);
            }
        }
        out.tracks.push_back(std::move(track));
    }
    gdx_mfsimg_close(handle);
    return true;
}

uint32_t Crc32(const uint8_t* data, size_t length) {
    struct Table {
        uint32_t entries[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                entries[i] = c;
            }
        }
    };
    static const Table built;
    const uint32_t* table = built.entries;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool EncodeGdxc(const std::string& name, const std::string& extension, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& out, std::string& error) {
    if (!IsTrackExtension(extension)) {
        error = "only CRSD and CRSE tracks can be exported";
        return false;
    }
    if (name.empty() || name.size() >= kNameField || name.find('\0') != std::string::npos) {
        error = "the track name must be 1 to 20 bytes";
        return false;
    }
    if (payload.size() != GDX_FOREIGN_TRACK_PAYLOAD_SIZE) {
        error = "the track payload is not " + std::to_string(GDX_FOREIGN_TRACK_PAYLOAD_SIZE) + " bytes";
        return false;
    }
    out.assign(kHeaderSize, 0);
    std::memcpy(out.data(), "GXC1", 4);
    PutU32(out, 0x04, kFormatVersion);
    PutU32(out, 0x08, kTypeTrack);
    PutU32(out, 0x0C, FourCc(extension));
    std::memcpy(out.data() + 0x10, name.data(), name.size());
    PutU32(out, 0x25, 0);
    PutU32(out, 0x29, static_cast<uint32_t>(payload.size()));
    PutU32(out, 0x2D, Crc32(payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool ExportTrack(const ImageTrack& track, const fs::path& output, bool overwrite, std::string& error) {
    if (track.inspection.fit == TrackFit::Refused || track.inspection.payload.empty()) {
        error = "this track does not pass the game's checks, so it is not exported";
        return false;
    }
    std::string extension = PathToUtf8(output.extension());
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (extension != ".gdxc") {
        error = "the export must end in .gdxc";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(output.parent_path(), ec)) {
        error = "the folder " + PathToUtf8(output.parent_path()) + " does not exist";
        return false;
    }
    if (fs::exists(output, ec) && !overwrite) {
        error = "a file already exists at " + PathToUtf8(output);
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!EncodeGdxc(track.name, track.extension, track.inspection.payload, bytes, error)) return false;
    fs::path temporary = output;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            fs::remove(temporary, ec);
            error = "cannot write " + PathToUtf8(temporary);
            return false;
        }
    }
    fs::rename(temporary, output, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "cannot move the export into place: " + ec.message();
        return false;
    }
    return true;
}

fs::path DefaultExportPath(const fs::path& game_dir, const ImageTrack& track) {
    std::string stem;
    for (const unsigned char c : track.name) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == ' ' || c == '.';
        stem += safe ? static_cast<char>(c) : '_';
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    if (stem.empty()) stem = "track";
    return game_dir / "exports" / PathFromUtf8(stem + ".gdxc");
}

}  // namespace gdx_companion
