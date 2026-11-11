/* port/gdx_host_placement.h -- buffers placed where no N64 address can name them.
 *
 * The port hands host pointers to decomp code as their low 32 bits (osVirtualToPhysical, K0_TO_PHYS), and
 * the resolvers rebuild a pointer by matching those bits against the registered buffers. N64 addresses are
 * 32-bit numbers too, so a buffer whose low 32 bits cover one of them makes that number mean two things,
 * and whichever meaning the resolver tries first wins. A buffer from gdx_host_alloc_placed keeps its low 32
 * bits inside a band no N64 address reaches, so the question never comes up for it.
 */
#ifndef GDX_HOST_PLACEMENT_H
#define GDX_HOST_PLACEMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The band a placed buffer's low 32 bits stay inside, [BEGIN, END). */
#define GDX_PLACEMENT_BAND_BEGIN 0x10000000u
#define GDX_PLACEMENT_BAND_END 0x80000000u

/* Nonzero when some byte of [begin, begin + size) has low 32 bits inside a window of N64 addresses that a
 * resolver can reach before their own mapping:
 *   0x00000000-0x0FFFFFFF  RDRAM offsets and segmented addresses
 *   0x80000000-0x80FFFFFF  KSEG0 RDRAM
 *   0x81000000-0x8FFFFFFF  segmented addresses with bit 31 set back by the bit 31 guess
 *   0xA0000000-0xA0FFFFFF  KSEG1 RDRAM
 * A size of 0 never collides; a size of 4 GB or more always does. */
int gdx_low32_window_collides(uintptr_t begin, size_t size);

/* size bytes of zeroed, writable memory whose low 32 bits stay inside the band from the first byte to the
 * last. When placement fails, or GDX_HOST_PLACEMENT=0 was set before the first call, the memory is placed
 * anywhere and the first such call logs it. Returns NULL for size 0 or when no memory is left. Free with
 * gdx_host_free_placed and the same size. */
void* gdx_host_alloc_placed(size_t size);
void gdx_host_free_placed(void* ptr, size_t size);

#ifdef __cplusplus
}

#include <cstddef>
#include <new>

/* std::vector allocator over the pair above, for vectors whose storage is registered as a host range. */
template <typename T>
struct GdxPlacedAllocator {
    using value_type = T;

    GdxPlacedAllocator() noexcept = default;

    template <typename U>
    GdxPlacedAllocator(const GdxPlacedAllocator<U>&) noexcept {
    }

    T* allocate(std::size_t count) {
        if (count > static_cast<std::size_t>(-1) / sizeof(T)) {
            throw std::bad_alloc();
        }
        void* memory = gdx_host_alloc_placed(count * sizeof(T));
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, std::size_t count) noexcept {
        gdx_host_free_placed(memory, count * sizeof(T));
    }
};

template <typename T, typename U>
bool operator==(const GdxPlacedAllocator<T>&, const GdxPlacedAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const GdxPlacedAllocator<T>&, const GdxPlacedAllocator<U>&) noexcept {
    return false;
}
#endif

#endif /* GDX_HOST_PLACEMENT_H */
