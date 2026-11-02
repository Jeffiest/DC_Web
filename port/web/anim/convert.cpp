// The animation editor's model converter (site/anim/): dcdata's model step (tools/dcdata/main.cpp, ConvertInto) for
// the packs the page copies into its raw tree, so the editor draws the same glTF the desktop editor does.
//
//   convert <raw> <output>
//
// The page runs it in a worker over Emscripten's memory file system: the raw tree holds the player's own packs (say
// dun/mainchara/c01d.chr), and <output>/data/ gets each one's .gltf, .gltf.bin, .game.json and textures.

#include <cstdio>
#include <exception>
#include <string_view>

#include "character_profile.hpp"
#include "kazarov/profile_reader.hpp"
#include "kazarov_bridge/rust/host_models.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: convert <raw> <output>\n");
        return 2;
    }
    try {
        auto reader = kazarov::make_profile_reader(
            [](std::string_view alias, std::string_view data) { return dcdata::profile::Read(alias, data); });
        const kazarov::ModelConvertReport report = kazarov::models_convert_dc1(argv[1], argv[2], std::move(reader), 1);
        std::printf("converted %llu texture sources into %llu PNGs and published %llu model files\n",
                    static_cast<unsigned long long>(report.texture_sources),
                    static_cast<unsigned long long>(report.pngs), static_cast<unsigned long long>(report.files));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "convert: %s\n", error.what());
        return 1;
    }
}
