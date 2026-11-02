#include "moonmod/item_icon_override.hpp"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "itemdata.hpp"
#include "localize/localize_texture.hpp"
#include "memcard.hpp"
#include "menu_draw.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "platform/imagefile.hpp"
#include "texture.hpp"
#include "render/texture_registry.hpp"

namespace dc::game {}
using namespace dc::game;
using namespace dc::localize;
using namespace dc::render;

namespace {

constexpr int kIconSize = 32;

struct ModIcon {
    std::string               game_path;
    std::unique_ptr<CTexture> texture;
    unsigned                  key = 0;
    bool                      failed = false;
};

std::map<int, ModIcon> g_mod_icons;

u_long Tex0(unsigned tbp, int tbw, int psm, int tw, int th) {
    return (u_long) tbp | ((u_long) tbw << 14) | ((u_long) psm << 20) | ((u_long) tw << 26) | ((u_long) th << 30) |
           ((u_long) 1 << 34);
}

} // namespace

void SetItemIconOverride(int item_no, const char *game_path) {
    auto found = g_mod_icons.find(item_no);
    if (found != g_mod_icons.end()) {
        if (found->second.key != 0) {
            PortReleaseKey(found->second.key);
        }
        g_mod_icons.erase(found);
    }
    if (game_path != nullptr && game_path[0] != 0) {
        g_mod_icons[item_no].game_path = game_path;
    }
}

CTexture *ItemIconOverrideTexture(int item_no) {
    auto found = g_mod_icons.find(item_no);
    if (found == g_mod_icons.end() || found->second.failed) {
        return nullptr;
    }
    ModIcon &icon = found->second;
    if (icon.texture && PortKeyLive(icon.key)) {
        return icon.texture.get();
    }
    const auto file = moonmod::FindNativeResourceOverride(icon.game_path);
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;
    if (!file || !imagefile::LoadPng(*file, width, height, rgba) || width <= 0 || height <= 0) {
        std::fprintf(stderr, "item icon: item %d: cannot read %s\n", item_no, icon.game_path.c_str());
        icon.failed = true;
        return nullptr;
    }
    PortDecodedTexture decoded;
    decoded.width = kIconSize;
    decoded.height = kIconSize;
    decoded.format = gfx::TextureFormat::Rgba8;
    decoded.has_alpha = true;
    decoded.levels.push_back(width == kIconSize && height == kIconSize
                                 ? std::move(rgba)
                                 : LocalizeResample(rgba.data(), width, height, kIconSize, kIconSize));
    icon.key = PortCreateTexture(decoded, PortTextureOwner::Other, nullptr);
    if (icon.key == 0) {
        return nullptr; // no renderer yet; asked again next time
    }
    auto texture = std::make_unique<CTexture>();
    texture->block = -1;
    texture->width = kIconSize;
    texture->height = kIconSize;
    texture->bpp = 4;
    std::snprintf(texture->name, sizeof texture->name, "mod icon %d", item_no);
    texture->tex0 = Tex0(icon.key, kIconSize / 64 + 1, SCE_GS_PSMCT32, 5, 5);
    icon.texture = std::move(texture);
    return icon.texture.get();
}

// RetCTex (item_icon3d.cpp) asks ItemIconOverrideTexture first, so a mod's own icon wins.

namespace {

std::map<std::string, ModIcon> g_hud_images;
std::map<std::string, ModIcon> g_portrait_images;

CTexture *PngTexture(std::map<std::string, ModIcon> &cache, const char *game_path, int size, int size_log2) {
    if (game_path == nullptr || game_path[0] == 0) return nullptr;
    ModIcon &icon = cache[game_path];
    if (icon.failed) return nullptr;
    if (icon.texture && PortKeyLive(icon.key)) return icon.texture.get();
    const auto file = moonmod::FindNativeResourceOverride(game_path);
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;
    if (!file || !imagefile::LoadPng(*file, width, height, rgba) || width <= 0 || height <= 0) {
        std::fprintf(stderr, "hud icon: cannot read %s\n", game_path);
        icon.failed = true;
        return nullptr;
    }
    PortDecodedTexture decoded;
    decoded.width = size;
    decoded.height = size;
    decoded.format = gfx::TextureFormat::Rgba8;
    decoded.has_alpha = true;
    decoded.levels.push_back(width == size && height == size
                                 ? std::move(rgba)
                                 : LocalizeResample(rgba.data(), width, height, size, size));
    icon.key = PortCreateTexture(decoded, PortTextureOwner::Other, nullptr);
    if (icon.key == 0) return nullptr; // no renderer yet; asked again next time
    auto texture = std::make_unique<CTexture>();
    texture->block = -1;
    texture->width = size;
    texture->height = size;
    texture->bpp = 4;
    std::snprintf(texture->name, sizeof texture->name, "hud icon");
    texture->tex0 = Tex0(icon.key, size / 64 + 1, SCE_GS_PSMCT32, size_log2, size_log2);
    icon.texture = std::move(texture);
    return icon.texture.get();
}

} // namespace

CTexture *HudImageTexture(const char *game_path) {
    return PngTexture(g_hud_images, game_path, kIconSize, 5);
}

CTexture *PortraitImageTexture(const char *game_path) {
    return PngTexture(g_portrait_images, game_path, 128, 7);
}
