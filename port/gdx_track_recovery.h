#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gdx::recovery {

using Bytes = std::vector<uint8_t>;
constexpr size_t MaxJournalBytes = 1024 * 1024;
constexpr size_t MaxChanges = 53;
enum class Kind : uint8_t { Track = 1, Extensions, Title, Forks, Settings, CentSlot, CenlSlot };
enum class State : uint8_t { Deleting = 1, Deleted, Restoring, Restored };
enum class Result { Ok, Invalid, Conflict, Io };

struct Change {
    Kind kind = Kind::Track;
    uint8_t slot = 0;
    Bytes before;
};

struct Record {
    uint64_t disk = 0;
    uint64_t id = 0;
    uint16_t directory = 0;
    std::string name;
    std::string extension;
    State state = State::Deleting;
    std::vector<Change> changes;
};

class Backend {
  public:
    virtual ~Backend() = default;
    virtual bool read(const Record&, const Change&, Bytes&) = 0;
    virtual bool write(const Record&, const Change&, const Bytes&) = 0;
    virtual bool flush(const Change&) = 0;
    virtual bool journal(const Record&) = 0;
};

bool encode(const Record&, Bytes&);
bool decode(const Bytes&, Record&);
bool matchesCupReferences(const Record&, uint32_t currentMask, bool allowRemoved);
Result beginDelete(Record&, Backend&);
Result beginRestore(Record&, Backend&);
Result resume(Record&, Backend&);

}
