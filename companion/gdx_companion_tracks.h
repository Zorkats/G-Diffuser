// Copyright (c) Zorkats
//
// The Tracks workspace: a read-only look at the Course Edit tracks on a foreign 64DD disk image, using the
// game's own image reader (port/gdx_mfs_image.c) and its foreign track checks (port/gdx_foreign_track.c),
// and an export of one track as a .gdxc the game's import menu reads. Nothing here writes the disk image,
// the game's disk or its stores. Machines on the image are listed, never exported.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gdx_foreign_track.h"

namespace gdx_companion {

// What the track needs from the game.
enum class TrackFit {
    Stock,          // passes with the stock height limit
    NeedsExtended,  // passes only with Extended course height on
    Refused,        // fails either way, or could not be read
};

struct TrackInspection {
    TrackFit fit = TrackFit::Refused;
    uint32_t stock_errors = 0;
    uint32_t extended_errors = 0;
    GdxForeignCourseDiagnostic diagnostic{};  // from the limit that passed, or the stock attempt
    std::vector<uint8_t> payload;             // host-order and repaired, when fit is not Refused
    int venue = 0;
    int bgm = 0;
    int control_points = 0;
};

struct ImageTrack {
    uint16_t entry_id = 0;
    std::string name;          // MFS name bytes, as stored; written to the .gdxc header unchanged
    std::string display_name;  // name with every byte outside printable ASCII shown as '?'
    std::string extension;     // CRSD or CRSE
    int32_t size = 0;
    bool big_endian = false;
    std::string read_error;  // set when the payload could not be extracted; fit is then Refused
    TrackInspection inspection;
};

struct ImageMachine {
    std::string display_name;
    std::string extension;
};

struct ImageListing {
    std::string path;  // UTF-8
    std::vector<ImageTrack> tracks;
    std::vector<ImageMachine> machines;
};

// Validates raw (size bytes from the image) against both height limits.
TrackInspection InspectTrackPayload(const uint8_t* raw, size_t size, bool big_endian);

// One line per error bit, in plain words.
std::vector<std::string> DescribeTrackErrors(uint32_t errors);

// Opens the image, lists it, reads and inspects every track, and closes it. Returns false with error set
// when the file is not an MFS image the game can read.
bool ReadDiskImage(const std::filesystem::path& image, ImageListing& out, std::string& error);

// CRC-32, zlib polynomial, as gdx_content_crc32.
uint32_t Crc32(const uint8_t* data, size_t length);

// The 0x41-byte GXC1 header followed by payload. extension is CRSD or CRSE; name is at most 20 bytes.
bool EncodeGdxc(const std::string& name, const std::string& extension, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& out, std::string& error);

// Writes track.inspection.payload to output (a .gdxc) through output.tmp. An existing file is replaced
// only when overwrite is true. Refuses a track whose fit is Refused.
bool ExportTrack(const ImageTrack& track, const std::filesystem::path& output, bool overwrite, std::string& error);

// <game>/exports/<name>.gdxc, the folder the game's import menu lists. Bytes that a file name cannot hold
// become '_'.
std::filesystem::path DefaultExportPath(const std::filesystem::path& game_dir, const ImageTrack& track);

}  // namespace gdx_companion
