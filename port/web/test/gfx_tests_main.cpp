// Kazarov's renderer tests in the browser (web workflow, docs/WEB.md): its gfx_*_test.cpp cases against Vulkan on
// WebGPU, offscreen, as headless Chromium on SwiftShader loses the device once a canvas presents. The page
// (gfx_tests.html) passes GoogleTest's arguments, e.g. ?args=--gtest_filter=GfxMisc.*, and --no-clip-distances
// runs them on the layer's fallback for adapters without WebGPU's clip distances.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

#include "platform/settings.hpp"

int main(int argc, char **argv) {
    SettingsSetEnvPrefix("DC");
    setenv("DC_GFX_OFFSCREEN", "1", 0);
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-clip-distances") == 0) {
            setenv("DC_WEBGPU_NO_CLIP_DISTANCES", "1", 1);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
