#include "support/alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::size_t> g_allocations{0};
}  // namespace

namespace sasr::test {

std::size_t allocation_count() { return g_allocations.load(std::memory_order_relaxed); }

}  // namespace sasr::test

// Replacement global allocation functions: forward to malloc/free, counting
// every call. Only the unsized forms are replaced; the sized/array forms
// the standard library synthesizes from them call through to these.
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }  // sized form; see [new.delete.single]

void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
