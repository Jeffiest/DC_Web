// The arenas' memory in the browser (platform/memory.hpp), in place of Kazarov's mmap one: page-aligned
// heap blocks. WebAssembly memory has no page protection, so the guard page after each block is ordinary
// memory: a run past the end is not caught as it is on the other platforms.

#include "platform/memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::size_t kPage = 0x1000;

std::size_t RoundUp(std::size_t value, std::size_t to) { return (value + to - 1) / to * to; }

} // namespace

std::size_t ArenaMemoryPageSize() { return kPage; }

ArenaMemory ArenaMemoryMap(std::size_t bytes, std::size_t lead) {
    std::size_t capacity = RoundUp(bytes == 0 ? 64 : bytes, 64);
    std::size_t map_size = RoundUp(lead + capacity, kPage) + kPage;
    auto       *map = static_cast<unsigned char *>(std::aligned_alloc(kPage, map_size));
    if (map == nullptr) {
        std::fprintf(stderr, "arena: cannot allocate %zu bytes\n", map_size);
        std::abort();
    }
    std::memset(map, 0, map_size);
    return {map, map_size, map + map_size - kPage - capacity, capacity, lead};
}

void ArenaMemoryZero(const ArenaMemory &memory) { ArenaMemoryZeroByRemap(memory); }

void ArenaMemoryZeroByRemap(const ArenaMemory &memory) { std::memset(memory.map, 0, memory.map_size - kPage); }

const unsigned char *ArenaMemoryGuard(const ArenaMemory &memory) { return memory.base + memory.capacity; }

void ArenaMemoryUnmap(ArenaMemory &memory) {
    std::free(memory.map);
    memory = {};
}
