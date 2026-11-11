/* Single byte-source shim. See gdx_segment_source.h.
 *
 * Resolves an absolute ROM read archive-first via the generated segment_blob
 * table (port/gen/AssetBindings.c), falling back to the raw ROM image
 * (gdx_rom_buffer). Blob payloads are loaded once per family and cached for the
 * process lifetime (ROM data is read-only) -- there is NO allocation on the
 * per-frame read path, only on the first load of each family.
 *
 * Thread safety: the graphics thread, the game thread, and the dedicated audio
 * std::thread (via the osEPiStartDma DMA sink) all call the shim.
 * First-load of a family (the only mutating operation) is serialized under one
 * lock, mirroring n64_sched.c's gdx_mq_lock (SRWLOCK on Windows, pthread_mutex
 * elsewhere). Once a family's payload pointer is published it is immutable, so
 * the payload memcpy is done AFTER releasing the lock.
 */
#include "gdx_segment_source.h"
#include "gdx_host_placement.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "port_log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
static SRWLOCK sSegLock = SRWLOCK_INIT;
static void seg_lock(void) { AcquireSRWLockExclusive(&sSegLock); }
static void seg_unlock(void) { ReleaseSRWLockExclusive(&sSegLock); }
/* Acquire/release ordering for the lock-free read fast path (below). The only
 * Windows target here is x86-64: aligned word loads/stores are already atomic and
 * the hardware memory model (TSO) never reorders load-load or store-store, so a
 * compiler barrier is all that is needed to realize acquire/release. This mirrors
 * n64_sched.c's Interlocked-based cross-thread style. */
#pragma intrinsic(_ReadWriteBarrier)
#define SEG_ACQUIRE_FENCE() _ReadWriteBarrier()
#define SEG_RELEASE_FENCE() _ReadWriteBarrier()
#else
#include <pthread.h>
static pthread_mutex_t sSegLock = PTHREAD_MUTEX_INITIALIZER;
static void seg_lock(void) { pthread_mutex_lock(&sSegLock); }
static void seg_unlock(void) { pthread_mutex_unlock(&sSegLock); }
/* GCC/Clang stand-alone fences give real acquire/release on every POSIX target
 * (including weakly-ordered ARM), keeping the fast path correct there too. */
#define SEG_ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define SEG_RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif

/* Generated blob table lookup (port/gen/AssetBindings.c, READ-only interface).
 * Containment semantics: returns the entry whose [rom_base, rom_base+size)
 * fully contains [rom_base_query, +size_needed), or NULL. */
typedef struct {
    unsigned int rom_base;
    unsigned int size;
    const char* o2r_key;
} GdxSegmentBlobEntry;
extern const GdxSegmentBlobEntry* gdx_lookup_segment_blob(unsigned int rom_base,
                                                          unsigned int size_needed);

/* Raw-ROM fallback source (port/rom_buffer.cpp). */
extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;

/* Raw archive-file reader (port/AssetLoader.cpp): copies min(fileSize, outSize) bytes
 * of the o2r file INCLUDING its 0x40 Torch resource header, returns 1 on success. The
 * size_t parameters must stay spelled exactly as in the C++ definition, or this C
 * prototype disagrees with it across the extern "C" boundary. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize,
                                          size_t* copiedSize);

/* Archive entry framing: 0x40-byte Torch header, u32 little-endian
 * payload size at 0x40, verbatim ROM-slice payload begins at 0x44. */
#define GDX_ARCHIVE_HEADER_BYTES 0x40u
#define GDX_ARCHIVE_SIZE_FIELD_BYTES 0x04u
#define GDX_ARCHIVE_PAYLOAD_OFFSET (GDX_ARCHIVE_HEADER_BYTES + GDX_ARCHIVE_SIZE_FIELD_BYTES) /* 0x44 */

/* Per-family cache + telemetry slots, discovered lazily by blob-entry identity -- the
 * generated table is static, so entry pointers are stable identities. 128 leaves ample
 * headroom over the 22 venue/geometry families plus the audio_blob families this shim
 * also serves. A family beyond the cap still serves correct bytes through the raw-ROM
 * fallback; it is just not cached or tracked. */
#define GDX_SEG_FAMILY_MAX 128

typedef struct {
    const GdxSegmentBlobEntry* entry;  /* identity (stable table pointer) */
    unsigned char* volatile payload;   /* cached blob payload view (process lifetime);
                                        * lock-free read fast path publishes this last */
    volatile unsigned int payloadSize; /* usable payload bytes at `payload` */
    int loadState;                     /* 0 = unattempted, 1 = loaded, 2 = failed */
    unsigned int fallbackReads;        /* per-family raw-ROM fallbacks */
    int loggedFallback;                /* rate-limit: first fallback per family logs once */
} GdxSegFamilySlot;

static GdxSegFamilySlot sFamilies[GDX_SEG_FAMILY_MAX];
/* Monotonically increasing count of populated slots. Published with a release
 * fence AFTER a new slot is fully initialized so the lock-free read path can
 * iterate [0, sFamilyCount) without the lock and never observe a torn slot. */
static volatile unsigned int sFamilyCount;
static unsigned int sUnmappedFallback; /* fallbacks where no blob contains the read */
static int sUnmappedLogged;

/* Strict archive mode. GDX_STRICT_ARCHIVE (any non-empty, non-"0" value)
 * turns EVERY raw-ROM fallback into a logged defect -- the soak evidence collector.
 * It NEVER aborts (logged-defect semantics): a fallback still serves byte-identical
 * raw ROM. The per-family "log once" rate-limit is lifted to one line per 100
 * fallbacks, each carrying the running count. Read from the environment exactly
 * once (getenv is idempotent, so the first-read race is benign: every racer
 * computes the same value). */
static volatile int sStrictMode = -1; /* -1 = unread, 0 = off, 1 = on */
static int seg_strict_mode(void) {
    int m = sStrictMode;
    if (m < 0) {
        const char* e = getenv("GDX_STRICT_ARCHIVE");
        m = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        sStrictMode = m;
    }
    return m;
}

/* Find or create the slot for a blob entry. Caller holds sSegLock. */
static GdxSegFamilySlot* seg_slot_for(const GdxSegmentBlobEntry* entry) {
    unsigned int i;
    unsigned int count = sFamilyCount; /* holding the lock: plain read is fine */
    GdxSegFamilySlot* slot;
    for (i = 0; i < count; i++) {
        if (sFamilies[i].entry == entry) {
            return &sFamilies[i];
        }
    }
    if (count >= GDX_SEG_FAMILY_MAX) {
        return NULL;
    }
    /* Fully initialize the new slot BEFORE it becomes reachable to the lock-free
     * reader. The release fence + the subsequent count publication guarantee that
     * a reader which observes the incremented count also observes these stores, so
     * every slot in [0, sFamilyCount) is fully constructed (payload NULL until
     * loaded, which the fast path treats as "take the lock"). */
    slot = &sFamilies[count];
    slot->entry = entry;
    slot->payload = NULL;
    slot->payloadSize = 0;
    slot->loadState = 0;
    slot->fallbackReads = 0;
    slot->loggedFallback = 0;
    SEG_RELEASE_FENCE();
    sFamilyCount = count + 1;
    return slot;
}

/* Lazily load and cache a family's blob payload. Caller holds sSegLock.
 * Returns 1 if the payload is available, 0 to force a raw-ROM fallback. A failed
 * load is sticky (loadState 2) so a missing archive entry falls back once and
 * then cheaply forever, without re-hitting the resource manager per read. */
static int seg_ensure_loaded(GdxSegFamilySlot* slot) {
    const GdxSegmentBlobEntry* e;
    size_t cap;
    size_t copied;
    unsigned char* buf;
    unsigned int payloadSize;

    if (slot->loadState == 1) {
        return 1;
    }
    if (slot->loadState == 2) {
        return 0;
    }

    e = slot->entry;
    /* Whole-file buffer: header + size field + the full family payload span. If
     * the archive's real payload is larger than the table promises, the copy is
     * truncated below and we fall back (conservative, still byte-correct). */
    cap = (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET + (size_t)e->size;
    /* main.cpp registers the audio payloads as host ranges, so every family buffer is placed clear
     * of the N64 address windows (gdx_host_placement.h) and freed with the matching call. */
    buf = (unsigned char*)gdx_host_alloc_placed(cap);
    if (buf == NULL) {
        slot->loadState = 2;
        return 0;
    }

    copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes(e->o2r_key, buf, cap, &copied) ||
        copied < (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET) {
        gdx_host_free_placed(buf, cap);
        slot->loadState = 2;
        return 0;
    }

    /* u32 little-endian payload size at 0x40. */
    payloadSize = (unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES] |
                  ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 1] << 8) |
                  ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 2] << 16) |
                  ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 3] << 24);

    /* The payload must be fully present and cover at least the span the table
     * promises (containment guarantees every served read fits inside e->size). */
    if (payloadSize < e->size ||
        (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET + (size_t)payloadSize > copied) {
        gdx_host_free_placed(buf, cap);
        slot->loadState = 2;
        return 0;
    }

    /* The whole file buffer stays alive (process-lifetime cache, never freed); the payload
     * view starts at +0x44. Publication order matters for the lock-free read fast path:
     * payloadSize and the copied bytes must settle BEFORE the release fence, with `payload`
     * published last, so a reader that acquire-loads a non-NULL payload also sees the
     * matching size and buffer contents. */
    slot->payloadSize = payloadSize;
    slot->loadState = 1;
    SEG_RELEASE_FENCE();
    slot->payload = buf + GDX_ARCHIVE_PAYLOAD_OFFSET;
    gdx_port_logf("[seg-src] blob loaded %s bytes=%u", e->o2r_key, payloadSize);
    return 1;
}

int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst) {
    const GdxSegmentBlobEntry* entry;
    const unsigned char* src = NULL;

    if (dst == NULL) {
        return 0;
    }

    entry = gdx_lookup_segment_blob((unsigned int)romBase, (unsigned int)size);
    if (entry != NULL) {
        /* Lock-free fast path: a published payload is immutable for the process lifetime,
         * so a read into an already-loaded family serves the copy without the lock and the
         * game and graphics threads never serialize on it. Only first-load and sticky-fail
         * families fall through to the lock below. */
        unsigned int count = sFamilyCount; /* aligned u32 load is atomic on the targets */
        unsigned int i;
        SEG_ACQUIRE_FENCE(); /* order the count load before the slot reads */
        for (i = 0; i < count; i++) {
            if (sFamilies[i].entry == entry) {
                unsigned char* published = sFamilies[i].payload;
                SEG_ACQUIRE_FENCE(); /* order the payload load before payload-derived reads */
                if (published != NULL) {
                    /* Containment (checked at lookup) guarantees
                     * (romBase - rom_base) + size <= payloadSize <= buffer length. */
                    memcpy(dst, published + ((unsigned int)romBase - entry->rom_base),
                           (size_t)size);
                    return 1;
                }
                break; /* slot exists but not yet loaded -> take the lock */
            }
        }

        seg_lock();
        {
            GdxSegFamilySlot* slot = seg_slot_for(entry);
            if (slot != NULL && seg_ensure_loaded(slot)) {
                /* Containment guarantees (romBase - rom_base) + size <= payloadSize. */
                src = slot->payload + ((unsigned int)romBase - entry->rom_base);
            } else if (slot != NULL) {
                slot->fallbackReads++;
                if (seg_strict_mode()) {
                    if ((slot->fallbackReads % 100u) == 1u) {
                        gdx_port_logf("[seg-src] STRICT: fallback family=%s rom=%X count=%u",
                                      entry->o2r_key, (unsigned int)romBase, slot->fallbackReads);
                    }
                } else if (!slot->loggedFallback) {
                    slot->loggedFallback = 1;
                    gdx_port_logf("[seg-src] fallback family=%s rom=%X", entry->o2r_key,
                                  (unsigned int)romBase);
                }
            }
        }
        seg_unlock();

        if (src != NULL) {
            /* Payload is immutable once published -- copy outside the lock so the
             * per-frame graphics path never serializes on a large texture memcpy. */
            memcpy(dst, src, (size_t)size);
            return 1;
        }
        /* Archive miss/short-read: fall through to the byte-identical raw ROM. */
    } else {
        seg_lock();
        sUnmappedFallback++;
        if (seg_strict_mode()) {
            if ((sUnmappedFallback % 100u) == 1u) {
                gdx_port_logf("[seg-src] STRICT: fallback family=<unmapped> rom=%X count=%u",
                              (unsigned int)romBase, sUnmappedFallback);
            }
        } else if (!sUnmappedLogged) {
            sUnmappedLogged = 1;
            gdx_port_logf("[seg-src] fallback family=<unmapped> rom=%X", (unsigned int)romBase);
        }
        seg_unlock();
    }

    /* Raw-ROM fallback -- byte-identical to the pre-shim direct read. */
    if (gdx_rom_buffer == NULL || (size_t)romBase + (size_t)size > gdx_rom_size) {
        return 0;
    }
    memcpy(dst, gdx_rom_buffer + (size_t)romBase, (size_t)size);
    return 1;
}

int GdxSegmentSourceContainingSpan(uint32_t romBase, uint32_t* outSpan) {
    /* size_needed = 1: find whichever blob's [rom_base, +size) contains romBase.
     * The generated table is static/const, so this needs no lock. */
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    if (entry == NULL) {
        return 0;
    }
    if (outSpan != NULL) {
        *outSpan = (entry->rom_base + entry->size) - (unsigned int)romBase;
    }
    return 1;
}

int GdxSegmentSourcePreload(uint32_t romBase) {
    /* size_needed = 1: locate whichever family's [rom_base, +size) contains
     * romBase, then drive it through the same lock + lazy-load machinery the read
     * path uses. Boot-time single-shot; the per-family cache makes it idempotent. */
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    int resident = 0;

    if (entry == NULL) {
        return 0;
    }

    seg_lock();
    {
        GdxSegFamilySlot* slot = seg_slot_for(entry);
        if (slot != NULL && seg_ensure_loaded(slot)) {
            resident = 1;
        }
    }
    seg_unlock();
    return resident;
}

int GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize) {
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    int found = 0;

    if (entry == NULL) {
        return 0;
    }

    seg_lock();
    {
        unsigned int i;
        unsigned int count = sFamilyCount; /* holding the lock: plain read is fine */
        for (i = 0; i < count; i++) {
            if (sFamilies[i].entry == entry) {
                if (sFamilies[i].payload != NULL) {
                    if (outPayload != NULL) {
                        *outPayload = sFamilies[i].payload;
                    }
                    if (outSize != NULL) {
                        *outSize = sFamilies[i].payloadSize;
                    }
                    found = 1;
                }
                break;
            }
        }
    }
    seg_unlock();
    return found;
}

unsigned int gdx_segment_source_fallback_total(void) {
    unsigned int total = 0;
    unsigned int i;
    seg_lock();
    for (i = 0; i < sFamilyCount; i++) {
        total += sFamilies[i].fallbackReads;
    }
    total += sUnmappedFallback;
    seg_unlock();
    return total;
}

int GdxSegmentSourceFamilyStats(unsigned int index, const char** outKey,
                                unsigned int* outFallbackReads) {
    int found = 0;
    seg_lock();
    if (index < sFamilyCount) {
        if (outKey != NULL) {
            *outKey = sFamilies[index].entry->o2r_key;
        }
        if (outFallbackReads != NULL) {
            *outFallbackReads = sFamilies[index].fallbackReads;
        }
        found = 1;
    }
    seg_unlock();
    return found;
}
