#pragma once

#include "gdx_track_recovery.h"
#include <filesystem>
#include <memory>

namespace gdx::recovery {
bool readFile(const std::filesystem::path&, Bytes&, size_t limit = MaxJournalBytes);
bool writeFile(const std::filesystem::path&, const Bytes&);
bool prepareDirectory(const std::filesystem::path&);
class FileLock {
  public:
    explicit FileLock(const std::filesystem::path&);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool held() const;
  private:
    intptr_t handle = -1;
};
}
