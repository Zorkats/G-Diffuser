// G-Diffuser — asset loader.
// Bridges the generated AssetBindings (C) to libultraship's ResourceManager: given an o2r
// resource key of the form "category/symbol", load it and return the raw data pointer the
// game expects. C linkage, because the generated C binding table calls in here.

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct LoadedAssetBuffer {
    const unsigned char* ptr = nullptr;
    size_t size = 0;
    std::string key;
};

std::vector<LoadedAssetBuffer> gLoadedAssetBuffers;

} // namespace

extern "C" void* GDiffuser_LoadAsset(const char* key) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return nullptr;
    }
    if (rm->LoadResource(key) == nullptr) {
        return nullptr; // not present, or no factory registered for this resource type
    }
    return rm->GetResourceRawPointer(key);
}

extern "C" int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    if ((key == nullptr) || (out == nullptr) || (outSize == 0)) {
        return 0;
    }

    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }

    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return 0;
    }

    auto resource = rm->LoadResource(key);
    if (resource == nullptr) {
        return 0;
    }

    /* LUS resource factories consume the 64-byte OTR header (ResourceLoader.cpp
     * BufferOffset) and the per-type sub-header before the resource is handed back, so
     * GetRawPointer()/GetPointerSize() are already payload-only (TextureFactory, for
     * instance, sets ImageData = buffer + 0x50). Never re-strip a header here: it
     * truncates any payload whose first bytes coincidentally look like a magic word. */
    const unsigned char* raw = static_cast<const unsigned char*>(resource->GetRawPointer());
    const size_t rawSize = resource->GetPointerSize();
    if ((raw == nullptr) || (rawSize == 0) || (rawSize > outSize)) {
        return 0;
    }

    std::memcpy(out, raw, rawSize);
    if (rawSize < outSize) {
        std::memset(static_cast<unsigned char*>(out) + rawSize, 0, outSize - rawSize);
    }
    if (copiedSize != nullptr) {
        *copiedSize = rawSize;
    }
    return 1;
}

extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    if ((key == nullptr) || (out == nullptr) || (outSize == 0)) {
        return 0;
    }

    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }

    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return 0;
    }

    /* Raw archive-file bytes, bypassing resource-factory deserialization. Staff-ghost
     * records are stored in the o2r as Torch "GhostRecord" resources, for which the port
     * registers no libultraship factory yet (see the TODO in
     * port/resource/ResourceFactories.cpp), and LoadResource() returns nullptr without one.
     * LoadFileProcess hands back the untouched file buffer -- 64-byte OTR/Torch header then
     * the Torch-serialized payload -- for the caller to parse directly.
     *
     * The partial read is intentional: a ghost entry carries ~16 KB of trailing replay data,
     * and a caller that only needs the leading record header passes a small buffer.
     *
     * Archive backends (O2rArchive/FolderArchive/OtrArchive) over-allocate File::Buffer by a
     * fixed +4096 guard region, so Buffer->size() is NOT the real entry size and using it
     * would zero-pad truncated reads instead of surfacing a short read to the exact-size
     * integrity gates downstream. File::TrueSize is the real byte count; Buffer->size()
     * survives only as a fallback when TrueSize is unset, and as a hard clamp so the copy
     * can never run past the allocation regardless of what TrueSize reports. */
    auto file = rm->LoadFileProcess(std::string(key));
    if ((file == nullptr) || (file->Buffer == nullptr)) {
        return 0;
    }

    const size_t bufferSize = file->Buffer->size();
    const size_t fileSize = (file->TrueSize != 0) ? file->TrueSize : bufferSize;
    if (fileSize == 0) {
        return 0;
    }

    size_t copy = (fileSize < outSize) ? fileSize : outSize;
    if (copy > bufferSize) {
        copy = bufferSize;
    }
    std::memcpy(out, file->Buffer->data(), copy);
    if (copiedSize != nullptr) {
        *copiedSize = copy;
    }
    return 1;
}

extern "C" void GDiffuser_RegisterLoadedAssetBuffer(const void* buffer, size_t size, const char* key) {
    if ((buffer == nullptr) || (size == 0) || (key == nullptr) || (key[0] == '\0')) {
        return;
    }

    const auto* ptr = static_cast<const unsigned char*>(buffer);
    for (LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if (entry.ptr == ptr) {
            entry.size = size;
            entry.key = key;
            return;
        }
    }

    gLoadedAssetBuffers.push_back({ ptr, size, key });
}

/* Containing-range variant of the lookup below, for per-tile atlas overrides
 * (devdocs/MODDING_ATLAS_DESIGN.md; mechanism corrected in devdocs/POST_1_0_SCOPING.md): a band
 * load's source pointer has the band byte offset baked in (LoadBlock/LoadTile row/column math in
 * the interpreter), so an exact-pointer match misses every non-first band. Finds the registered
 * buffer whose [ptr, ptr+size) contains `addr` and reports the offset within it. When nested
 * registrations overlap, the tightest base (highest ptr <= addr) wins so a sub-asset is not
 * swallowed by a larger container registered over the same range. NULL when nothing contains it. */
extern "C" const char* GDiffuser_LookupLoadedAssetKeyContaining(const void* addr, size_t* outByteOffset) {
    if (addr == nullptr) {
        return nullptr;
    }

    const auto* ptr = static_cast<const unsigned char*>(addr);
    const LoadedAssetBuffer* best = nullptr;
    for (const LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if ((entry.ptr == nullptr) || (ptr < entry.ptr) || (ptr >= entry.ptr + entry.size)) {
            continue;
        }
        if ((best == nullptr) || (entry.ptr > best->ptr)) {
            best = &entry;
        }
    }
    if (best == nullptr) {
        return nullptr;
    }
    if (outByteOffset != nullptr) {
        *outByteOffset = static_cast<size_t>(ptr - best->ptr);
    }
    return best->key.c_str();
}

/* Exact-base variant for the bridge's RDRAM whole-image override path (issue #27): that caller
 * needs the REGISTERED buffer size to gate pack replacements (a pack payload smaller than the
 * buffer is a single atlas band, not a whole image), which the containing-range lookup does not
 * report. Matches only entry bases, never interior addresses. */
extern "C" const char* GDiffuser_LookupLoadedAssetKeyAndSize(const void* buffer, size_t* outSize) {
    if (buffer == nullptr) {
        return nullptr;
    }

    const auto* ptr = static_cast<const unsigned char*>(buffer);
    for (LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if (entry.ptr == ptr) {
            if (outSize != nullptr) {
                *outSize = entry.size;
            }
            return entry.key.c_str();
        }
    }
    return nullptr;
}

extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified) {
    if (buffer == nullptr) {
        return nullptr;
    }

    const auto* ptr = static_cast<const unsigned char*>(buffer);
    for (LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if ((entry.ptr != ptr) || (entry.size < minSize)) {
            continue;
        }

        if (requireUnmodified) {
            auto ctx = Ship::Context::GetInstance();
            if (ctx == nullptr) {
                return nullptr;
            }
            auto rm = ctx->GetResourceManager();
            if (rm == nullptr) {
                return nullptr;
            }
            auto resource = rm->LoadResource(entry.key.c_str());
            if (resource == nullptr) {
                return nullptr;
            }
            void* raw = resource->GetRawPointer();
            const size_t rawSize = resource->GetPointerSize();
            if ((raw == nullptr) || (rawSize == 0) || (rawSize > entry.size)) {
                return nullptr;
            }
            if (std::memcmp(entry.ptr, raw, rawSize) != 0) {
                return nullptr;
            }
        }

        return entry.key.c_str();
    }

    return nullptr;
}
