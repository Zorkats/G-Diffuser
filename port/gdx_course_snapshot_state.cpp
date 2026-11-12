#include "gdx_course_snapshot_state.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_registry.h"
#include "gdx_recovery_metadata.h"
#include <cstring>

namespace gdx::snapshot {
namespace {
recovery::Record record(const Generation& g) {
    recovery::Record r;
    r.disk = g.disk;
    r.directory = g.directory;
    const auto separator = g.courseId.size() - 5;
    r.name = g.courseId.substr(0, separator);
    r.extension = g.courseId.substr(separator + 1);
    return r;
}

void toNode(const GdxBranchEditPoint& p, GdxBranchNode& n) {
    std::memcpy(n.pos, p.pos, sizeof(n.pos));
    n.radiusLeft = p.radiusLeft;
    n.radiusRight = p.radiusRight;
    n.trackSegmentInfo = p.trackSegmentInfo;
    n.bankAngle = p.bankAngle;
    n.pit = p.parts[0];
    n.dash = p.parts[1];
    n.dirt = p.parts[2];
    n.ice = p.parts[3];
    n.jump = p.parts[4];
    n.landmine = p.parts[5];
    n.gate = p.parts[6];
    n.building = p.parts[7];
    n.sign = p.parts[8];
}

void fromNode(const GdxBranchNode& n, GdxBranchEditPoint& p) {
    std::memcpy(p.pos, n.pos, sizeof(p.pos));
    p.radiusLeft = n.radiusLeft;
    p.radiusRight = n.radiusRight;
    p.trackSegmentInfo = n.trackSegmentInfo;
    p.bankAngle = n.bankAngle;
    p.parts[0] = n.pit;
    p.parts[1] = n.dash;
    p.parts[2] = n.dirt;
    p.parts[3] = n.ice;
    p.parts[4] = n.jump;
    p.parts[5] = n.landmine;
    p.parts[6] = n.gate;
    p.parts[7] = n.building;
    p.parts[8] = n.sign;
}
}

namespace {
bool expandState(const Generation& generation, Bytes& restored) {
    Bytes verified;
    if (!encode(generation, verified)) {
        return false;
    }
    const auto binding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(generation.courseId.data()), generation.courseId.size());
    const auto& bytes = generation.metadata[size_t(Metadata::Forks)];
    GdxBranchSet forks{};
    if (!bytes.empty() && gdx_branch_decode(bytes.data(), bytes.size(), binding, &forks) != 0) {
        return false;
    }
    std::array<GdxBranchEditPoint, GDX_BRANCH_EXTRA_POINTS> tail{};
    for (int i = 0; i < forks.extendedCount; ++i) {
        fromNode(forks.extended[i], tail[size_t(i)]);
    }
    restored.resize(gdx_course_snapshot_runtime_bytes());
    const uint32_t checksum = (generation.base[8] & 0x80) ? generation.nativeChecksum :
        uint32_t(generation.base[4] | (uint32_t(generation.base[5]) << 8) |
                 (uint32_t(generation.base[6]) << 16) | (uint32_t(generation.base[7]) << 24));
    if (!gdx_course_snapshot_expand(restored.data(), generation.base.data(), tail.data(),
                                    generation.pointCount, checksum)) {
        return false;
    }
    return true;
}
}

bool validateState(const Generation& generation) {
    Bytes restored;
    return expandState(generation, restored);
}

bool expandRuntime(const Generation& generation, Bytes& runtime) {
    return expandState(generation, runtime);
}

bool preflightState(const Generation& generation) {
    if (!validateState(generation)) {
        return false;
    }
    auto r = record(generation);
    for (size_t i = 0; i < generation.metadata.size(); ++i) {
        r.changes.push_back({recovery::Kind(i + 2), 0, generation.metadata[i]});
    }
    return recovery::canRestoreMetadata(r);
}

bool preflightStates(const std::vector<Generation>& generations) {
    std::vector<recovery::Record> records;
    records.reserve(generations.size());
    for (const auto& g : generations) {
        if (!validateState(g)) {
            return false;
        }
        auto r = record(g);
        for (size_t i = 0; i < g.metadata.size(); ++i) {
            r.changes.push_back({recovery::Kind(i + 2), 0, g.metadata[i]});
        }
        records.push_back(std::move(r));
    }
    return recovery::canRestoreMetadata(records);
}

namespace {
// readState and peekState differ in one step: a track never saved through the snapshot path (an unsealed base
// with no generation) gets its generation published by readState and only built in memory by peekState.
Result readStateWith(Backend& backend, Generation& identity, const Bytes& base, bool publishLegacy) {
    if (base.size() != BaseBytes) {
        return Result::Invalid;
    }
    Generation loaded;
    auto status = resolve(backend, identity.disk, identity.directory, identity.courseId, base, loaded);
    if (status == Result::Missing && !(base[8] & 0x80)) {
        Bytes restored(gdx_course_snapshot_runtime_bytes());
        uint32_t checksum;
        std::memcpy(&checksum, base.data() + 4, 4);
        if (!gdx_course_snapshot_expand(restored.data(), base.data(), nullptr, base[1], checksum)) {
            return Result::Invalid;
        }
        loaded = identity;
        if (!captureState(loaded, restored.data()) || !legacyImage(loaded) || loaded.base != base) {
            return Result::Invalid;
        }
        status = publishLegacy ? publish(backend, loaded) : Result::Ok;
    }
    if (status != Result::Ok) {
        return status;
    }
    if (!validateState(loaded)) {
        return Result::Invalid;
    }
    identity = std::move(loaded);
    return Result::Ok;
}
}

Result readState(Backend& backend, Generation& identity, const Bytes& base) {
    return readStateWith(backend, identity, base, true);
}

Result peekState(Backend& backend, Generation& identity, const Bytes& base) {
    return readStateWith(backend, identity, base, false);
}

Result loadState(Backend& backend, Generation& identity, const Bytes& base, void* runtime) {
    if (!runtime) {
        return Result::Invalid;
    }
    Generation loaded = identity;
    const auto status = readState(backend, loaded, base);
    if (status != Result::Ok) {
        return status;
    }
    if (!restoreState(loaded, runtime)) {
        return Result::Invalid;
    }
    identity = std::move(loaded);
    return Result::Ok;
}

Result prepareSave(Backend& backend, Generation& identity, const void* runtime, const Bytes& previousBase) {
    if (!previousBase.empty()) {
        Generation previous;
        const auto status = resolve(backend, identity.disk, identity.directory, identity.courseId, previousBase, previous);
        if (status != Result::Ok) {
            return status;
        }
        Bytes verifiedRuntime;
        if (!expandState(previous, verifiedRuntime)) {
            return Result::Invalid;
        }
    }
    Generation next = identity;
    if (!captureState(next, runtime)) {
        return Result::Invalid;
    }
    const auto status = publish(backend, next);
    if (status != Result::Ok) {
        return status;
    }
    identity = std::move(next);
    return Result::Ok;
}

bool captureState(Generation& generation, const void* runtime) {
    Generation next = generation;
    next.base.resize(BaseBytes);
    if (key(next.disk, next.directory, next.courseId, next.base).empty()) {
        return false;
    }
    std::array<GdxBranchEditPoint, GDX_BRANCH_EXTRA_POINTS> tail{};
    const int result = gdx_course_snapshot_capture(runtime, next.base.data(), tail.data());
    if (!result) {
        return false;
    }
    next.pointCount = uint8_t(result - 1);
    std::memcpy(&next.nativeChecksum, next.base.data() + 4, 4);
    auto r = record(next);
    for (size_t i = 0; i < next.metadata.size(); ++i) {
        if (!recovery::readMetadata(r, recovery::Kind(i + 2), next.metadata[i])) {
            return false;
        }
    }
    GdxBranchSet forks{};
    if (gdx_branch_store_get(next.courseId.c_str(), &forks) && forks.ringCount != next.pointCount) {
        return false;
    }
    forks.ringCount = next.pointCount;
    forks.courseChecksum = next.nativeChecksum;
    forks.extendedCount = next.pointCount > 64 ? next.pointCount - 64 : 0;
    forks.baseCrc = gdx_course_registry_crc32(next.base.data(), CourseBytes);
    for (int i = 0; i < forks.extendedCount; ++i) {
        toNode(tail[size_t(i)], forks.extended[i]);
    }
    const auto binding = gdx_course_registry_crc32(
        reinterpret_cast<const unsigned char*>(next.courseId.data()), next.courseId.size());
    auto& bytes = next.metadata[size_t(Metadata::Forks)];
    bytes.resize(GDX_BRANCH_BLOCK_MAX_BYTES);
    const int length = gdx_branch_encode(&forks, binding, bytes.data(), bytes.size());
    if (length <= 0) {
        return false;
    }
    bytes.resize(size_t(length));
    if (!seal(next)) {
        return false;
    }
    generation = std::move(next);
    return true;
}

bool restoreState(const Generation& generation, void* runtime) {
    if (!runtime) {
        return false;
    }
    Bytes restored;
    if (!expandState(generation, restored)) {
        return false;
    }
    auto r = record(generation);
    std::array<Bytes, size_t(Metadata::Count)> previous;
    for (size_t i = 0; i < generation.metadata.size(); ++i) {
        r.changes.push_back({recovery::Kind(i + 2), 0, generation.metadata[i]});
        if (!recovery::readMetadata(r, recovery::Kind(i + 2), previous[i])) {
            return false;
        }
    }
    if (!recovery::canRestoreMetadata(r)) {
        return false;
    }
    for (size_t i = 0; i < generation.metadata.size(); ++i) {
        if (!recovery::writeMetadata(r, recovery::Kind(i + 2), generation.metadata[i])) {
            for (size_t j = 0; j <= i; ++j) {
                recovery::writeMetadata(r, recovery::Kind(j + 2), previous[j]);
            }
            return false;
        }
    }
    std::memcpy(runtime, restored.data(), restored.size());
    return true;
}
}
