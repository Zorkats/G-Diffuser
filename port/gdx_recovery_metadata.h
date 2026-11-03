#pragma once
#include "gdx_track_recovery.h"

namespace gdx::recovery {
bool readMetadata(const Record&, Kind, Bytes&);
bool writeMetadata(const Record&, Kind, const Bytes&);
bool canRestoreMetadata(const Record&);
bool canRestoreMetadata(const std::vector<Record>&);
}
