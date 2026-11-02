#include <gtest/gtest.h>

#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataset.hpp"
#include "main.hpp"
#include "platform/memory.hpp"
#include "platform/arena.hpp"
#include "platform_fixture.hpp"
#include "render/texture_registry.hpp"
#include "texture.hpp"

namespace dc::test {

using namespace dc::game;

namespace {

bool Aligned64(const void *pointer) {
    return (reinterpret_cast<std::uintptr_t>(pointer) & 63) == 0;
}

} // namespace

TEST(PlatformArena, CarvesWithHeadroom) {
    InitializeDataBuffer();
    SetDataBuffer(&VisualData, 1000);
    ASSERT_TRUE(Aligned64(VisualData.base));
    ASSERT_TRUE(VisualData.limit == 4000);
    ASSERT_TRUE(VisualData.used == 0);

    // Past what retail's 1000 quadwords would hold.
    u_char *first = VisualData.Alloc(1500);
    ASSERT_TRUE(first == VisualData.base);
    std::memset(first, 0x5A, std::size_t{1500} * 16);
    VisualData.Alloc(1);
    u_char *aligned = VisualData.Alloc64(1000);
    ASSERT_TRUE(Aligned64(aligned));
    ASSERT_TRUE(aligned == &VisualData.base[std::ptrdiff_t{1504} * 16]);
    std::memset(VisualData.base, 0x33, static_cast<std::size_t>(VisualData.limit) * 16);

    SetDataBuffer(&MotionData, 10);
    ASSERT_TRUE(MotionData.base != VisualData.base);
    ASSERT_TRUE(MotionData.limit == 40);
    ASSERT_TRUE(Aligned64(MotionData.Alloc64(3)));
    ASSERT_TRUE(Aligned64(MotionData.Alloc64(1)));
    ASSERT_TRUE(MotionData.used == 5);
}

TEST(PlatformArena, WorldsKeepTheirOwnBlocks) {
    // MoonMod (O2 C1): a town and a dungeon carve the same owners; each world keeps its own blocks
    SetNextDataWorld(101);
    InitializeDataBuffer();
    SetDataBuffer(&TextureData, 100);
    u_char *town = TextureData.base;
    std::memset(town, 0x7A, std::size_t{100} * 16);

    SetNextDataWorld(200);
    InitializeDataBuffer();
    SetDataBuffer(&TextureData, 100);
    ASSERT_TRUE(TextureData.base != town);
    ASSERT_TRUE(town[0] == 0x7A && town[100 * 16 - 1] == 0x7A);

    // back in the town's world: the same block, still as the town left it until its world is cleared
    ArenaSetWorld(101);
    SetDataBuffer(&TextureData, 100);
    ASSERT_TRUE(TextureData.base == town);
    ASSERT_TRUE(town[0] == 0x7A);
    SetNextDataWorld(101);
    InitializeDataBuffer();
    ASSERT_TRUE(town[0] == 0);

    // anything else is world 0 again
    InitializeDataBuffer();
    ASSERT_TRUE(ArenaWorld() == 0);
}

TEST(PlatformArena, WorldsKeepTheirOwnTextures) {
    // MoonMod (O2 C1): the manager's release in one world leaves another world's textures alive
    using render::PortCreatePlaceholder;
    SetNextDataWorld(101);
    InitializeDataBuffer();
    ASSERT_TRUE(render::PortTextureWorld() == 101);
    unsigned town_palette = 0;
    const unsigned town = PortCreatePlaceholder("world test town", 8, 8, 8, render::PortTextureOwner::Manager,
                                                &town_palette);
    ASSERT_TRUE(town != 0 && render::PortKeyLive(town));

    SetNextDataWorld(200);
    InitializeDataBuffer();
    ASSERT_TRUE(TextureManagerKept(101));
    render::PortReleaseOwner(render::PortTextureOwner::Manager);
    ASSERT_TRUE(render::PortKeyLive(town));

    // world 0 again, then the town's own release takes its texture
    InitializeDataBuffer();
    ASSERT_TRUE(render::PortTextureWorld() == 0);
    render::PortSetTextureWorld(101);
    render::PortReleaseOwner(render::PortTextureOwner::Manager);
    ASSERT_TRUE(!render::PortKeyLive(town));
    render::PortSetTextureWorld(0);
}

TEST(PlatformArena, ReusesAndZeroes) {
    InitializeDataBuffer();
    SetDataBuffer(&TextureData, 2000);
    u_char *base = TextureData.base;
    std::memset(base, 0xFF, std::size_t{2000} * 16);
    InitializeDataBuffer();
    SetDataBuffer(&TextureData, 1000);
    ASSERT_TRUE(TextureData.base == base);
    ASSERT_TRUE(TextureData.limit == 4000);
    for (int i = 0; i < 2000 * 16; ++i) {
        ASSERT_TRUE(base[i] == 0);
    }

    SetDataBuffer(&TextureData, 100000);
    ASSERT_TRUE(TextureData.base != base);
    ASSERT_TRUE(base[0] == 0);
    TextureData.base[100000 * 4 * 16 - 1] = 1;
}

TEST(PlatformArena, OverflowAborts) {
    InitializeDataBuffer();
    SetDataBuffer(&VisualData, 100);
    KZ_ASSERT_ABORT([] { VisualData.Alloc(401); });
    KZ_ASSERT_ABORT([] { VisualData.Alloc64(400); });
    KZ_ASSERT_EXIT([] { VisualData.Alloc(400); }, 0);
    // An overrun that skips the allocator runs into the guard page behind the block.
    KZ_ASSERT_FAULT([] { VisualData.base[static_cast<std::ptrdiff_t>(VisualData.limit) * 16] = 1; });
    KZ_ASSERT_ABORT([] { SystemMesBuffer.Alloc(6001); });
}

TEST(PlatformArena, RejectsNegativeAndUnrepresentableRequests) {
    InitializeDataBuffer();
    SetDataBuffer(&VisualData, 100);
    KZ_ASSERT_ABORT([] { VisualData.Alloc(-1); });
    KZ_ASSERT_ABORT([] { VisualData.Alloc64(-1); });
    KZ_ASSERT_ABORT([] { VisualData.Alloc(INT_MAX); });
}

TEST(PlatformArena, ModeBuffers) {
    BufferAllClear();
    ASSERT_TRUE(VisualData.limit == 600000 * 4);
    ASSERT_TRUE(ActiveData0.limit == 25000 * 4 && ActiveData1.limit == 25000 * 4);
    ASSERT_TRUE(ActiveData0.base != ActiveData1.base);
    ASSERT_TRUE(WorkBuffer == &workbuffer);
    ASSERT_TRUE(workbuffer.limit == 4096 * 4);
    ASSERT_TRUE(Aligned64(read_buffer));
    read_buffer[100000 * 4 * 4 - 1] = 1;

    InitializeDataBuffer();
    ASSERT_TRUE(WaterData.limit == 40 && ActiveData0.limit == 100000);
    SetPacketReadBuffer(30000, 140000);
    ASSERT_TRUE(workbuffer.limit == 2048 * 4 && workbuffer.used == 0);
    // EdMenuBuffer starts 0x180000 bytes before read_buffer (InitWorkBuffer).
    u_char *menu = reinterpret_cast<u_char *>(read_buffer) - 0x180000;
    std::memset(menu, 1, std::size_t{0x3A2E0} * 16);

    u_char *global = GlobalDataBuffer.Alloc64(10);
    ASSERT_TRUE(Aligned64(global));
    GlobalDataBuffer.Alloc(1);
    ASSERT_TRUE(Aligned64(GlobalDataBuffer.Alloc64(1)));

    SystemMesBuffer.used = 1;
    SystemMesBuffer.Align64();
    ASSERT_TRUE(Aligned64(&SystemMesBuffer.block[SystemMesBuffer.used]));
}

} // namespace dc::test
