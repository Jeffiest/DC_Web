#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>

#include "data_fixture.hpp"
#include "dataread.hpp"
#include "dataread_port.hpp"
#include "gfx/context.hpp"
#include "platform/paths.hpp"
#include "tex_fixture.hpp"

using namespace dc::test;
using namespace texfix;

namespace {

constexpr std::array<uint8_t, 4> kWhite = {255, 255, 255, 0x80};
constexpr std::array<uint8_t, 4> kGreen = {0, 255, 0, 0x80};

void Enter(Bytes &img, int block, int mipmap = 0) {
    TexManager.BeginEnterTextureBlock(block);
    TexManager.EnterIMGFile(img.data(), block, mipmap, 0);
    TexManager.EndEnterTextureBlock(block);
}

PortTextureRef Resolve(const char *name) {
    std::string mutable_name(name);
    return PortTextureFromCTexture(TexManager.GetTexture(mutable_name.data(), -1));
}

// Retail's Conv8to32, PageConv8to32 and BlockConv8to32 (ps2/src/texture.cpp), the specification of
// the "IM2" layout; they are static there, so they are restated here.
void BlockConv8to32(const uint8_t *source, uint8_t *destination) {
    static const int lut[128] = {
        0, 36, 8, 44, 1, 37, 9, 45, 2, 38, 10, 46, 3, 39, 11, 47, 4, 32, 12, 40, 5, 33, 13, 41,
        6, 34, 14, 42, 7, 35, 15, 43, 16, 52, 24, 60, 17, 53, 25, 61, 18, 54, 26, 62, 19, 55, 27, 63,
        20, 48, 28, 56, 21, 49, 29, 57, 22, 50, 30, 58, 23, 51, 31, 59, 4, 32, 12, 40, 5, 33, 13, 41,
        6, 34, 14, 42, 7, 35, 15, 43, 0, 36, 8, 44, 1, 37, 9, 45, 2, 38, 10, 46, 3, 39, 11, 47,
        20, 48, 28, 56, 21, 49, 29, 57, 22, 50, 30, 58, 23, 51, 31, 59, 16, 52, 24, 60, 17, 53, 25, 61,
        18, 54, 26, 62, 19, 55, 27, 63};
    int out_index = 0;
    for (unsigned i = 0; i < 4; i++) {
        int lut_index = (i & 1) * 64;
        for (unsigned j = 0; j < 16; j++) {
            for (unsigned k = 0; k < 4; k++) {
                destination[out_index++] = source[lut[lut_index++]];
            }
        }
        source += 64;
    }
}

void PageConv8to32(const uint8_t *source, uint8_t *destination) {
    static const int block_table[32] = {0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23,
                                        8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31};
    auto             block_column = std::make_unique<int[]>(32);
    auto             block_row = std::make_unique<int[]>(32);
    int              entry = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            block_column[block_table[entry]] = j;
            block_row[block_table[entry]] = i;
            entry++;
        }
    }
    auto work8 = std::make_unique<uint8_t[]>(256);
    auto work32 = std::make_unique<uint8_t[]>(256);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            const uint8_t *source_cursor = source + i * 2048 + j * 16;
            for (int k = 0; k < 16; k++) {
                std::memcpy(work8.get() + k * 16, source_cursor + k * 128, 16);
            }
            BlockConv8to32(work8.get(), work32.get());
            int      block_index = block_table[i * 8 + j];
            uint8_t *destination_cursor = destination + block_row[block_index] * 2048 + block_column[block_index] * 32;
            for (int k = 0; k < 8; k++) {
                std::memcpy(destination_cursor + k * 256, work32.get() + k * 32, 32);
            }
        }
    }
}

Bytes Conv8to32(int width, int height, const Bytes &source) {
    Bytes destination(static_cast<size_t>(width) * height);
    auto  work8 = std::make_unique<uint8_t[]>(8192);
    auto  work32 = std::make_unique<uint8_t[]>(8192);
    int   pages_x = ((width - 1) >> 7) + 1;
    int   pages_y = ((height - 1) >> 6) + 1;
    int   row_bytes;
    int   row_count;
    if (pages_x == 1) {
        row_bytes = width * 2;
    } else {
        width = 128;
        row_bytes = 256;
    }
    if (pages_y == 1) {
        row_count = height >> 1;
    } else {
        height = 64;
        row_count = 32;
    }
    for (int i = 0; i < pages_y; i++) {
        for (int j = 0; j < pages_x; j++) {
            const uint8_t *source_cursor = source.data() + i * (pages_x * (width << 6)) + width * j;
            for (int k = 0; k < height; k++) {
                std::memcpy(work8.get() + k * 128, source_cursor, width);
                source_cursor += width * pages_x;
            }
            PageConv8to32(work8.get(), work32.get());
            uint8_t *destination_cursor = destination.data() + i * (pages_x * (row_bytes * row_count)) + row_bytes * j;
            for (int k = 0; k < row_count; k++) {
                std::memcpy(destination_cursor, work32.get() + k * 256, row_bytes);
                destination_cursor += row_bytes * pages_x;
            }
        }
    }
    return destination;
}

// GitHub's macOS runner GPU reads palette index 1 wrong; real Apple GPUs do not.
bool OnParavirtualGpu() {
    return std::strstr(gfx::detail::g.properties.deviceName, "Paravirtual") != nullptr;
}

} // namespace

TEST(TexDecode, Rgb32Alpha) {
    TexEnv env;
    Bytes  img = Img({
        {"rgb32", Tim2({TIM2_RGB32, 2, 2, {Rgba32({Gs(255, 0, 0), Gs(0, 255, 0), Gs(0, 0, 255, 0x40), Gs(255, 0, 0, 0)})}, {}, 0})}
    });
    Enter(img, 0);
    PortTextureRef ref = Resolve("rgb32");
    ASSERT_TRUE(ref.valid && ref.width == 2 && ref.height == 2);
    ASSERT_TRUE(ref.binding.palette == gfx::kNullTexture);
    env.gfx.Frame(kWhite, [&] { env.Draw(ref, 0, 0, 200, 200, 0, 0, 2, 2, AlphaBlend()); });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 255, 0, 0));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 0, 255, 0));
    // GS alpha 0x40 is half: (Cs - Cd) * As + Cd over white.
    ASSERT_TRUE(env.gfx.PixelNear(50, 150, 128, 128, 255, 3));
    ASSERT_TRUE(env.gfx.PixelNear(150, 150, 255, 255, 255));
}

// A town block can overlap a fixed texture's simulated PS2 address while both native images remain valid.
TEST(TexDecode, FixedAndBlockTexturesSurviveLegacyVramOverlap) {
    TexEnv env;
    char   fixed_name[] = "fixed";
    Bytes  fixed(8 * 8 * 4, 0);
    Bytes  menu(8 * 8 * 4, 0);
    for (size_t i = 0; i < fixed.size(); i += 4) {
        fixed[i] = 255;
        fixed[i + 3] = 0x80;
        menu[i + 1] = 255;
        menu[i + 3] = 0x80;
    }
    TexManager.EnterFixTexture(fixed_name, fixed.data(), 8, 8, 4, nullptr, 0, 0,
                               nullptr, nullptr, nullptr, 0, 0);

    TexManager.blocks[16].vram_top = 16320;
    TexManager.blocks[16].vram_end = 16320;
    Bytes img = Img({
        {"menu", Tim2({TIM2_RGB32, 8, 8, {menu}, {}, 0})}
    });
    Enter(img, 16);

    PortTextureRef fixed_ref = Resolve("fixed");
    PortTextureRef menu_ref = Resolve("menu");
    ASSERT_TRUE(fixed_ref.valid && menu_ref.valid);
    ASSERT_GT(TexManager.blocks[16].vram_end, TexManager.vram_fix);
    env.gfx.Frame(kWhite, [&] {
        env.Draw(fixed_ref, 0, 0, 100, 100, 0, 0, 8, 8);
        env.Draw(menu_ref, 100, 0, 100, 100, 0, 0, 8, 8);
    });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 255, 0, 0));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 0, 255, 0));
}

TEST(TexDecode, Rgb24TakesAlphaFromTexa) {
    TexEnv env;
    Bytes  img = Img({
        {"rgb24", Tim2({TIM2_RGB24, 2, 1, {{255, 128, 0, 0, 0, 0}}, {}, 0})}
    });
    Enter(img, 0);
    PortTextureRef ref = Resolve("rgb24");
    ASSERT_TRUE(ref.valid);
    auto info = gfx::GetTextureInfo(ref.binding.texture);
    ASSERT_TRUE(info && !info->has_alpha);
    sceGsTex0 tex0;
    std::memcpy(&tex0, &TexManager.GetTexture(1)->tex0, sizeof tex0);
    ASSERT_TRUE(tex0.PSM == SCE_GS_PSMCT24);
    // TEXA: TA0 0x80 for colour, AEM makes the black texel transparent.
    env.gfx.Frame(kWhite, [&] { env.Draw(ref, 0, 0, 200, 100, 0, 0, 2, 1, AlphaBlend()); });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 255, 128, 0));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 255, 255, 255));
}

TEST(TexDecode, Rgb16WidensAndTakesTheAlphaBit) {
    TexEnv env;
    Bytes  pixels(4);
    Put16(pixels, 0, 0x8000 | (31 << 10) | (16 << 5));
    Put16(pixels, 2, 0x001F);
    Bytes img = Img({
        {"rgb16", Tim2({TIM2_RGB16, 2, 1, {pixels}, {}, 0})}
    });
    Enter(img, 0);
    PortTextureRef ref = Resolve("rgb16");
    ASSERT_TRUE(ref.valid);
    env.gfx.Frame(kGreen, [&] { env.Draw(ref, 0, 0, 200, 100, 0, 0, 2, 1, AlphaBlend()); });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 0, 128, 248));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 0, 255, 0));
}

// The GS reads an 8-bit CLUT in CSM1 with bits 3 and 4 of the index swapped: entry 8 is the
// file's ninth-from-sixteen and entry 16 the file's eighth-from-eight.
TEST(TexDecode, Idtex8ReadsTheClutInCsm1Order) {
    TexEnv env;
    Bytes  clut = Clut256({
        {8, Gs(255, 0, 0)},
        {16, Gs(0, 255, 0)},
        {0, Gs(0, 0, 255)},
        {255, Gs(255, 255, 255)},
        {31, Gs(255, 255, 0, 0x40)}
    });
    Bytes  img = Img({
        {"idx8", Tim2({TIM2_IDTEX8, 4, 1, {{8, 16, 0, 255}}, clut, 256})}
    });
    Enter(img, 0);
    PortTextureRef ref = Resolve("idx8");
    ASSERT_TRUE(ref.valid && ref.binding.palette != gfx::kNullTexture);
    CTexture *texture = TexManager.GetTexture(1);
    sceGsTex0 tex0;
    std::memcpy(&tex0, &texture->tex0, sizeof tex0);
    ASSERT_TRUE(tex0.PSM == SCE_GS_PSMT8 && tex0.CLD == 1 && tex0.CBP != 0);

    uint32_t palette[256];
    ASSERT_TRUE(PortPaletteEntries(tex0.CBP, palette));
    ASSERT_TRUE(palette[8] == Rgba(0, 255, 0));
    ASSERT_TRUE(palette[16] == Rgba(255, 0, 0));
    ASSERT_TRUE(palette[0] == Rgba(0, 0, 255));
    ASSERT_TRUE(palette[255] == Rgba(255, 255, 255));
    ASSERT_TRUE(palette[31] == Rgba(255, 255, 0, 0x80));
    for (unsigned i = 0; i < 256; i++) {
        ASSERT_TRUE(PortClutCsm1(PortClutCsm1(i)) == i);
    }
    ASSERT_TRUE(PortClutCsm1(8) == 16 && PortClutCsm1(15) == 23 && PortClutCsm1(40) == 48 && PortClutCsm1(7) == 7);

    env.gfx.Frame({0, 0, 0, 0x80}, [&] { env.Draw(ref, 0, 0, 400, 100, 0, 0, 4, 1); });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 0, 255, 0));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 255, 0, 0));
    ASSERT_TRUE(env.gfx.PixelNear(250, 50, 0, 0, 255));
    ASSERT_TRUE(env.gfx.PixelNear(350, 50, 255, 255, 255));
}

TEST(TexDecode, Idtex4ExpandsToIndex8) {
    TexEnv env;
    if (OnParavirtualGpu()) {
        GTEST_SKIP() << "the CI runner's virtual GPU misreads palette index 1";
    }
    Bytes clut = Rgba32({Gs(255, 255, 255), Gs(255, 0, 0), Gs(0, 255, 0), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         Gs(0, 0, 255)});
    Bytes img = Img({
        {"idx4", Tim2({TIM2_IDTEX4, 4, 1, {{0x21, 0x0F}}, clut, 16})}
    });
    Enter(img, 0);
    PortTextureRef ref = Resolve("idx4");
    ASSERT_TRUE(ref.valid && ref.width == 4);
    auto info = gfx::GetTextureInfo(ref.binding.texture);
    ASSERT_TRUE(info && info->format == gfx::TextureFormat::Index8);
    std::vector<uint8_t> indices;
    uint32_t             width;
    uint32_t             height;
    ASSERT_TRUE(gfx::ReadbackTexture(ref.binding.texture, indices, width, height));
    ASSERT_TRUE(width == 4 && indices[0] == 1 && indices[1] == 2 && indices[2] == 15 && indices[3] == 0);
    env.gfx.Frame({0, 0, 0, 0x80}, [&] { env.Draw(ref, 0, 0, 400, 100, 0, 0, 4, 1); });
    ASSERT_TRUE(env.gfx.PixelNear(50, 50, 255, 0, 0));
    ASSERT_TRUE(env.gfx.PixelNear(150, 50, 0, 255, 0));
    ASSERT_TRUE(env.gfx.PixelNear(250, 50, 0, 0, 255));
    ASSERT_TRUE(env.gfx.PixelNear(350, 50, 255, 255, 255));
}

TEST(TexDecode, Im2UnswizzleInvertsConv8to32) {
    std::mt19937 random(7);
    const int    sizes[][2] = {
        {16,  16 },
        {32,  16 },
        {64,  32 },
        {128, 64 },
        {64,  128},
        {256, 64 },
        {256, 256},
        {512, 128}
    };
    for (auto [width, height] : sizes) {
        Bytes linear(static_cast<size_t>(width) * height);
        for (uint8_t &texel : linear) {
            texel = static_cast<uint8_t>(random());
        }
        Bytes swizzled = Conv8to32(width, height, linear);
        Bytes back(linear.size());
        PortUnswizzle8(width, height, swizzled.data(), back.data());
        if (back != linear) {
            std::fprintf(stderr, "unswizzle differs at %dx%d\n", width, height);
        }
        ASSERT_TRUE(back == linear);
    }
}

TEST(TexDecode, Im2ArchiveDrawsUnswizzled) {
    TexEnv env;
    if (OnParavirtualGpu()) {
        GTEST_SKIP() << "the CI runner's virtual GPU misreads palette index 1";
    }
    Bytes linear(32 * 32);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            linear[y * 32 + x] = static_cast<uint8_t>(1 + x / 16 + 2 * (y / 16));
        }
    }
    Bytes mip(16 * 16);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            mip[y * 16 + x] = static_cast<uint8_t>(1 + x / 8 + 2 * (y / 8));
        }
    }
    Bytes clut = Clut256({
        {1, Gs(255, 0,   0)  },
        {2, Gs(0,   255, 0)  },
        {3, Gs(0,   0,   255)},
        {4, Gs(255, 255, 255)}
    });
    Bytes img = Img({
                        {"swz", Tim2({TIM2_IDTEX8, 32, 32, {Conv8to32(32, 32, linear), Conv8to32(16, 16, mip)}, clut, 256})}
    },
                    true);
    Enter(img, 0, 1);
    PortTextureRef ref = Resolve("swz");
    ASSERT_TRUE(ref.valid);
    std::vector<uint8_t> indices;
    uint32_t             width;
    uint32_t             height;
    ASSERT_TRUE(gfx::ReadbackTexture(ref.binding.texture, indices, width, height));
    ASSERT_TRUE(Bytes(indices.begin(), indices.end()) == linear);
    auto info = gfx::GetTextureInfo(ref.binding.texture);
    ASSERT_TRUE(info && info->mip_levels == 2);
    env.gfx.Frame({0, 0, 0, 0x80}, [&] { env.Draw(ref, 0, 0, 320, 320, 0, 0, 32, 32); });
    ASSERT_TRUE(env.gfx.PixelNear(80, 80, 255, 0, 0));
    ASSERT_TRUE(env.gfx.PixelNear(240, 80, 0, 255, 0));
    ASSERT_TRUE(env.gfx.PixelNear(80, 240, 0, 0, 255));
    ASSERT_TRUE(env.gfx.PixelNear(240, 240, 255, 255, 255));
}

TEST(TexDecode, MipmapsAsSupplied) {
    TexEnv env;
    Bytes  base(8 * 8 * 4, 0x40);
    Bytes  half(4 * 4 * 4, 0x80);
    Bytes  quarter(2 * 2 * 4, 0xC0);
    Bytes  img = Img({
        {"mips",  Tim2({TIM2_RGB32, 8, 8, {base, half, quarter}, {}, 0})},
        {"plain", Tim2({TIM2_RGB32, 8, 8, {base, half, quarter}, {}, 0})}
    });
    Enter(img, 0, 1);
    PortTextureRef ref = Resolve("mips");
    ASSERT_TRUE(ref.valid);
    auto info = gfx::GetTextureInfo(ref.binding.texture);
    ASSERT_TRUE(info && info->mip_levels == 3);
    std::string name = "mips";
    CTexture   *texture = TexManager.GetTexture(name.data(), -1);
    ASSERT_TRUE(texture->tex1 == SCE_GS_SET_TEX1(0, 2, 1, 5, 1, 0, -120));
    ASSERT_TRUE(texture->image[1] != nullptr && texture->image[2] != nullptr);
    // TEX1 MMAG picks the filter.
    ASSERT_TRUE(PortTextureFromTex0(texture->tex0, texture->tex1).binding.filter == gfx::Filter::Linear);
    ASSERT_TRUE(PortTextureFromTex0(texture->tex0, 0).binding.filter == gfx::Filter::Nearest);

    Bytes again = img;
    TexManager.BeginEnterTextureBlock(1);
    TexManager.EnterIMGFile(again.data(), 1, 0, 0);
    TexManager.EndEnterTextureBlock(1);
    name = "plain";
    CTexture *plain = TexManager.GetTexture(name.data(), 1);
    ASSERT_TRUE(plain != nullptr && plain->tex1 == 0);
    auto plain_info = gfx::GetTextureInfo(PortTextureFromCTexture(plain).binding.texture);
    ASSERT_TRUE(plain_info && plain_info->mip_levels == 1);
}

TEST(TextureBuffer, CompactsBlocksAndRelocatesTexturePointers) {
    TexEnv env;
    TexManager.blocks[1].buffer = staging + 8;
    TexManager.blocks[1].buffer_end = staging + 10;
    TexManager.blocks[2].buffer = staging + 16;
    TexManager.blocks[2].buffer_end = staging + 18;
    staging[8] = 0x1234;
    staging[9] = 0x5678;
    staging[16] = 0x9ABC;
    staging[17] = 0xDEF0;
    TexManager.textures[1].block = 1;
    TexManager.textures[1].image[0] = reinterpret_cast<u_int *>(staging + 8);

    ASSERT_EQ(TexManager.CleanUpBuffer(), 1);
    ASSERT_EQ(TexManager.blocks[1].buffer, staging);
    ASSERT_EQ(TexManager.blocks[1].buffer_end, staging + 2);
    ASSERT_EQ(TexManager.blocks[2].buffer, staging + 8);
    ASSERT_EQ(TexManager.textures[1].image[0], reinterpret_cast<u_int *>(staging));
    ASSERT_EQ(staging[0], 0x1234);
    ASSERT_EQ(staging[1], 0x5678);
    ASSERT_EQ(staging[8], 0x9ABC);
    ASSERT_EQ(staging[9], 0xDEF0);
}

TEST(TextureBuffer, RejectsBlocksOutsideStagingBuffer) {
    TexEnv env;
    TexManager.blocks[1].buffer = staging + 8;
    TexManager.blocks[1].buffer_end = staging + std::size(staging) + 1;

    ASSERT_EQ(TexManager.CleanUpBuffer(), 0);
    ASSERT_EQ(TexManager.blocks[1].buffer, staging + 8);
    ASSERT_EQ(TexManager.buffer_used, 0);
}

TEST(Texture, BakedDocumentPathsFollowDc1SourceIdentity) {
    DataReadSource source;
    source.path = "img/mt01.img";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/img/mt01_img.json");

    source.path = "img/mt01.tm2";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/img/mt01_tm2.json");

    source.path = "img_6/title.img";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/img_6/title_img.json");

    source.path = "commenu/a_fre/kgetoan2.img__";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/commenu/a_fre/kgetoan2_img__.json");

    source.path = "map/room.pac";
    source.member = "part.ipk/panel.img";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/map/panel_img-5ea003ec7ffe1ca5.json");

    source.member = "hero.chr/panel.img";
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/map/hero-panel_img-6fdaa27efdabbb25.json");

    source.path = "normalized/img/mt01.img";
    source.member.clear();
    EXPECT_EQ(PortTextureDocumentFromSource(source), "textures/normalized/img/mt01_img.json");
}

TEST(Texture, Dc1ContainerNamesAreStableAndLongPathsHashLikeRust) {
    const std::array<std::string, 6> expected = {
        "textures/commenu/a_eng/dunenter/dunenter_img-e4f1eb26b1393e1b.json",
        "textures/commenu/a_eng/dunenter/dunenter_img-a2e14711da50f58a.json",
        "textures/commenu/a_eng/dunenter/dunenter_img-f840864b5542ad51.json",
        "textures/commenu/a_eng/dunenter/dunenter_img-7dce6242a747eec0.json",
        "textures/commenu/a_eng/dunenter/dunenter_img-81d6e21f9f2f588f.json",
        "textures/commenu/a_eng/dunenter/dunenter_img-9ddcfc8ad7597c0e.json",
    };
    std::set<std::string> actual;
    for (unsigned index = 0; index < expected.size(); index++) {
        const std::string alias = "commenu/a_eng/dunenter/dunenter" + std::to_string(index) +
                                  ".pak/dunenter.img";
        const std::string path = PortTextureDocumentFromAlias(alias);
        EXPECT_EQ(path, expected[index]);
        actual.insert(path);
    }
    EXPECT_EQ(actual.size(), expected.size());

    const std::string long_alias = "path/" + std::string(180, 'a') + "/texture.img";
    EXPECT_EQ(PortTextureDocumentFromAlias(long_alias), "textures/long-path-01e6fc2a3093d2f98c7909ab.json");
    EXPECT_EQ(PortTextureDocumentFromAlias("root/../pic%FF.img"),
              "textures/root/%2E%2E/pic%25FF_img-beb47efc782afc7d.json");
    EXPECT_EQ(PortTextureDocumentFromAlias("map/room.pac/part.ipk/panel.img#entry:1"),
              "textures/map/panel-entry-1_img-bb9569f33eb90803.json");
}

TEST(Texture, BakedPngLoaderReadsSchema3ThroughDataVfs) {
    namespace fs = std::filesystem;
    const fs::path    dir = datafix::TempDir("texture_baked_vfs");
    const fs::path    data = dir / "data";
    const std::string document =
        R"({"schema_version":3,"groups":[0],"entries":[{"name":"TEST","pictures":[{"levels":["test.png","test.png"]}]}]})";
    datafix::WriteBytes(data / "textures/img/test_img.json", datafix::Bytes(document.begin(), document.end()));
    const std::string canvas_document =
        R"({"schema_version":3,"groups":[0],"entries":[{"name":"CANVAS","pictures":[{"canvas":true}]}]})";
    datafix::WriteBytes(data / "textures/img/canvas_img.json",
                        datafix::Bytes(canvas_document.begin(), canvas_document.end()));
    constexpr std::uint8_t png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x10, 0x50, 0x30, 0x68,
        0x00, 0x00, 0x01, 0x85, 0x00, 0xE1, 0xE2, 0xC4, 0x23, 0x36, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    datafix::WriteBytes(data / "textures/img/test.png", datafix::Bytes(std::begin(png), std::end(png)));

    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    PathsSetDataRoot(data);
    InitCDFile();
    PortDecodedTexture decoded;
    std::string        error;
    ASSERT_TRUE(PortLoadBakedTexture("textures/img/test_img.json", "TEST", 0, 0, 0, 3, nullptr, 0, decoded, error))
        << error;
    EXPECT_EQ(decoded.format, gfx::TextureFormat::Rgba8);
    EXPECT_EQ(decoded.width, 1);
    EXPECT_EQ(decoded.height, 1);
    ASSERT_EQ(decoded.levels.size(), 2);
    EXPECT_EQ(decoded.levels[0], (std::vector<std::uint8_t>{16, 32, 48, 128}));
    PortDecodedTexture source24;
    ASSERT_TRUE(PortLoadBakedTexture("textures/img/test_img.json", "TEST", 0, 1, 1, 1, nullptr, 0,
                                     source24, error, std::nullopt, 3))
        << error;
    EXPECT_FALSE(source24.has_alpha);
    PortDecodedTexture source32;
    ASSERT_TRUE(PortLoadBakedTexture("textures/img/test_img.json", "TEST", 0, 1, 1, 1, nullptr, 0,
                                     source32, error, std::nullopt, 4))
        << error;
    EXPECT_TRUE(source32.has_alpha);
    // Cached parsing must not hide edits to a loose document at the same path.
    std::string invalid_document = document;
    invalid_document[invalid_document.find("\"schema_version\":3") + std::strlen("\"schema_version\":")] = '0';
    datafix::WriteBytes(data / "textures/img/test_img.json",
                        datafix::Bytes(invalid_document.begin(), invalid_document.end()));
    EXPECT_FALSE(PortLoadBakedTexture("textures/img/test_img.json", "TEST", 0, 1, 1, 1, nullptr, 0,
                                      source32, error));
    datafix::WriteBytes(data / "textures/img/test_img.json", datafix::Bytes(document.begin(), document.end()));
    ASSERT_TRUE(PortLoadBakedTexture("textures/img/test_img.json", "TEST", 0, 1, 1, 1, nullptr, 0,
                                     source32, error))
        << error;
    EXPECT_EQ(source32.levels[0], (std::vector<std::uint8_t>{16, 32, 48, 128}));
    PortDecodedTexture canvas;
    ASSERT_FALSE(PortLoadBakedTexture("textures/img/canvas_img.json", "CANVAS", 0, 1, 1, 0, nullptr, 0,
                                      canvas, error));
    EXPECT_EQ(error, "canvas texture keeps its runtime backing");
    DataReadReset();
    PathsReset();
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

TEST(Texture, DuplicateTextureNamesUseExactSourceImgOrdinal) {
    namespace fs = std::filesystem;
    const fs::path            dir = datafix::TempDir("texture_duplicate_names");
    const fs::path            data = dir / "data";
    std::vector<std::uint8_t> img(232);
    std::memcpy(img.data(), "IMG", 3);
    img[4] = 2;
    const auto write32 = [&img](std::size_t offset, std::uint32_t value) {
        img[offset] = static_cast<std::uint8_t>(value);
        img[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        img[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        img[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    const auto write_texture = [&img, &write32](std::size_t header, std::array<std::uint8_t, 4> pixel) {
        std::memcpy(img.data() + header, "TIM2", 4);
        img[header + 6] = 1;
        const std::size_t picture = header + 16;
        write32(picture, 52);
        write32(picture + 8, 4);
        img[picture + 12] = 48;
        std::copy(pixel.begin(), pixel.end(), img.begin() + static_cast<std::ptrdiff_t>(header + 64));
    };
    std::memcpy(img.data() + 16, "DUP", 3);
    write32(16 + 32, 112);
    std::memcpy(img.data() + 64, "DUP", 3);
    write32(64 + 32, 164);
    write_texture(112, {1, 2, 3, 4});
    write_texture(164, {5, 6, 7, 8});
    datafix::WriteBytes(data / "dup.img", img);

    const std::string document =
        R"({"schema_version":3,"groups":[0],"entries":[{"name":"DUP","pictures":[{"levels":["red.png"]}]},{"name":"DUP","pictures":[{"levels":["blue.png"]}]}]})";
    datafix::WriteBytes(data / "textures" / "dup_img.json", datafix::Bytes(document.begin(), document.end()));
    constexpr std::uint8_t red_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    constexpr std::uint8_t blue_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x60, 0xF8, 0xFF,
        0x1F, 0x00, 0x03, 0x02, 0x01, 0xFF, 0xE6, 0x77, 0x0B, 0xAE, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    datafix::WriteBytes(data / "textures" / "red.png", datafix::Bytes(std::begin(red_png), std::end(red_png)));
    datafix::WriteBytes(data / "textures" / "blue.png", datafix::Bytes(std::begin(blue_png), std::end(blue_png)));

    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    PathsSetDataRoot(data);
    InitCDFile();
    std::vector<std::uint8_t> loaded(img.size() + 2048);
    char                      path[] = "dup.img";
    int                       loaded_size = 0;
    ASSERT_EQ(LoadFile2(path, loaded.data(), &loaded_size, 0), 1);
    DataReadSource first_source;
    ASSERT_TRUE(DataReadSourceRange(loaded.data() + 176, 4, first_source));
    EXPECT_EQ(PortTextureEntryIndexFromSource(first_source), 0);
    DataReadSource second_source;
    ASSERT_TRUE(DataReadSourceRange(loaded.data() + 228, 4, second_source));
    EXPECT_EQ(PortTextureEntryIndexFromSource(second_source), 1);
    for (unsigned index = 0; index < 2; index++) {
        PortDecodedTexture decoded;
        std::string        error;
        ASSERT_TRUE(PortLoadBakedTexture("textures/dup_img.json", "DUP", 0, 1, 1, 1, nullptr, 0,
                                         decoded, error, index))
            << error;
        ASSERT_EQ(decoded.levels.size(), 1);
        EXPECT_EQ(decoded.levels[0], index == 0 ? (std::vector<std::uint8_t>{255, 0, 0, 255})
                                                : (std::vector<std::uint8_t>{0, 0, 255, 255}));
    }
    DataReadReset();
    PathsReset();
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

TEST(Texture, BakedDocumentPathMapMatchesRustDc1Plan) {
    const char *map_path = std::getenv("CHRONICLE_TEXTURE_LAYOUT_MAP");
    if (map_path == nullptr) {
        GTEST_SKIP() << "set CHRONICLE_TEXTURE_LAYOUT_MAP to the private dcdata texture-map artifact";
    }
    std::ifstream stream(map_path, std::ios::binary);
    ASSERT_TRUE(stream) << map_path;
    const nlohmann::json mappings = nlohmann::json::parse(stream);
    ASSERT_TRUE(mappings.is_array());
    std::size_t mismatches = 0;
    for (const auto &mapping : mappings) {
        const std::string source_alias = mapping.at("source_alias").get<std::string>();
        const std::string expected = mapping.at("document_path").get<std::string>();
        const std::string actual = PortTextureDocumentFromAlias(source_alias);
        if (actual != expected) {
            ++mismatches;
            ADD_FAILURE() << source_alias << " mapped to " << actual << ", expected " << expected;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

// The game copies item and weapon icons as index images (PSM T8), whatever the images are here; a
// baked one is RGBA.
TEST(TexDecode, ColourImagesTakeIndexPsmCopies) {
    TexEnv env;
    Bytes  img = Img({
        {"src", Tim2({TIM2_RGB32, 2, 2, {Rgba32({Gs(255, 0, 0), Gs(0, 255, 0), Gs(0, 0, 255), Gs(255, 255, 255)})}, {}, 0})},
        {"dst", Tim2({TIM2_RGB32, 2, 2, {Rgba32({0, 0, 0, 0})}, {}, 0})                                                    }
    });
    Enter(img, 0);
    PortTextureRef src = Resolve("src");
    PortTextureRef dst = Resolve("dst");
    ASSERT_TRUE(src.valid && dst.valid);
    const sceGsTex0 &src_tex0 = *reinterpret_cast<const sceGsTex0 *>(&TexManager.GetTexture(const_cast<char *>("src"), -1)->tex0);
    const sceGsTex0 &dst_tex0 = *reinterpret_cast<const sceGsTex0 *>(&TexManager.GetTexture(const_cast<char *>("dst"), -1)->tex0);
    EXPECT_TRUE(PortMoveImage(src_tex0.TBP0, SCE_GS_PSMT8, 0, 0, 2, 2, dst_tex0.TBP0, SCE_GS_PSMT8, 0, 0));
}
