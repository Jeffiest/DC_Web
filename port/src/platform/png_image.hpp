#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// Kazarov's imagefile::LoadPng takes only the RGBA8 files of its own texture catalog. Translations,
// the UI framework's plates and the renderer's own screenshots (RGB) are any standard PNG, so these
// are decoded here, to RGBA.
bool PngLoadRgba(const std::filesystem::path &file, int &width, int &height, std::vector<std::uint8_t> &rgba);
bool PngDecodeRgba(std::span<const std::uint8_t> bytes, int &width, int &height,
                   std::vector<std::uint8_t> &rgba, bool *cache_hit = nullptr);

struct PngDecodeCacheStats {
    std::size_t entries;
    std::size_t resident_bytes;
    std::size_t capacity_bytes;
};

// Counts compressed keys and decoded pixels; allocator bookkeeping is not included.
PngDecodeCacheStats PngDecodeCacheGetStats();
