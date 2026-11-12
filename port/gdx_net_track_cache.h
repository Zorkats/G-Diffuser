// port/gdx_net_track_cache.h -- the cache of downloaded custom tracks, kept beside mods/ (G-NET O4.3).
// Copyright (c) Zorkats
//
// A track lives at <data root>/online/cache/tracks/<hex id>.gcp as its content bytes, named by the identity
// those bytes hash to (plan decision 12). Every read is checked against the identity in its name, so a damaged
// or swapped file is a miss and never a track. Nothing here touches a save, MFS or the snapshot repository.
#pragma once

#include "net/gdx_net_track_pack.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace gdx::net {

class TrackCache {
  public:
    static constexpr size_t kDefaultMaxFiles = 64;
    static constexpr size_t kDefaultMaxBytes = 8 * 1024 * 1024;

    // The folder must be absolute; it is created when missing. Store drops the least recently used tracks
    // until both caps hold again.
    explicit TrackCache(const std::filesystem::path& folder, size_t maxFiles = kDefaultMaxFiles,
                        size_t maxBytes = kDefaultMaxBytes);

    // False for a relative folder, a zero limit, or a folder that cannot be created.
    bool ok() const;

    // A regular file of that name exists. It does not verify the bytes; Load is the checked read.
    bool Has(const track::TrackId& id) const;

    // Clears bytes first. On a hit the track becomes the most recently used. A file that fails the check is
    // deleted, so the next player fetches it again instead of reading the same damage.
    bool Load(const track::TrackId& id, std::vector<unsigned char>& bytes);

    // Refuses bytes that fail the check against id, and bytes larger than the byte cap, since no eviction
    // could ever make them fit.
    bool Store(const track::TrackId& id, const std::vector<unsigned char>& bytes);

  private:
    std::filesystem::path folder_;
    size_t maxFiles_;
    size_t maxBytes_;
    bool ok_;
};

} // namespace gdx::net
