/* port/gdx_host_placement.c -- buffers placed where no N64 address can name them; see the header.
 *
 * How a buffer gets its low 32 bits into the band: reserve 4 GB more address space than the buffer needs,
 * without committing any of it. Every low32 value occurs somewhere inside such a reservation, so the address
 * whose low 32 bits start the band is in it (or the reservation's own start already fits the band). Map the
 * buffer there and give the rest back. The high 32 bits stay where the system put the reservation, so
 * address randomization keeps everything above bit 32.
 */
#include "gdx_host_placement.h"

#include <stdlib.h>
#include <string.h>

#include "port_log.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#if UINTPTR_MAX > 0xFFFFFFFFu
#define GDX_PLACEMENT_64BIT 1
#define GDX_LOW32_SPAN ((size_t)0x100000000ULL)
#else
#define GDX_PLACEMENT_64BIT 0
#endif

typedef struct GdxLow32Window {
    uint64_t begin;
    uint64_t end;
} GdxLow32Window;

/* The windows of gdx_low32_window_collides. 0x80000000-0x8FFFFFFF covers both the KSEG0 RDRAM addresses and
 * the segmented addresses the bit 31 guess turns into 0x81000000-0x8FFFFFFF. */
static const GdxLow32Window kTokenWindows[] = {
    { 0x00000000u, 0x10000000u },
    { 0x80000000u, 0x90000000u },
    { 0xA0000000u, 0xA1000000u },
};

int gdx_low32_window_collides(uintptr_t begin, size_t size) {
    const uint64_t low32Span = 0x100000000ULL;
    uint64_t first;
    uint64_t end;
    size_t i;

    if (size == 0) {
        return 0;
    }
    if ((uint64_t)size >= low32Span) {
        return 1;
    }

    first = (uint64_t)begin & 0xFFFFFFFFu;
    end = first + (uint64_t)size;
    for (i = 0; i < sizeof(kTokenWindows) / sizeof(kTokenWindows[0]); i++) {
        const GdxLow32Window* window = &kTokenWindows[i];
        /* A span that runs past 4 GB wraps around to low32 0, so the window counts a second time one
         * span up. */
        if ((first < window->end) && (end > window->begin)) {
            return 1;
        }
        if ((first < window->end + low32Span) && (end > window->begin + low32Span)) {
            return 1;
        }
    }
    return 0;
}

static int sPlacementEnabled = -1; /* -1 until the first call reads GDX_HOST_PLACEMENT */
static int sFallbackLogged = 0;

static int placement_enabled(void) {
    if (sPlacementEnabled < 0) {
        const char* value = getenv("GDX_HOST_PLACEMENT");
        sPlacementEnabled = ((value != NULL) && (strcmp(value, "0") == 0)) ? 0 : 1;
    }
    return sPlacementEnabled;
}

#ifndef _WIN32
static size_t page_round(size_t size) {
    long pageSize = sysconf(_SC_PAGESIZE);
    size_t page = (pageSize > 0) ? (size_t)pageSize : 4096u;
    return (size + page - 1u) & ~(page - 1u);
}
#endif

#if GDX_PLACEMENT_64BIT
/* How far into a reservation whose first byte has low 32 bits regionLow32 the buffer starts: 0 when the
 * reservation's own start already keeps all size bytes inside the band, else the distance to the next
 * address whose low 32 bits are the band's start. Both values are aligned to the reservation granularity,
 * so the distance is too. */
static size_t placement_offset(uint32_t regionLow32, size_t size) {
    if ((regionLow32 >= GDX_PLACEMENT_BAND_BEGIN) &&
        ((uint64_t)regionLow32 + (uint64_t)size <= (uint64_t)GDX_PLACEMENT_BAND_END)) {
        return 0;
    }
    return (size_t)(uint32_t)(GDX_PLACEMENT_BAND_BEGIN - regionLow32);
}

#ifdef _WIN32
static void* placement_map(size_t size) {
    const SIZE_T total = (SIZE_T)size + (SIZE_T)GDX_LOW32_SPAN;
    int attempt;

    /* Windows cannot give back part of a reservation, so it is released whole and the chosen address mapped
     * again. Another thread can take that address in between; try again then. */
    for (attempt = 0; attempt < 4; attempt++) {
        void* region = VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_NOACCESS);
        uintptr_t target;
        void* placed;

        if (region == NULL) {
            return NULL;
        }
        target = (uintptr_t)region + placement_offset((uint32_t)(uintptr_t)region, size);
        VirtualFree(region, 0, MEM_RELEASE);
        placed = VirtualAlloc((void*)target, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (placed == (void*)target) {
            return placed;
        }
        if (placed != NULL) {
            VirtualFree(placed, 0, MEM_RELEASE);
        }
    }
    return NULL;
}
#else
static void* placement_map(size_t size) {
    const size_t mapped = page_round(size);
    const size_t total = mapped + GDX_LOW32_SPAN;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* region;
    uintptr_t base;
    uintptr_t target;
    uintptr_t tail;

#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    region = mmap(NULL, total, PROT_NONE, flags, -1, 0);
    if (region == MAP_FAILED) {
        return NULL;
    }
    base = (uintptr_t)region;
    target = base + placement_offset((uint32_t)base, mapped);
    tail = target + mapped;
    if (target > base) {
        munmap(region, target - base);
    }
    if (base + total > tail) {
        munmap((void*)tail, (base + total) - tail);
    }
    if (mprotect((void*)target, mapped, PROT_READ | PROT_WRITE) != 0) {
        munmap((void*)target, mapped);
        return NULL;
    }
    return (void*)target;
}
#endif
#endif /* GDX_PLACEMENT_64BIT */

static void* fallback_map(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* memory = mmap(NULL, page_round(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (memory == MAP_FAILED) ? NULL : memory;
#endif
}

void* gdx_host_alloc_placed(size_t size) {
    void* memory = NULL;
    const char* reason;

    if (size == 0) {
        return NULL;
    }

#if GDX_PLACEMENT_64BIT
    if (!placement_enabled()) {
        reason = "GDX_HOST_PLACEMENT=0";
    } else if (size > (size_t)(GDX_PLACEMENT_BAND_END - GDX_PLACEMENT_BAND_BEGIN)) {
        reason = "larger than the band";
    } else {
        memory = placement_map(size);
        reason = "the address space reservation failed";
    }
#else
    (void)placement_enabled;
    reason = "32-bit build";
#endif

    if (memory == NULL) {
        memory = fallback_map(size);
        if ((memory != NULL) && !sFallbackLogged) {
            sFallbackLogged = 1;
            gdx_port_logf("[placement] a buffer of %zu bytes was placed anywhere (%s): base=%p low32=%08X "
                          "meets an N64 address window=%d\n",
                          size, reason, memory, (unsigned)((uintptr_t)memory & 0xFFFFFFFFu),
                          gdx_low32_window_collides((uintptr_t)memory, size));
        }
    }
    return memory;
}

void gdx_host_free_placed(void* ptr, size_t size) {
    if ((ptr == NULL) || (size == 0)) {
        return;
    }
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, page_round(size));
#endif
}
