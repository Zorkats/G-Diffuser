// Copyright (c) Zorkats
//
// In-page log with a byte cap. The oldest entries are dropped first, and the drop count is kept
// so the UI can say that older lines were removed.
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace gdx_companion {

enum class LogLevel { Info, Warning, Error };

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string text;
};

class BoundedLog {
public:
    static constexpr size_t kCapBytes = 256 * 1024;
    static constexpr size_t kMaxEntryBytes = 4 * 1024;
    static constexpr size_t kEntryOverheadBytes = 16;

    // Longer text is cut at kMaxEntryBytes on a UTF-8 boundary. Control characters become '?'.
    void Append(LogLevel level, std::string_view text);

    const std::deque<LogEntry>& entries() const { return entries_; }
    size_t retained_bytes() const { return bytes_; }
    size_t dropped_entries() const { return dropped_; }

private:
    std::deque<LogEntry> entries_;
    size_t bytes_ = 0;
    size_t dropped_ = 0;
};

}  // namespace gdx_companion
