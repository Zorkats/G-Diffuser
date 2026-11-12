// port/gdx_net_track_cache.cpp -- the cache of downloaded custom tracks, kept beside mods/ (G-NET O4.3).
// Copyright (c) Zorkats

#include "gdx_net_track_cache.h"

#include "gdx_recovery_file.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace gdx::net {
namespace {

namespace fs = std::filesystem;
using track::TrackId;

const std::string kExtension = ".gcp";
// writeFile names its temporary file <final name>.writing-<ticks>-<sequence>.
const std::string kTempMarker = ".writing-";
constexpr size_t kHexLength = track::kIdBytes * 2;

// A write takes milliseconds, so an older temporary file was left by a write that never finished. A younger
// one may belong to another game copy on the same data root, so it is left alone.
constexpr auto kStaleTempAge = std::chrono::minutes(10);

struct Entry {
    fs::path path;
    uintmax_t size = 0;
    fs::file_time_type written;
};

bool IsLowerHex(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'a' && c <= 'f';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

bool AllDigits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Exactly 64 lower-case hex digits and the extension. Anything else is not ours: it is never counted or deleted.
bool ParseName(const std::string& name, TrackId& id) {
    if (name.size() != kHexLength + kExtension.size()) {
        return false;
    }
    const std::string hex = name.substr(0, kHexLength);
    if (name.compare(kHexLength, kExtension.size(), kExtension) != 0 || !IsLowerHex(hex)) {
        return false;
    }
    return track::FromHex(hex, id);
}

bool IsTempName(const std::string& name) {
    const std::string marker = kExtension + kTempMarker;
    if (name.size() <= kHexLength + marker.size() || name.compare(kHexLength, marker.size(), marker) != 0 ||
        !IsLowerHex(name.substr(0, kHexLength))) {
        return false;
    }
    const std::string rest = name.substr(kHexLength + marker.size());
    const size_t dash = rest.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    return AllDigits(rest.substr(0, dash)) && AllDigits(rest.substr(dash + 1));
}

fs::path PathFor(const fs::path& folder, const TrackId& id) {
    return folder / (track::ToHex(id) + kExtension);
}

// A symlink, junction or directory under a managed name is never followed, read, counted or deleted.
bool IsRegularFile(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_regular_file(status);
}

bool IsAbsent(const fs::path& path) {
    std::error_code ec;
    return fs::symlink_status(path, ec).type() == fs::file_type::not_found;
}

// True when the path is gone afterwards: a file another game copy already removed counts as gone.
bool RemoveFile(const fs::path& path) {
    if (!IsRegularFile(path)) {
        return IsAbsent(path);
    }
    std::error_code ec;
    fs::remove(path, ec);
    return !ec || IsAbsent(path);
}

// Deletes path only when the name on disk is exactly the managed name. On Windows an upper-case file answers
// to the lower-case lookup, so the directory entry is what decides, not the lookup.
bool RemoveExact(const fs::path& folder, const fs::path& path) {
    const std::string wanted = path.filename().string();
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string() == wanted) {
            return RemoveFile(path);
        }
    }
    return false;
}

bool ReadEntry(const fs::path& path, Entry& entry) {
    std::error_code ec;
    entry.path = path;
    entry.size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    entry.written = fs::last_write_time(path, ec);
    return !ec;
}

// Lists the managed files, and removes temporary files that an interrupted write left behind. Other entries
// are not ours and are not touched.
void Scan(const fs::path& folder, std::vector<Entry>& entries) {
    entries.clear();
    std::error_code ec;
    const fs::file_time_type now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        const std::string name = path.filename().string();
        TrackId id{};
        if (ParseName(name, id)) {
            Entry entry;
            if (IsRegularFile(path) && ReadEntry(path, entry)) {
                entries.push_back(entry);
            }
        } else if (IsTempName(name) && IsRegularFile(path)) {
            std::error_code stat;
            const fs::file_time_type written = fs::last_write_time(path, stat);
            if (!stat && now - written > kStaleTempAge) {
                RemoveFile(path);
            }
        }
    }
}

// Drops the least recently used managed files until both caps hold. The file just stored is never dropped.
// A removal that fails leaves the cache over its cap until the next Store, which runs this again.
void Evict(const fs::path& folder, size_t maxFiles, size_t maxBytes, const fs::path& keep) {
    std::vector<Entry> entries;
    Scan(folder, entries);
    uintmax_t bytes = 0;
    for (const Entry& entry : entries) {
        bytes += entry.size;
    }
    // Equal stamps happen on coarse clocks; the name keeps the order the same on every run.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.written != b.written) {
            return a.written < b.written;
        }
        return a.path.filename() < b.path.filename();
    });
    size_t count = entries.size();
    for (const Entry& entry : entries) {
        if (count <= maxFiles && bytes <= maxBytes) {
            break;
        }
        if (entry.path.filename() == keep.filename()) {
            continue;
        }
        if (RemoveFile(entry.path)) {
            count--;
            bytes -= entry.size;
        }
    }
}

} // namespace

TrackCache::TrackCache(const fs::path& folder, size_t maxFiles, size_t maxBytes)
    : folder_(folder), maxFiles_(maxFiles), maxBytes_(maxBytes), ok_(false) {
    try {
        if (!folder.is_absolute() || maxFiles == 0 || maxBytes == 0) {
            return;
        }
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec) {
            return;
        }
        const bool directory = fs::is_directory(folder, ec);
        if (ec || !directory) {
            return;
        }
        ok_ = true;
        std::vector<Entry> ignored;
        Scan(folder_, ignored);
    } catch (...) {
        ok_ = false;
    }
}

bool TrackCache::ok() const {
    return ok_;
}

bool TrackCache::Has(const track::TrackId& id) const {
    try {
        return ok_ && IsRegularFile(PathFor(folder_, id));
    } catch (...) {
        return false;
    }
}

bool TrackCache::Load(const track::TrackId& id, std::vector<unsigned char>& bytes) {
    bytes.clear();
    try {
        if (!ok_) {
            return false;
        }
        const fs::path path = PathFor(folder_, id);
        if (!IsRegularFile(path)) {
            return false;
        }
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec) {
            // Gone between the check and the read: a miss, not a damaged file.
            return false;
        }
        if (size > track::kMaxBytes) {
            RemoveExact(folder_, path);
            return false;
        }
        gdx::recovery::Bytes raw;
        if (!gdx::recovery::readFile(path, raw, track::kMaxBytes)) {
            return false;
        }
        std::vector<unsigned char> data(raw.begin(), raw.end());
        if (track::Verify(data.data(), data.size(), id, nullptr) != track::Verdict::Ok) {
            RemoveExact(folder_, path);
            return false;
        }
        // Recency lives in the write time, so the order survives a restart without a side index.
        std::error_code touch;
        fs::last_write_time(path, fs::file_time_type::clock::now(), touch);
        bytes = std::move(data);
        return true;
    } catch (...) {
        bytes.clear();
        return false;
    }
}

bool TrackCache::Store(const track::TrackId& id, const std::vector<unsigned char>& bytes) {
    try {
        if (!ok_ || bytes.size() > maxBytes_) {
            return false;
        }
        if (track::Verify(bytes.data(), bytes.size(), id, nullptr) != track::Verdict::Ok) {
            return false;
        }
        std::error_code ec;
        fs::create_directories(folder_, ec);
        const fs::path path = PathFor(folder_, id);
        const gdx::recovery::Bytes data(bytes.begin(), bytes.end());
        if (!gdx::recovery::writeFile(path, data)) {
            return false;
        }
        Evict(folder_, maxFiles_, maxBytes_, path);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gdx::net
