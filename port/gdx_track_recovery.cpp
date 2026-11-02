#include "gdx_track_recovery.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gdx::recovery {
namespace {
uint32_t crc(const Bytes& bytes, size_t size) {
    uint32_t value = ~0u;
    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
        }
    }
    return ~value;
}

void put(Bytes& out, uint64_t value, int size) {
    for (int i = 0; i < size; ++i) {
        out.push_back(uint8_t(value >> (i * 8)));
    }
}

bool valid(const Record& r) {
    if (!r.id || !r.disk || r.name.empty() || r.name.size() > 20 ||
        (r.extension != "CRSD" && r.extension != "CRSE") ||
        int(r.state) < 1 || int(r.state) > 4 || r.changes.size() < 5 || r.changes.size() > MaxChanges) {
        return false;
    }
    for (unsigned char c : r.name) {
        if (c < 32 || c > 126 || c == '/' || c == '.' || c == ':') {
            return false;
        }
    }
    std::array<bool, 256> seen{};
    size_t total = 128;
    for (const auto& c : r.changes) {
        const int kind = int(c.kind);
        if (kind < 1 || kind > 7 || c.slot >= (kind >= 6 ? 24 : 1) || c.before.size() > 65536) {
            return false;
        }
        const size_t key = size_t(kind) * 32 + c.slot;
        if (seen[key] || (kind == 1 && c.before.empty()) ||
            (kind == 6 && !c.before.empty() && c.before.size() != 9) ||
            (kind == 7 && !c.before.empty() && c.before.size() != 21)) {
            return false;
        }
        if (kind >= 6 && !c.before.empty() &&
            (!c.before[0] || std::find(c.before.begin(), c.before.end(), 0) == c.before.end())) {
            return false;
        }
        seen[key] = true;
        total += 6 + c.before.size();
    }
    for (int kind = 1; kind <= 5; ++kind) {
        if (!seen[size_t(kind) * 32]) {
            return false;
        }
    }
    return total <= MaxJournalBytes;
}

Result check(const Record& r, Backend& b, bool restoring, bool strict) {
    for (const auto& c : r.changes) {
        Bytes current;
        if (!b.read(r, c, current)) {
            return Result::Io;
        }
        if (strict ? (restoring ? !current.empty() : current != c.before)
                   : (!current.empty() && current != c.before)) {
            return Result::Conflict;
        }
    }
    return Result::Ok;
}
}

bool encode(const Record& r, Bytes& out) {
    if (!valid(r)) {
        return false;
    }
    Bytes bytes{'G', 'T', 'R', '1'};
    put(bytes, 1, 4);
    put(bytes, r.disk, 8);
    put(bytes, r.id, 8);
    put(bytes, r.directory, 2);
    put(bytes, uint8_t(r.state), 1);
    put(bytes, r.name.size(), 1);
    bytes.insert(bytes.end(), r.extension.begin(), r.extension.end());
    put(bytes, r.changes.size(), 4);
    bytes.insert(bytes.end(), r.name.begin(), r.name.end());
    for (const auto& c : r.changes) {
        put(bytes, uint8_t(c.kind), 1);
        put(bytes, c.slot, 1);
        put(bytes, c.before.size(), 4);
        bytes.insert(bytes.end(), c.before.begin(), c.before.end());
    }
    put(bytes, crc(bytes, bytes.size()), 4);
    out = std::move(bytes);
    return true;
}

bool decode(const Bytes& bytes, Record& out) {
    if (bytes.size() < 41 || bytes.size() > MaxJournalBytes || std::memcmp(bytes.data(), "GTR1", 4)) {
        return false;
    }
    size_t pos = 4;
    auto get = [&](int size) {
        uint64_t value = 0;
        for (int i = 0; i < size; ++i) {
            value |= uint64_t(bytes[pos++]) << (8 * i);
        }
        return value;
    };
    if (get(4) != 1) {
        return false;
    }
    Record r;
    r.disk = get(8);
    r.id = get(8);
    r.directory = uint16_t(get(2));
    r.state = State(get(1));
    const size_t nameSize = size_t(get(1));
    r.extension.assign(reinterpret_cast<const char*>(bytes.data() + pos), 4);
    pos += 4;
    const size_t count = size_t(get(4));
    if (nameSize == 0 || nameSize > 20 || count > MaxChanges || nameSize > bytes.size() - pos - 4) {
        return false;
    }
    r.name.assign(reinterpret_cast<const char*>(bytes.data() + pos), nameSize);
    pos += nameSize;
    for (size_t i = 0; i < count; ++i) {
        if (bytes.size() - pos < 10) {
            return false;
        }
        Change c;
        c.kind = Kind(get(1));
        c.slot = uint8_t(get(1));
        const size_t size = size_t(get(4));
        if (size > 65536 || size > bytes.size() - pos - 4) {
            return false;
        }
        c.before.assign(bytes.begin() + pos, bytes.begin() + pos + size);
        pos += size;
        r.changes.push_back(std::move(c));
    }
    if (pos + 4 != bytes.size() || get(4) != crc(bytes, bytes.size() - 4) || !valid(r)) {
        return false;
    }
    out = std::move(r);
    return true;
}

bool matchesCupReferences(const Record& r, uint32_t currentMask, bool allowRemoved) {
    uint32_t capturedMask = 0;
    for (const auto& c : r.changes) {
        if (c.kind == Kind::CentSlot) {
            if (c.slot >= 24) {
                return false;
            }
            capturedMask |= uint32_t(1) << c.slot;
        }
    }
    return allowRemoved ? (currentMask & ~capturedMask) == 0 : currentMask == capturedMask;
}

Result resume(Record& r, Backend& b) {
    if (!valid(r)) {
        return Result::Invalid;
    }
    if (r.state == State::Deleted || r.state == State::Restored) {
        return Result::Ok;
    }
    const bool restoring = r.state == State::Restoring;
    const auto preflight = check(r, b, restoring, false);
    if (preflight != Result::Ok) {
        return preflight;
    }
    std::array<size_t, MaxChanges> order{};
    for (size_t i = 0; i < r.changes.size(); ++i) {
        order[i] = i;
    }
    auto rank = [&](size_t i) {
        const int kind = int(r.changes[i].kind);
        // A restored reference must never precede its track and metadata.
        return restoring ? kind : (kind >= 6 ? kind - 6 : kind + 2);
    };
    std::stable_sort(order.begin(), order.begin() + r.changes.size(),
                     [&](size_t a, size_t c) { return rank(a) < rank(c); });
    const Bytes empty;
    for (size_t i = 0; i < r.changes.size(); ++i) {
        const auto& c = r.changes[order[i]];
        const auto& target = restoring ? c.before : empty;
        Bytes current;
        if (!b.read(r, c, current)) {
            return Result::Io;
        }
        if (current != target && current != (restoring ? empty : c.before)) {
            return Result::Conflict;
        }
        if (current != target && !b.write(r, c, target)) {
            return Result::Io;
        }
        // A previous failed flush may have left the intended value in memory only.
        if (!b.flush(c) || !b.read(r, c, current)) {
            return Result::Io;
        }
        if (current != target) {
            return Result::Conflict;
        }
    }
    Record completed = r;
    completed.state = restoring ? State::Restored : State::Deleted;
    if (!b.journal(completed)) {
        return Result::Io;
    }
    r = std::move(completed);
    return Result::Ok;
}

Result beginDelete(Record& r, Backend& b) {
    if (!valid(r) || r.state != State::Deleting) {
        return Result::Invalid;
    }
    const auto checked = check(r, b, false, true);
    if (checked != Result::Ok) {
        return checked;
    }
    if (!b.journal(r)) {
        return Result::Io;
    }
    return resume(r, b);
}

Result beginRestore(Record& r, Backend& b) {
    if (!valid(r) || r.state != State::Deleted) {
        return Result::Invalid;
    }
    const auto checked = check(r, b, true, true);
    if (checked != Result::Ok) {
        return checked;
    }
    Record restoring = r;
    restoring.state = State::Restoring;
    if (!b.journal(restoring)) {
        return Result::Io;
    }
    r = std::move(restoring);
    return resume(r, b);
}
}
