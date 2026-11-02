// Package-owned PNG overrides for Chronicle's decoded native textures.
// PNGs live at mods/<package>/textures/<native texture name>.png. Later packages
// in MoonFactory's resolved load order win, matching model replacement behavior.

#include "moonmod/mod_texture.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "localize/localize_texture.hpp"
#include "moonmod/runtime.hpp"
#include "platform/imagefile.hpp"
#include "render/texture_registry.hpp"

namespace dc::game {}
using namespace dc::game;
using namespace dc::localize;
using namespace dc::render;

namespace fs = std::filesystem;

namespace {

struct TextureState {
    bool initialized = false;
    std::map<std::string, fs::path, std::less<>> by_name;
};

TextureState &State() {
    static TextureState state;
    return state;
}

std::string Fold(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool PngWithinDecodeBudget(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    unsigned char header[24]{};
    if (!input.read(reinterpret_cast<char *>(header), sizeof(header)) || header[0] != 0x89 || header[1] != 'P' ||
        header[2] != 'N' || header[3] != 'G' || header[12] != 'I' || header[13] != 'H' || header[14] != 'D' ||
        header[15] != 'R') {
        return false;
    }
    const std::uint64_t width = (static_cast<std::uint64_t>(header[16]) << 24) |
                                (static_cast<std::uint64_t>(header[17]) << 16) |
                                (static_cast<std::uint64_t>(header[18]) << 8) | header[19];
    const std::uint64_t height = (static_cast<std::uint64_t>(header[20]) << 24) |
                                 (static_cast<std::uint64_t>(header[21]) << 16) |
                                 (static_cast<std::uint64_t>(header[22]) << 8) | header[23];
    return width > 0 && height > 0 && width <= 16384 && height <= 16384 && width * height <= (64u * 1024u * 1024u) / 4;
}

void Initialize() {
    TextureState &state = State();
    if (state.initialized) return;
    state.initialized = true;

    const auto records = moonmod::SharedRuntime().Mods();
    for (const std::string &mod_id : moonmod::SharedRuntime().LoadedModOrder()) {
        const auto record = std::ranges::find(records, mod_id, &moonmod::ModRecord::id);
        if (record == records.end() || !record->enabled || record->state != moonmod::ModState::Loaded) continue;
        const fs::path textures = record->package_root / "textures";
        std::error_code error;
        if (!fs::is_directory(textures, error)) continue;
        for (fs::directory_iterator it(textures, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            const fs::file_status status = it->symlink_status(entry_error);
            if (entry_error || !fs::is_regular_file(status) || Fold(it->path().extension().string()) != ".png") continue;
            const std::string name = Fold(it->path().stem().string());
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) continue;
            entry_error.clear();
            const auto size = fs::file_size(it->path(), entry_error);
            if (entry_error || size == 0 || size > 64 * 1024 * 1024) continue;
            state.by_name[name] = it->path();
        }
    }
}

} // namespace

bool ModsTextureOverride(const char *name, PortDecodedTexture &texture) {
    if (name == nullptr || texture.width == 0 || texture.height == 0 || texture.levels.empty()) return false;
    Initialize();
    const auto found = State().by_name.find(Fold(name));
    if (found == State().by_name.end()) return false;
    if (!PngWithinDecodeBudget(found->second)) {
        std::fprintf(stderr, "mods: rejected oversized or malformed PNG texture override %s\n", found->second.string().c_str());
        return false;
    }

    int source_width = 0;
    int source_height = 0;
    std::vector<uint8_t> rgba;
    if (!imagefile::LoadPng(found->second, source_width, source_height, rgba) || source_width <= 0 || source_height <= 0) {
        std::fprintf(stderr, "mods: cannot read texture override %s\n", found->second.string().c_str());
        return false;
    }
    std::vector<std::vector<uint8_t>> levels;
    levels.reserve(texture.levels.size());
    for (size_t level = 0; level < texture.levels.size(); ++level) {
        const int width = std::max(1, static_cast<int>(texture.width >> level));
        const int height = std::max(1, static_cast<int>(texture.height >> level));
        levels.push_back(LocalizeResample(rgba.data(), source_width, source_height, width, height));
    }
    bool translucent = false;
    for (size_t i = 3; i < levels.front().size(); i += 4) {
        if (levels.front()[i] != 255) {
            translucent = true;
            break;
        }
    }
    texture.levels = std::move(levels);
    texture.format = gfx::TextureFormat::Rgba8;
    texture.has_alpha = translucent || texture.has_alpha;
    texture.four_bit = false;
    std::fprintf(stderr, "mods: replaced texture %s from %s\n", name, found->second.string().c_str());
    return true;
}
