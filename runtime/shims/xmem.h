#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::xmem {

// Layout of a GDK title's exported `__XMemAllocationHooks__` table (measured in the retail executable).
struct XMemAllocationHooks {
    using AllocFn = void* (*)(std::size_t size, std::uint64_t attributes);
    using FreeFn = void (*)(void* ptr, std::uint64_t attributes);
    AllocFn alloc = nullptr;
    FreeFn free = nullptr;
};

void set_allocation_hooks(const XMemAllocationHooks* hooks);
const XMemAllocationHooks* allocation_hooks();

bool commit_physical_range(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

}
