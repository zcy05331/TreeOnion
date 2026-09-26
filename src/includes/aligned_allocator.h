#ifndef ALIGNED_ALLOCATOR_H
#define ALIGNED_ALLOCATOR_H

#include <cstdlib>
#include <new>
#include <cstddef>
#include <memory>
#include <vector>

// A deleter that frees memory with std::free (for pointers allocated via aligned allocators)
template <typename T>
struct AlignedDeleter {
    void operator()(T* p) const noexcept {
        std::free(static_cast<void*>(p));
    }
};

// Factory: create a unique_ptr to an aligned array using std::aligned_alloc + std::free
// Usage: auto ptr = make_unique_aligned<uint64_t, 64>(count);
template <typename T, std::size_t Alignment>
inline std::unique_ptr<T[], AlignedDeleter<T>> make_unique_aligned(std::size_t count) {
    if (count == 0) {
        return std::unique_ptr<T[], AlignedDeleter<T>>(nullptr);
    }
    std::size_t total_size = count * sizeof(T);
    if (total_size % Alignment != 0) {
        total_size = ((total_size / Alignment) + 1) * Alignment;
    }
    void* raw = std::aligned_alloc(Alignment, total_size);
    if (!raw) {
        throw std::bad_alloc();
    }
    return std::unique_ptr<T[], AlignedDeleter<T>>(static_cast<T*>(raw));
}

// std::vector allocator with a fixed over-alignment. glibc serves large
// requests from mmap and returns a pointer 16 bytes past the page start, so
// every 64-byte AVX-512 load of a plain std::vector buffer spans two cache
// lines; buffers the matrix kernels stream use this instead.
template <typename T, std::size_t Alignment>
struct AlignedAllocator {
    using value_type = T;
    template <typename U>
    struct rebind { using other = AlignedAllocator<U, Alignment>; };
    AlignedAllocator() noexcept = default;
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {}
    T *allocate(std::size_t n) {
        return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t(Alignment)));
    }
    void deallocate(T *p, std::size_t) noexcept {
        ::operator delete(p, std::align_val_t(Alignment));
    }
    friend bool operator==(const AlignedAllocator &, const AlignedAllocator &) noexcept {
        return true;
    }
};

template <typename T>
using CacheAlignedVector = std::vector<T, AlignedAllocator<T, 64>>;

#endif // ALIGNED_ALLOCATOR_H
