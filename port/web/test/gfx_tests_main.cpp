// Kazarov's renderer tests in the browser (web workflow, docs/WEB.md): its gfx_*_test.cpp cases against Vulkan on
// WebGPU, offscreen, as headless Chromium on SwiftShader loses the device once a canvas presents. The page
// (gfx_tests.html) passes GoogleTest's arguments, e.g. ?args=--gtest_filter=GfxMisc.*; --no-clip-distances runs them
// on the layer's fallback for adapters without WebGPU's clip distances, and --mali as the layer runs on Arm's GPUs.

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
        } else if (std::strcmp(argv[i], "--mali") == 0) {
            // What the layer does on Arm's GPUs besides: held submissions, a frame at a time, finished frames shown.
            setenv("DC_WEBGPU_BATCH", "1", 1);
            setenv("DC_WEBGPU_ONE_FRAME", "1", 1);
            setenv("DC_WEBGPU_PRESENT_FINISHED", "1", 1);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
