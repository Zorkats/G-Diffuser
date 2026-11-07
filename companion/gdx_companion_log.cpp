// Copyright (c) Zorkats

#include "gdx_companion_log.h"

#include <utility>

namespace gdx_companion {

void BoundedLog::Append(LogLevel level, std::string_view text) {
    std::string clean;
    size_t limit = text.size() < kMaxEntryBytes ? text.size() : kMaxEntryBytes;
    if (limit < text.size()) {
        // Back up to the start of a UTF-8 sequence so the cut never splits a character.
        while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) --limit;
    }
    clean.reserve(limit + 3);
    for (size_t i = 0; i < limit; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        clean += (c < 0x20) ? '?' : static_cast<char>(c);
    }
    if (limit < text.size()) clean += "...";

    const size_t cost = clean.size() + kEntryOverheadBytes;
    bytes_ += cost;
    entries_.push_back(LogEntry{level, std::move(clean)});

    while (bytes_ > kCapBytes && !entries_.empty()) {
        const LogEntry& oldest = entries_.front();
        bytes_ -= oldest.text.size() + kEntryOverheadBytes;
        entries_.pop_front();
        ++dropped_;
    }
}

}  // namespace gdx_companion
